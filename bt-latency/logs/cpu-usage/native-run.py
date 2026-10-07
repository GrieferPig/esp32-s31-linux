from pathlib import Path
import sys,subprocess,os,json
r=Path('/home/grieferpig/esp32-s31-linux');base=r/'bt-latency/logs/cpu-usage';kind=sys.argv[1]
assert kind in ('spp','ble')
b=base/('native-'+kind)
py='/home/grieferpig/.espressif/python_env/idf6.2_py3.13_env/bin/python'
argv=[py,str(r/'bt-latency/native-idf/flash_native.py'),kind,'--build-dir',str(b/'project/build'),'--log-dir',str(b),'--execute']
(b/'flash-command.json').write_text(json.dumps(argv,indent=2))
with (b/'flash.txt').open('wb') as f:subprocess.run(argv,stdout=f,stderr=subprocess.STDOUT,check=True)
assert (b/'flash.txt').read_text().count('Verification successful (digest matched).')==4
subprocess.run(['python3',str(r/'bt-latency/native_monitor.py'),str(b/'boot.raw'),'20'],stdout=subprocess.DEVNULL,check=True)
env=os.environ.copy()
for key in ('S31_BOARD_STATS','S31_BOARD_PROFILE','S31_SPP_PACKET_MASK','S31_BLE_INTERVAL','S31_BLE_VALUE_LEN','S31_SPP_START_CONTROL'):env.pop(key,None)
env.update(S31_BT_ADDR='30:ED:A0:F3:D4:AD')
if kind=='spp':env.update(S31_SPP_FRAME_SIZE='990',S31_SPP_START_CONTROL='1')
else:env.update(S31_BLE_INTERVAL='12',S31_BLE_VALUE_LEN='495')
for i in range(2):
 d=b/f'run{i}'
 argv=['python3',str(r/'bt-latency/cpu_measure_run.py'),'native',kind,str(d)]
 x=subprocess.run(argv,env=env)
 if x.returncode:raise SystemExit(x.returncode)
 subprocess.run(['python3',str(r/'bt-latency/analyze_cpu_comparison.py'),'native',str(d)],check=True)
