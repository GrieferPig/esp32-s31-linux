# ESP32-S31 i80 LCD 测试

基于 ESP-IDF 官方 [i80_controller 示例](https://github.com/espressif/esp-idf/tree/a602e67b0bf9ee0806dc4e1df7afc9affedf5c33/examples/peripherals/lcd/i80_controller)。默认运行全屏 DMA 竖条动画，也可以在 `idf.py menuconfig` 中关闭 **Run continuous full-screen DMA animation**，运行原版 LVGL 动画。默认使用 S31 的 LCD_CAM 原生 i80 输出，数据总线 8 位，WR 时钟设置为 13 MHz（实际整数分频约 13.33 MHz），像素格式 RGB565，DMA 绘图缓冲区放在内部 RAM。

| LCD 信号 | ESP32-S31 |
| --- | --- |
| LCD_D0 | GPIO43 |
| LCD_D1 | GPIO42 |
| LCD_D2–LCD_D7 | GPIO44、45、46、47、48、49，按位顺序 |
| WR# | GPIO40 |
| D/C 或 RS | GPIO39 |
| CS# | GPIO38 |
| RESET# | GPIO3 |
| RD# | GPIO36 |
| GND | 共地 |

表格记录本机盾板上实测通过的映射。HiLetgo 3-01-1292 盾板按背面 `LCD_*` 丝印接线。右侧两段排针分开排列，`LCD_D2` 到 `LCD_D7` 在一段，`LCD_D0` 和 `LCD_D1` 在靠 `SD_SS` 的另一段。控制排针用 `LCD_RST`，电源排针另有 Arduino 的 `RESET`。模块的 `5V` 与 `GND` 用于供电，S31 与盾板共地。[盾板背面图](https://m.media-amazon.com/images/I/61-8TF8LK%2BL._AC_SL1100_.jpg)

背光由模块供电。RD# 接 GPIO36，写入时保持高电平，读回时由 GPIO 产生读脉冲。默认面板为 ILI9341，使用 320×240 横屏和 RGB565 写入。可通过 `idf.py menuconfig` 调整配置。

上电后，固件先用 LCD_CAM DMA 向 GRAM 写入 64 个已知像素，再读取 ILI9341 的 RDID4（`0xD3`）和 RAMRD（`0x2E`）。S31 的 LCD_CAM 负责写入，读回时将 D0–D7 切换成输入，由 GPIO36 驱动 RD#。RAMRD 丢弃首个 dummy 字节，将后续 RGB666 数据转换为 RGB565，逐像素与写入值比较，随后启动所选动画。将 RD 引脚设为 `-1` 可用于只接写入信号的模块。

## 构建和验收

在支持 `esp32s31` 的 ESP-IDF 环境中进入本目录。工程默认目标是 `esp32s31`。首次构建会从 ESP Component Registry 下载依赖。

```sh
idf.py build
```

在已获准烧写的 S31 测试板上执行以下命令，将 `PORT` 替换为该测试板的端口。工程默认以 115200 波特率监视串口。默认分区配置写入 bootloader、分区表和应用三个镜像，应看到三次 `Hash of data verified`。

```sh
idf.py -p PORT -b 2000000 flash
idf.py -p PORT monitor
```

串口出现 `GRAM PASS: 64/64 pixels match` 时，已确认 DMA 写入的像素可以从面板 GRAM 读回。若出现 `GRAM FAIL`，日志会列出首批不匹配的像素、上下拉条件下的芯片 ID 和状态寄存器，以及纯 GPIO 写入的回读结果。全屏模式显示移动的黄色竖条，每帧传输整块屏幕（320×240、RGB565，共 153,600 字节），每 120 帧按 DMA 完成事件报告实测 FPS。启动时还通过 ILI9341 的 GET_SCANLINE（`0x45`）独立测量面板扫描频率。

i80 时序依据见 [ESP32-S31 TRM](https://documentation.espressif.com/esp32-s31_technical_reference_manual_en.pdf) §24.3.7.2 和 §24.5.2，LCD_CAM 的 `LCD_CAM_LCD_RGB_MODE_EN`、`LCD_CAM_LCD_CMD` 与 `LCD_CAM_LCD_DOUT`。数据、WR 和 D/C 由 ESP-IDF 的 `esp_lcd` 驱动经 GPIO Matrix 路由。

ILI9341 的 8 位总线数据格式、RAMRD、RDID4 和 RD# 时序分别见 [ILI9341 数据手册](https://www.olimex.com/Products/ARM/ST/STM32-LCD/resources/ILI9341.pdf) §7.6.3、§8.2.24、§8.3.23 和 §19.3。
