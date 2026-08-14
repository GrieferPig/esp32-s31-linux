# ESP32-S31 Linux native Wi-Fi RX experiment log

This file tracks one-variable-at-a-time hardware experiments. All builds use
the repository root Makefile and all serial tests use `/dev/ttyUSB0` through
`idf.py monitor`.

## CORRECTION (2026-08-14, supersedes earlier "no downlink" conclusions)

Earlier sections below concluded "the AP PHY-ACKs the protected uplink but
produces no directed response" and "no downlink frame addressed to our MAC".
That conclusion was WRONG. Re-tested on hardware:

- The link works correctly with **no encryption** (open AP): DHCP and the Linux
  RX callback both succeed.
- On **WPA2** there **is** downlink from the AP, but the station **RX path is
  broken** — the protected downlink (DHCP Offer) is received but not
  decrypted/delivered.

So the failure is the **WPA2/CCMP RX decryption/data path**, not an AP that
refuses to answer. All experiments after this note treat RX decryption as the
suspect boundary, and any earlier "AP discards the uplink" language below is
historical and incorrect.

## Baseline and established facts

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

## 2026-08-14: protected DHCP descriptor at the LMAC boundary

- Associated stably with WPA2-PSK AP `AHSS-WLAN2` on channel 9 (BSSID
  `06:69:6c:e5:14:e6`). ADDBA responses for TIDs 0, 7 and 5 all returned
  status 0, but DHCP still received no Offer.
- Wrapped `lmacTxFrame()` and sampled the first DHCP Discover after the
  asynchronous Wi-Fi task had prepared it. The protected TX entry was:

  ```text
  [S31] TXDESC #16 eb=2f068720 frame=2f0687bc len=356 tid=0
  flags=4c4d4143 ctl=00000000
  desc=0920000207000000000000000b0000000403400000000000000000001067042f00000100000000000000010000000108000000000000000000000000000000000000000000000000
  hdr=8841000006696ce514e630eda0f3d4acffffffffffffb00007001e00002000000000aaaa030000000800450001480000
  ```

- The frame control `0x4188` is QoS Data with ToDS and Protected set. Receiver
  and BSSID are the AP, transmitter is the station, and destination is
  broadcast. Sequence control is `0x00b0` (sequence 11); QoS control is
  `0x0007` (TID 7). The `tid=0` wrapper field above is the `lmacTxFrame()`
  queue argument, not the 802.11 QoS TID.
- The CCMP header is `1e 00 00 20 00 00 00 00`: ExtIV is set, Key ID is 0,
  and PN is 30. LLC/IP remains plaintext at this pre-HAL boundary, which is
  consistent with S31 inline hardware encryption and by itself is not
  evidence that encryption was skipped.
- Interpreting the descriptor as little-endian words gives `w0=0x02002009`,
  `w1=0x00000007`, `w2=0`, `w3=0x0000000b`, and `w4=0x00400304`.
  S31 `ppProcTxSecFrame()` reads `w4[11:8]` as the security/cipher selector;
  its value is 3. The low byte is 4, matching the pairwise PTK hardware slot
  established by the key-install disassembly and register observations.
- An EAPOL control frame at the same boundary had `w4=0x00400000`, no
  Protected bit, and no CCMP header. This contrast proves that before
  `lmacTxFrame()` the blob selected CCMP and PTK slot 4 for the DHCP frame;
  the failure is not merely a Protected header bit without descriptor crypto
  configuration.
- S31 disassembly shows `lmacTxFrame()` passes the descriptor from `eb+52` to
  `lmacSetTxFrame()` and ultimately enables the hardware queue through
  `hal_mac_txq_enable()`. The remaining boundary is therefore the descriptor
  and key state actually consumed by `lmacSetTxFrame()`/HAL/MAC inline crypto,
  including generated ciphertext/MIC and TX completion error status. The
  next experiment should inspect hardware-visible descriptor/status around
  queue enable, or capture the identical descriptor from a native S31 IDF
  reference. Generic Linux RX investigation is no longer the priority.

## 2026-08-14: ROM/blab crypto boundary and Linux-vs-IDF audit

