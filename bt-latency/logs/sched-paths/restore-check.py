from pathlib import Path
import subprocess,json
r=Path('/home/grieferpig/esp32-s31-linux');d=Path(__file__).resolve().parent
assert (d/'restore-verify.log').read_text().count('Verification successful (digest matched).')==6
jobs=[('wifi',['python3','-u','bt-latency/wifi_final_check.py',str(d/'restored-wifi')]),
 ('ready',['python3','-u','bt-latency/logs/kernel-hz100/restore-ready.py',str(d/'restored-ready'),'rr20'])]
for name,argv in jobs:
 (d/(name+'-command.json')).write_text(json.dumps(argv,indent=2))
 with (d/(name+'.log')).open('wb') as f:x=subprocess.run(argv,cwd=r,stdout=f,stderr=subprocess.STDOUT)
 assert x.returncode==0,name
 print(name,'passed',flush=True)
