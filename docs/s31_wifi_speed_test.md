# ESP32-S31 Linux Wi-Fi 网速测试流程

在原生 Linux (S-mode) Wi-Fi blob 驱动上做吞吐/下载测试的标准流程。

## 前提

- 板卡已通过 `make flash-all` 刷写 OpenSBI + Linux + rootfs + bootloader。
- 串口只能通过 `/dev/ttyUSB0` 和 `idf.py monitor`（带 stdio/PTY）监控。
- 登录 Linux 后进入 shell（用户 `root`，无需密码）。

## 测试步骤

```sh
# 1. 拉起 wlan0 接口
ip link set wlan0 up

# 2. 关联 WPA2 AP（示例：ChinaNet-38D07C / AH3s0564ZhF）
wifi-connect wlan0 ChinaNet-38D07C AH3s0564ZhF

# 3. DHCP 获取地址
udhcpc -i wlan0

# 4. 下载一个大文件测速（qwen 官方 setup exe）
wget -O /dev/null "https://assets.qwenwork.cn/release/latest/qwenworkcn-setup-x64.exe"
```

## 期望结果

- `udhcpc` 拿到 `192.168.5.x` 的租约（网关 `192.168.5.1`）。
- `wget` 完整下载 setup exe（不中途死机，`heap malloc ... failed` 不出现）。
- 网速目标：`1 MB/s` 以上。

## 关联诊断

- 无线健康自检：内核启动后 `esp32s31-radio` 会打印一次
  `serialized core ready ... heap=<used>/<total> peak=<peak>`，
  用于观察内部 HP SRAM blob heap 的占用。
- 关联成功后 RX/TX 会打印 `[S31] Wi-Fi RX #n` / `[S31] Wi-Fi TX #n` 前若干帧。

## 已知问题记录

- 2026-08-15：大文件下载数秒后出现
  `esp32s31-radio: heap malloc size=1848 caps=0x0 failed`，随后所有请求无响应。
  根因是内部 HP SRAM blob heap 过小（约 147 KiB），Wi-Fi RX 动态缓冲池
  (dynamic_rx_buf_num=40, rx_ba_win=32) 在大流量下耗尽堆。

## 修复与当前状态（2026-08-15）

修复内容（均在 `linux-esp32-s31` / `radio-experiment`）：

1. **heap 缩小**：`radio_stack.c` 的 `s31_radio_stack_task` 把
   `static_rx_buf_num=10`、`dynamic_rx_buf_num=16`、`rx_ba_win=6`
   （原生 IDF 是 16/40/32，Linux heap 只有约 147 KiB 放不下）。
2. **RX 环溢出**：`esp32s31-radio-smode.c` 的
   `s31_radio_run_linux_pass()` 把 RX/HCI 投递移到 blob gate 之前，
   并把 `s31_radio_wifi_rx_pending()` 加入 worker 的 wait 条件，
   使收帧后立即交给 netstack，而不是等 10ms tick。
3. **TX 队列停摆**：`s31_wifi_xmit()` 遇到 TX 环满返回 `NETDEV_TX_BUSY` 后
   从未被唤醒，导致永久停摆。新增 `esp32s31_radio_wifi_ops.tx_wakeup`
   回调，blob pass 排空 TX 环后调用 `netif_wake_queue()`。
4. **UART 轮询**：`esp_log_level_set("*", ESP_LOG_WARN)` +
   `s31_wifi_tx_done` 每帧打印改为只打前 16 帧，避免 `esp_rom_printf()`
   忙等 115200 波特 UART 拉长 Wi-Fi 任务的 gate hold。
5. **RX 投递路径**：`netif_rx()` 改为 `netif_receive_skb()`（worker 在
   process context 投递，避免 backlog/ksoftirqd 往返）。

当前实测（`ChinaNet-38D07C`，16/16 RX/TX 环）：

- 关联 WPA2 + `udhcpc` 拿到 192.168.5.138 均正常。
- `wget` 下载 qwen setup exe 可稳定推进到 24%~32%（约 56~72 MB）且不再
  死机/掉线；速率约 235 KB/s（未达 1 MB/s 目标）。
- 大流量下 blob heap 峰值约 148.8 KiB（150.4 KiB 总量），RX esf_buf
  (`size=1848`) 在突发时偶发 `malloc ... failed`，导致 RX 丢包与 TCP
  重传，这是当前吞吐瓶颈。