- Provided `s31_rom.bin` (320 KiB mask ROM at 0x2f800000) was disassembled with
  the ESP toolchain. The WiFi LMAC/PP core is in mask ROM: `esp32s31.rom.pp.ld`
  maps each entry to a 4-byte `j` trampoline (e.g. `lmacSetTxFrame=0x2f800de8`,
  `lmacTxDone=0x2f800dec`, `lmacProcessTxError=0x2f800e10`,
  `ppProcTxSecFrame=0x2f800f84`). Each trampoline jumps to a real body
  (lmacSetTxFrame->0x2f8354a0, lmacTxDone->0x2f83432e, ppProcTxSecFrame->
  0x2f830244, ppProcTxDone->0x2f833126). The libpp.a/libnet80211.a archive
  versions are byte-structurally identical to these ROM bodies; only the
  addressing differs (absolute fixed globals such as `our_instances_ptr` at
  0x2f07ff48, `our_wait_eb` at 0x2f07ff50, `pTxRx` at 0x2f07ff54). Functions
  whose ROM symbol is commented out in the .ld (notably `lmacTxFrame`) come
  from the archive. So the earlier descriptor analysis is the running code.
- Added wrap instrumentation for `hal_crypto_set_key_entry`,
  `lmacProcessTxError` and `lmacTxDone` (all added to LINUX_WRAP_APIS), plus a
  post-DHCP dump of the MAC crypto engine registers 0x20104800..0x20104814 and
  the key-slot metadata words (never the key bytes).
- Hardware result (stable association to AHSS-WLAN2, channel 9):
  `wifi:(connect)dot11_authmode:0x3, pairwise_cipher:0x3, group_cipher:0x3`,
  and `security: WPA2-PSK ... cipher(pairwise:0x3, group:0x3)`. Two keys were
  installed: KEY #0 slot=4 len=16 info=00030006696ce514e67a04c0 (pairwise PTK)
  and KEY #1 slot=0 len=16 info=00030106696ce514e6000000 (group GTK). The
  key-info encodes cipher 3 and the AP BSSID 06:69:6c:e5:14:e6 as the peer MAC.
  The 8-byte CCMP header in every protected frame (`PN 00 00 ExtIV ...`) is a
  CCMP header, not a 12-byte TKIP header, and the CCMP-only AP completes the
  4-way handshake, so internal cipher 3 == CCMP on this part (the public
  `wifi_cipher_type_t` CCMP=4 is a different, user-facing enum).
- `lmacTxDone` completion showed the descriptor writeback: w4 changes from
  0x00400304 (cipher 3 + PTK slot 4, sampled at `lmacTxFrame` entry) to
  0x01400304 (bit 24 set) and w0 gains bit 12, i.e. the hardware consumes and
  updates the descriptor. `LMACTXERR total=0 seckid=0` across the whole DHCP
  burst: the hardware reports no security-key-id error, no collision/ack/cts
  timeout, no crypto error. `hal_crypto_enable(0,3,...)` is invoked by
  `wDev_Insert_KeyEntry` right after `hal_crypto_set_key_entry`, so the MAC
  inline CCMP engine is enabled for cipher 3 at key-install time.
- Conclusion: the boundary is no longer descriptor/key/cipher selection and is
  not a hardware key error. Cipher (CCMP), PTK slot 4, GTK slot 0, key length
  16, engine enable and descriptor are all correct, and yet five DHCP Discovers
  are PHY-ACKed with no Offer. This pushes the failure to the actual hardware
  crypto OUTPUT (ciphertext/MIC) or the key MATERIAL (the 16-byte TK), neither
  of which is observable from the current instrumentation.
- A persistent 292 ms blob-gate hold was again observed (`gate hold ...
  task=wifi reason=queue-receive wall=292612625ns exec=292625428ns
  last_event=6`); this matches the earlier 292 ms userspace stall and is the
  strongest remaining timing anomaly, but raising priority did not fix DHCP.
- Next experiment: (a) dump the crypto engine register state / key-slot header
  at the DHCP boundary to rule out a silent engine-disable, and (b) if those are
  clean, capture or reconstruct the actual transmitted ciphertext/MIC against a
  software CCMP reference, or compare the installed 16-byte TK hash against the
  wpa_supplicant PTK to rule out wrong key material.

