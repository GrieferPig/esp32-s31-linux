# ESP32-S31 Linux native Wi-Fi RX experiment log

This file tracks one-variable-at-a-time hardware experiments. All builds use
the repository root Makefile and all serial tests use `/dev/ttyUSB0` through
`idf.py monitor`.

## Baseline and established facts

- Known RX-capable baseline: top commit `1b94b7f`, Linux submodule
  `b00f15d279e4`. It uses the cooperative FreeRTOS shim; RX works but large
  packets are unstable and throughput is only several KiB/s.
- Kthread baseline: top `4fc50b5`, Linux submodule `84ddd7d022e1` plus current
  worktree fixes. Scan and WPA2 association work, TX submission/completion
  work, but five DHCP Discovers receive no Offer and `s31_wifi_rx()` is never
  called.
- `esp_wifi_internal_reg_rxcb()` returns zero. The S31 ROM map places
  `sta_rxcb` at `0x2f07ff6c`, netstack ref at `0x2f07ff68`, and netstack free
  at `0x2f07ff78`.
- Wi-Fi interrupt delivery has already been tested and is functional. Do not
  repeat generic IRQ tests; use source-120 counts only as a layer boundary
  when diagnosing a specific RX run.
- All blob-retained objects, task stacks/TCBs, sync objects, queues, RX/TX
  rings and staging frames have been moved into internal HP SRAM.

## 2026-08-13 kthread compatibility fixes

- Replaced the FIFO priority-inverting blob-gate `mutex_trylock()+schedule()`
  loop with a blocking mutex acquisition.
- Made critical-lock suspension occur before releasing the blob gate.
- Fixed the payload trampoline to use explicit RISC-V ABI argument registers.
- Added direct task notifications and missing SMP critical/task APIs used by
  the linked S31 IDF archives.
- Copied custom `.s31_radio.data` and cleared `.s31_radio.bss` before payload
  startup.
- Moved RTOS tick mutation out of arbitrary-mm hard IRQ context and into the
  init-mm radio worker.
- Disabled remote Wi-Fi, WPA3/GMAC and modem clock checking for the local S31
  data-path experiment.

## 2026-08-13 instrumentation image

- Added bounded logs for source 120/55 hard IRQ and serialized bottom half.
- Added bounded logs for ISR send/task receive on the measured Wi-Fi dispatcher
  queue (`len=200`, `item=8`).
- Added a post-association dump of the three ROM callback slots.
- Build: `make linux` succeeded. Flash verification succeeded.
- Boot result before factory-loader restoration: hart1 repeatedly stopped at
  `0x402209ce` in `__sbi_expected_trap`; DMI reported a PMP store-access halt,
  `a3=0`, `sp=0x50ff4e70`. The factory app printed that hart0 FreeRTOS was
  continuing. The worktree had lost the earlier SRAM scrub/hart0 park patch.
- Next controlled variable: restore the bounded HP-SRAM scrub and permanently
  park hart0 after releasing hart1, rebuild/flash the factory loader, then run
  the same instrumentation image.

## 2026-08-13 corrected flash layout and queue-boundary result

- The current partition table is `persist=0x2a0000..0x400000` and
  `linux=0x400000..0xa00000`. `build/xipImage` starts directly with its Linux
  image header; it has no `0x160000` prefix. Writing it at the older
  `0x2a0000` address makes OpenSBI execute file offset `0x160000` at
  `0x40400000` and recursively fault through `__sbi_expected_trap`.
- Corrected the Linux flash using root `make flash-linux` (offset `0x400000`).
  Restored the accidentally overwritten persist partition with root
  `make flash-persist`. Linux, overlay and radio then booted normally.
- Restored bootloader HP-SRAM scrub and hart0 park with root `make bootloader`
  and `make flash-bootloader`; both remain enabled in this run.
- Association to `ChinaNet-38D07C` at BSSID `f4:fc:49:5a:1a:b0`, channel 7,
  succeeded. PTK/GTK install succeeded and callback slots were:
  `ref=0xc022f996`, `sta=0xc022fb0a`, `free=0xc022f9ac`, with `sta` exactly
  equal to `s31_wifi_rx`.
- Source 120 hard IRQ and serialized callback counts advanced together. The
  measured `200 x 8` Wi-Fi queue had successful ISR sends and task receives;
  the kthread drained it to zero repeatedly without a lost wake or backlog.
