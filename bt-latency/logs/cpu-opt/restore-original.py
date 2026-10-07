from pathlib import Path
import json,subprocess
r=Path('/home/grieferpig/esp32-s31-linux');b=Path(__file__).resolve().parent/'restore-original';b.mkdir()
cmd=json.loads((r/'bt-latency/logs/kernel-hz100/flash-command.json').read_text())
(b/'flash-command.json').write_text(json.dumps(cmd,indent=2))
for key in ('flash','verify'):
 with (b/(key+'.txt')).open('wb') as f:subprocess.run(cmd[key],cwd=r,stdout=f,stderr=subprocess.STDOUT,check=True)
assert (b/'verify.txt').read_text().count('Verification successful (digest matched).')==6
print('Original six slots verified',flush=True)
argv=['python3','-u',str(r/'bt-latency/wifi_final_check.py'),str(b/'wifi')]
(b/'wifi-command.json').write_text(json.dumps(argv,indent=2))
with (b/'wifi.txt').open('wb') as f:subprocess.run(argv,cwd=r,stdout=f,stderr=subprocess.STDOUT,check=True)
print('Restored Wi-Fi scan checked',flush=True)
argv=['python3','-u',str(r/'bt-latency/logs/kernel-hz100/restore-ready.py'),str(b/'ready'),'rr20']
(b/'ready-command.json').write_text(json.dumps(argv,indent=2))
with (b/'ready.txt').open('wb') as f:subprocess.run(argv,cwd=r,stdout=f,stderr=subprocess.STDOUT,check=True)
print('BLE-ready baseline restored',flush=True)
