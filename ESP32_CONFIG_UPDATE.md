# esp32-config 配置体验更新

这是可以直接覆盖到 `esp32-s31-linux` 主仓库根目录的源码更新包。
代码基于主仓库提交 `a6b62c6426f06f00ff3be7ee8e6ab1c67a1ff104`。
文档只使用用户提供的 `docs-new.zip` 作为基线，没有使用远端 docs 仓库。

## 覆盖与构建

在已有主仓库外执行（将路径换成自己的仓库）：

```sh
unzip -o esp32-config-update.zip -d /path/to/esp32-s31-linux
cd /path/to/esp32-s31-linux
make rootfs
```

在现有工具链及其他构建依赖准备好的环境中运行。`make rootfs` 会重新合并
BusyBox 配置、重建本地工具和带补丁的 BTstack，并生成镜像能力清单及时区数据。
保留原来的 4 MiB rootfs 分区大小检查。
构建完成后，使用主仓库的烧录目标，例如：

```sh
make flash-existing-rootfs PORT=/dev/ttyUSB0
```

烧录端口和镜像 profile 应与现有开发板配置一致。本包是源码，不是可烧录固件。
如需 USB ACM/ECM 或其他被精简镜像关闭的外设，应按 `docs/` 的构建配置指南构建
一般用途镜像，并使用与该 rootfs 匹配的 kernel/DTB。

ZIP 中的路径没有额外的顶层包装目录。代码部分包含 12 个已修改文件与
31 个新增文件；`docs/` 包含附件基线加更新后的完整 129 个源文件。
其中 26 个文档文件发生变更，包含 4 个新页面。
另外提供本说明与 `ESP32_CONFIG_UPDATE_MANIFEST.json`。
没有依赖删除操作，也没有打包调查用内核副本、构建缓存或测试产生的二进制。
若 `docs` 已初始化为 Git 子模块，覆盖后的文档变更位于该子模块的工作树；本包不修改子模块引用。

## 用户可见变化

| 主菜单 | 内容 |
|---|---|
| System | 主机名、root 登录密码、时区、NTP/手动时间、开机程序 |
| Network | Wi-Fi 选网及单次密码、DHCP/静态 IPv4、DNS |
| Bluetooth | 开关、设备名、清除保存配对、连接恢复 |
| Interfaces | 每个外设的当前引脚和参数、持久 GPIO、USB 模式 |
| Memory & storage | 已有 swap 设备、一个可移动卷的挂载与启动策略 |
| Maintenance | 配置导出/导入、按类别重置、完整状态 |

- 没有添加 SSH，也没有启用 Dropbear。
- Wi-Fi 配置菜单输入一次密码后保存并连接。扫描选项直接用于选择 SSID。
  新 CLI `wifi connect` 从标准输入读取一行密码；旧的非终端 `wifi configure`
  保留三行、仅保存协议，供现有自动化兼容。
- 普通 GPIO 菜单提供应用控制、输入及上下拉、输出低/高。
  一个轻量进程持有托管引脚，退出菜单后继续保持，Linux 启动服务恢复保存值。
  没有 GPIO 赋值时不会为了应用空配置而启动常驻服务。
  原有短时输出测试仅保留在 CLI 诊断命令中。
- 外设页面用当前值预填，显示保存值的差异；固定接线只读。
  返回丢弃草稿，保存才提交，失败保留输入。
  等价配置的保存不会重新探测硬件；正在挂载或作为 swap 使用的 SD 卡会阻止控制器改动。
  当前配置记录无法确认时，不会拿默认值冒充当前值。
- Bluetooth 名称同时用于 Classic、BLE 和 GAP；清配对只执行一次。
  共享无线模块的恢复仍保持 Bluetooth 先于 Wi-Fi 的顺序。
- USB serial/ECM 由真实 ConfigFS gadget 实现，支持按实际 ttyGSN 选择应用串口或登录控制台。
  ECM 板端默认 `192.168.7.2/24`，电脑端手动配置同网段地址。
- Swap 和文件系统配置只使用已有设备，不格式化磁盘；停止仅影响本工具管理的设备。
- 功能清单来自最终内核 `.config`，只宣告已编入镜像的外设驱动。
  默认精简镜像仍可能只显示 USB Host，并隐藏其他未编译外设。
- 时区包含九个真实 IANA TZif 文件；NTP 默认关闭。
  开机程序使用绝对路径及逐项原样参数，默认关闭；保存配置不会立即启动程序。
  程序应保持前台运行，输出默认丢弃，退出结果保留到本次系统重启。
- 配置恢复先校验再写入，失败可回滚；导入和重置默认等待 Linux 重启应用。
  保存的 Wi-Fi 凭据包含在配置备份中；登录密码、用户程序、蓝牙配对数据库不包含。

## 文档

中英文同步更新：配置工具和 CLI 参考、网络、外设、USB、配置备份、构建 profile、
首次启动及 Wi-Fi 高级使用。新增“系统设置”和“内存与存储”两组中英文指南。
附件原有的主题、语言切换和 Sphinx 配置保持不变。

## 验证与范围

- Python 行为测试共 **128 项**：**127 项通过、1 项跳过**。
  跳过项是实际进程会话/PID 生命周期测试，当前环境的 `/proc` 与子进程 PID 视图不一致。
- GPIO C 与菜单套件通过，覆盖持有 FD、输入读取、方向/偏置/输出更新、错误回滚、
  导入后单引脚编辑、占用保护及取消行为。
  Unix socket IPC 子测试因当前环境返回 `EPERM` 而明确跳过。
- 相关 C 程序以 `-Wall -Wextra -Werror` 编译；GPIO 另通过静态分析与 ASan/UBSan。
- 24 个相关 shell 文件通过语法检查。
- 22 个 BTstack 补丁按顺序应用到精确上游版本，新增命名与一次性清配对处理函数编译运行通过。
- 英文、简体中文、入口页严格 Sphinx 构建通过。
- **4,556 个本地链接、125 个生成 HTML 页面**检查无缺失文件或锚点。
- 覆盖包检查包含 CRC、成员路径、文件内容哈希、脚本权限，以及在干净基线上解压后逐文件比对。

没有在此环境完成完整 RISC-V 固件交叉构建或真实开发板验证；因此不宣称已验证
最终镜像大小、实际 GPIO 电平、无线连接、USB 枚举/互通、NTP 联网和低内存 swap 行为。

### 可重跑的主要测试

从主仓库根目录运行（overlay 测试需要已初始化的内核子模块）：

```sh
python3 -m unittest tools.tests.test_esp32_config_frontend tools.tests.test_esp32_config_network tools.tests.test_esp32_config_interfaces tools.tests.test_esp32_config_system tools.tests.test_esp32_config_assets -v
python3 -m unittest tools.tests.test_s31_overlay tools.tests.test_s31_overlay_describe tools.tests.test_esp32_config_bluetooth -v
python3 tests/test_esp32_config_storage.py -v
sh tests/esp32-config/test-gpio.sh
```

BTstack 的源级测试读取 `build/btstack-source`（正常 `make btstack-source` 准备），
或通过 `S31_BTSTACK_TEST_SOURCE` 指向精确上游的未打补丁源树。

安装附件文档的 requirements 后：

```sh
make -C docs html SPHINXOPTS="-W --keep-going"
```