- Five DHCP Discovers were transmitted and completed, but no RX callback ran
  and no lease was offered. Thus generic IRQ delivery, the main Wi-Fi queue,
  kthread wakeup, and callback registration are not the stopping layer.
- During WPA association the blob allocated IDF interrupt source 55. S31 IDF
  identifies source 55 as AES. Linux had independently probed the same AES/SHA
  register island before Wi-Fi initialization. Next controlled variable:
  disable the Linux `esp32s31-crypto` DT node so the blob is the sole owner of
  AES while testing encrypted data RX.

## 2026-08-13 Linux crypto ownership experiment

- Disabled the Linux DT crypto node, rebuilt using the root targets and
  flashed the current OpenSBI/Linux images. The following boot had no Linux
  crypto probe, proving the Wi-Fi blob was the sole AES-island owner.
- Scan, WPA2 association, source-55 AES registration, PTK/GTK installation,
  source-120 service and the 200 x 8 PP queue all remained functional.
- Five DHCP Discovers completed TX but received no Offer, and `s31_wifi_rx()`
  still did not run. Linux crypto ownership is therefore not the RX blocker.
- Keep Linux crypto disabled while Wi-Fi owns this hardware; this also keeps
  the next compatibility-layer experiments Wi-Fi-only.

## 2026-08-13 active FreeRTOS ABI audit

- Final `vmlinux` garbage collection proves that scheduler suspend/resume and
  generic task-notify APIs are not referenced by the live Wi-Fi image. The
  live subset is task create/delay/delete, `ulTaskGenericNotifyTake`, critical
  sections, queue/semaphore/mutex/event-group APIs, plus the Wi-Fi OS adapter.
- BT is not enabled (`CONFIG_BT_ENABLED` is unset) and the payload is compiled
  with `S31_WIFI_ONLY`. Software/external coexistence are also unset; only the
  adapter objects required by the Wi-Fi OS contract remain linked.
- Disassembly of the matching ESP-IDF S31 `libpp.a` establishes the exact RX
  queue boundary: `ppTask()` receives 8-byte messages from `xphyQueue`; event
  ID 13 calls `ppProcessRxPktHdr()`. Matching `libnet80211.a` then shows the
  data branch in `sta_input()` calling `sta_rxcb(buffer, len, eb)`.
- Next image traces event 13 specifically at queue send/receive. This separates
  a FreeRTOS ISR/queue wake contract failure from a later PP/net80211 drop.

## 2026-08-13: PP event-ID trace on Linux kthreads

- Build/flash succeeded from the root `make linux` / `make flash-linux` targets.
- `ChinaNet-38D07C` associated on the second attempt at BSSID
  `f4:fc:49:5a:1a:b0`, channel 7 (2442 MHz), RSSI about -54 dBm.
- The 200 x 8 main Wi-Fi/PP queue was traced after association. Every observed
  send was followed by a receive and the queue drained to zero; observed event
  IDs were 0, 5, 7, 16, 17, 23, 25, and 29.
- Event 13 (`ppProcessRxPktHdr` in the matching S31 `libpp.a` jump table) was
  never sent or received. Five DHCP Discovers completed TX successfully but no
  Offer arrived and the station RX callback count remained zero.
- Conclusion: this run rules out a lost wake in the main PP queue consumer.
  The stop is before `ppTask()` receives a data RX header event: either the RX
  ISR/descriptor path never creates event 13, or an earlier ISR-side FreeRTOS
  contract prevents it. Generic source-120 IRQ and bottom-half delivery remain
  active and are not being re-tested as an unknown.
- Next experiment: enable the requested promiscuous callback as an independent
  pre-netstack observation point and log the actual source-120 ISR pointer for
  disassembly against the matching IDF archive.

## 2026-08-13: promiscuous RX diagnostic

- Source 120 installs ROM ISR `wDev_ProcessFiq` at `0x2f8010f0`.
- Enabling promiscuous mode after association immediately caused event 13 to be
  produced and drained. Promiscuous data callbacks also fired, proving RX DMA,
  ISR descriptor processing, PP queue dispatch, and the Linux kthread consumer
  can all carry RX frames.
- Five DHCP Discovers still received no Offer at the Linux station callback.
  Thus promiscuous mode activates a previously inactive MAC RX delivery/filter
  path, but the normal associated/decrypted station path is still not reaching
  `sta_rxcb`.
