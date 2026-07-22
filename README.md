# Linux 6.12 for ESP32-S31

An attempt to port MMU Linux to S31's architecture (`RV32IMAFBCNSUX`).

Module tested: ESP32-S31-WROOM-3 E1H16R16V (ESP32-S31 Core Board).

[Example boot log](docs/example_boot_log.txt)

## Build/Flash Instructions

Refer to the [Build Instructions](docs/build.md).

## Porting progress

### General

| Feature | Status |
|---|---|
| Busybox init | 🟡 Untested |
| Wireless (ESP-Hosted) | 🔴 Not implemented |
| Dual hart SMP | ⚫ Maybe? |

### Peripheral Drivers
| Feature | Status |
|---|---|
| AXI GDMA | 🟡 Untested |
| Cache driver | 🟡 Untested |
| CLIC/CLINT interrupt driver | 🟡 Untested |
| PMP/APM | 🔴 Not implemented (properly) |
| Timers | 🔴 Not implemented |
| Flash block dev driver | 🔴 Not implemented |
| Security accelerators | 🔴 Not implemented |
| LP subsystem & IPC | 🔴 Not implemented |
| AHB GDMA | 🔴 Not implemented |


### Connectivity Drivers
| Feature | Status |
|---|---|
| UART | 🟡 Untested  |
| USB HS | 🟡 Untested |
| GPIO | 🔴 Not implemented |
| pinctrl/GPIO Matrix | 🔴 Not implemented |
| I2C | 🔴 Not implemented |
| I2S | 🔴 Not implemented |
| SPI | 🔴 Not implemented |
| RMT | 🔴 Not implemented |
| EMAC | 🔴 Not implemented |
| SDIO | 🔴 Not implemented |
| USB Serial/JTAG | 🔴 Not implemented |


> 🟢 **Stable** — Fully tested and working | 🟡 **Untested** — Implemented but not verified 

Also check out [these command outputs](#appendix).

## S31 Quirks

(For more hardware references, see `docs/` folder)

This port was done before S31 TRM is available, therefore these guessworks were made:

### CLIC v. PLIC v. CLINT

S31 uses CLIC and CLINT similar to P4. Linux expects PLIC. Therefore a custom CLIC driver is needed. I referenced [this CLIC patch]() from ***TODO: I forgot where the patch come from*** to get the CLIC working.

Also, standard RISC-V interrupt CSRs are not usable, presumably because, from P4's TRM, CLINT interrupts are routed to CLIC and `mtvec.MODE` is hardwired to `0x3` (CLIC mode). Patches needed to make OpenSBI interrupts work.

### S mode

S31's supervisor mode is not standard and has absolutely no usage in ESP-IDF so a lot of these CSR uses were mostly guessed from either P4's TRM or CSR probing (see `docs/`). For example, the use of `sclicbase(?)` and the lack of `sie`.

S31 implemented [SCLIC (Supervisor CLIC?)](https://esp32.com/viewtopic.php?t=48188) which is confusing since there is no known standardization; According to all laws of esp-idf, `mcliccfg.NMBITS` is not writable. ***IT IS WRITABLE!*** And setting it to `0b01` enables writes to the `clicintattr[i].MODE` field and thus enabling the use of S-mode interrupts.

### OpenSBI and Linux XIP

To save the *precious* 16MB PSRAM memory, OpenSBI was modified to use XIP in flash and internal SRAM (hence the `3915901 KB` firmware size in OpenSBI banner, since flash and SRAM mappings are not continuous).

In mainline linux, XIP support on RISC-V was removed, so 6.12 was used instead which has proper XIP support. 

## Appendix

(as of 7/21/26)

```
~ # coremark
2K performance run parameters for coremark.
CoreMark Size    : 666
Total ticks      : 11580
Total time (secs): 11.580000
Iterations/Sec   : 949.913644
Iterations       : 11000
Compiler version : GCC16.1.0
Compiler flags   : -O2 -static -march=rv32imac_zicsr_zifencei -mabi=ilp32  
Memory location  : Please put data memory location here
                        (e.g. code in flash, data on heap etc)
seedcrc          : 0xe9f5
[0]crclist       : 0xe714
[0]crcmatrix     : 0x1fd7
[0]crcstate      : 0x8e3a
[0]crcfinal      : 0x33ff
Correct operation validated. See README.md for run and reporting rules.
CoreMark 1.0 : 949.913644 / GCC16.1.0 -O2 -static -march=rv32imac_zicsr_zifencei -mabi=ilp32   / Heap
```

```
# cat /proc/cpuinfo
processor       : 0
hart            : 0
isa             : rv32imafc_zicntr_zicsr_zifencei_zca_zcf_zbb
mmu             : sv32
uarch           : espressif,esp32s31
mvendorid       : 0x612
marchid         : 0x80000003
mimpid          : 0x1
hart isa        : rv32imafc_zicntr_zicsr_zifencei_zca_zcf_zbb
```

```
# uname -a
Linux (none) 6.12.0-00036-gdccf9c1b2c4f-dirty #30 Thu Jul  2 23:48:23 CST 2026 riscv32
```

```
# free -h
              total        used        free      shared  buff/cache   available
Mem:          15004        1892       12784           0         328       12108
Swap:             0           0           0
```

```
# ps -aux
    1 0          380 S    /bin/sh -i
    2 0            0 SW   [kthreadd]
    3 0            0 SW   [pool_workqueue_]
    4 0            0 IW   [kworker/0:0-eve]
    5 0            0 IW<  [kworker/0:0H]
    6 0            0 IW   [kworker/u4:0-ev]
    7 0            0 IW<  [kworker/R-mm_pe]
    8 0            0 SW   [ksoftirqd/0]
    9 0            0 SW   [kdevtmpfs]
   10 0            0 IW<  [kworker/R-inet_]
   11 0            0 SW   [oom_reaper]
   12 0            0 IW<  [kworker/R-write]
   13 0            0 SW   [kcompactd0]
   14 0            0 IW<  [kworker/R-kbloc]
   15 0            0 SW   [kswapd0]
   16 0            0 IW   [kworker/0:1-pm]
   17 0            0 IW   [kworker/u4:1-ev]
   18 0            0 SW   [khvcd]
   19 0            0 IW<  [kworker/R-mld]
   20 0            0 IW<  [kworker/R-ipv6_]
   34 0          380 R    ps -aux
```

```
# cat /proc/meminfo
MemTotal:          15004 kB
MemFree:           12788 kB
MemAvailable:      12112 kB
Buffers:               0 kB
Cached:              328 kB
SwapCached:            0 kB
Active:                4 kB
Inactive:             32 kB
Active(anon):          4 kB
Inactive(anon):       32 kB
Active(file):          0 kB
Inactive(file):        0 kB
Unevictable:         328 kB
Mlocked:               0 kB
SwapTotal:             0 kB
SwapFree:              0 kB
Dirty:                 0 kB
Writeback:             0 kB
AnonPages:            52 kB
Mapped:              208 kB
Shmem:                 0 kB
KReclaimable:          0 kB
Slab:                756 kB
SReclaimable:          0 kB
SUnreclaim:          756 kB
KernelStack:         352 kB
PageTables:           24 kB
SecPageTables:         0 kB
NFS_Unstable:          0 kB
Bounce:                0 kB
WritebackTmp:          0 kB
CommitLimit:        7500 kB
Committed_AS:        304 kB
VmallocTotal:     524288 kB
VmallocUsed:          12 kB
VmallocChunk:          0 kB
Percpu:               32 kB
```