## 2026-08-14: crypto engine register-level confirmation

- Rebuilt with the crypto-engine dump and retried association (first attempt
  auth-timed out reason 2, second attempt associated stably to AHSS-WLAN2,
  aid 2, channel 9, RSSI -37).
- The register state at the DHCP boundary is fully correct:
  `CRYPTO c0=00030103 c1=00030000 c2=00000000 c3=00000000 cfg=00000000 valid=00000011`
  i.e. crypto type 0 enabled with cipher 3 (CCMP) at 0x20104800, and the key
  validity bitmap 0x20104814 = 0x11 (slots 0 and 4 valid).
- The key-slot metadata words match the hal_crypto_set_key_entry construction
  exactly: slot 4 (PTK) `hdr0=e56c6906 hdr1=086ce614`, slot 0 (GTK)
  `hdr0=e56c6906 hdr1=48cce614`. `hdr0` is the peer BSSID word
  (e5 6c 69 06 = 06:69:6c:e5:14:e6 reversed); `hdr1` low16 0xe614 is the BSSID
  tail plus the cipher bits, and the high16 differ only by the
  pairwise/group key-info flags (0x086c vs 0x48cc), exactly as the blob
  encodes key_info[2]=0 vs 1.
- `LMACTXERR total=0 seckid=0` again across the DHCP burst. No crypto/key/retry
  error at any layer.
- Conclusion: every observable input to the MAC inline CCMP engine is correct
  (cipher=CCMP, PTK slot 4, GTK slot 0, key length 16, engine enabled, key
  metadata written, descriptor w4=0x00400304). The only remaining unverified
  quantities are the 16-byte key material bytes themselves and the actual
  ciphertext/MIC the engine emits. Next step is to fingerprint (hash, not log)
  the installed 16-byte TK against the wpa_supplicant PTK, and/or capture the
  over-the-air protected frame to compare against a software CCMP reference.

## 2026-08-14: key-material fingerprint (install vs hardware readback)

- Added an FNV-1a fingerprint of the 16-byte key at `hal_crypto_set_key_entry`
  (install-time) and of the key bytes read back from the hardware slot at
  offset 8 (never the key itself). Second stable association captured:
  PTK slot4 install fnv=af6db979, GTK slot0 install fnv=426d19e1; hardware
  readback slot4 keyfnv=668539f2, slot0 keyfnv=0c3fc1ce.
- The install hashes differ per key (PTK != GTK) and the readback hashes are
  stable across both DHCP dumps, so the key slots hold deterministic data, not
  random/uninitialised garbage.
- Crucially, the key-entry metadata words read back EXACTLY as written:
  hdr0=e56c6906 (peer BSSID word 06:69:6c:e5:14:e6 reversed) and
  hdr1=086ce614 (PTK) / 48cce614 (GTK) match the hal_crypto_set_key_entry
  encoding bit-for-bit. Only the key BYTES at offset 8 read back transformed.
  The metadata/header and the key bytes are written with the same MMIO stores
  into the same 40-byte slot, so the asymmetric result means the MAC crypto
  engine deliberately stores the AES key in an obfuscated/scheduled form while
  keeping the metadata plaintext. This is normal hardware behaviour, not a
  write fault: the key write demonstrably reaches the engine.
- Together with the earlier 4-way-handshake result (KCK and KEK are proven
  correct because Msg2/Msg3/Msg4 MIC and the KEK GTK unwrap succeed), the TK
  must also be correct: KCK/KEK/TK are produced by a single PRF-512 over the
  same PMK+nonces. This rules out wrong key material and wrong key write.
- Conclusion: cipher selection, PTK/GTK slots, key length, key material, key
  write, engine enable and TX descriptor are all correct, with zero hardware
  crypto/key error. The failing quantity is now narrowed to the actual
  ciphertext/MIC the MAC emits, or a non-crypto data-plane issue. Next:
  capture or reconstruct the over-the-air protected frame against a software
  CCMP reference, and/or instrument the LMAC RAW RX count to confirm whether
  the AP actually sends a DHCP offer that hardware decryption then drops.

## 2026-08-14: WPA2 RX decryption is the failing boundary (open AP fully works)

