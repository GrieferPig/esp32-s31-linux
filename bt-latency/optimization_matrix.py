#!/usr/bin/env python3
"""Compare Os/O2 BTstack binaries on one image and one clean controller setup."""
import os,sys,json,subprocess,time,re
from pathlib import Path
from analyze_board_stats import summarize
root=Path(__file__).resolve().parent
base=Path(sys.argv[1]);base.mkdir(parents=True,exist_ok=True)
worker_cpu=int(os.environ.get('S31_RADIO_WORKER_CPU','-1'))
if worker_cpu not in (-1,0,1):raise SystemExit('Invalid worker CPU')
env=os.environ.copy();env.update(S31_BOARD_STATS='1',S31_SPP_FRAME_SIZE='990')
def board(dest,phase,cmd,timeout=30):
    (dest/(phase+'-command.txt')).write_text(cmd+'\n')
    x=subprocess.run(['python3',str(root/'board_command.py'),cmd,str(timeout)],capture_output=True)
    (dest/(phase+'.raw')).write_bytes(x.stdout+x.stderr)
    if x.returncode:raise RuntimeError(phase+' failed')
    return x.stdout
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
board(setup,'binary-hashes','sha256sum /usr/sbin/s31-btstack-a2dp /usr/sbin/s31-btstack-a2dp-o2')
for index,mode in enumerate(sys.argv[2:] or ['Os','O2','Os','O2']):
    if mode not in ('Os','O2'):raise SystemExit('Unknown optimization')
    dest=base/f'{mode}-{index}';dest.mkdir()
    binary='s31-btstack-a2dp'+('-o2' if mode=='O2' else '')
    benv='S31_HCI_DEVICE=/dev/s31-hci S31_BTSTACK_PAIRABLE=1 S31_BTSTACK_COEX=0 S31_BTSTACK_BURST=24 S31_BTSTACK_SPP_BYTES=990 S31_BTSTACK_BLE_BYTES=495 S31_BTSTACK_INTERVAL=12 S31_BTSTACK_COALESCE_WAKEUPS=0 S31_BTSTACK_LOCAL_CALLBACKS=1 S31_BTSTACK_NAME="S31 Radio"'
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
    argv=['python3',str(root/'fresh_spp_run.py'),str(dest)]
    (dest/'command.json').write_text(json.dumps({'argv':argv,'optimization':mode,'binary':binary,'env':{k:v for k,v in env.items() if k.startswith('S31_')}},indent=2))
    with (dest/'wrapper.txt').open('wb') as f:
        x=subprocess.run(argv,env=env,stdout=f,stderr=subprocess.STDOUT)
    print(dest.name,'exit',x.returncode,(dest/'run.txt').read_text(),flush=True)
    stats=summarize(dest);print(json.dumps({**stats,'tasks':stats['tasks'][:4]}),flush=True)
    worker=next(v for v in stats['tasks'] if v['name'].startswith('kworker/u'))
    out=board(dest,'worker-affinity',f"cat /proc/{worker['pid']}/status; cat /proc/{worker['pid']}/stat")
    if worker_cpu>=0 and not re.search(rb'^Cpus_allowed_list:\s+'+str(worker_cpu).encode()+rb'\r?$',out,re.M):
        raise SystemExit('Worker mask not confirmed')
    board(dest,'final-log','cat /tmp/opt.log')
    if x.returncode:raise SystemExit(x.returncode)
