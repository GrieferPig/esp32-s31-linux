# ESP32-S31 Linux Wi-Fi Blob Handoff

你正在 `/home/grieferpig/s31linux` 继续 ESP32-S31 Linux native Wi-Fi/BT blob 移植工作。

目标：
完全移除 FreeRTOS 运行时，由 Linux kthread 兼容层运行 ESP-IDF Wi-Fi/BT blob。最终必须稳定关联 WPA2 Wi-Fi、DHCP 成功，并能够下载 `baidu.com`。不要停留在静态分析，修改后必须上板验证。

开始前：
1. 完整读取根目录 `AGENTS.md`。
2. 完整读取 `docs/s31_hardware/` 下所有文件。
3. 阅读 `docs/s31_wifi_rx_experiments.md`，避免重复已排除实验。
4. 检查 dirty worktree，保留所有现有改动，不要回退用户或前序 agent 的修改。

当前实机状态：
- OpenSBI/Linux 已恢复正常启动。
- 此前启动失败是 flash corruption/分区地址错误，不是 OpenSBI trap。
- 必须用根目录 `make flash-all` 刷写；Linux `xipImage` 位于 flash `0x400000`。
- 当前 Linux 6.12、CLIC、radio worker、compat RTOS、Wi-Fi task 均正常启动。
- radio 和 Wi-Fi kthread 已设为高优先级，用户明确要求不要回退。
- `wlan0` 能正常注册、扫描和关联。
- 当前现场扫描不到 `AHSS-WLAN1/5`；可以先用可见的 `AHSS-WLAN2` 验证。
- 密码均为 `flysanshi`。

已经验证工作的方向：
- 开放 AP 上曾完整收到 DHCP Offer/Ack并获得地址，证明以下通用路径基本可用：
  - Linux cfg80211/fullmac 接口
  - Wi-Fi RX IRQ
  - ISR 延迟执行
  - kthread 唤醒
  - RX queue/callback
  - Linux 网络栈收包
- WPA2 AP `AHSS-WLAN2` 当前可以稳定完成：
  - 802.11 authentication/association
  - WPA2 四次握手
  - PTK/GTK 安装
  - `wlan0` 进入 `LOWER_UP`
- 已确认：
  - cipher 为 CCMP
  - PTK 使用硬件 slot 4
  - GTK 使用硬件 slot 0
  - key valid bitmap 为 `0x11`
  - key metadata、长度和 KCK/KEK/TK 推导无明显错误
  - CCMP 引擎 type0 已使能
  - DHCP Discover 能进入 HMAC/LMAC并获得 TX completion
  - Wi-Fi blob、ESP PHY/HAL 归档与原生 IDF 构建基本一致
- RX/TX staging、queue timeout、FromISR 四参数 ABI、tick bridge、callee-saved 寄存器和任务栈大小已有修正。
- 所有 blob 可直接访问或长期持有的对象必须位于内部 HP SRAM，该约束不能放松。

当前失败：
- `AHSS-WLAN2` WPA2 关联成功后，`udhcpc` 连续发送 5 次 Discover，但没有 Offer。
- `wget http://baidu.com` 因无地址/DNS失败。
- DHCP TX completion 存在，但 AP 没有发送可见的目标下行数据。
- RXSTAT 中若干 10-bit 错误计数已饱和到 `1023`，但部分在扫描阶段已饱和，不能直接认定为 MIC 错误。
- 当前启动日志存在：
  `esp_phy_load_cal_data_from_nvs: NVS has not been initialized`
  虽然关联仍成功，但需要与原生 IDF初始化顺序核对。

已排除或不足以修复问题的方向：
- 单纯提升 radio/wi-fi kthread 优先级。
- 强制 DHCP 从 TID 7 改为 TID 0。
- 在 TX 调用窗口模拟旧 cooperative worker 的不可 yield 上下文。
- 切换 modem clock checking Kconfig。
- 替换历史 `esp_hw_support` 归档。
- 扩大 task SRAM 栈。
- 修复完整整数 callee-saved 保存。
- promisc callback、key wrapper等诊断 hook。
- 把问题笼统归因于 Linux RX callback：开放 AP 已证明该路径可工作。

