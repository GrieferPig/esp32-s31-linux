#!/usr/bin/env python3
"""Same-image Classic radio-worker CPU-placement control with clean Linux boots."""
import os,sys,json,subprocess,re
from pathlib import Path
from analyze_board_stats import summarize
root=Path(__file__).resolve().parent;base=Path(sys.argv[1]);base.mkdir(parents=True,exist_ok=True)
env=os.environ.copy();env.update(S31_BOARD_STATS='1',S31_SPP_FRAME_SIZE='990')
def board(dest,phase,command,timeout):
    (dest/(phase+'-command.txt')).write_text(command+'\n')
    r=subprocess.run(['python3',str(root/'board_command.py'),command,str(timeout)],capture_output=True)
    (dest/(phase+'.raw')).write_bytes(r.stdout+r.stderr)
    if r.returncode:raise RuntimeError(phase+' failed')
    return r.stdout
for index,value in enumerate(sys.argv[2:] or ['-1','0','1','-1','0']):
    cpu=int(value);count,rx,tx=10,32,16
    if cpu not in (-1,0,1):raise SystemExit('invalid worker CPU')
    dest=base/f'worker{cpu}-{index}';dest.mkdir()
    print('Starting',dest.name,flush=True)
    reset=['/home/grieferpig/.espressif/python_env/idf6.2_py3.13_env/bin/esptool','--chip','esp32s31','--port','/dev/ttyUSB0','--baud','115200','--no-stub','read-mac']
    (dest/'reset-command.json').write_text(json.dumps(reset,indent=2))
    r=subprocess.run(reset,capture_output=True)
    (dest/'reset.raw').write_bytes(r.stdout+r.stderr)
    if r.returncode:raise SystemExit('Hardware reset failed')
    subprocess.run(['python3',str(root/'native_monitor.py'),str(dest/'boot.raw'),'30'],stdout=subprocess.DEVNULL,check=True)
    r=subprocess.run(['python3',str(root/'board_console.py'),'root','3'],capture_output=True)
    (dest/'login.raw').write_bytes(r.stdout+r.stderr)
    board(dest,'configuration','mkdir -p /tmp/cfg; printf "enabled=1\\nindex=0\\nle=1\\n" > /tmp/cfg/bluetooth.conf',20)
    board(dest,'overlay','ESP32_CONFIG_DIR=/tmp/cfg S31_RADIO_VOLATILE_MODE=bt /usr/sbin/s31-overlay apply radio-bluetooth --volatile',25)
    cmd=f'timeout -s KILL 90 /usr/sbin/s31-modload /usr/lib/s31-radio/esp32s31-radio.ko.xz mode=bt direct_hci=1 bt_tx_buffers={count} bt_hci_rx_slots={rx} bt_hci_tx_slots={tx} bt_worker_cpu={cpu}; cat /sys/module/esp32s31_radio/parameters/bt_tx_buffers; echo 1 > /sys/module/esp32s31_radio/parameters/bt_gate_sleep; echo 6 > /sys/kernel/profiling; cat /sys/kernel/profiling; dmesg | grep "HCI rings"; dmesg | grep "ordered worker CPU"; dmesg | tail -30'
    output=board(dest,'bringup',cmd,110)
    if f'HCI rings rx={rx} tx={tx}'.encode() not in output:raise SystemExit('Ring depths not confirmed')
    if f'ordered worker CPU={cpu}'.encode() not in output:raise SystemExit('Worker CPU not confirmed')
    benv='S31_HCI_DEVICE=/dev/s31-hci S31_BTSTACK_PAIRABLE=1 S31_BTSTACK_COEX=0 S31_BTSTACK_BURST=24 S31_BTSTACK_SPP_BYTES=990 S31_BTSTACK_BLE_BYTES=495 S31_BTSTACK_INTERVAL=12 S31_BTSTACK_COALESCE_WAKEUPS=0 S31_BTSTACK_LOCAL_CALLBACKS=1 S31_BTSTACK_NAME="S31 Radio"'
    start=f'{benv} /usr/sbin/s31-btstack-a2dp -u 0 -l none >/tmp/buffers.log 2>&1 &'
    cmd=f's31_start() {{ {start} }}; n=0; while [ "$n" -lt 3 ]; do s31_start; sleep 8; cat /tmp/buffers.log; grep -q "up and running" /tmp/buffers.log && break; killall s31-btstack-a2dp; sleep 2; n=$((n+1)); done; grep -q "up and running" /tmp/buffers.log && taskset -p 2 $(pidof s31-btstack-a2dp)'
    output=board(dest,'start',cmd,40)
    expected=f' acl_packets={count}'.encode()
    if expected not in output:raise SystemExit('Controller did not confirm requested buffer count')
    argv=['python3',str(root/'fresh_spp_run.py'),str(dest)]
    (dest/'command.json').write_text(json.dumps({'argv':argv,'env':{k:v for k,v in env.items() if k.startswith('S31_')},'worker_cpu':cpu,'controller_tx_buffers':count,'rx_ring_slots':rx,'tx_ring_slots':tx},indent=2))
    with (dest/'wrapper.txt').open('wb') as out:
        r=subprocess.run(argv,env=env,stdout=out,stderr=subprocess.STDOUT)
    print(dest.name,'exit',r.returncode,(dest/'run.txt').read_text(),flush=True)
    summary=summarize(dest)
    print(json.dumps({**summary,'tasks':summary['tasks'][:4]}),flush=True)
    workers=[v for v in summary['tasks'] if v['name'].startswith('kworker/u')]
    if not workers:raise SystemExit('No active unbound worker in snapshots')
    worker=workers[0]
    output=board(dest,'worker-affinity',f"cat /proc/{worker['pid']}/status; cat /proc/{worker['pid']}/stat",20)
    if cpu >= 0 and not re.search(rb'^Cpus_allowed_list:\s+'+str(cpu).encode()+rb'\r?$',output,re.M):
        raise SystemExit('Active worker affinity did not match requested CPU')
    board(dest,'final-log','cat /tmp/buffers.log',20)
    if r.returncode:raise SystemExit(r.returncode)