未解决：吞吐只有 ~235 KB/s，距离 1 MB/s 还差约 4x。根因是内部 HP SRAM
blob heap 只有 ~147 KiB，RX 突发峰值需求（~42 个 1848 字节 esf_buf）加
16/16 环（~51 KiB）加任务栈（~32 KiB）已贴近堆顶；Wi-Fi 任务单次
queue-receive 最长可持 gate ~278 ms（含 ROM UART 轮询 + compat 层同步 +
blob 收发包处理），拉高了并发 RX buffer 需求。进一步优化需扩 heap（回收
DMA 描述符区）或缩短 gate hold。

## BLE/BR/EDR blob 尝试（2026-08-15，未完成）

- 已把 BT 控制器 blob（`libbt.a` + `libble_app.a` + `libbredr_app.a` +
  `libbtdm_common.a`）链接进 Linux payload：`radio-experiment/Makefile`
  去掉 `-DS31_WIFI_ONLY`、`esp32s31_defconfig` 开 `CONFIG_BT_ESP32S31` /
  `CONFIG_BT_BREDR`、`sdkconfig.radio.defaults` 开 `CONFIG_BT_ENABLED` /
  `CONFIG_BTDM_CTRL_MODE_BTDM`、`boot_link.txt` 加入 BT 归档，并新增
  `esp32s31_radio_bt_enable()` 命令 + `S31_RADIO_COMMAND_BT_ENABLE`。
- 实测：`esp_bt_controller_init rc=0`、`esp_bt_controller_enable rc=0`、
  `esp_vhci_host_register_callback rc=0`、`hci0` 注册、btdm 任务启动。
  但 btdm 任务随后在 RF 校准（`NVS has not been initialized` →
  full calibration）中忙等，占死 CPU，导致 Linux 用户态无法登录。
- 另一个 blocker：BT 使能后 `wifi_config_t` 布局随 `build-radio` 的 BT
  config 变化，`esp_wifi_set_config` 报
  `Config authmode threshold is invalid`，Wi-Fi 关联随之失败。
- 尝试扩 heap（把 factory app 停车后空闲的 `0x2f018000..0x2f030000` 96 KiB
  作为 gen_pool 第二 chunk）后，Wi-Fi 的 DHCP/数据路径出错（blob 内部对
  heap 地址分布有隐含假设，分块后 esf_buf 落进低 chunk 导致数据不通），
  该扩 heap 改动已回退。
- 结论：BT blob 能链接、控制器能 init/enable，但 RF 校准忙等 + Wi-Fi config
  布局冲突 + heap 不足三重阻塞，当前未走通 BLE 扫描。已把 BT 相关改动
  全部回退，回到可用的 Wi-Fi-only 状态。

## 连续 heap 尝试与回退（round1，2026-08-15）

为了给 BT 腾出更大的 blob heap，尝试把 `.s31_radio.data` 的 VMA 从
`0x2f030000` 直接下移到 `0x2f018000`（回收 factory app 停车后空闲的 96 KiB），
使 heap 变成**连续** 242 KiB（`0x2f034c30..0x2f071800`），而不是之前
失败的「低 chunk 分块」方案。改动点：`shared/s31_memory_layout.h` 的
`S31_RADIO_HEAP_BASE=0x2F018000` + `vmlinux-xip.lds.S` 的
`.s31_radio.data 0x2f018000`，并同步重刷 bootloader（factory app 的
`RADIO_SRAM_START` 也变成 0x2f018000，boot log 确认
`boot: radio heap 0x2f018000..0x2f071800`）。

**结果：Wi-Fi 下载硬 stall（回归）。** 下载在 ~448k（另一次实测 4856k，
即非确定性）后完全停摆：不再有 `Wi-Fi RX/TX`、`LMACTXDONE` 事件，heap 报告
`used=132048 free=116720` 恒定（heap 充足，排除 heap 大小）。gate-held PC
采样显示 blob 卡在 compat 层队列/同步路径（`s31_queue_send_locked` /
`xQueueReceive` / `s31_linux_blob_suspend`），不是硬死锁而是数据路径停摆。
而 147 KiB heap 基线（`0x2f030000`）能稳定跑到 72 MB。结论：blob 的
`.data/.bss/.dram1`（含 DMA 可见数据）不能整体下移，`0x2f018000..0x2f030000`
对 blob 有隐含的地址/一致性依赖，扩 heap 必须另找来源（缩小 boot 占用或
回收其他 carve-out），不能侵占 factory app 保留区。已回退到 `0x2f030000`，
并重刷 bootloader 恢复。

## 吞吐优化进行中（round1，2026-08-15）

