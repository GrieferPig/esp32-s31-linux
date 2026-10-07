from pathlib import Path
import os,subprocess,json
r=Path('/home/grieferpig/esp32-s31-linux');b=r/'bt-latency/logs/native-btdm'
py='/home/grieferpig/.espressif/python_env/idf6.2_py3.13_env/bin/python'
env=os.environ.copy()
for key in ('S31_BOARD_STATS','S31_BOARD_PROFILE','S31_SPP_PACKET_MASK'):env.pop(key,None)
env.update(S31_BT_ADDR='30:ED:A0:F3:D4:AD',S31_SPP_START_CONTROL='1',S31_SPP_FRAME_SIZE='990')
for i in range(2):
 d=b/f'run{i}';d.mkdir()
 argv=['python3',str(r/'bt-latency/fresh_spp_run.py'),str(d)]
 (d/'command.json').write_text(json.dumps({'argv':argv,'env':{k:v for k,v in env.items() if k.startswith('S31_')}},indent=2))
 with (d/'wrapper.txt').open('wb') as f:x=subprocess.run(argv,env=env,stdout=f,stderr=subprocess.STDOUT)
 print(d.name,x.returncode,(d/'run.txt').read_text(),flush=True)
 if x.returncode:raise SystemExit(x.returncode)
