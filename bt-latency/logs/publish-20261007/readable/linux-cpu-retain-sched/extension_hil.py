from pathlib import Path
import subprocess,json,re
r=Path('/home/grieferpig/esp32-s31-linux');d=Path(__file__).resolve().parent
dest=d/'extension-hil';dest.mkdir()
def board(where,phase,cmd,timeout=30):
 (where/(phase+'-command.txt')).write_text(cmd+'\n')
 x=subprocess.run(['python3',str(r/'bt-latency/board_command.py'),cmd,str(timeout)],capture_output=True)
 (where/(phase+'.raw')).write_bytes(x.stdout+x.stderr);assert x.returncode==0
 return x.stdout
reset=['/home/grieferpig/.espressif/python_env/idf6.2_py3.13_env/bin/esptool','--chip','esp32s31','--port','/dev/ttyUSB0','--baud','115200','--no-stub','read-mac']
(dest/'reset-command.json').write_text(json.dumps(reset,indent=2))
x=subprocess.run(reset,capture_output=True);(dest/'reset.raw').write_bytes(x.stdout+x.stderr);assert x.returncode==0
subprocess.run(['python3',str(r/'bt-latency/native_monitor.py'),str(dest/'boot.raw'),'30'],stdout=subprocess.DEVNULL,check=True)
x=subprocess.run(['python3',str(r/'bt-latency/board_console.py'),'root','3'],capture_output=True);(dest/'login.raw').write_bytes(x.stdout+x.stderr)
quiet='stty -echo; exec /bin/sh +i'
(dest/'quiet-command.txt').write_text(quiet+'\n')
x=subprocess.run(['python3',str(r/'bt-latency/board_console.py'),quiet,'2'],capture_output=True)
(dest/'quiet.raw').write_bytes(x.stdout+x.stderr);assert x.returncode==0
board(dest,'knob-check','ls /sys/module/esp32s31_ext/parameters; cat /sys/module/esp32s31_ext/parameters/lazy_user')
outputs=[]
for lazy in (0,1):
 out=dest/('lazy' if lazy else 'eager');out.mkdir()
 board(out,'run',f'echo {lazy} > /sys/module/esp32s31_ext/parameters/lazy_user; echo 1 > /sys/module/esp32s31_ext/parameters/diagnostics; timeout -s KILL 180 /usr/sbin/s31-ext-test > /tmp/ext-test.txt 2>&1; rc=$?; echo 0 > /sys/module/esp32s31_ext/parameters/diagnostics; echo TEST_RC=$rc; cat /sys/module/esp32s31_ext/parameters/stats',200)
 subprocess.run(['python3',str(d/'pull_ext_test.py'),str(out)],check=True)
 text=(out/'ext-test.txt').read_text();outputs.append(text)
 assert 'S31 FPU/HWLoop/PIE context tests: PASS' in text or 'S31 FPU/HWLoop/PIE instruction and context tests: PASS' in text,text[-1500:]
 print('lazy',lazy,'context PASS',flush=True)
# Context and supported instruction assertions must be the same, not just the final marker.
assert outputs[0]==outputs[1], 'Eager/lazy instruction results differ'
board(dest,'disable','echo 0 > /sys/module/esp32s31_ext/parameters/lazy_user; echo 0 > /sys/module/esp32s31_ext/parameters/diagnostics')
print('Eager/lazy real instruction + fork/yield/signal results identical',flush=True)