- Next: hook the PP station callback at `pTxRx + 0x3f8` while promiscuous mode is
  active to distinguish PP/net80211 rejection from final `sta_rxcb` dispatch.

## 2026-08-13: FreeRTOS build-ABI configuration audit

- The last-known-good 1b94b7 dependency configuration had
  `CONFIG_FREERTOS_UNICORE=y` and `CONFIG_FREERTOS_NUMBER_OF_CORES=1`.
- The current `build-radio/sdkconfig` unexpectedly had
  `CONFIG_FREERTOS_NUMBER_OF_CORES=2` because its dependency build stopped
  inheriting the normal bootloader sdkconfig. The Linux bridge executes the
  compatibility world on one hart only.
- This changes IDF portMUX/critical/core-affinity behavior beneath the Wi-Fi OS
  adapter and is an ABI/semantic mismatch with the kthread shim. Added
  `CONFIG_FREERTOS_UNICORE=y` to `sdkconfig.radio.defaults`; rebuild must confirm
  generated `NUMBER_OF_CORES=1` before flashing.
- Unicore alone increased event-13 burst delivery but did not restore DHCP.
  Restore the full 1b94b7 dependency-build baseline as well (normal sdkconfig
  followed by radio overrides); this also removes the unexpected
  `CONFIG_FREERTOS_TASK_FUNCTION_WRAPPER=y` drift. This affects only the IDF
  libraries linked into Linux, not bootloader runtime initialization.

## 2026-08-14: ISR yield semantic audit

- `vPortYieldFromISR()` was a no-op. Although queue wakes made the Wi-Fi
  kthread runnable, the serialized radio worker retained the blob gate and
  continued task-context TX/timer work before the task could consume ISR
  events. Native FreeRTOS switches to a newly unblocked higher-priority task at
  this boundary.
- Added an explicit gate handoff after each deferred blob ISR callback: leave
  blob context, `cond_resched()`, then re-enter before continuing the pass.
  This preserves single-owner blob execution while reproducing the important
  ISR-to-Wi-Fi-task ordering.

## 2026-08-14: kthread scheduler API equivalence audit

- The ISR-boundary gate release plus `cond_resched()` did not restore data RX:
  association and CCMP key install completed, TX completions reported success,
  but station PP received only beacons and five DHCP discovers got no offer.
- Found two concrete FreeRTOS semantic gaps in the kthread bridge:
  `vTaskDelay(0)` used `cond_resched()` while a SCHED_FIFO task still owned the
  blob gate, so it was not an actual scheduler yield; `vTaskPrioritySet()`
  changed only the SRAM TCB and left the Linux kthread priority unchanged.
- Implemented an explicit zero-delay yield by suspending the blob gate,
  calling Linux `yield()`, and reacquiring it. Priority changes now update the
  associated kthread's SCHED_FIFO priority. Added bounded logs to establish
  whether the closed Wi-Fi path exercises either operation.
- Those bounded logs showed neither API is exercised on the failing path.
- TX completion inspection proved Linux Ethernet frames are copied into
  internal blob buffers and become valid protected QoS-ToDS frames. DHCP uses
  the correct AP/STA/broadcast addresses. CCMP PN and 802.11 sequence numbers
  advance monotonically, so neither source-buffer lifetime nor a repeated PN
  explains the AP dropping traffic.
- Found a more fundamental kthread ABI gap: the cooperative switch at 1b94b7
  explicitly saved `fs0..fs11` and `fcsr`, while the kthread bridge called
  `kernel_fpu_end()` at every blocking operation without first retaining the
  payload's ILP32F callee-saved registers. `kernel_fpu_end()` restores the
  kthread's pre-blob FPU state, discarding the Wi-Fi task state. Added an HP
  SRAM FP save area to each Linux compatibility task and save/restore it around
  every blob suspend/resume boundary.
- Adding the per-task FP state pushed the already tight HP SRAM heap over a
  latent leak: completed compatibility kthreads freed their payload stack but
  not their TCB or notify/suspend bridge objects. Added an explicit payload
  task-release ABI called by the Linux trampoline after task exit. Repeated
  connection tasks can now reuse the same SRAM instead of failing the second
  16-KiB stack allocation.

## 2026-08-14: legacy 11b/g data-path experiment

