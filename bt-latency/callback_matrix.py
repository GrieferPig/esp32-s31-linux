#!/usr/bin/env python3
"""Same-image A/B for callback wake coalescing, with matched native payloads."""
import os,sys,json,subprocess
from pathlib import Path
from analyze_board_stats import summarize
root=Path(__file__).resolve().parent
protocol=sys.argv[1];base=Path(sys.argv[2]);modes=sys.argv[3:] or ['0','1','0','1']
if protocol not in ('spp','ble'):raise SystemExit('bad protocol')
base.mkdir(parents=True,exist_ok=True)
env=os.environ.copy();env.update(S31_BOARD_STATS='1',S31_SPP_FRAME_SIZE='990')
for index,mode in enumerate(modes):
    if mode not in ('0','1'):raise SystemExit('mode must be0 or1')
    dest=base/f'{protocol}-mode{mode}-{index}';dest.mkdir(exist_ok=False)
    pairable='1' if protocol=='spp' else '0'
    board_env=f'S31_HCI_DEVICE=/dev/s31-hci S31_BTSTACK_PAIRABLE={pairable} S31_BTSTACK_COEX=0 S31_BTSTACK_BURST=24 S31_BTSTACK_SPP_BYTES=990 S31_BTSTACK_BLE_BYTES=495 S31_BTSTACK_INTERVAL=12 S31_BTSTACK_COALESCE_WAKEUPS={mode} S31_BTSTACK_NAME="S31 Radio"'
    log=f'/tmp/wake-{protocol}-{index}.log'
    start=f'{board_env} /usr/sbin/s31-btstack-a2dp -u 0 -l none >{log} 2>&1 &'
    command=f'echo 40 > /sys/module/esp32s31_radio/parameters/bt_tick_ms; killall s31-btstack-a2dp 2>/dev/null; sleep 1; {start} sleep 8; cat {log}; if ! grep -q "up and running" {log}; then killall s31-btstack-a2dp; sleep 2; {start} sleep 8; cat {log}; fi; grep -q "up and running" {log}'
    result=subprocess.run(['python3',str(root/'board_command.py'),command,'35'],capture_output=True)
    (dest/'start.raw').write_bytes(result.stdout+result.stderr)
    if result.returncode:raise SystemExit('BTstack did not start')
    expected=b'coalesced' if mode=='1' else b'legacy'
    if b'S31 callback wakeups: '+expected not in result.stdout:raise SystemExit('mode not confirmed')
    argv=['python3',str(root/('fresh_fd_run.py' if protocol=='ble' else 'fresh_spp_run.py')),str(dest)]
    (dest/'command.json').write_text(json.dumps({'board_command':command,'argv':argv,'env':{k:v for k,v in env.items() if k.startswith('S31_')}},indent=2))
    with (dest/'wrapper.txt').open('wb') as out:
        result=subprocess.run(argv,env=env,stdout=out,stderr=subprocess.STDOUT)
    print(dest.name,'exit',result.returncode,flush=True)
    if (dest/'run.txt').exists():print((dest/'run.txt').read_text(),flush=True)
    if result.returncode:raise SystemExit(result.returncode)
    stats=summarize(dest)
    print(json.dumps({k:v for k,v in stats.items() if k!='tasks'}),flush=True)
