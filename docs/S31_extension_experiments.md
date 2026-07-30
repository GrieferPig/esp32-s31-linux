# ESP32-S31 ISA extension experiments

Last updated: 2026-07-30

## Hardware and toolchain baseline

- The local S31 hardware reference identifies the HP core ISA as
  `RV32IMAFBCNSUX`.
- ESP-IDF's current S31 RISC-V toolchain is GCC 15.2.0 plus binutils 2.45.
  The Linux/musl build pins the commits behind the matching
  `esp-15.2.0_20251204` releases.  S31's
  `soc_caps.h` and generated `sdkconfig` declare the Espressif extensions
  `xesploop` and `xespv`; Xespv machine code is assembled with
  `-mespv-spec=2p2`.  They do not declare `xespdsp`, Zcb, Zcmp, or Zcmt, so
  those extensions are intentionally not enabled for S31.
- Linux uses the soft-float `ilp32` ABI.  The `f` extension may still be used
  in userspace while keeping the syscall and libc ABI compatible with ilp32.
- `n`, `s`, and `u` describe privilege architecture rather than compiler code
  generation options.  They are implemented by the S31-specific OpenSBI and
  Linux privileged/trap paths, not appended to GCC's `-march` string.

## Board results

The test image was built from `rootfs/s31_ext_test.S` and
`rootfs/s31_ext_test.c` and run on ESP32-S31 revision v0.0, hart 1 under Linux
6.12.  Each Xespv instruction is isolated in a child process with a two-second
alarm so one illegal or non-terminating instruction cannot hide later results.

| Extension | Coverage | Result |
| --- | ---: | --- |
| RV32F | 27 instruction classes, including FCSR access | 27/27 pass |
| Xesploop | setupi, setup, starti, endi, counti, count; both loop slots | 6/6 pass |
| Xespv 2.2 | every non-HWLoop form in IDF `xesppie.S` | 353/354 pass |
| F/HWLoop/PIE context | signal delivery plus four fork workers, 200 yield iterations each | pass |

The one repeatable failure is case 270:

```text
esp.fft.cmul.s16.st.xp q0,q1,q2,x10,x9,0,0,0
opcode 0x2211087f
Linux result: SIGILL (signal 4)
```

The same assembler-generated opcode was temporarily executed on hart 1 in
bootloader M-mode after PIE was enabled.  Hart 1 did not reach OpenSBI and the
hart 0 watchdog/probe path restarted the system.  The probe was removed after
capture and the normal bootloader was rebuilt and flashed.  This establishes
that the failure is below Linux context management.  It does not distinguish
between an unimplemented instruction on revision v0.0 and an undocumented
hardware prerequisite.

The previous USB-UART noise bytes were deliberately excluded from this ISA
result because they are unrelated to instruction execution.

## Reproducible opcode generation

`rootfs/gen_s31_pie_cases.sh` reads the authoritative ESP-IDF decoder corpus:

```text
$IDF_PATH/components/esp_gdbstub/test_gdbstub_host/rv_decode/xesppie.S
```

It mechanically assigns safe scalar/vector operands, invokes
`riscv32-esp-elf-gcc`, verifies the object has the `xespv2p2` ELF attribute,
checks that exactly 354 four-byte instructions were emitted, and writes
`rootfs/s31_pie_cases.inc`.  The tested IDF source SHA256 is:

```text
649e5fc30c7de30326715c4ef91af6710bd8fe7c44bbb105e60622fe5257de79
```

Do not hand-edit the generated include.  `make initramfs` regenerates it.

## Optimization policy

The S31 Linux/musl compiler is configured for:

```text
rv32imafbc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs_
xesploop_xespv
```

with `-mabi=ilp32 -mespv-spec=2p2`.  This exposes all compiler-supported S31
extensions and includes compressed instructions.  Components must still obey
their execution-context rules:

- Linux kernel C code must not emit floating-point or PIE instructions because
  kernel execution cannot borrow userspace coprocessor state arbitrarily.
- OpenSBI trap paths must preserve S-mode FPU, HWLoop and PIE state before any
  implementation code can use those resources.
- Userland may use F, compressed, bit-manipulation, Xesploop and Xespv.  The
  single failing FFT store form above must not be selected explicitly until its
  prerequisite or silicon support is understood.

## Linux/musl toolchain and optimized image audit

The `GrieferPig/crosstool-NG-s31` workflow builds the configuration in
`configs/riscv32-esp-linux-musl.config`; `make toolchain` installs its pinned
release and `make toolchain-source` rebuilds it from a sibling checkout.  The
resulting tuple and versions are:

```text
riscv32-esp-linux-musl
GCC 15.2.0 (Espressif GCC commit 0dbf584943ac179894690b389f3a37926bb4cd33)
binutils 2.45 (Espressif binutils commit 99fe9ecb9fb65a69be65944eb1bbd7e42dfe0857)
musl 1.2.5, Linux headers 6.12, ilp32 ABI
```

The 2026-07-30 clean builds made through the root Makefile produced these
artifact-level results.  The compressed ratios count decoded 16-bit
instructions rather than relying only on configuration symbols.

| Artifact | Effective policy / ELF result | 16-bit instructions |
| --- | --- | ---: |
| OpenSBI `fw_payload.elf` | integer-safe IMA+B+C; no automatic F/Xesp state use | 29,932 / 52,577 (56.93%) |
| Linux `vmlinux` | `CONFIG_RISCV_ISA_C=y`, V disabled; integer-safe IMA+B+C C code | 753,868 / 1,207,698 (62.42%) |
| Buildroot BusyBox | full userspace ISA through the compiler wrapper | 100,040 / 179,575 (55.71%) |
| Buildroot CoreMark | full userspace ISA through the compiler wrapper | 1,968 / 3,649 (53.93%) |
| `s31-ext-test` | ELF attribute includes F, C, B, Xesploop 1.0 and Xespv 2.2 | 348 / 1,602 (21.72%) |

The unpadded XZ squashfs is 5,812,224 bytes and the flashed image is padded to
the 6,144,000-byte partition size.  The compiler contains G++, but no selected
target package uses C++; the otherwise-unused `libstdc++.so` runtime is omitted
from the compact board image in the post-build step.  An ELF dependency scan
confirmed that no installed executable requires it.
