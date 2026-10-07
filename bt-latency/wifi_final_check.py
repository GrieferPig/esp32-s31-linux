#!/usr/bin/env python3
"""Volatile combo-mode Wi-Fi scan check; never writes saved configuration."""
import subprocess,sys,json,re
from pathlib import Path
root=Path(__file__).resolve().parent
base=Path(sys.argv[1]);base.mkdir(parents=True)
def board(phase,cmd,timeout=30):
 (base/(phase+'-command.txt')).write_text(cmd+'\n')
 x=subprocess.run(['python3',str(root/'board_command.py'),cmd,str(timeout)],capture_output=True)
 (base/(phase+'.raw')).write_bytes(x.stdout+x.stderr)
 if x.returncode:raise RuntimeError(phase+' failed')
 return x.stdout
reset=['/home/grieferpig/.espressif/python_env/idf6.2_py3.13_env/bin/esptool','--chip','esp32s31','--port','/dev/ttyUSB0','--baud','115200','--no-stub','read-mac']
(base/'reset-command.json').write_text(json.dumps(reset,indent=2))
x=subprocess.run(reset,capture_output=True);(base/'reset.raw').write_bytes(x.stdout+x.stderr)
if x.returncode:raise SystemExit(x.returncode)
subprocess.run(['python3',str(root/'native_monitor.py'),str(base/'boot.raw'),'30'],check=True,stdout=subprocess.DEVNULL)
for phase,cmd,seconds in [('login','root','3'),('shell','stty -echo; exec /bin/sh +i','2')]:
 (base/(phase+'-command.txt')).write_text(cmd+'\n')
 x=subprocess.run(['python3',str(root/'board_console.py'),cmd,seconds],capture_output=True,check=True)
 (base/(phase+'.raw')).write_bytes(x.stdout+x.stderr)
board('configuration','mkdir -p /tmp/cfg; printf "enabled=1\\n" > /tmp/cfg/wifi.conf; printf "enabled=1\\nindex=0\\nle=1\\n" > /tmp/cfg/bluetooth.conf')
board('overlay','ESP32_CONFIG_DIR=/tmp/cfg S31_RADIO_VOLATILE_MODE=combo /usr/sbin/s31-overlay apply radio-combo --volatile')
board('bringup','timeout -s KILL 90 /usr/sbin/s31-modload /usr/lib/s31-radio/esp32s31-radio.ko.xz mode=combo direct_hci=1',110)
out=board('frontends','cat /sys/module/esp32s31_radio/parameters/mode; ls -l /dev/s31-hci; ip link show wlan0; cat /sys/devices/platform/soc/soc:radio/radio_health; dmesg')
if not re.search(rb'^combo\r?$',out,re.M) or b'wlan0' not in out or b'/dev/s31-hci' not in out:raise SystemExit('Combo frontends not confirmed')
board('up','ip link set wlan0 up')
out=board('scan','iw dev wlan0 scan',60)
count=len(re.findall(rb'^BSS ',out,re.M))
if count<1:raise SystemExit('No AP observed; raw evidence retained')
board('health','cat /sys/devices/platform/soc/soc:radio/radio_health; ip link show wlan0; dmesg')
board('down','ip link set wlan0 down')
(base/'result.json').write_text(json.dumps({'mode':'combo','observed_bss':count,'scan_pass':True,'association':'not tested; no authorized AP','config':'volatile /tmp/cfg'},indent=2))
print('Combo Wi-Fi scan passed; observed BSS:',count,flush=True)
