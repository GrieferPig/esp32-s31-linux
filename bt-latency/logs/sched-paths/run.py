from pathlib import Path
import os,subprocess,json
r=Path('/home/grieferpig/esp32-s31-linux');d=Path(__file__).resolve().parent
for kind in ('spp','ble'):
 env=os.environ.copy();env['S31_CPU_PROTOCOL']=kind
 if kind=='ble':env.pop('S31_RESUME_FROM',None)
 argv=['python3','-u',str(d/'matrix.py'),str(d/kind),'rr20','rr20','rr20','rr20']
 (d/(kind+'-command.json')).write_text(json.dumps({'argv':argv,'S31_CPU_PROTOCOL':kind,'profile_order':[0,1,1,0]},indent=2))
 with (d/(kind+'-matrix.log')).open('wb') as f:
  x=subprocess.run(argv,cwd=r,env=env,stdout=f,stderr=subprocess.STDOUT)
 assert x.returncode==0,kind
 for run in sorted((d/kind).glob('rr20-*')):
  x=subprocess.run(['python3',str(r/'bt-latency/analyze_cpu_comparison.py'),'linux',str(run)],capture_output=True)
  (run/'cpu-analysis.log').write_bytes(x.stdout+x.stderr)
  assert x.returncode==0,str(run)
  x=subprocess.run(['python3',str(d/'analyze.py'),str(run)],capture_output=True)
  (run/'cost-analysis.log').write_bytes(x.stdout+x.stderr)
  assert x.returncode==0,str(run)
 print(kind,'completed four runs',flush=True)
