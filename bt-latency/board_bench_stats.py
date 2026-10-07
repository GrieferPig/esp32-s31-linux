"""Optional Linux board snapshots outside the timed receive window."""
import os,subprocess,time,json
from pathlib import Path

def snapshot(dest,phase):
    if os.environ.get('S31_BOARD_STATS') != '1':
        return
    command='echo CPU_STAT; cat /proc/stat; echo MEMORY; cat /proc/meminfo; echo RADIO_HEALTH; cat /sys/devices/platform/soc/soc:radio/radio_health; echo PROCESS_STAT; cat /proc/[0-9]*/stat'
    start=time.monotonic()
    result=subprocess.run(['python3',str(Path(__file__).with_name('board_command.py')),command,'12'],capture_output=True)
    end=time.monotonic()
    (dest/(phase+'-board.raw')).write_bytes(result.stdout+result.stderr)
    (dest/(phase+'-board-time.json')).write_text(json.dumps({'start_monotonic':start,'end_monotonic':end,'returncode':result.returncode},indent=2))
    if result.returncode:
        raise RuntimeError('Board snapshot failed: '+phase)