下载期 gate-held PC 采样（147 KiB 基线）解析符号后发现热路径开销集中在：

- `s31_gate_timing_released` / `s31_gate_timing_acquired`（gate 每次进出做
  `task_sched_runtime()` + `spin_lock_irqsave` 统计，纯诊断开销）；
- `__wake_up_common_lock`（compat 层 wait/wake）；
- `task_sched_runtime`（调度统计，主要来自 gate timing）；
- `kernel_fpu_begin/end`（gate 每次进出的 FPU 保存/恢复，必要开销）；
- `vprintk_store` / `console_flush_all`（periodic heap/PC 报告刷屏）。

已做改动：关闭 gate-hold 直方图统计（`s31_gate_timing_enabled=false`，把
acquire/release 改成 disabled 时无锁早退）；heap 报告/PC 采样周期 5s→20s；
在 `s31_idf_alloc`/`__wrap_heap_caps_free` 加 size 直方图，periodic 报告里
打印各 size 桶的 live/peak。

实测结果（`ChinaNet-38D07C`，147 KiB heap）：

- 关闭 gate-hold 统计后吞吐 ~212 KB/s（vs 基线 ~220 KB/s），**无明显提升**，
  说明 gate-hold 直方图的开销不是吞吐瓶颈。
- size 直方图定位出 125 KiB boot 占用的构成：Linux 侧 WiFi RX/TX 环
  （16+16 × 1616 B ≈ 51 KiB，走 `s31_radio_sram_alloc`）+ worker 栈 8 KiB +
  blob heap_caps 分配 ~55 KiB（任务栈 ~21 KiB + 静态 RX 10×1848 + 小对象）。
  动态 RX/TX buffer 只剩 ~24 KiB，而 RX 突发需要 ~42 个 1848 esf_buf，
  因此 `malloc size=1848 failed` 持续出现（opt1 全程 ~686 次）→ RX 丢包 →
  TCP 重传。
- 尝试把 TX 环 16→8 腾 12 KiB 给 RX（`S31_WIFI_TX_SLOTS=8` +
  `dynamic_rx_buf_num=20` + `dynamic_tx_buf_num=16`）后，下载在 615k 处
  `TX ring full, dropping frame len=54` 停摆——TX 环 8 槽装不下 TLS 握手 +
  ACK 突发，已回退到 16 槽。
- 结论：147 KiB heap 是硬约束。RX 环 16 槽不能减（AMPDU 突发溢出）、TX 环
  16 槽不能减（握手停摆）、RX/TX 动态 buffer 受 24 KiB 余量限制。吞吐要
  上 1 MB/s 必须先解决 heap 扩容（连续 heap 已证伪，见上节），或改用
  零拷贝 RX（esf_buf 直接进 net stack，省掉 51 KiB 环）。

## 零拷贝 RX + SRAM 扩 reclaim + max-throughput 配置（round2，2026-08-15）

- **零拷贝 RX**：把 25.6 KiB 的 RX 数据环换成 12 B/slot 的 esf_buf 描述符环
  （`s31_wifi_rx_desc`），blob 直接把 esf_buf 交给 Linux，省掉一次 memcpy +
  25 KiB SRAM。esf_buf 的回收放到 worker 的 blob pass（gate 内），经
  `s31_radio_wifi_free_rx_buffer()` 调 `esp_wifi_internal_free_rx_buffer`。
  RX 环加深度到 128 槽（代价仅 ~1.5 KiB）。
- **heap 扩容到 350 KiB**：① 工厂 app 停车后的 FreeRTOS 堆 `0x2f018000..0x2f030000`
  （96 KiB，**关键：必须在 loader 里 `clear_sram_for_linux` 一并 memset +
  Cache_WriteBack_Invalidate，否则脏 cache 导致 DHCP/数据路径不通**）；
  ② ROM 下载模式缓冲区尾 + PRO CPU 启动栈 `0x2f078c00..0x2f07cfb0`（17 KiB）；
  ③ **blob 的 `.rodata/.srodata`（82.4 KiB 只读常量）挪到 XIP flash**（lds
  里从 `.s31_radio.data` 移到 `.text`），blob data+bss 从 115 KiB 缩到 32 KiB。
- **max-throughput 配置（ESP-IDF 参考）**：`static_rx=48 / dynamic_rx=72 /
  dynamic_tx=64 / rx_ba_win=64 / tx_ba_win=64`（tx_ba_win 是编译期 Kconfig，
  加进 `sdkconfig.radio.defaults`）。350 KiB heap 装得下：`allocfail=0`。