- Limited the station protocol bitmap to `WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G`
  before association, then rebuilt and flashed through the root Makefile.
- The second association attempt completed at channel 7. The blob explicitly
  reported `phymode(0x2, 11bg)`, `phy: bg`, `ht:0`, and installed PTK/GTK.
- Despite HT being disabled, every Ethernet data frame was still emitted as a
  protected QoS data frame with QoS control `0x0007` (TID 7). Five DHCP
  Discovers completed at MAC status success, but no station data callback ran
  and no lease was offered.
- Conclusion: disabling 11n removes HT/AMPDU capability but does not disable
  WMM/QoS, and does not restore RX. Resume the FreeRTOS compatibility audit;
  do not treat the ADDBA loop as the root cause yet.

## 2026-08-14: current-task identity audit

- Bounded mutex/semaphore/event/task-delay tracing showed that every deferred
  source-120 ISR callback ran with `s31_rtos_current() == NULL`. The blob then
  attempted hundreds of recursive-mutex releases from the orphan context and
  every release failed. Native FreeRTOS retains a `pxCurrentTCB` while an ISR
  runs, and its timer/service callbacks also execute as real tasks.
- Added an internal-SRAM pseudo TCB for radio-worker, deferred-ISR, timer and
  direct data-path entries. This removed the repeated failed recursive-mutex
  operations and association still completed, but five DHCP Discovers again
  received no Offer.
- This closes a real kthread compatibility gap but is not sufficient. The next
  correction is to run all worker-side blob entries on an internal-SRAM stack
  and preserve their ILP32F state across bridge waits, just as compatibility
  kthreads already do.
## 2026-08-14: worker HP-SRAM stack first hardware run

- Added a dedicated 16-KiB internal-SRAM stack for deferred IRQ/timer/direct
  blob calls, plus foreign-worker FP callee-saved context preservation.
- Boot and Wi-Fi init completed, and the stack allocation occurred after the
  one-shot `radio-init` task exited.  However it reduced the radio heap to
  about 19 KiB free; `esp_wifi_connect()` then failed a 1296-byte allocation
  and returned 257 (`ESP_ERR_NO_MEM`).  Thus this run did not test RX.
- `%p` in the kernel log was hashed and did not reveal the real range; use
  `%px` for the next range check.
- Next experiment reduces the compatibility-task minimum and worker stack to
  8 KiB.  ESP-IDF stack depth is bytes, and forcing every smaller task to 16
  KiB was not FreeRTOS-compatible and unnecessarily exhausted HP SRAM.
## 2026-08-14: deferred ISR context contract

- The S31 IDF adapter's `_is_from_isr` calls hardware `xPortCanYield()`.
  Linux defers source 120 to the serialized radio kthread, so this always
  reports task context even while the compatibility layer has entered the
  Wi-Fi ISR (`s31_rtos_isr_depth != 0`).
- This is a concrete FreeRTOS emulation mismatch at a callback directly used
  by the closed PP blob.  Override `g_wifi_osi_funcs._is_from_isr` with the
  compatibility ISR-depth state for the next hardware experiment.
- Hardware result: association remained successful and PP event batching
  changed, but five DHCP discovers still received no Offer.  Keep the fix
  because it restores the IDF adapter contract; it is not sufficient alone.
- Next run keeps promiscuous mode enabled after association (rather than the
  previous enable/disable probe) while retaining the normal STA RX callback.

## 2026-08-14: known-good baseline and worker priority audit

- Persistent promiscuous mode sees many valid protected frames between the AP
  and other stations, but no downlink frame addressed to our MAC after five
  DHCP discovers.  The AP ACKs the 802.11 TX completions but apparently does
  not accept/decrypt the data payload; the failure is before normal STA RX.
- An exact `1b94b7` worktree plus its matching bootloader boots the cooperative
  scheduler.  Re-linking that source against the present ESP-IDF master stalls
  at the changed crypto IRQ path, so it is useful for semantic comparison but
  is not a bit-identical runnable historical baseline.
- Found a Linux priority inversion: the radio worker used `sched_set_fifo()`
  (FIFO 50), while Wi-Fi priority 23 maps to FIFO 63.  FreeRTOS hardware ISR
  service must outrank tasks.  Next run assigns the worker FIFO 90; it blocks
  at the end of every pass, allowing Wi-Fi tasks to execute.
