from pathlib import Path
import subprocess,json
r=Path('/home/grieferpig/esp32-s31-linux')
b=r/'bt-latency/logs/cpu-usage/restore-linux'
b.mkdir()
commands=json.loads((r/'bt-latency/logs/kernel-hz100/flash-command.json').read_text())
(b/'command.json').write_text(json.dumps(commands,indent=2))
for key in ('flash','verify'):
 with (b/(key+'.txt')).open('wb') as f:
  subprocess.run(commands[key],cwd=r,stdout=f,stderr=subprocess.STDOUT,check=True)
assert (b/'verify.txt').read_text().count('Verification successful (digest matched).')==6
argv=['python3','-u',str(r/'bt-latency/logs/kernel-hz100/restore-ready.py'),str(b/'ready'),'rr20']
(b/'ready-command.json').write_text(json.dumps(argv,indent=2))
with (b/'ready.txt').open('wb') as f:
 subprocess.run(argv,cwd=r,stdout=f,stderr=subprocess.STDOUT,check=True)
print('Linux six slots verified and BLE-ready setup restored',flush=True)
