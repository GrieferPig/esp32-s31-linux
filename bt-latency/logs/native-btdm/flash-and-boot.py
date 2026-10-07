from pathlib import Path
import os,subprocess,json
r=Path('/home/grieferpig/esp32-s31-linux');b=r/'bt-latency/logs/native-btdm'
py='/home/grieferpig/.espressif/python_env/idf6.2_py3.13_env/bin/python'
argv=[py,str(r/'bt-latency/native-idf/flash_native.py'),'spp','--build-dir',str(b/'project/build'),'--log-dir',str(b),'--execute']
(b/'flash-command.json').write_text(json.dumps(argv,indent=2))
with (b/'flash.txt').open('wb') as f:subprocess.run(argv,stdout=f,stderr=subprocess.STDOUT,check=True)
subprocess.run(['python3',str(r/'bt-latency/native_monitor.py'),str(b/'boot.raw'),'20'],check=True,stdout=subprocess.DEVNULL)
