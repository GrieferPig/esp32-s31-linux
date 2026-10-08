from pathlib import Path
import os,subprocess,json
r=Path('/home/grieferpig/esp32-s31-linux');d=Path(__file__).resolve().parent
for kind in ('spp','ble'):
 env=os.environ.copy();env['S31_CPU_PROTOCOL']=kind
 argv=['python3','-u',str(d/'matrix.py'),str(d/('final-'+kind)),'baseline','lazy','all','all','lazy','baseline']
 (d/('final-'+kind+'-command.json')).write_text(json.dumps({'argv':argv,'S31_CPU_PROTOCOL':kind,'conditions':['baseline','lazy','all','all','lazy','baseline']},indent=2))
 with (d/('final-'+kind+'.log')).open('wb') as f:
  x=subprocess.run(argv,cwd=r,env=env,stdout=f,stderr=subprocess.STDOUT)
 assert x.returncode==0,kind
 for run in sorted((d/('final-'+kind)).glob('*-[0-9]')):
  x=subprocess.run(['python3',str(d/'analyze_ext.py'),str(run)],capture_output=True)
  (run/'extension-analysis.log').write_bytes(x.stdout+x.stderr);assert x.returncode==0
 print(kind,'six checked real runs completed',flush=True)
