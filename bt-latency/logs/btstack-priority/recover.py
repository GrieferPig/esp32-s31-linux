from pathlib import Path
import subprocess,os,json,sys
r=Path('/home/grieferpig/esp32-s31-linux');b=r/'bt-latency/logs/btstack-priority/recovery'
b.mkdir()
sys.path.insert(0,str(r/'bt-latency'))
from analyze_board_stats import summarize
env=os.environ.copy();env.update(S31_BOARD_STATS='1',S31_SPP_FRAME_SIZE='990')
for index,policy in enumerate(['rr20','normal','rr20']):
 d=b/f'{policy}-{index}';d.mkdir()
 change='chrt -r -p 20' if policy=='rr20' else 'chrt -o -p 0'
 cmd=change+' $(cat /tmp/opt.pid); chrt -p $(cat /tmp/opt.pid); cat /proc/$(cat /tmp/opt.pid)/stat'
 (d/'priority-command.txt').write_text(cmd+'\n')
 x=subprocess.run(['python3',str(r/'bt-latency/board_command.py'),cmd,'20'],capture_output=True)
 (d/'priority.raw').write_bytes(x.stdout+x.stderr)
 if x.returncode:raise SystemExit(x.returncode)
 expected=b'SCHED_RR' if policy=='rr20' else b'SCHED_OTHER'
 if expected not in x.stdout:raise SystemExit('policy missing')
 argv=['python3',str(r/'bt-latency/fresh_spp_run.py'),str(d)]
 (d/'command.json').write_text(json.dumps({'argv':argv,'env':{k:v for k,v in env.items() if k.startswith('S31_')}},indent=2))
 with (d/'wrapper.txt').open('wb') as f:x=subprocess.run(argv,env=env,stdout=f,stderr=subprocess.STDOUT)
 print(d.name,x.returncode,(d/'run.txt').read_text(),flush=True)
 print(json.dumps(summarize(d)),flush=True)
 if x.returncode:raise SystemExit(x.returncode)