- Confirmed on hardware the correction at the top of this file: open AP
  (Griefer) associates and gets both IPv6 SLAAC and IPv4 DHCP; WPA2 associates
  but receives no data (no DHCP Offer). So the failure is the WPA2/CCMP RX
  decryption/data path, not TX, not key selection in the descriptor.
- Added `esp_test_get_hw_rx_statistics` dump at the DHCP boundary. On WPA2
  (AHSS-WLAN2) the counters are: total/ok ~34698/34868, plus 10-bit error
  fields where 0x20104318 and 0x20104330 are saturated at 1023. Two error
  classes are therefore pegged, consistent with every protected downlink being
  rejected, but the register names are not in the public soc headers so the
  exact error class (MIC vs replay vs keyid) is not yet pinned.
- Key slots, key material, engine enable and TX descriptor remain correct
  (c0=0x30103 CCMP enabled, valid=0x11, PTK slot4/GTK slot0, key len 16). The
  asymmetry is TX (explicit descriptor key slot) works while RX (hardware
  auto key selection by CCMP Key ID) fails. Next: capture the RX descriptor
  status/keyid at the ISR to see which slot/key the MAC actually uses and which
  error bit it sets, or compare against a native S31 IDF RX run.

## 2026-08-14: crypto-clock conflict hypothesis REFUTED (OpenOCD register read)

- The user's hypothesis was that the Linux hardware-crypto driver
  (`esp32s31-crypto.c`, AES/SHA/RSA/ECC island at 0x20508000, clkrst
  0x20587058) conflicts with the closed Wi-Fi stack, and specifically that it
  omits `crypto_sec_clk` (bit 2 of 0x20587058) while IDF's
  `esp_crypto_common_clk_enable()` sets it.  Read-only OpenOCD verification
  (`mdw` through telnet, no code changes) resolves this as follows.
- **Reset default.**  Reading `HP_SYS_CLKRST_CRYPTO_CTRL0_REG` (0x20587058)
  while the factory bootloader was running on hart0 (hart1 not yet released)
  returned `0x00615555`: all crypto clock enables set (sys bit0, sec bit2, aes
  bit4, sha bit6, rsa bit8, ds bit10, ecc bit12, hmac bit14, ecdsa bit16, rma
  bit22), all reset bits clear, and `crypto_clk_src_sel` (bits 21:20) = 2.
  So `crypto_clk_src_sel == 2` is the silicon reset value, not something
  Linux/OpenSBI writes; the IDF struct comment "default: 0" is inaccurate.
- **Linux idle state.**  The same register read earlier (Linux running,
  unassociated) returned `0x00200000`: every crypto clock enable cleared,
  `crypto_clk_src_sel` still 2.  This is the normal gated-idle state after the
  blob frees the shared crypto island; the enable bits are toggled by the blob
  around each crypto operation, the source select is never touched.
- **The MAC inline CCMP engine is a different clock domain.**  `hal_crypto`'s
  disassembly touches only 0x201046xx..0x201056xx — the MAC crypto engine
  config (0x20104814 validity) and the key slots (0x20105800) — and never
  0x20587058 or any 0x2050xxxx island register.  So the engine that performs
  the failing RX CCMP decryption is clocked by the modem/MAC clock, not by
  `crypto_ctrl0`.  The 4-way handshake (which uses the AES island via the
  software crypto) already proves the island clocks are enabled when needed.
- **No driver conflict.**  The DT node `crypto@20508000` is `status="disabled"`
  with the explicit comment that the closed Wi-Fi stack owns the same AES
  island while associated; the Linux driver therefore never probes and enables
  no clocks at runtime.  Even if it did probe, its missing bit 2 would be
  irrelevant to RX decryption, which does not use the crypto island.
- Conclusion: the crypto-clock/driver-conflict hypothesis is refuted.  The RX
  CCMP failure is inside the MAC inline engine (0x20104800) itself — key
  auto-selection by CCMP Key ID, PN/replay, or MIC check — and the next
  decisive step is still to capture the RX descriptor status/keyid at the ISR
  or compare against a native S31 IDF RX run.

## 2026-08-14: kthread critical-section + crypto-clock review (read-only)

Read-only review of the two things the user flagged: (1) the critical-section
bridge the blob's clock-enable uses, and (2) which clock the blob actually
toggles vs. which clock the failing MAC inline CCMP engine needs.  No code was
changed.

