# ESP32-S31 LP-core firmware

This project builds the Linux-managed LP-core payload with ESP-IDF's native
`ulp_embed_binary()` flow. The LP application uses the native hardware mailbox
runtime and the protocol in `external/linux/include/linux/soc/espressif/esp32s31-lp-protocol.h`.

Build and stage the ELF and raw binary into the root filesystem overlay:

```sh
make lp-firmware
```

The native build lives in `out/lp/`; the staged files are
`out/staging/overlay/lib/firmware/esp32s31/s31-lp-core.{elf,bin}`.
Use `make rootfs` or `make image` to include them in the root filesystem.

Linux remoteproc loads `s31-lp-core.elf` into the first 8 KiB of LP SRAM,
including its stack and shared data. The following 20 KiB retain OpenSBI's HP
context during suspend-to-RAM; the last KiB contains PM handoffs and the RTC
anchor. Firmware segments and entry points cannot use these reserved regions.
The `S02s31-lp` service starts it after the root filesystem is available. The raw
binary is staged as a recovery artifact; it is not a second running firmware.

Applications use the Linux RTC class for time and alarms, and Linux system PM
for suspend. The root filesystem supplies Buildroot's util-linux `hwclock`
for RV32 time64 support and BusyBox `rtcwake`, without a project-specific
BusyBox patch. `--noadjfile` avoids writing a drift file on a read-only root.
For example, with a valid system clock:

```sh
hwclock -u --noadjfile -w -f /dev/rtc0
cat /sys/power/state
cat /sys/power/mem_sleep
rtcwake -u -d /dev/rtc0 -m freeze -s 5
rtcwake -u -d /dev/rtc0 -m mem -s 5
```

`freeze` uses suspend-to-idle and the native RTC interrupt. Suspend-to-RAM
hands the alarm to LP/OpenSBI, retains HP SRAM and PSRAM contents, and restores
CPU, cache and peripheral state before resuming Linux. The Linux `deep`
mem-sleep label still means RAM-retaining suspend; it is distinct from the
platform's cold-boot deep-sleep poweroff path. Radio suspend disconnects current
links, and applications reconnect after resume; wake-on-radio is not provided.

GPIO consumers select wakeup through the normal Linux IRQ/device wakeup APIs
(for example `wakeup-source` on a `gpio-keys` device). GPIO0–7 have LP inputs
and can select suspend-to-RAM wake independently of an RTC alarm. HP-only GPIO
wakeup is supported in `freeze`; selecting it for `mem` returns an error before
entry. The LP descriptor carries each pad's trigger and pull configuration.
Wake selection must be ready by the consumer's ordinary suspend callback;
changes during late/noirq suspend abort entry rather than silently losing a
wake source. An active level or a wake detected during preparation also aborts
entry.
LP-captured GPIO events are restored to the native GPIO IRQ path before device
IRQs resume, including pulses which ended while HP was asleep. Hardware pending
latches coalesce LP replay and normal GPIO IRQ resends with already pending
pad events.
Private MEM wake parameters and the `sleep_test`/`gpio_test` controls
have been removed. Suspend and cold-boot poweroff use Linux PM and RTC APIs;
the LP firmware no longer implements a separate dry-run or deep-reboot mode.

Linux, LP firmware and OpenSBI must use the same sleep ABI. Version 2 retains
the 112-byte record and adds per-pad trigger types and bias masks in the
previous GPIO words. A mismatched firmware rejects the request.

Normal Linux `poweroff` enters non-retentive deep sleep. An enabled native RTC
alarm becomes the cold-boot wake deadline; without an alarm the board stays
off until an external reset. For example:

```sh
hwclock -u --noadjfile -w -f /dev/rtc0
echo 0 > /sys/class/rtc/rtc0/wakealarm
echo +20 > /sys/class/rtc/rtc0/wakealarm
poweroff
```

This uses the kernel RTC/sys-off interfaces, not a private `deep_sleep` sysfs
control. The bundled BusyBox `rtcwake` does not implement `-m off`; applications
can arm `/dev/rtc0` using `RTC_WKALM_SET` and then request normal poweroff.
Shutdown transfers the calibrated absolute RTC deadline through a single-use,
checksummed LP SRAM record independent of LP firmware or the MEM descriptor.
A second checksummed SRAM record retains the RTC wall-clock anchor across
known HP-only software and deep-sleep resets. A power-on, unknown reset, bad
checksum or counter discontinuity leaves RTC time invalid until it is set.
ROM-owned boot-time, log-control and wake-stub metadata are not scratch space.
An alarm that expires after handoff but before sleep causes an explicit reboot
without claiming a sleep/wake transition. A rejected PMU transition halts; it
is not converted into a software reset that could masquerade as successful
hardware wakeup. Deep sleep discards Linux RAM, unlike suspend-to-RAM.
PMU programming, the final clock handoff and sleep request run entirely from
HP SRAM, with flash prediction disabled and the other hart held in reset with
its CPU/CLIC clocks gated. This includes the register table, the last RTC
deadline check and failure path.

CPUIdle exposes `POLL` and `SMP-WFI` under each CPU's `cpuidle` directory. Both
retain the platform's pending-interrupt recovery. OpenSBI CLIC function 7
reports the private WFI guard in microseconds; Linux includes that guard and a
transition allowance in the WFI latency estimate. `POLL` lets the standard
`/dev/cpu_dma_latency` interface select an IRQ-enabled state when WFI cannot
meet the requested latency. The estimate is conservative, not a measurement
of typical hardware exit time or an energy break-even point. Associated Wi-Fi
keeps CPU0 polling to preserve radio responsiveness.

CPU frequency policy uses the standard `cpufreq` sysfs interface. Crypto and
DMA runtime PM use device `power/control` and `power/runtime_status`, with
active operations retaining their hardware leases. Linux idle accounting and
runtime status do not measure electrical current; supply measurements are
needed to quantify board-level power savings.

System shutdown closes crypto admission and waits for queued completions and
existing SDK leases before stopping the engines. Device links quiesce radio
consumers before the crypto supplier. Accepted operations finish through their
normal completion/error paths;
new operations receive a shutdown error while the system powers off or reboots.
