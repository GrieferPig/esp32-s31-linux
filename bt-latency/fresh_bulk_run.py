#!/usr/bin/env python3
import subprocess,time,select,os,sys
from pathlib import Path
dest=Path(sys.argv[1]);dest.mkdir(exist_ok=True)
p=subprocess.Popen(['bluetoothctl','--agent','NoInputNoOutput'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
os.set_blocking(p.stdout.fileno(),False);buf=b''
def run(cmd,wait):
 global buf
 p.stdin.write((cmd+'\n').encode());p.stdin.flush()
 end=time.monotonic()+wait
 while time.monotonic()<end:
  if select.select([p.stdout],[],[],.2)[0]:buf+=p.stdout.read() or b''
for cmd,wait in [('agent on',.5),('default-agent',.5),('remove 30:ED:A0:F3:D4:AE',1),('power off',2),('power on',2),('scan le',8),('pair 30:ED:A0:F3:D4:AE',7),('scan off',.5)]:
 run(cmd,wait)
(dest/'fresh-pair.raw').write_bytes(buf)
print('\n'.join(l for l in buf.decode(errors='replace').splitlines() if any(x in l.lower() for x in ['failed','successful','paired:','bonded:'])),flush=True)
with (dest/'run.txt').open('w') as f:
 subprocess.run(['python3','-u',str(Path(__file__).resolve().parent/'logs/burst24/run-bonded.py'),'30:ED:A0:F3:D4:AE','ff10','ff12','40'],stdout=f,stderr=subprocess.STDOUT,timeout=240)
run('quit',.1);p.wait(timeout=5)
