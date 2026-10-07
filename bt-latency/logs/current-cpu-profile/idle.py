from pathlib import Path
import os,sys,subprocess,json
r=Path('/home/grieferpig/esp32-s31-linux');sys.path.insert(0,str(r/'bt-latency'))
from board_bench_stats import snapshot
from analyze_board_stats import summarize
from analyze_cpu_comparison import linux
d=Path(sys.argv[1]);d.mkdir()
subprocess.run(['python3',str(r/'bt-latency/cpu_upload_probe.py'),str(r/'bt-latency/logs/cpu-usage/cpu_idle_probe'),str(d/'upload')],check=True)
os.environ.update(S31_BOARD_STATS='1',S31_BOARD_PROFILE='1')
snapshot(d,'before')
cmd='/tmp/cpu-idle-probe 30 > /tmp/cpu-idle-sample.txt'
(d/'sample-command.txt').write_text(cmd+'\n')
x=subprocess.run(['python3',str(r/'bt-latency/board_command.py'),cmd,'45'],capture_output=True,check=True)
(d/'sample.raw').write_bytes(x.stdout+x.stderr)
snapshot(d,'after')
x=subprocess.run(['python3',str(r/'bt-latency/board_command.py'),'cat /tmp/cpu-idle-sample.txt','15'],capture_output=True,check=True)
(d/'cpu-idle.raw').write_bytes(x.stdout+x.stderr)
s=summarize(d);v=linux(d);(d/'idle-summary.json').write_text(json.dumps(v,indent=2))
print(v['total_capacity_busy_pct'],s['tasks'][:4],flush=True)
subprocess.run(['python3',str(r/'bt-latency/pull_cpu_profile.py'),str(d)],check=True)
