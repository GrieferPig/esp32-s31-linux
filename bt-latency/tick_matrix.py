#!/usr/bin/env python3
"""Controlled Bluetooth-only timer cadence runs; start BTstack with matched payloads first."""
import os,sys,subprocess,json,re
from pathlib import Path
root=Path(__file__).resolve().parent
protocol=sys.argv[1]
if protocol not in ('ble','spp'):raise SystemExit('protocol must be ble or spp')
base=Path(sys.argv[2]);base.mkdir(parents=True,exist_ok=True)
ticks=[int(x) for x in sys.argv[3:]] or [40,10,1,40]
env=os.environ.copy();env['S31_BOARD_STATS']='1'
env['S31_SPP_FRAME_SIZE']='990'
def stats(path):
    text=path.read_text(errors='replace').replace('\r','')
    cpu=[int(v) for v in re.search(r'^cpu +(.+)$',text,re.M).group(1).split()][:8]
    counters={k:int(v) for k,v in re.findall(r'\b(hci_rx_dropped|hci_tx_dropped|worker_passes|heap_used|heap_peak|ticks)=(\d+)',text)}
    return cpu,counters
for index,tick in enumerate(ticks):
    if not 1<=tick<=40:raise SystemExit('tick outside1..40ms')
    dest=base/f'{protocol}-tick{tick}-{index}';dest.mkdir(exist_ok=False)
    command=f'echo {tick} > /sys/module/esp32s31_radio/parameters/bt_tick_ms; test "$(cat /sys/module/esp32s31_radio/parameters/bt_tick_ms)" = "{tick}"'
    result=subprocess.run(['python3',str(root/'board_command.py'),command,'10'],capture_output=True)
    (dest/'set-tick.raw').write_bytes(result.stdout+result.stderr)
    if result.returncode:raise SystemExit('setting timer failed')
    argv=['python3',str(root/('fresh_fd_run.py' if protocol=='ble' else 'fresh_spp_run.py')),str(dest)]
    (dest/'command.json').write_text(json.dumps({'board_command':command,'argv':argv,'env':{k:v for k,v in env.items() if k.startswith('S31_')}},indent=2))
    with (dest/'wrapper.txt').open('wb') as out:
        result=subprocess.run(argv,env=env,stdout=out,stderr=subprocess.STDOUT)
    print(dest.name,'exit',result.returncode,flush=True)
    if (dest/'run.txt').exists():print((dest/'run.txt').read_text(),flush=True)
    if result.returncode:raise SystemExit(result.returncode)
    before,bc=stats(dest/'before-board.raw');after,ac=stats(dest/'after-board.raw')
    delta=[a-b for a,b in zip(after,before)];total=sum(delta)
    data={'cpu_busy_pct_aggregate':100*(total-delta[3]-delta[4])/total,
          'cpu_system_irq_softirq_pct':100*(delta[2]+delta[5]+delta[6])/total,
          'counter_delta':{k:ac[k]-bc[k] for k in bc},'before':bc,'after':ac,
          'note':'CPU spans snapshots around receiver, including connect/discovery overhead; throughput uses only40s receiver window.'}
    (dest/'board-summary.json').write_text(json.dumps(data,indent=2))
    print(json.dumps(data),flush=True)
