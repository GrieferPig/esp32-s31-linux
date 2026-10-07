from pathlib import Path
import json,subprocess
r=Path('/home/grieferpig/esp32-s31-linux');b=Path(__file__).resolve().parent
cmd=json.loads((b/'flash-command.json').read_text())
for key in ('flash','verify'):
 with (b/(key+'.txt')).open('wb') as f:subprocess.run(cmd[key],cwd=r,stdout=f,stderr=subprocess.STDOUT,check=True)
assert (b/'verify.txt').read_text().count('Verification successful (digest matched).')==6
print('Candidate full six slots explicitly hash verified',flush=True)
