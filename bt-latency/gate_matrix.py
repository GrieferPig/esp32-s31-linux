#!/usr/bin/env python3
"""Same-image, same-affinity BT-only sleeping-gate A/B with CPU samples."""
import os,sys,json,subprocess
from pathlib import Path
from analyze_board_stats import summarize
root=Path(__file__).resolve().parent;base=Path(sys.argv[1]);base.mkdir(parents=True,exist_ok=True)
env=os.environ.copy();env.update(S31_BOARD_STATS='1',S31_BOARD_PROFILE='1',S31_SPP_FRAME_SIZE='990')
for i,mode in enumerate(sys.argv[2:] or ['0','1','0','1']):
    if mode not in ('0','1'):raise SystemExit('gate mode0 or1 only')
    dest=base/f'gate{mode}-{i}';dest.mkdir()
    cmd=f'echo {mode} > /sys/module/esp32s31_radio/parameters/bt_gate_sleep; cat /sys/module/esp32s31_radio/parameters/bt_gate_sleep; taskset -p 2 $(pidof s31-btstack-a2dp); taskset -p $(pidof s31-btstack-a2dp)'
    r=subprocess.run(['python3',str(root/'board_command.py'),cmd,'15'],capture_output=True)
    (dest/'configure.raw').write_bytes(r.stdout+r.stderr)
    if r.returncode:raise SystemExit('gate configure failed')
    argv=['python3',str(root/'fresh_spp_run.py'),str(dest)]
    (dest/'command.json').write_text(json.dumps({'board_command':cmd,'argv':argv,'env':{k:v for k,v in env.items() if k.startswith('S31_')}},indent=2))
    with (dest/'wrapper.txt').open('wb') as out:
        r=subprocess.run(argv,env=env,stdout=out,stderr=subprocess.STDOUT)
    print(dest.name,'exit',r.returncode,(dest/'run.txt').read_text(),flush=True)
    summary=summarize(dest)
    print(json.dumps({**summary,'tasks':summary['tasks'][:5]}),flush=True)
    subprocess.run(['python3',str(root/'pull_cpu_profile.py'),str(dest)],check=True)
    with (dest/'kernel-profile.json').open('w') as out:
        subprocess.run(['python3',str(root/'analyze_cpu_profile.py'),str(dest/'kernel.profile'),str(base.parent/'System.map')],stdout=out,check=True)
    report=json.loads((dest/'kernel-profile.json').read_text())
    print('KERNEL_PROFILE',report['samples'],report['symbols'][:5],flush=True)
    if r.returncode:raise SystemExit(r.returncode)