- Hardware result with FIFO 90: association succeeds but DHCP still gets no
  Offer.  Keep the priority correction, but it is not sufficient.
- Found double tick advancement: the hard TIMG IRQ calls
  `s31_rtos_hard_tick()` and the worker later calls `s31_rtos_tick()` for the
  same pending count; both incremented RTOS/esp_timer time.  This produced an
  effective 200-Hz clock.  The worker half now dispatches callbacks only.
- Hardware result of removing the second advancement: association startup
  aborts in `ets_timer_arm` with ESP_ERR_INVALID_STATE (0x102).  The existing
  timer shim evidently couples callback dispatch/rearm to that advancement;
  revert this isolated change and redesign timer semantics separately.

## 2026-08-14: S31 hardware crypto slot and kthread-stack audit

- Disassembly of the current S31 blob shows pairwise keys use hardware slot 4;
  `hal_crypto_set_key_entry()` writes the validity bitmap at `0x20104814` and
  a 40-byte slot at `0x20105800 + slot * 40`.
- On hardware, WPA2 association set validity bits for pairwise slot 4 and
  group slot 0 (`0x10`, then `0x11`).  The key data window does not read back
  as plaintext (only sparse byte lanes resemble the input), so it cannot be
  used as evidence that the key write failed.  Do not log key material again.
- The exact S31 IDF `wifi_module_enable()` is only
  `modem_clock_module_enable(PERIPH_WIFI_MODULE)` on the independent-clock
  target.  Its dependency table already expands that call to WIFI_MAC,
  WIFI_APB, WIFI_BB, WIFI_BB_44M, COEXIST, WIFI_BB_80X1 and SOC_PLL_SOURCE_CG;
  the current wrapper therefore is not a reduced clock-enable sequence.
- A more concrete kthread mismatch remains: payload functions execute and
  enter Linux wait/scheduler code on an 8-KiB SRAM stack.  Native FreeRTOS
  never puts Linux mutex/wait/scheduler frames below the blob frames.  Add
  runtime stack-watermark measurements at each blob suspension before making
  further scheduler changes.
- Hardware stack measurements were small: radio-init peaked near 544/8192
  bytes and wifi-connect near 528/4096 bytes; the main Wi-Fi task did not cross
  the first 512-byte reporting boundary at its waits.  Stack exhaustion is not
  the present RX blocker.
- The linked payload contains no task-notification or scheduler-suspend entry
  points (they were garbage-collected), so their incomplete emulation cannot
  affect this build.  The active ABI is limited to tasks, queues/semaphores,
  event groups, critical sections and tick queries.
- Redesign the double-tick correction using one RV32-atomic 32-bit hard-tick
  epoch.  The prior experiment let a hard IRQ mutate a 64-bit microsecond
  counter concurrently with task timer operations, which is itself invalid on
  RV32 and may explain the `ESP_ERR_INVALID_STATE` regression.
## 2026-08-14: atomic 32-bit esp_timer epoch experiment

- Changed the hard-IRQ-owned `esp_timer` epoch from a directly updated 64-bit
  microsecond counter to an RV32-atomic 32-bit 10-ms tick counter.
- Linux boot and Wi-Fi init completed, but immediately after `STA_START` and
  the first source-120 bottom half, IDF's legacy `ets_timer_arm()` aborted
  because `esp_timer_start_once()` returned `ESP_ERR_INVALID_STATE`.
- Symbol/disassembly identified the failure at `ets_timer_arm` line 79, after
  its normal stop-then-start sequence.  This version therefore cannot be used
  to judge RX and was reverted to the previous timer epoch behavior.
- The audit also found a concrete FreeRTOS ABI mismatch:
  `uxQueueMessagesWaiting()` returned the compatibility layer's internal
  mutex ownership count.  Native FreeRTOS exposes the inverse token count
  (one when free, zero when held).  The public function now translates mutex
  counts while leaving ordinary queues/semaphores unchanged.

## 2026-08-14: frozen FreeRTOS/esp_timer time root cause

- Timer boundary tracing showed all connect-time alarms were based at zero
  (`1000000`, `120000`, `1000` rather than current uptime plus the delay),
  followed by an attempted zero alarm and `ESP_ERR_INVALID_ARG`.
