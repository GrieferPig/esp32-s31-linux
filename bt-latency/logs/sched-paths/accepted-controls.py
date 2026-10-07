from pathlib import Path
import os,subprocess,json
r=Path('/home/grieferpig/esp32-s31-linux');d=Path(__file__).resolve().parent
assert (d/'restore-verify.log').read_text().count('Verification successful (digest matched).')==6
for kind in ('spp','ble'):
 env=os.environ.copy();env['S31_CPU_PROTOCOL']=kind
 argv=['python3','-u','bt-latency/cpu_linux_matrix.py',str(d/('accepted-'+kind)),'rr20','rr20']
 (d/('accepted-'+kind+'-command.json')).write_text(json.dumps(dict(argv=argv,S31_CPU_PROTOCOL=kind),indent=2))
 with (d/('accepted-'+kind+'.log')).open('wb') as f:
  x=subprocess.run(argv,cwd=r,env=env,stdout=f,stderr=subprocess.STDOUT)
 assert x.returncode==0,kind
 for run in sorted((d/('accepted-'+kind)).glob('rr20-*')):
  x=subprocess.run(['python3','bt-latency/analyze_cpu_comparison.py','linux',str(run)],cwd=r,capture_output=True)
  (run/'cpu-analysis.log').write_bytes(x.stdout+x.stderr)
  assert x.returncode==0,run
 print('accepted',kind,'two controls passed',flush=True)
