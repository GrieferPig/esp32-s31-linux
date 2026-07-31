# ESP32-S31 动态外设配置与测试

系统只构建一个基础 DTB `esp32s31_generic.dtb`。容易产生引脚、时钟或
硬件资源冲突的外设通过运行时 DT overlay 启用；不再使用 `S31_PROFILE`，
也不需要为切换外设重新构建 Linux/OpenSBI。

内核的 `/dev/s31-overlay` 管理器允许多个互不冲突的 overlay 同时存在。
加载前会检查物理 GPIO、GPIO matrix 输入信号以及 `sdmmc-host` 等独占
硬件资源；有冲突时返回 `EBUSY`，并在内核日志中指出冲突对象。同名
overlay 的再次加载是原子 reassign：先移除旧实例，应用新实例，失败则
恢复旧实例。pinctrl 在设备解绑后把对应 GPIO 恢复为高阻安全态，并撤销
GPIO matrix 和专用 GMAC/SDIO pinmux 选择。

## 配置命令

可用配置：

```sh
s31-overlay list
```

应用并持久化多个互不冲突的配置：

```sh
s31-overlay apply timers
s31-overlay apply gdma
s31-overlay status
```

临时应用（重启后恢复已持久化的配置）：

```sh
s31-overlay apply pwm-counter --volatile
```

只移除一个配置，或移除全部配置：

```sh
s31-overlay remove gdma
s31-overlay remove --all
```

带 GPIO matrix route 的 overlay 可以在加载时指定 GPIO。先查看可分配项：

```sh
s31-overlay routes uart1
s31-overlay routes pwm-counter
```

然后加载或动态 reassign；同名 overlay 的其他 route 保持 DTBO 默认值：

```sh
s31-overlay apply uart1 uart1.tx=35 uart1.rx=36
s31-overlay apply uart1 uart1.tx=46 uart1.rx=47
s31-overlay apply pwm-counter ledc0.out=38 pcnt0.in=39
```

GPIO26–32 承载当前正在运行的 XIP Flash 总线，GPIO33/34/41 是 IDF 标记
的无效 pad，GPIO58/59 被 Linux UART0 console 占用，配置程序会直接拒绝。
两个 active overlay 使用同一 GPIO、两个 matrix input 使用同一输入信号、
SDMMC0/1 同时占用同一个 host，或者动态 route 移入另一 overlay 已占用的
pad，都会返回 `Device or resource busy`。AHB-GDMA 与模拟比较器共享 HP
interrupt matrix source 22，也作为独占资源检测，不能同时启用。
matrix output 信号允许硬件 fan-out，但每个输出 pad 仍只能有一个 owner。

GMAC RGMII、SDMMC、ADC、触摸和比较器使用 IDF 指定的专用 IO-MUX pad。
这类信号不能像 GPIO matrix 信号一样任意换脚；SDMMC 的两套合法 IO-MUX
布局分别由 `sdmmc0`、`sdmmc1` DTBO 表示。GMAC/SDMMC dedicated-pad 选择
会随设备 probe/remove 自动设置和释放。

`persist` 是 flash 末尾的 16 KiB MTD 分区。其前 2 KiB 保存带序列号和
CRC32 的 DTBO 配置；启动脚本 `S03s31-overlay` 在用户空间启动早期恢复
持久配置。DTBO 本身随只读 rootfs 安装在 `/usr/lib/s31-overlays`。从
`0x800` 起预留给 Key Manager 的 HUK/密钥恢复记录；DTBO 更新会保留与
回写同一擦除块内的 HUK 主记录。持久化格式固定为 v1，不读取、迁移或
写入其他版本。

| overlay | 启用内容 | 测试方式 |
| --- | --- | --- |
| `timers` | SYSTIMER、RTC timer | Counter 计数递增 |
| `pwm-counter` | LEDC0/1、MCPWM0–3、PCNT0/1、Sigma-Delta | 注册数量及 PWM apply |
| `gdma` | AHB-GDMA | `dmatest` 64 KiB、1 次（随机偏移/长度） |
| `analog` | ADC、温度、触摸、模拟比较器 | IIO/hwmon 采样与事件接口 |
| `gmac` | GMAC，最高 1 Gbit/s | 接口注册；链路测试需外部 PHY |
| `sdmmc0` | SDMMC slot 0 | 仅软件构建，不访问卡 |
| `sdmmc1` | SDMMC slot 1 | 仅软件构建，不访问卡 |
| `uart1` | UART1，可动态分配 TX/RX matrix GPIO | `/dev/ttyS1` 注册 |
| `uart2` | UART2，可动态分配 TX/RX matrix GPIO | `/dev/ttyS2` 注册 |