- Driver inspection found that the TIMG hard IRQ only increments
  `s31_tick_pending`; it never calls the payload's `s31_rtos_hard_tick()`.
  At the same time, the worker implementation of `s31_rtos_tick()` only
  dispatched callbacks and advanced neither `s31_tick` nor the esp_timer
  epoch.  Therefore `xTaskGetTickCount()` and `esp_timer_get_time()` stayed at
  zero indefinitely despite normal hardware tick IRQ activity.
- Fixed `s31_rtos_tick()` to advance the FreeRTOS tick and esp_timer epoch once
  per pending TIMG event, then dispatch expired callbacks under the blob gate.
- Hardware validation after the fix: the first association attempt timed out
  with reason 201, the immediate retry completed WPA2 association and remained
  connected.  All timer alarms were based on current uptime and the previous
  zero-alarm abort disappeared.  DHCP still sent five completed encrypted
  Discover frames without an Offer; normal RX still saw only beacons while
  promiscuous RX saw ambient data.  Next run removes the promiscuous/PP
  diagnostic hooks to test the unmodified filtered station receive path.

## 2026-08-14: FromISR queue ABI mismatch

- Compared the compatibility declarations against the exact ESP-IDF
  FreeRTOS-Kernel-SMP headers used to build the S31 payload.
- Found `xQueueGenericSendFromISR()` implemented with three arguments while
  IDF declares four; the missing fourth argument is `xCopyPosition`.
  Direct blob callers therefore lost send-to-front/overwrite ordering and all
  ISR sends were forced to the back of the queue.
- Corrected the ABI and queue operation, and changed both FromISR send/receive
  paths so `pxHigherPriorityTaskWoken` is only promoted to true, never cleared
  by a later operation in the same ISR.

## 2026-08-14: connect the real hard tick contract

- The earlier frozen-time fix advanced epochs only when the worker acquired
  the blob gate.  That still differs from FreeRTOS: a blob task busy-waiting
  with the gate held must observe tick interrupts advancing time.
- Connected TIMG1 hardirq to the already-exported `s31_rtos_hard_tick()`.
  The hook only updates `s31_tick` and a naturally atomic 32-bit esp_timer
  tick counter in internal HP SRAM.  The worker no longer advances either
  epoch and remains solely responsible for serialized timer callbacks.
- Directly calling the low-address payload hook from hardirq then faulted at
  `0x2f04718c` once the interrupted context used a normal userspace page table;
  low radio identity mappings intentionally exist only in `init_mm`.
- Reworked the boundary: TIMG hardirq continues to update its Linux atomic
  counter, while payload `xTaskGetTickCount()` and `esp_timer_get_time()` read
  that counter through `s31_linux_tick_count()`.  This preserves hard-IRQ time
  progress without any hardirq access to low payload virtual addresses.

## 2026-08-14: finite-wait and `_is_from_isr` contract audit

- Corrected queue/semaphore/event finite waits to retain one absolute timeout
  across competing or spurious wakes.  Event-group timeout now returns the
  current event bits, as FreeRTOS does, rather than always returning zero.
- Hardware remained stably associated, but five DHCP Discovers still received
  no Offer.  The traced 200-entry Wi-Fi queue processed control events
  (including 23/25/29), with no data event 13 and no registered STA callback.
- Comparing the current override with S31 `esp_adapter.c` and `1b94b7` found a
  more direct compatibility error: Wi-Fi's `_is_from_isr` callback is really
  `!xPortCanYield()`.  On S31 CLIC that is true both in an ISR and while a task
  has raised the interrupt threshold for a critical section.  The Linux
  override checked only deferred ISR depth, allowing blob code in a critical
  section to select blocking queue APIs.  It now also checks the current
  compatibility TCB's critical nesting depth; hardware retest follows.

## 2026-08-14: promiscuous observation of the DHCP exchange

- Enabled promiscuous capture only as a diagnostic after STA connection while
  retaining the normal registered STA RX callback.  Event 13 on the 200-entry
  Wi-Fi queue is actively sent and consumed for promiscuous data, so the
  current Linux kthread, queue wakeup and Wi-Fi worker path can process a high
  rate of real data events.
- Five DHCP Discover attempts completed TX.  The transmitted frames are
  protected QoS data to the associated AP, with monotonically increasing CCMP
  packet numbers.  No captured air frame had receiver/destination MAC
  `30:ed:a0:f3:d4:ac`, although downlink traffic to other associated clients
  was visible.
