from pathlib import Path
import subprocess,json
r=Path('/home/grieferpig/esp32-s31-linux');b=r/'bt-latency/logs/kernel-hz100'
args=json.loads((b/'flash-command.json').read_text())
for phase in ['flash','verify']:
 with (b/(phase+'.txt')).open('wb') as f:subprocess.run(args[phase],cwd=r,stdout=f,stderr=subprocess.STDOUT,check=True)
assert (b/'verify.txt').read_text().count('Verification successful (digest matched).')==6
subprocess.run(['python3',str(r/'bt-latency/native_monitor.py'),str(b/'initial-boot.raw'),'30'],stdout=subprocess.DEVNULL,check=True)
print('Full six-slot set verified and boot captured',flush=True)