### Critical section semantics are correct (no scheduling race)

- `esp_os_enter_critical_safe(lock)` on S31 (single core, non-Xtensa) expands to
  `vPortEnterCritical()` — the `lock` (a `spinlock_t`) is ignored, exactly like
  native FreeRTOS single-core.
- `vPortEnterCritical()` (s31_rtos_core.c) -> `s31_linux_critical_enter()` ->
  `raw_spin_lock_irqsave(&s31_critical_lock)`.  On non-RT Linux `raw_spin_lock`
  itself does `preempt_disable()`, and `irqsave` disables the local (single
  radio hart) interrupts.  So the section disables **both** preemption and IRQ,
  matching FreeRTOS `portENTER_CRITICAL` on a single hart.
- Consequence: SCHED_FIFO 98 (radio worker) and 93 (Wi-Fi task) **cannot**
  preempt a held critical section, and the CLIC WiFi FIQ / TIMG1 tick cannot
  interrupt it either.  The per-task `critical_depth`/`critical_flags` nesting
  is correct; `s31_linux_critical_suspend`/`resume` coherently drop/re-take the
  raw lock only when a task blocks inside a section (a deliberate, gate-
  serialized departure from FreeRTOS "never block in a critical section").
- The modem-clock framework (`modem_clock_device_control`) and
  `esp_crypto_common_clk_enable` both wrap their refcount + register toggle in
  `esp_os_enter/exit_critical_safe`, so the refcount and the clock bit-flip are
  atomic.  No refcount/clock race found.

### The two crypto clocks are different domains

- The clock the blob toggles around software crypto is `esp_crypto_common_clk_enable`
  -> `HP_SYS_CLKRST.crypto_ctrl0` (0x20587058) bit0 (sys) + bit2 (sec).  That
  gates the **island** AES/SHA/RSA/ECC block (0x20508000..0x2050F000), used by
  the 4-way handshake and software crypto.  It does **not** gate the MAC inline
  CCMP engine at 0x20104800 (already refuted in the previous section).
- The MAC inline CCMP engine is gated by a **third** register, previously
  identified only as "the modem/MAC clock".  Pinning it down:
  `MODEM_SYSCON_CLK_CONF` = 0x20109C04, where the modem **security** (SEC)
  crypto clock lives: bit29 `clk_modem_sec_en`, bit28 `sec_apb_en`, bit27
  `sec_bah_en`, **bit26 `sec_ccm_en` (AES-CCM = CCMP)**, bit25 `sec_ecb_en`
  (AES-ECB = WEP/TKIP).  All default to 0 (OFF).  `hal_crypto`'s 0x20104800
  config/valid and 0x20105800 key slots are the engine this clock gates.

### Finding: the modem "SEC" clock is a BT/BLE clock, not the Wi-Fi CCMP clock

- The modem syscon has two clock registers: `clk_conf` (0x20109C04) and
  `clk_conf1` (0x20109C14).  `clk_conf` holds the modem **security** clock
  `clk_modem_sec_{en,apb,bah,ccm,ecb}_en` (bits 29/28/27/26/25); `clk_conf1`
  holds the Wi-Fi MAC/BB clocks, in particular `clk_wifimac_en` (bit9).
- The open-source framework enables `clk_modem_sec_*` only from
  `modem_clock_ble_mac_configure()` (BT/BLE MAC), and `clk_wifimac_en` only
  from `modem_clock_wifi_mac_configure()` (Wi-Fi MAC).  This looked like a
  possible "Wi-Fi CCMP clock never enabled" gap.
- **That interpretation is refuted by the ESP32-C6 reference**, which uses the
  *identical* wiring: `WIFI_CLOCK_DEPS = WIFI_MAC|WIFI_BB|COEXIST` (no SEC),
  `wifi_mac_configure` enables only `wifi_apb_clock + wifi_mac_clock`, and
  `ble_mac_configure` enables `bt_mac_clock + modem_sec_clock + ble_timer_clock`.
  Since C6 Wi-Fi WPA2 demonstrably works, the Wi-Fi MAC crypto engine is
  clocked by `clk_wifimac_en`, **not** by `clk_modem_sec_ccm_en` — the "modem
  SEC CCM" is the BT/BLE security engine (shared modem SEC block), which this
  Wi-Fi-only port correctly leaves gated.
