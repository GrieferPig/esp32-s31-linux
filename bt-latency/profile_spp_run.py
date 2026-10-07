#!/usr/bin/env python3
"""Measure legacy callback SPP before and during standard kernel sampling."""
import os,subprocess,sys,json
from pathlib import Path
from analyze_board_stats import summarize
root=Path(__file__).resolve().parent;base=Path(sys.argv[1])
env=os.environ.copy();env.update(S31_BOARD_STATS='1',S31_SPP_FRAME_SIZE='990')
for mode in ('off','on'):
    dest=base/('spp-profile-'+mode);dest.mkdir()
    if mode=='on':
        cmd='echo 6 > /sys/kernel/profiling; cat /sys/kernel/profiling; ls -l /proc/profile'
        r=subprocess.run(['python3',str(root/'board_command.py'),cmd,'15'],capture_output=True)
        (dest/'enable-command.txt').write_text(cmd+'\n')
        (dest/'enable.raw').write_bytes(r.stdout+r.stderr)
        if r.returncode:raise SystemExit('profile enable failed')
        env['S31_BOARD_PROFILE']='1'
    argv=['python3',str(root/'fresh_spp_run.py'),str(dest)]
    (dest/'command.json').write_text(json.dumps({'argv':argv,'env':{k:v for k,v in env.items() if k.startswith('S31_')}},indent=2))
    with (dest/'wrapper.txt').open('wb') as out:
        r=subprocess.run(argv,env=env,stdout=out,stderr=subprocess.STDOUT)
    print(mode,'exit',r.returncode,(dest/'run.txt').read_text(),flush=True)
    summary=summarize(dest)
    print(json.dumps({**summary,'tasks':summary['tasks'][:6]}),flush=True)
    if mode=='on':
        subprocess.run(['python3',str(root/'pull_cpu_profile.py'),str(dest)],check=True)
        with (dest/'kernel-profile.json').open('w') as out:
            subprocess.run(['python3',str(root/'analyze_cpu_profile.py'),str(dest/'kernel.profile'),str(base/'System.map')],stdout=out,check=True)
        report=json.loads((dest/'kernel-profile.json').read_text())
        print('KERNEL_PROFILE',report['samples'],report['symbols'][:25],flush=True)
    if r.returncode:raise SystemExit(r.returncode)
