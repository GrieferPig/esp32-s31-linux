#!/usr/bin/env python3
"""Same-image Classic controller buffer-depth control with clean Linux boots."""
import os,sys,json,subprocess
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
for index,value in enumerate(sys.argv[2:] or ['4','10','4','10']):
    count=int(value)
    if not 3<=count<=10:raise SystemExit('controller buffer range3..10')
    dest=base/f'buffers{count}-{index}';dest.mkdir()
    print('Starting',dest.name,flush=True)
    r=subprocess.run(['python3',str(root/'board_console.py'),'reboot -f','3'],capture_output=True)
    (dest/'reboot.raw').write_bytes(r.stdout+r.stderr)
    subprocess.run(['python3',str(root/'native_monitor.py'),str(dest/'boot.raw'),'30'],stdout=subprocess.DEVNULL,check=True)
    r=subprocess.run(['python3',str(root/'board_console.py'),'root','3'],capture_output=True)
    (dest/'login.raw').write_bytes(r.stdout+r.stderr)
    cmd=f'mkdir -p /tmp/cfg; printf "enabled=1\\nindex=0\\nle=1\\n" > /tmp/cfg/bluetooth.conf; export ESP32_CONFIG_DIR=/tmp/cfg S31_RADIO_VOLATILE_MODE=bt; /usr/sbin/s31-overlay apply radio-bluetooth --volatile; timeout -s KILL 90 /usr/sbin/s31-modload /usr/lib/s31-radio/esp32s31-radio.ko.xz mode=bt direct_hci=1 bt_tx_buffers={count}; cat /sys/module/esp32s31_radio/parameters/bt_tx_buffers; echo 1 > /sys/module/esp32s31_radio/parameters/bt_gate_sleep; echo 6 > /sys/kernel/profiling; cat /sys/kernel/profiling; dmesg | tail -30'
    board(dest,'bringup',cmd,110)
    benv='S31_HCI_DEVICE=/dev/s31-hci S31_BTSTACK_PAIRABLE=1 S31_BTSTACK_COEX=0 S31_BTSTACK_BURST=24 S31_BTSTACK_SPP_BYTES=990 S31_BTSTACK_BLE_BYTES=495 S31_BTSTACK_INTERVAL=12 S31_BTSTACK_COALESCE_WAKEUPS=0 S31_BTSTACK_LOCAL_CALLBACKS=0 S31_BTSTACK_NAME="S31 Radio"'
    start=f'{benv} /usr/sbin/s31-btstack-a2dp -u 0 -l none >/tmp/buffers.log 2>&1 &'
    cmd=f's31_start() {{ {start} }}; s31_start; sleep 8; cat /tmp/buffers.log; if ! grep -q "up and running" /tmp/buffers.log; then killall s31-btstack-a2dp; sleep 2; s31_start; sleep 8; cat /tmp/buffers.log; fi; grep -q "up and running" /tmp/buffers.log && taskset -p 2 $(pidof s31-btstack-a2dp)'
    output=board(dest,'start',cmd,35)
    expected=f' acl_packets={count}'.encode()
    if expected not in output:raise SystemExit('Controller did not confirm requested buffer count')
    argv=['python3',str(root/'fresh_spp_run.py'),str(dest)]
    (dest/'command.json').write_text(json.dumps({'argv':argv,'env':{k:v for k,v in env.items() if k.startswith('S31_')},'controller_tx_buffers':count},indent=2))
    with (dest/'wrapper.txt').open('wb') as out:
        r=subprocess.run(argv,env=env,stdout=out,stderr=subprocess.STDOUT)
    print(dest.name,'exit',r.returncode,(dest/'run.txt').read_text(),flush=True)
    summary=summarize(dest)
    print(json.dumps({**summary,'tasks':summary['tasks'][:4]}),flush=True)
    board(dest,'final-log','cat /tmp/buffers.log',20)
    if r.returncode:raise SystemExit(r.returncode)