优先排查方向：
1. 发送时序与同步语义
   - 对每个 WPA2阶段记录：
     IRQ产生 -> radio worker运行 -> blob ISR -> Wi-Fi task唤醒 ->
     key install -> TX enqueue -> `esp_wifi_internal_tx()` ->
     LMAC submit -> TX completion。
   - 分别记录 wall time、task runtime和 off-CPU time。
   - 已观察到一般 `irq -> worker` 平均约 0.57~0.81 ms，最大约 12.8 ms。
   - 曾观察 Wi-Fi task 在 queue-receive 路径持有 blob gate 约 290 ms，
     runtime也接近290 ms，说明不是普通 Linux 用户态抢占，需要定位这段blob执行。
   - 检查 gate 持有期间是否阻止更高优先级兼容任务或 ISR后续阶段运行。

2. RX buffer ownership/freeing
   - 精确对照 ESP-IDF `wifi_default_action_sta_connected()`、
     `esp_wifi_internal_reg_rxcb()` 和 `esp_wifi_internal_free_rx_buffer()`。
   - 确认 `eb` 与 payload buffer 的所有权、释放时机、重复释放和提前释放。
   - 检查 WPA2受保护帧是否在正常 RX callback之前被错误释放。

3. `*_internal` allocation语义
   - 审计 blob实际调用的 malloc/calloc/realloc/free、heap_caps接口。
   - 检查 alignment、zeroing、capability、可用大小和失败返回语义。
   - 确认所有 queue、semaphore、event、timer、task TCB/stack、
     TX/RX buffer和descriptor都在内部 HP SRAM。

4. FreeRTOS同步语义
   - queue send/receive front/back/overwrite。
   - semaphore/mutex递归、owner、priority inheritance。
   - FromISR的 `pxHigherPriorityTaskWoken` 累积语义。
   - event group clear/wait和timeout返回值。
   - unblock后是否应立即运行更高优先级任务。
   - lost wakeup：pending清零和kthread进入wait之间的竞态。

5. timer/time-unit行为
   - 确认 FreeRTOS tick为100 Hz，毫秒/tick换算与IDF一致。
   - 确认硬tick只推进一次，但持有blob gate忙等时仍能看到时间增长。
   - 检查esp_timer、ets_timer和FreeRTOS timer的arm/stop/rearm语义。
   - 对比原生IDF WPA2阶段的timer创建、到期和重臂顺序。

6. radio/crypto初始化冲突
   - 一比一对照ESP-IDF S31启动顺序，包括：
     `esp_security_init`、modem clock/power、PHY、Wi-Fi module、crypto。
   - 检查Linux已有硬件crypto驱动是否触碰同一时钟、reset、DMA或寄存器。
   - 核实该驱动即使未启用，clock/power/reset状态是否与IDF不同。
   - 检查NVS未初始化是否导致PHY校准或安全状态遗漏。

7. CCMP RX key lookup
   - TX可显式使用slot，RX依赖Key ID/地址自动匹配。
   - 对照原生IDF和Linux下key table全量元数据、BSSID、interface、keyid、
     pairwise/group选择和replay counter初始化。
   - 不要假设blob本身错误；优先寻找OSI语义、初始化顺序或其他Linux驱动冲突。

实机测试流程：
- 关闭OpenOCD后才能刷写。
- 构建只能使用根目录：
  `make linux`
  必要时再执行其他根Makefile target。
- 刷写使用：
  `make flash-all`
- 串口只能使用 `/dev/ttyUSB0` 和带stdio/PTY的 `idf.py monitor`。
- Linux登录后：
  `ip link set wlan0 up`
  `wifi-scan wlan0`
  `wifi-connect wlan0 AHSS-WLAN2 flysanshi`
  `udhcpc -i wlan0 -n -q -t 5`
  `wget -O /tmp/baidu.html http://baidu.com`
- 如果 `AHSS-WLAN1` 或 `AHSS-WLAN5` 可见，优先按用户指定网络测试。
- OpenOCD只允许telnet/DMI，不使用GDB。

修改原则：
- 一次只验证一个有判别力的假设。
- 每轮把假设、修改、镜像、串口结果和结论追加到
  `docs/s31_wifi_rx_experiments.md`。
- 诊断日志不得长期输出TK、PMK或其他敏感密钥材料。
- 取得证据后移除高洪泛诊断。
- 不修改OpenSBI来掩盖Wi-Fi问题。
- 若启动停在OpenSBI，先按AGENTS.md的flash corruption流程完整重刷，
  不要立即分析trap ABI。

下一步建议：
先给290 ms Wi-Fi queue-receive执行段增加低开销分段时间戳，定位具体blob事件和调用点；同时记录对应期间的tick、IRQ pending、radio worker可运行状态和gate owner。确认它是正常协议等待、兼容层错误忙等，还是因lost wakeup直到超时才返回。然后再决定修queue/timer/gate调度中的哪一层。