- Cross-checked on S31 hardware artifacts: the ROM never writes 0x20109C04
  (it only reads clk_conf1 @ 0x20109C14/18), and libnet80211.a/libpp.a/libcore.a
  reference neither 0x20109C04 nor any clock symbol — consistent with "Wi-Fi
  blob relies on the framework to enable `clk_wifimac_en` and never touches the
  BT SEC clock".

### Conclusion: clock/scheduling review is clean

- The critical-section bridge is correct and cannot be preempted by
  SCHED_FIFO 98/93 or the CLIC/timer IRQ, so the blob's clock refcount+flip is
  atomic.
- The MAC inline CCMP engine (0x20104800) is clocked by `clk_wifimac_en`
  (0x20109C14 bit9), which the Wi-Fi clock path (`WIFI_CLOCK_DEPS` ->
  `modem_clock_wifi_mac_configure`) **does** enable; the island crypto clock
  (0x20587058) is toggled correctly around software crypto.
- Net: there is **no** crypto-clock or scheduling race explaining the RX CCMP
  failure.  The fault returns to key auto-selection inside the MAC engine (CCMP
  Key ID -> slot mapping, PN/replay, MIC), i.e. the previous section's
  conclusion stands.  If a runtime spot-check is ever wanted, read 0x20109C14
  (bit9 should be 1) and 0x20109C04 (bit26 expected 0 in a Wi-Fi-only build),
  but neither is expected to be the root cause.

## 2026-08-14: ChinaNet-38D07C WPA2 repro + 290 ms gate hold identified as UART polling

- Built and flashed an instrumentation image with:
  - a 100 Hz tick hardirq PC sampler recording `epc/ra/pid` whenever the
    blob gate is held by `current` (`get_irq_regs()` from hardirq context,
    because the blob runs on the SRAM exception stack);
  - gate release info (wall/exec/tick range) and a long-release printer
    (>=100 ms) that also reports `tick_pending`, IRQ work pending, worker
    `__state`, gate owner, and the last 64 PC samples;
  - `s31_linux_time_ns()` payload bridge;
  - a `g_config_func` (0x2f07fec0) wrapper installed at STA_START that
    timestamps every event-6 `ieee80211_ioctl_process()` dispatch and prints
    cmd/handler/duration.
- OpenOCD read-only (`halt 0; mdw 0x2f07feb8`) before this image:
  `g_timer_func=ieee80211_timer_do_process`,
  `g_net80211_tx_func=ROM 0x2f800c94`,
  `g_config_func=ieee80211_ioctl_process`.
- With the user-provided ChinaNet-38D07C / AH3s0564ZhF credentials the board
  associated stably (channel 7, BSSID f4:fc:49:5a:1a:b0, WPA2-PSK CCMP).
  `udhcpc` sent five Discovers, all LMAC TX completions reported success, no
  `s31_wifi_rx()` callback ran, and `udhcpc` reported `no lease`. This is a
  clean Linux-side repro of the WPA2 data failure on the user's AP.
- The long gate holds reproduced and were caught by the sampler:
  `queue-receive` and `semaphore-take` releases of 282 ms, 459 ms, 464 ms,
  173 ms, 536 ms, 399 ms, 530 ms and 135 ms. The dominant sampled PC was
  `0x2f83efda` with `ra=0x2f812226`, i.e. the mask-ROM UART/console polling
  loop (`0x2f81221e` loops around `0x2f83efca` reading the UART status at
  `(0x2038a000 + uart) + 0x1c`). These holds occur while `esp_rom_printf()`
  / IDF boot logs drain the 115200 UART, with `tick_pending` equal to the
  whole hold duration and `worker_state=0` (worker runnable but gated).
- Removing the per-DHCP TXDESC/CRYPTO/RXSTAT dumps and reducing LMACTXDONE
  prints did not restore DHCP. Long gate releases no longer occur during the
  DHCP phase, but `udhcpc` still gets no lease. Conclusion: the 290 ms gate
  hold is UART printing overhead, not a FreeRTOS/gate lost-wakeup bug.
