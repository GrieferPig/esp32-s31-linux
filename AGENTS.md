# 目标：为esp32-s31移植mmu linux

！！！不要移除！！！重要信息。

我在给esp32-s31移植mmu Linux。项目包含bootloader,OpenSBI,Linux. 用idf作为唯一S31硬件参考，同时docs/里面有一些我总结出来的硬件参考。必要时运行openocd(不能用gdb) debug。用中文思考。硬件usage以ESP-IDF为准(IDF在~/.espressif，使用前source export.sh)。疑问停下来问我。在任何任务开始前，如果docs/目录或其中的文件不在context window里，先读取docs/s31_hardware/下的所有文件。

# 注意-关键信息！
- 你应当假设你的所有关于RISC-V硬件的知识都是错的。你对S31的硬件只是应该按照以下优先级参考

1. docs/中开头为S31的 reference
2. ESP-IDF的S31部分
3. docs/中开头为P4的 reference （可能会有错误，如address或者reg layout）

- 任何构建应使用根目录 Makefile 中对应的 target 完成：`make opensbi`、`make linux`、`make coremark`、`make initramfs`、`make bootloader`。不要直接调用子仓库的构建系统；bootloader 的 `make bootloader` target 必须通过 ESP-IDF 的 `idf.py` 构建
- 使用idf以及任何来自idf的工具（如openocd）都必须先source idf的export.sh
- 刷写命令：`make flash-all`, 单分区刷写参考bootloader/partitions.csv
- openocd运行命令：. '/home/grieferpig/.espressif/tools/activate_idf_master.sh' && openocd -f board/esp32s31-builtin.cfg。！！！flash前必须关闭openocd！！！
- 所有S mode register和一些其他register在openocd无法直接读取，需要使用riscv dm_write/dm_read
- 如果要监控Serial，必须使用ttyUSB0，以及`idf.py monitor`指令，并为idf.py提供一个stdio。！！！不能使用其他tty monitor！！！注意，idf.py monitor不会自动返回，你需要添加timeout
- s31有概率重置的时候卡在bootloader，这时候应该再次重置
- 所有会被 Wi-Fi/BT blob 代码或 blob 执行上下文直接读取、写入或长期持有的内存都必须位于内部 HP SRAM，禁止使用 PSRAM 或普通 Linux `kmalloc`/`vmalloc` 内存。这包括 blob heap、兼容层 task stack/TCB、queue、event、同步对象、临时参数和 buffer、RX/TX staging buffer、DMA descriptor。Linux 通用控制对象只有在确认不会被 blob 直接访问时才可位于普通内存。
- 用中文思考。用中文思考。用中文思考。

---

以下是探测硬件的一些经验，如果你有新的发现，写在这里记住。

# ESP32-S31 Debug Notes

## Flash corruption 判别

刷写不完整或 flash 内容损坏时，串口可能卡在opensbi，仍能打印到
`hart1 released to OpenSBI; parking loader hart0`，随后 OpenOCD 会看到
hart1 停在 `__sbi_expected_trap`，并显示看似合理的 PMP/load fault、异常
`mepc` 或 `mtval`。这类现象不能直接归因于 OpenSBI trap ABI 或 Linux；先
关闭 OpenOCD，erase flash并重新用 `esptool -p /dev/ttyUSB0 -b 2000000 write-flash`
完整刷写并校验 bootloader、payload 和 `xipImage`，必要时先复位板卡，再
重复串口启动观察。只有同一镜像经完整校验后仍稳定复现，才进行 DMI 和
OpenSBI 源码级定位。

## Reading Hart Registers Through DMI

Use one OpenOCD telnet connection for the complete operation. Both target harts
must be halted first. Do not use `exec_progbuf` for hart 1: with the current SMP
target configuration it executes on hart 0.

Select the hart through `dmcontrol` (`0x10`):

```text
# hart 0; bit 0 keeps dmactive set
riscv dmi_write 0x10 0x00000001

# hart 1; hartsel is in bits 16+, bit 0 keeps dmactive set
riscv dmi_write 0x10 0x00010001
```

Read an RV32 CSR with an Access Register abstract command:

```text
# Replace XXX with the three-digit CSR number.
riscv dmi_write 0x17 0x00220XXX
riscv dmi_read 0x16
riscv dmi_read 0x04
```

The sequence is mandatory: write `command`, read `abstractcs`, then read
`data0`. For example, hart 1 `satp` (`0x180`) is:

```text
riscv dmi_write 0x10 0x00010001
riscv dmi_write 0x17 0x00220180
riscv dmi_read 0x16
riscv dmi_read 0x04
```

A successful command currently reports `abstractcs = 0x02000001`. Check
`cmderr` in bits 26:24 before accepting `data0`. A failed command can report
`abstractcs = 0x02000301`; in that case `data0` is stale and must not be used.
Clear the error before issuing another abstract command:

```text
riscv dmi_write 0x16 0x07000000
```

The current OpenOCD/S31 combination rejects `sip` (`CSR 0x144`) with
`cmderr = 3`, even when it is listed in `riscv expose_csrs`. Skip it or clear
the error immediately afterward. The following hart 1 CSRs have been verified:

```text
sstatus  0x100 = 0x80018020
stvec    0x105 = 0xc03d5383
sscratch 0x140 = 0x00000000
sepc     0x141 = 0xc03d24dc
scause   0x142 = 0x88000007
stval    0x143 = 0x00000000
satp     0x180 = 0x80850045
```

Read a GPR using register number `0x1000 + xN`. For example:

```text
# x1 / ra
riscv dmi_write 0x17 0x00221001
riscv dmi_read 0x16
riscv dmi_read 0x04

# x2 / sp
riscv dmi_write 0x17 0x00221002
riscv dmi_read 0x16
riscv dmi_read 0x04
```

OpenOCD telnet replies contain command echoes, prompts, and sometimes NUL
bytes. A script must strip NUL bytes and parse the last hexadecimal value from
the response. Keep the connection open across all three commands; repeatedly
opening connections or omitting the `abstractcs` read has produced stale
`data0` values during debugging.
