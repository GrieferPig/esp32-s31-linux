#!/usr/bin/env python3
"""Capture a steady CPU window inside the checked 40-second transfer."""
import os,sys,time,json,threading,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[2]
platform,kind=sys.argv[1:3];dest=Path(sys.argv[3]);dest.mkdir(parents=True,exist_ok=True)
assert platform in ('linux','native') and kind in ('spp','ble')
env=os.environ.copy();env.pop('S31_BOARD_PROFILE',None);env['S31_BOARD_STATS']='0'
env['S31_STREAM_STARTED_FILE']=str(dest/'stream-start.json')
stop=threading.Event();errors=[];thread=None;monitor=None
def board(phase,cmd,timeout):
 (dest/(phase+'-command.txt')).write_text(cmd+'\n')
 t0=time.monotonic()
 x=subprocess.run(['python3',str(root/'board_command.py'),cmd,str(timeout)],capture_output=True)
 (dest/(phase+'.raw')).write_bytes(x.stdout+x.stderr)
 (dest/(phase+'-host-time.json')).write_text(json.dumps({'start':t0,'end':time.monotonic(),'returncode':x.returncode},indent=2))
 if x.returncode:raise RuntimeError(phase+' failed')
def observe_linux():
 try:
  deadline=time.monotonic()+180
  while not (dest/'stream-start.json').exists():
   if stop.wait(.05):return
   if time.monotonic()>deadline:raise RuntimeError('No first payload')
  if stop.wait(5):return
  enable=os.environ.get('S31_COMPAT_PROFILE','0')
  assert enable in ('0','1')
  cmd=f"echo 1 > /proc/s31_cost; echo {enable} > /proc/s31_cost; /tmp/cpu-idle-probe 30 > /tmp/cpu-idle-sample.txt; echo 0 > /proc/s31_cost; cat /proc/s31_cost > /tmp/s31-cost.txt"
  board('cpu-sample',cmd,45)
 except Exception as e:errors.append(str(e))
if platform=='linux':
 thread=threading.Thread(target=observe_linux);thread.start()
else:
 monitor=subprocess.Popen(['python3',str(root/'native_monitor.py'),str(dest/'cpu-uart.raw'),'130'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
argv=['python3',str(root/('fresh_spp_run.py' if kind=='spp' else 'fresh_fd_run.py')),str(dest)]
(dest/'command.json').write_text(json.dumps({'argv':argv,'platform':platform,'kind':kind,'env':{k:v for k,v in env.items() if k.startswith('S31_')}},indent=2))
with (dest/'wrapper.txt').open('wb') as f:
 x=subprocess.run(argv,env=env,stdout=f,stderr=subprocess.STDOUT)
if platform=='linux':
 stop.set();thread.join()
 if x.returncode==0 and not errors:
  board('cpu-idle','cat /tmp/cpu-idle-sample.txt',20)
  board('tasks','cat /proc/[0-9]*/stat; cat /sys/devices/platform/soc/soc:radio/radio_health',20)
  subprocess.run(['python3',str(Path(__file__).with_name('pull.py')),str(dest)],check=True)
else:
 deadline=time.monotonic()+20
 while time.monotonic()<deadline and b'CPU_DONE' not in (dest/'cpu-uart.raw').read_bytes():time.sleep(.2)
 monitor.terminate();monitor.wait(timeout=5)
 if b'CPU_DONE' not in (dest/'cpu-uart.raw').read_bytes():errors.append('Native CPU sample missing')
print((dest/'run.txt').read_text(),flush=True)
if errors:print('CPU_ERRORS',errors,flush=True)
raise SystemExit(x.returncode or bool(errors))