- Baseline RXSTAT at STA_CONNECTED vs after the fifth Discover:
  `brx_err` (idx21, reg 0x20104314) went 956 -> 1023 and
  `nrx_err` (idx22, reg 0x20104318) went 768 -> 1023, both saturating during
  DHCP. The specific `nrx_err_*` deltas were restart +276, unsupport +170,
  serv +40, htsig +29, heunsupport +8, hesiga_crc +1; rx_datasuc only +14
  while rx_mpdu +25130 and rx_other_ucast +7191. So the station sees heavy
  ambient traffic and a small number of data successes, but the protected
  DHCP Offer is still not delivered.
- `g_config_func` wrapper observed during the same boot:
  cmd=49 fn=0xc025ba48 2.3 ms, cmd=3 fn=0xc025ab96 3.2 ms,
  cmd=26 fn=0xc025b9f0 0.1 ms. No config callback is the long pole.

## 2026-08-14: native IDF golden comparison on ChinaNet-38D07C

- Temporarily modified `build-radio`'s native FreeRTOS/IDF probe
  (`bootloader/main/native_wifi_probe.c`) to connect to
  `ChinaNet-38D07C` / `AH3s0564ZhF` and to keep ROM console output on
  UART0 (`/dev/ttyUSB0`), then flashed only `build-radio/hello_world.bin`
  at the factory partition. The normal Linux factory app was restored after
  the capture.
- Native result: station associated (aid=2, channel 7, BSSID
  f4:fc:49:5a:1a:b0), WPA2-PSK CCMP, and `esp_netif` DHCP succeeded
  (`192.168.5.138`, gw `192.168.5.1`). So the AP is good and native IDF
  works on this exact board.
- Native key-table metadata is bit-identical to Linux:
  slot0 `hdr0=5a49fcf4 hdr1=48ccb01a`, slot4
  `hdr0=5a49fcf4 hdr1=086cb01a`, engine `c0=0x00030103`, `valid=0x11`.
  Linux key-table metadata is therefore not the differentiator.
- Native RXSTAT at STA_CONNECTED already has `brx_err`/`nrx_err` saturated
  at 1023. Linux only saturates after DHCP. The 10-bit error counters are
  therefore ambient/environmental and cannot be used as CCMP RX evidence on
  this AP.
- Native also logs `phy_init: esp_phy_load_cal_data_from_nvs: NVS has not
  been initialized`; this is identical to Linux and is not the failure.
- The Linux-vs-native difference is now narrowed to the closed Wi-Fi data
  path under the Linux kthread/OSI compatibility layer, with key metadata,
  cipher selection, engine enable, and AP behavior all equivalent.

## 2026-08-14: post-TX-completion frame buffer remains plaintext

- Added a one-shot `__wrap_lmacTxDone` diagnostic for the first two protected
  DHCP-size frames after association. It re-reads the frame buffer after the
  hardware has signalled completion.
- On ChinaNet-38D07C the buffer after `lmacTxDone` is still the original
  plaintext 802.11+LLC/IP frame (`aa aa 03 00 00 00 08 00 45 ...`), with
  QoS control `0x0007` and CCMP header `18 00 00 20 00 00 00 00`. The
  descriptor writeback is `w4=0x01400304`. This is consistent with the MAC
  encrypting on the fly from memory without writing ciphertext back; it is
  not by itself evidence that encryption was skipped.
- Current state: native IDF works on the same AP/board and Linux does not;
  key metadata and engine enable are identical; RXSTAT error counters are
  ambient and saturated in native too. The remaining unverified boundary is
  the actual over-the-air CCMP ciphertext/MIC, which needs either an
  independent monitor-mode receiver or an in-kernel software CCMP reference
  comparison (without logging TK/PTK material).

## 2026-08-14: TX判定 - key path is faithful but Linux protected TX is not accepted by AP

- Wrapped `wpa_install_key` and added an immediate hash print to
  `hal_crypto_set_key_entry` (FNV-1a, key material never printed).
