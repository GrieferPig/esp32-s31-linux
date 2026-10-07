from pathlib import Path
import json,re,statistics
b=Path(__file__).resolve().parent;rows=[]
for p in sorted(b.glob('*/*/cpu-summary.json')):
 d=p.parent;x=json.loads(p.read_text());t=x['throughput']
 for k in ('gaps','gap_events','duplicates','bad_patterns','bad_pattern_frames','partial'):assert t.get(k,0)==0,(p,k)
 health=(d/'health.raw').read_text()
 assert 'hci_rx_dropped=0 hci_tx_dropped=0' in health,p
 def counters(phase):
  s=(d/('counters-'+phase+'.raw')).read_text()
  return {k:int(v) for k,v in re.findall(r'/parameters/(bt_\w+)\s+(\d+)',s)}
 before,after=counters('before'),counters('after')
 changes={k:after[k]-v for k,v in before.items()}
 cpu=x['total_capacity_busy_pct'];rate=t['KiBs'];ticks=x['cpu_tick_deltas']['cpu']
 rows.append({'group':d.parent.name,'case':d.name,'cpu_pct':cpu,'host_KiBs':rate,'approx_CPU_ms_per_KiB':cpu*20/rate,'system_tick_seconds':ticks[2]/100,'user_tick_seconds':ticks[0]/100,'counter_deltas':changes,'source':str(p.relative_to(b))})
(b/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
for x in rows:print(x['group'],x['case'],round(x['cpu_pct'],3),round(x['host_KiBs'],3),x['counter_deltas'])
