from pathlib import Path
import json
d=Path(__file__).resolve().parent
s=json.loads((d/'summary.json').read_text());cleanup=json.loads((d/'cleanup.json').read_text())
assert set(s['accepted'])=={'ble','spp'} and cleanup['explicit_digest_matches']==6
p=s['mean_categories_pct']
def mean_actor(kind,actor,cat):
 rows=[x for x in s['runs'] if x['protocol']==kind and x['enabled']]
 return sum(sum(y['total_capacity_pct'] for y in x['actor_categories'] if y['actor']==actor and y['category']==cat) for x in rows)/len(rows)
rows=[
 ('Controller / payload',lambda k:p[k]['radio_payload']),
 ('Scheduler',lambda k:p[k]['scheduler']),
 ('Hard IRQ',lambda k:p[k]['hardirq']),
 ('Radio integration',lambda k:p[k]['radio_integration']),
 ('Gate + sync + radio enqueue',lambda k:sum(p[k][v] for v in ('gate','sync_wait','sync_wake','sync_critical','radio_queue'))),
 ('BTstack syscall bodies',lambda k:sum(p[k][v] for v in ('read','write','select_poll','time','other_syscall'))),
 ('BTstack outside annotated scopes',lambda k:mean_actor(k,'btstack','outside')),
 ('Soft IRQ',lambda k:p[k]['softirq'])]
text='''# Scheduler and BTstack syscall path costs

Scheduling and IRQ handling are stronger next targets than direct gate/wait work or BTstack syscall bodies. The payload itself also consumes substantial CPU. These measurements locate execution; they do not yet prove which change will reduce it, or assign the whole Linux versus ESP-IDF gap to one layer.

## Real board timing

Diagnostic dist 559a02aabaf6fdca, same accepted settings, basedsurface peer, LE1M. Eight checked 40-second board-to-host transfers: off/on/on/off for each protocol. Four enabled profiles passed exact per-CPU nanosecond conservation, zero clock/state errors and SHA256-verified capture. All eight transfers have zero sequence/pattern errors and HCI drops.

Percentages below use both cores' elapsed capacity: 50% equals one full core. Nested categories are exclusive; sleeping waits are excluded. Payload includes the controller and open payload shim/timer work, rather than only the closed blob.

| Annotated path | BLE capacity | SPP capacity |
|---|---:|---:|
'''
for label,f in rows:text+=f'| {label} | {f("ble"):.2f}% | {f("spp"):.2f}% |\n'
text+='''\nWithin BTstack syscall bodies, select/poll is the largest category (BLE 1.57%, SPP 3.51% of capacity). Read, write and time calls together are smaller. Syscall architecture entry/exit is outside these body scopes; BTstack outside therefore includes userspace and unannotated trap/SBI boundaries, and cannot be called pure userspace time.

The direct compatibility bridge is not free: gate/sync/enqueue costs about 8% of total capacity, plus about 6% in integration. It is not established as the sole cause. Scheduler and hard IRQ categories together occupy 23.04% for BLE and 27.90% for SPP. IRQ actor names denote the interrupted task, not the IRQ source.

## Overhead controls and performance

Two repetitions per condition; values are means. CPU uses wall minus NO_HZ idle/iowait residency, distinct from the annotated path partition. Throughput is host-received, not pump-to-controller.

| Protocol / image | CPU capacity busy | Host KiB/s |
|---|---:|---:|
'''
for kind in ('ble','spp'):
 a=s['accepted'][kind]
 text+=f'| {kind.upper()} / accepted | {a["cpu_pct"]:.3f}% | {a["KiBs"]:.3f} |\n'
 for enabled in (0,1):
  a=s['groups'][kind+'-'+str(enabled)]
  text+=f'| {kind.upper()} / diagnostic {"on" if enabled else "off"} | {a["cpu_pct"]:.3f}% | {a["KiBs"]:.3f} |\n'
text+='''\nEnabling timing adds 0.90 CPU percentage points in BLE and 1.71 in SPP versus the same diagnostic disabled. Corresponding throughput changes are -0.11% and -0.22%. The diagnostic-disabled versus restored accepted CPU means differ by +0.99 points in BLE and -0.006 points in SPP. Those checks expose the always-present hooks plus run variation. Compare raw repetitions rather than treating these small samples as a tight overhead bound.

Task-state idle includes idle-loop execution and unannotated trap/firmware boundaries, whereas NO_HZ measures idle residency. Thus the annotated non-idle means (BLE 66.61%, SPP 79.27%) differ from the CPU probe means (69.90%, 81.94%). Do not rescale the categories to erase that difference. Instrumentation and boundary overhead remain included.

## What changed, and precise next work

The diagnostic adds nested timing scopes, task state that survives sleep/migration, scheduler-switch accounting, IRQ/softIRQ overrides, and a root-only frozen /proc/s31_cost readout. It never uses timer-PC samples to apportion time. Tests exercise sleep exclusion, nested attribution, freezing, invalid clocks/states and conservation; real runs validate both CPU windows and instrumentation overhead. Ten patch-applied scratch files match the frozen diagnostic snapshots byte for byte. Six flash-safety tests passed.

Event rates averaged over enabled windows:

| Event / second across both CPUs | BLE | SPP |
|---|---:|---:|
'''
for name in ('context_switch','hardirq','worker_call','sync_wait','sync_wake','deferred_isr','direct_isr'):
 text+=f'| {name} | {s["event_rates_per_second"]["ble"][name]:.1f} | {s["event_rates_per_second"]["spp"][name]:.1f} |\n'
text+='''\nNEXT: split scheduler costs into task selection, switch_mm, the custom coprocessor SBI switch and finish_task_switch; split IRQ costs by interrupt source and entry/dispatch/exit. Also time esp32s31_ext_enter_kernel/exit_user separately from BTstack userspace. The source calls custom-state SBI during applicable context switches and restores extension state at the user-return boundary; these are concrete candidates, not separately measured culprits. ASIDs are deliberately disabled because S31 TLBs do not reliably distinguish user translations, so that setting is not an established safe optimization.

## Evidence and restoration

Exact commands, every run's raw host payload and UART records, frozen timings, parsers, summary.json, build manifest/hashes, source snapshots and diagnostic.patch are retained here. See method.md for scopes and reproduction. archive-manifest.json records lossless gzip SHA256 round trips; large raw files and source snapshots may be stored as .gz.

No production optimization is retained. Original kernel sources are restored exactly to HEAD; submodule clean. Full accepted six-slot set e168e8c0f293a4d4 is restored with six explicit digest matches, and dist/current points to it. Diagnostic build artifacts remain available for reproduction; out/ still contains diagnostic build outputs and needs a rebuild before source-matched use. Persist was never written.
'''
text+=f'\nPost-restore Wi-Fi scan found {cleanup["wifi_scan_bss"]} BSS; AP association is untested. BLE advertising, CPU1/RR20 and controller health are confirmed. No emulator was used.\n'
(d/'report.md').write_text(text)
print('report written')
