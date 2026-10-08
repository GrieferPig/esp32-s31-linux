from pathlib import Path
import subprocess,json,sys,hashlib,shutil
r=Path('/home/grieferpig/esp32-s31-linux'); d=Path(__file__).resolve().parent
dist=sys.argv[1]; assert len(dist)==16 and all(c in '0123456789abcdef' for c in dist)
original='e168e8c0f293a4d4'
cmds=json.loads((r/'bt-latency/logs/kernel-hz100/flash-command.json').read_text())
cmds={k:[s.replace(original,dist) for s in v] for k,v in cmds.items()}
suffix='restore' if dist==original else 'diagnostic'
(d/(suffix+'-flash-command.json')).write_text(json.dumps(cmds,indent=2))
for phase in ('flash','verify'):
 with (d/(suffix+'-'+phase+'.log')).open('wb') as f:
  x=subprocess.run(cmds[phase],cwd=r,stdout=f,stderr=subprocess.STDOUT)
 assert x.returncode==0,phase
 if phase=='verify':
  text=(d/(suffix+'-'+phase+'.log')).read_text()
  assert text.count('Verification successful (digest matched).')==6,text
 print(phase,'passed',dist,flush=True)
