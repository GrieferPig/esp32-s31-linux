#!/usr/bin/env python3
"""Measure steady Linux CPU usage during checked BLE or SPP transfers."""
import os,sys,json,subprocess,time,re
from pathlib import Path
root=Path(__file__).resolve().parents[2]
base=Path(sys.argv[1]);base.mkdir(parents=True,exist_ok=True)
worker_cpu=int(os.environ.get('S31_RADIO_WORKER_CPU','-1'))
if worker_cpu not in (-1,0,1):raise SystemExit('Invalid worker CPU')
kind=os.environ.get('S31_CPU_PROTOCOL','spp')
assert kind in ('spp','ble')
env=os.environ.copy();env.update(S31_BOARD_STATS='1',S31_SPP_FRAME_SIZE='990')
def board(dest,phase,cmd,timeout=30):
    (dest/(phase+'-command.txt')).write_text(cmd+'\n')
    x=subprocess.run(['python3',str(root/'board_command.py'),cmd,str(timeout)],capture_output=True)
    (dest/(phase+'.raw')).write_bytes(x.stdout+x.stderr)
    if x.returncode:raise RuntimeError(phase+' failed')
    return x.stdout
if os.environ.get('S31_RESUME_FROM') is None:
    setup=base/'setup';setup.mkdir()
    reset=['/home/grieferpig/.espressif/python_env/idf6.2_py3.13_env/bin/esptool','--chip','esp32s31','--port','/dev/ttyUSB0','--baud','115200','--no-stub','read-mac']
    (setup/'reset-command.json').write_text(json.dumps(reset,indent=2))
    x=subprocess.run(reset,capture_output=True);(setup/'reset.raw').write_bytes(x.stdout+x.stderr)
    if x.returncode:raise SystemExit('Hardware reset failed')
    subprocess.run(['python3',str(root/'native_monitor.py'),str(setup/'boot.raw'),'30'],stdout=subprocess.DEVNULL,check=True)
    x=subprocess.run(['python3',str(root/'board_console.py'),'root','3'],capture_output=True)
    (setup/'login.raw').write_bytes(x.stdout+x.stderr)
    shell_cmd='stty -echo; exec /bin/sh +i'
    (setup/'shell-command.txt').write_text(shell_cmd+'\n')
    x=subprocess.run(['python3',str(root/'board_console.py'),shell_cmd,'2'],capture_output=True,check=True)
    (setup/'shell.raw').write_bytes(x.stdout+x.stderr)
    board(setup,'configuration','mkdir -p /tmp/cfg; printf "enabled=1\\nindex=0\\nle=1\\n" > /tmp/cfg/bluetooth.conf')
    board(setup,'overlay','ESP32_CONFIG_DIR=/tmp/cfg S31_RADIO_VOLATILE_MODE=bt /usr/sbin/s31-overlay apply radio-bluetooth --volatile')
    out=board(setup,'bringup',f'timeout -s KILL 90 /usr/sbin/s31-modload /usr/lib/s31-radio/esp32s31-radio.ko.xz mode=bt direct_hci=1 bt_tx_buffers=10 bt_hci_rx_slots=32 bt_hci_tx_slots=16 bt_worker_cpu={worker_cpu}; echo 1 > /sys/module/esp32s31_radio/parameters/bt_gate_sleep; echo 40 > /sys/module/esp32s31_radio/parameters/bt_tick_ms; echo 6 > /sys/kernel/profiling; dmesg | grep "HCI rings"; dmesg | grep "ordered worker CPU"',110)
    if b'HCI rings rx=32 tx=16' not in out or f'ordered worker CPU={worker_cpu}'.encode() not in out:raise SystemExit('Configuration not confirmed')
    board(setup,'module-symbols',"cat /proc/modules",20)
    out=board(setup,'profiler-ready','echo 0 > /proc/s31_pc_profile; cat /proc/s31_pc_profile',20)
    if b'# cpu=0 samples=' not in out:raise RuntimeError('Diagnostic profiler unavailable')
    board(setup,'binary-hashes','sha256sum /usr/sbin/s31-btstack-a2dp /usr/sbin/s31-btstack-a2dp-o2')
    subprocess.run(['python3',str(root/'cpu_upload_probe.py'),str(root/'logs/cpu-usage/cpu_idle_probe'),str(setup/'probe-upload')],check=True)