- **TX 环 16→48 槽**：修掉 ACK 洪泛导致的 `TX ring full, dropping frame`。
- 结论：实测（Griefer AP）下载 57.1M/300s ≈ **190 KB/s**，无硬停摆（中途一段
  stalled 自行恢复）。**瓶颈不在 WiFi 空口，而在 CDN/互联网**（用户确认）。
  本地 payload 测速 `http://10.131.205.113:8081` 因 board 关联/DHCP 抖动 +
  服务器未响应暂未测到。

## malloc-fail 控制台洪泛修复（round1，2026-08-15）

`__wrap_heap_caps_malloc` 失败时原实现每次 `pr_err` + `s31_radio_heap_report`。
RX buffer 短缺时（AMPDU 突发）每个帧都打两行，而 printk 会在 blob gate 内
同步刷 UART0 DMA console（115200 波特），一个突发就能把 Wi-Fi 任务卡死在
console 输出上，表现为下载在几十 KB 处整体停摆/无输出（opt3 多次复现：
16 KB / 20 s / 27 s 处停）。修复：失败计数用 `atomic_inc` 累计，打印改成
`pr_err_ratelimited`，heap report 只在计数低 8 位为 0 时（每 256 次）打一次；
periodic 报告里带上 `allocfail=N` 总数。修复后下载恢复稳定推进（复测到
32+ MB 无停摆，allocfail=527 只零星打印），说明此前的停摆有一部分确实是
console 洪泛把 gate 卡死，而不是纯数据路径死锁。

但长时间下载（20 min 档）仍在 ~98 MB（43%）处停摆：allocfail 累计到 725
后 RX/TX 数据路径完全停止（`used=132048 free=18416` 恒定，内核周期报告
照常），属于 RX buffer 池（~18 个 esf_buf）在 AMPDU 突发下被击穿 → TCP
拥塞窗口塌缩 → 连接僵死，而不是 console 或硬死锁。这是 147 KiB heap 的
**稳定性**上限，与吞吐上限同源：RX 突发峰值需要 ~42 个 esf_buf，但 boot
占用 125 KiB 后只剩 ~24 KiB 给动态 buffer，最多 ~18 个 RX buffer。要同时
解决吞吐与停摆，仍绕不开 heap 扩容（连续 heap 已证伪，见上节）或零拷贝
RX（省 51 KiB 环换 ~27 个 RX buffer）。

## 换 AP win 测试（round3，2026-08-15）

- `/tmp/cmd_*.txt` 里 `wifi-connect wlan0` 已从 `Griefer` 改为 `win`。
- `win` AP 关联成功：channel=1，DHCP 拿到 `192.168.10.15`，ping 宿主机
  `192.168.10.4` 4/4 通。
- 原测速服务器 `http://10.131.205.113:8081/payload` 在 win 网络不可达：
  board 上 wget `Connecting...` 后 TCP SYN 无响应，最终
  `wget: can't connect to remote host: Operation timed out`。期间 periodic
  报告全程 `allocfail=0 rx_drop=0 tx_drop=0`，不是 driver buffer 丢包。
  宿主机 `192.168.10.4` 同样 ping/connect 不到 10.131.205.113。
- 改在宿主机起本地 HTTP server：`python3 -m http.server 8082 --bind 0.0.0.0`
  目录 `/tmp`，文件 `payload50.bin`（50 MB，`dd if=/dev/zero`）。
- board 上 `wget -O /dev/null http://192.168.10.4:8082/payload50.bin`：
  **50 MB 完整下载完成，无 stall**。periodic 报告全程
  `allocfail=0 rx_drop=0 tx_drop=0`，无 `TX ring full`，无断连。
- 吞吐稳定约 **3.1 Mbps**（用 periodic 时间戳锚定：42 s 时 4.3 MB →
  142 s 时 45.9 MB，每 20 s 推进约 7.4-7.7 MB）。速率偏低但平稳，
  可能受 2.4G ch1 干扰/链路 MCS 限制。
- 结论：win ch1 下本地同子网数据路径无 stall；原先 Griefer ch11 的
  payload 首包后 stall 还不能简单归因，因为原服务器在 win 网络不可达，
  无法做同服务器换信道对照。下一步若要继续区分同频干扰 vs 驱动问题，
  建议把测速服务器放进两个 AP 都能同子网可达的位置（或让 win AP 桥接到
  10.131.205.0/24），再跑同一 payload 的 ch1 vs ch11 对照。