应用配置后运行：

```sh
s31-peripheral-test
```

也可以显式指定当前测试配置：

```sh
s31-peripheral-test gdma
```

基础 DT 中的 TIMG0/TIMG1/RTC watchdog 每次都会做非破坏性的注册、ping
和 magic-close 测试。

## 持久化测试

```sh
s31-overlay apply timers
s31-overlay apply gdma
s31-overlay apply uart1 uart1.tx=35 uart1.rx=36
s31-overlay status
reboot
# 重启后
s31-overlay status
s31-peripheral-test
s31-overlay remove --all
```

重启后 active 集合应同时包含 `timers`、`gdma` 和带原 GPIO 分配的
`uart1`，Counter、GDMA 和 `/dev/ttyS1` 应继续可用。最后的
`remove --all` 会把持久配置恢复为空集合。

## 需要额外硬件的测试

- GMAC 1 Gbit/s：需要 RGMII PHY、正确的时钟/延迟、网线以及支持
  1000BASE-T 的对端；板上未连接链路时只能验证驱动和接口注册。
- LEDC/MCPWM/Sigma-Delta：软件可以验证 sysfs 配置；频率、占空比和
  Sigma-Delta 波形需要示波器或逻辑分析仪连接 overlay 指定的输出引脚。
- PCNT：需要外部脉冲源，或把一个 PWM 输出回接到 PCNT 输入。
- 模拟比较器：需要向对应输入脚施加跨越参考电平的电压，并从 IIO
  event 接口读取上升/下降事件。
- ADC 精度：需要已知直流电压源和共地连接，以检查量程、线性和校准。
- 触摸：GPIO14/channel 8 已验证可完成转换；灵敏度、阈值和实际触摸动作
  仍需要适当的触摸电极。当前样片的部分未接通道不会产生 `meas_done`，
  读取这些通道会返回 `ETIMEDOUT`。
- SDMMC0/1：按当前要求不做上板访问测试。后续测试需要匹配所选 slot
  引脚的卡座、电源、上拉、card-detect/write-protect 接线和 SD 卡。
- watchdog：非破坏性测试不会触发复位；复位验证需要允许程序停止喂狗
  并接受整板重启。

GPTimer 和 RMT 按当前范围不移植，也没有对应 overlay。

## 本次上板结果

通过 `/dev/ttyUSB0` 完成刷写和串口测试。已验证：多个无冲突 DTBO 并存、
物理 GPIO/matrix input/独占 host/interrupt source 冲突拒绝、同名 DTBO
事务式 GPIO reassign 及失败回滚；16 KiB `persist` 的 DTBO 配置
写入、CRC 和跨重启自动恢复；TIMG0/TIMG1/RTC
watchdog ping/magic-close；SYSTIMER/RTC timer 递增；7 个 PWM 控制器和 2 个
PCNT 注册及 PWM apply；ADC oneshot、温度采样、触摸 channel 8 转换、比较器
事件接口；GMAC 驱动以 DWMAC1000 模式 probe/remove；AHB-GDMA 使用 12-byte
IDF 描述符布局完成 64 KiB `dmatest`，结果为 1 次、0 failure。

PWM/Counter 默认路由使用 GPIO20–25 和 GPIO35–37。首轮测试曾把 SDM/PCNT
默认放到 GPIO26–28，上板确认这些 pad 一旦切出 Flash IO-MUX 会立即中断
XIP 取指；最终实现已改用安全引脚，并在两层校验中永久拒绝 GPIO26–32。

未执行 SDMMC 卡访问（按要求只实现软件）。GMAC 1 Gbit/s 线速、PWM/SDM
实际波形、PCNT 外部计数、比较器阈值越过、ADC 精度和 watchdog 复位测试，
需要上一节列出的外部硬件或允许破坏性复位。