for index,mode in enumerate(sys.argv[2:] or ['normal','nice10','rr20','normal']):
    if index<int(os.environ.get('S31_RESUME_FROM','0')):continue
    if mode not in ('normal','nice10','rr20','rr90'):raise SystemExit('Unknown priority')
    dest=base/f'{mode}-{index}';dest.mkdir()
    binary='s31-btstack-a2dp'
    benv=f'S31_BTSTACK_ADVERTISE={1 if kind=='ble' else 0} S31_HCI_DEVICE=/dev/s31-hci S31_BTSTACK_PAIRABLE={0 if kind=='ble' else 1} S31_BTSTACK_COEX=0 S31_BTSTACK_BURST=24 S31_BTSTACK_SPP_BYTES=990 S31_BTSTACK_BLE_BYTES=495 S31_BTSTACK_INTERVAL=12 S31_BTSTACK_COALESCE_WAKEUPS=0 S31_BTSTACK_LOCAL_CALLBACKS=1 S31_BTSTACK_NAME="S31 Radio"'
    board(dest,'stop','killall s31-btstack-a2dp s31-btstack-a2dp-o2 2>/dev/null; sleep 1')
    out=b''
    for attempt in range(3):
        if attempt:
            board(dest,f'stop{attempt}',f'killall {binary} 2>/dev/null; sleep 1')
        prefix='taskset 2 ' if attempt==0 else ''
        cmd=f'{benv} {prefix}/usr/sbin/{binary} -u 0 -l none >/tmp/opt.log 2>&1 & echo $! >/tmp/opt.pid'
        board(dest,f'launch{attempt}',cmd,20)
        time.sleep(8)
        out=board(dest,f'init{attempt}','cat /tmp/opt.log',20)
        if b'up and running' in out:break
    else:raise SystemExit('BTstack did not initialize after3 attempts')
    board(dest,'affinity','taskset -p 2 $(cat /tmp/opt.pid)')
    if b'acl_packets=10' not in out or b'S31 local callbacks: enabled' not in out:raise SystemExit('BTstack configuration not confirmed')
    if f'S31 BLE advertising complete: requested={1 if kind=='ble' else 0} status=0'.encode() not in out:raise SystemExit('Advertising command not confirmed')
    priority_cmd = {'normal':'chrt -o -p 0', 'nice10':'renice -n -10 -p', 'rr20':'chrt -r -p 20', 'rr90':'chrt -r -p 90'}[mode]
    out=board(dest,'priority',priority_cmd+' $(cat /tmp/opt.pid); chrt -p $(cat /tmp/opt.pid); cat /proc/$(cat /tmp/opt.pid)/stat')
    lines=out.decode(errors='replace').splitlines()
    stat=next(line for line in lines if re.match(r'^\d+ \(',line))
    fields=stat[stat.rindex(')')+2:].split()
    nice=int(fields[16]); rtprio=int(fields[37]); policy=int(fields[38])
    expected={'normal':(0,0,0),'nice10':(-10,0,0),'rr20':(0,20,2),'rr90':(0,90,2)}[mode]
    if (nice,rtprio,policy)!=expected:raise SystemExit('Priority not confirmed: '+repr((nice,rtprio,policy)))
    env['S31_COMPAT_PROFILE']=('0','1','1','0')[index]
    argv=['python3',str(Path(__file__).with_name('measure.py')),'linux',kind,str(dest)]
    (dest/'command.json').write_text(json.dumps({'argv':argv,'advertising':1 if kind=='ble' else 0,'priority':mode,'binary':binary,'env':{k:v for k,v in env.items() if k.startswith('S31_')}},indent=2))
    with (dest/'matrix-wrapper.txt').open('wb') as f:
        x=subprocess.run(argv,env=env,stdout=f,stderr=subprocess.STDOUT)
    print(dest.name,'exit',x.returncode,(dest/'run.txt').read_text(),flush=True)
    board(dest,'final-log','cat /tmp/opt.log')
    if x.returncode:raise SystemExit(x.returncode)