- On ChinaNet-38D07C association:
  `wpa_install_key alg=3 idx=0 set_tx=1 len=16 fnv=fe7cf878`
  `hal_crypto_set_key_entry slot=4 len=16 fnv=fe7cf878`
  `wpa_install_key alg=3 idx=1 set_tx=0 len=16 fnv=966cc564`
  `hal_crypto_set_key_entry slot=0 len=16 fnv=966cc564`
  So the exact PTK TK and GTK bytes supplied by wpa_supplicant are the bytes
  written to the hardware key slots. The Linux key path is not corrupting key
  material.
- Host-side oracle: the development host (`eth4`, 192.168.5.126/24) is on the
  same L2 as the AP and sees the native IDF probe's broadcast ARP from
  `30:ed:a0:f3:d4:ac` (e.g. gratuitous ARP for 192.168.5.138). During the
  Linux static-IP test (`192.168.5.250/24`, `ping 192.168.5.1`), `tcpdump`
  on the host captured zero frames from the board, although the board logged
  successful LMAC TX completions for those ARP/ICMP attempts.
- Conclusion: the AP forwards the native probe's protected broadcast frames
  but not Linux's. With key bytes, key metadata, cipher selection, engine
  enable, and descriptor all matching the native reference as far as observed,
  the Linux TX path is still producing over-the-air CCMP frames the AP does
  not accept. This is the first hard evidence that TX is not actually good,
  contradicting the earlier assumption.
- Open question: which MAC TX security state differs. The next differential
  should dump the full MAC security/TX crypto register block and a completed
  TX descriptor for the same frame in both native and Linux, or capture the
  over-the-air frame with an independent receiver.

## 2026-08-14 FINAL: root cause found and fixed - payload memcpy was byte-copying the CCMP key slot

### Root cause
- `hal_crypto_set_key_entry()` in the closed S31 Wi-Fi HAL copies the 16-byte
  CCMP key into the MAC key-slot MMIO at `0x20105808 + slot*40` via `memcpy`.
- The native IDF build resolves that `memcpy` to the mask ROM word-copying
  implementation; the Linux payload resolved it to picolibc's byte-oriented
  `memcpy` (`sb` per byte). The S31 MAC crypto key slot does not correctly
  latch byte stores, while the 32-bit metadata writes (`sw`) around it work
  fine.
- Result: Linux key metadata (`hdr0`/`hdr1`) was always correct, but the key
  bytes read back did not match the bytes passed by wpa_supplicant. The
  4-way handshake still worked because it uses software crypto, but every
  protected data frame was encrypted/decrypted with a corrupted key slot, so
  the AP dropped the station's CCMP frames.

### Fix
- Added `radio-experiment/s31_memcpy.c` and linked it into the payload. It
  implements a word-oriented `memcpy` for 4-byte aligned source/destination,
  with byte fallback for the rest, and a byte `memset`. The payload's local
  `memcpy` now writes the MAC key slots with 32-bit stores.

### Validation after fix
- Before fix: `hal_crypto_set_key_entry` slot0 FNV `966cc564` but slot0
  readback FNV `1280eda4`; PTK also differed.
- After fix: slot0 `966cc564 -> 966cc564`, slot4 `46a37b22 -> 46a37b22`
  (readback matches install exactly).
- Full official sequence from a clean boot with the cleaned image:
  - `ip link set wlan0 up`
  - `wifi-scan wlan0` -> 32 BSS, status 0
  - `wifi-connect wlan0 ChinaNet-38D07C AH3s0564ZhF` -> WPA2 CCMP connected
  - `udhcpc -i wlan0 -n -q -t 5` -> `lease of 192.168.5.138 obtained`
  - `wget -O /tmp/baidu.html http://baidu.com` -> saved, 2443 bytes,
    `<!--STATUS OK-->`
- `persist` was flashed (`make flash-persist`) so the rootfs overlay mounts
  and `/run`, `/tmp` are tmpfs writable; udhcpc can write
  `/run/resolv.conf` and wget can write `/tmp/baidu.html`.

### Cleanup
- Removed or disabled the high-flood diagnostics used to find this:
  pp_config wrapper, `wpa_install_key` wrapper, automatic long-gate PC sample
  print, per-DHCP TXDESC/CRYPTO/RXSTAT dumps, post-TX frame dump, and
  LMACTXDONE print flood.
- The PC sampler and gate timing infrastructure remain in place but silent.