- A successful TX completion only establishes a PHY ACK.  The strongest
  current hypothesis is that the AP discards the protected uplink after ACK,
  for example because the CCMP/MIC/key state is invalid; losing a DHCP Offer
  in the Linux RX callback is not supported by this capture.
- The current bridge saves only `ra`, `sp`, and `s0`..`s3` across a payload
  task entry.  Normal ABI returns happen to preserve `s4`..`s11`, but
  `vTaskDelete(self)` jumps over that unwinding.  Expand the saved context to
  all integer callee-saved registers before further crypto/TX diagnosis.

## 2026-08-14: complete integer context and same-LAN static ARP test

- Expanded the payload trampoline and `vTaskDelete(self)` escape to save and
  restore `ra`, `sp`, and all `s0`..`s11`.  Root `make linux` succeeded and
  the hardware still associated stably, but DHCP again received no Offer.
- Disassembly of S31 `ieee80211_classify()` shows UDP ports 67/68 are
  deliberately assigned access category/TID 7.  The observed QoS value is not
  a Linux priority propagation bug.
- The development host reaches the same AP over `192.168.5.0/24`; its gateway
  is `192.168.5.1` with MAC `f4:fc:49:5a:1a:b0`, exactly the associated BSSID.
  Assigning unused `192.168.5.250/24` to the board still left
  `192.168.5.1 INCOMPLETE`, and five pings sent from the development host to
  the board also failed ARP.  The failure is the protected data plane, not
  only the DHCP server.
- Remove promiscuous/queue tracing and key/auth wrappers for the next build so
  PP and crypto timing match the last known-good source as closely as possible.

## 2026-08-14: IDF LMAC/HMAC statistics at the protected-data boundary

- Removed promiscuous, queue and key/auth diagnostics, then rebuilt and flashed
  through the root Makefile. WPA2 association remained stable, but five DHCP
  Discovers again received no Offer.
- Triggered `esp_wifi_statis_dump(UINT32_MAX)` after the fifth successful DHCP
  submission. All Wi-Fi buffer classes reported zero flow-control and OOM
  failures. LMAC TX reported 26 frames with zero lifetime/source/age/timeout
  failures; HMAC TX reported 15 station frames.
- LMAC RX reported only two MPDUs and HMAC RX reported only two station data
  frames over the association/DHCP interval. These are consistent with the
  protected association handshake; no DHCP response entered HMAC. Hardware RX
  and management/beacon counts continued advancing normally.
- Combined with the promiscuous capture and same-LAN static ARP test, this
  places the failure before the Linux station RX callback: the AP ACKs the
  protected uplink at PHY/MAC level but does not produce a directed response.
  The next audit therefore compares the kthread shim's active FreeRTOS
  scheduling/critical/mutex semantics against `1b94b7`, then the S31 hardware
  crypto/modem-clock initialization used after key installation.

## 2026-08-14: modem-support archive differential

- Enabling `CONFIG_ESP_MODEM_CLOCK_ENABLE_CHECKING` changed the linked modem
  support implementation but did not change the hardware result: association
  succeeded and five DHCP Discovers received no Offer.
- Replaced only `libesp_hw_support.a` with the preserved artifact from the
  known-good dependency build; the Wi-Fi, PHY and HAL archives were already
  byte-identical. This also associated successfully but DHCP still failed.
- Therefore neither the clock-checking Kconfig switch nor the historical
  `esp_hw_support` archive difference is sufficient. Restore the current
  archive and continue with the worker/task-context FreeRTOS contract.

## 2026-08-14: worker critical-section and QoS/TID diagnostics

- Wrapped only `esp_wifi_internal_tx()` in the compatibility critical section
  to reproduce the old cooperative worker's `!xPortCanYield()` state at this
  boundary. Association remained stable, but five DHCP Discovers still got no
  Offer; the diagnostic was reverted.
- Wrapped `ieee80211_classify()` to force best-effort TID 0 instead of the
  blob's deliberate DHCP TID 7. TX completion showed QoS control `0x0000` and
  ADDBA requests moved to TID 0, proving the override was active, but DHCP
  still got no Offer. This diagnostic was also reverted.
- These results rule out the worker's immediate critical state and DHCP's WMM
  classification as sufficient causes. Continue with the exact task, mutex,
  blocking and wake/preemption contract.
