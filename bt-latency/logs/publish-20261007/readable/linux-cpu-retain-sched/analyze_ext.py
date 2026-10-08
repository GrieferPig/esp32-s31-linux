import re,json,sys
from pathlib import Path
p=Path(sys.argv[1]);text=(p/'cpu-idle.raw').read_text(errors='replace').replace('\r','')
window=re.search(r'EXT_WINDOW begin_ns=(\d+) end_ns=(\d+)',text);assert window
seconds=(int(window[2])-int(window[1]))/1e9;assert 29<seconds<32
def fields(phase):
 m=re.search(r'EXT_'+phase+r' ([^\n]+)',text);assert m
 return {key:int(v) for key,v in re.findall(r'(\w+)=(\d+)',m[1])}
a,b=fields('BEFORE'),fields('AFTER')
delta={key:b[key]-a[key] for key in a}
assert all(v>=0 for v in delta.values()),delta
freq=320000000 # built DT timebase; retain DT source and actual boot check
n=delta['bt_syscall_suspend'];m=delta['bt_syscall_restore']
cal=delta['timer_pair_ticks']/n if n else None
save=delta['bt_syscall_suspend_ticks']/freq
restore=delta['bt_syscall_restore_ticks']/freq
result={'extension_window_seconds':seconds,'timebase_hz':freq,'delta':delta,
 'bt_syscall_extension_seconds':save+restore,
 'total_capacity_pct':100*(save+restore)/(seconds*2),
 'timer_pair_ticks_per_suspend':cal,
 'timer_pair_us':cal/freq*1e6 if cal else None,
 'timer_corrected_estimate_seconds':max(0,save+restore-(n+m)*(cal or 0)/freq),
 'timer_corrected_estimate_capacity_pct':100*max(0,save+restore-(n+m)*(cal or 0)/freq)/(seconds*2),
 'suspend_boundary_us':save/n*1e6 if n else None,
 'restore_boundary_us':restore/m*1e6 if m else None,
 'syscall_suspend_rate_s':n/seconds,
 'syscall_restore_rate_s':m/seconds,
 'scope':'BTstack syscall extension suspend/restore functions, including timer read and counter instrumentation; excludes IRQ transitions and other users'}
(p/'extension-summary.json').write_text(json.dumps(result,indent=2))
print(json.dumps(result,indent=2))
