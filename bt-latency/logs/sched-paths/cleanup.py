from pathlib import Path
import json,re,subprocess
r=Path('/home/grieferpig/esp32-s31-linux');d=Path(__file__).resolve().parent
assert (d/'restore-verify.log').read_text().count('Verification successful (digest matched).')==6
wifi=json.loads((d/'restored-wifi/result.json').read_text())
assert wifi['scan_pass'] and wifi['observed_bss']>0
health=(d/'restored-ready/rr20-0/final-health.raw').read_text()
assert all(v in health for v in ('state=2','bt_init=0','bt_enable=0','hci_rx_dropped=0','hci_tx_dropped=0'))
log=(d/'restored-ready/rr20-0/final-log.raw').read_text()
assert 'up and running' in log and 'S31 BLE advertising complete: requested=1 status=0' in log
priority=(d/'restored-ready/rr20-0/priority.raw').read_text()
assert 'SCHED_RR' in priority and '20' in priority
assert not subprocess.check_output(['git','status','--porcelain'],cwd=r/'linux-esp32-s31').strip()
assert subprocess.check_output(['git','branch','--show-current'],cwd=r,text=True).strip()=='bt-latency-fix'
assert (r/'dist/current').resolve().name=='e168e8c0f293a4d4'
result=dict(accepted_dist='e168e8c0f293a4d4',full_six_slots_restored=True,explicit_digest_matches=6,
 persist_written=False,kernel_source_clean=True,production_changes_retained=False,
 wifi_scan_bss=wifi['observed_bss'],wifi_association_tested=False,ble_advertising_confirmed=True,
 btstack_priority='RR20',btstack_cpu=1,dist_current='e168e8c0f293a4d4',emulator_used=False)
(d/'cleanup.json').write_text(json.dumps(result,indent=2))
print(json.dumps(result,indent=2))
