from pathlib import Path
import json,collections,statistics
d=Path(__file__).resolve().parent
def source_text(p):
 import gzip
 return p.read_text(errors='replace') if p.exists() else gzip.decompress(Path(str(p)+'.gz').read_bytes()).decode(errors='replace')
runs=[];groups={};paths={}
for kind in ('spp','ble'):
 for p in sorted((d/kind).glob('rr20-*')):
  c=json.loads((p/'cpu-summary.json').read_text())
  a=json.loads((p/'cost-analysis.json').read_text())
  t=c['throughput']
  health=source_text(p/'tasks.raw')
  assert 'hci_rx_dropped=0' in health and 'hci_tx_dropped=0' in health,health[-1000:]
  assert all(t.get(k,0)==0 for k in ('partial','gap_events','duplicates','bad_pattern_frames','bad_patterns','seq_errors','gaps')),t
  rows=collections.Counter()
  entries=collections.Counter()
  for x in a['rows']:
   rows[x['category']]+=x['ns']
   entries[x['category']]+=x['entries']
  runs.append(dict(protocol=kind,run=p.name,enabled=a['enabled'],cpu_pct=c['total_capacity_busy_pct'],KiBs=t['KiBs'],throughput=t,
   occupancy_busy_pct=100*(1-rows['idle']/a['capacity_ns']),categories_pct={k:100*v/a['capacity_ns'] for k,v in rows.items()},
   categories_entries=dict(entries),events=a['events'],capacity_ns=a['capacity_ns'],actor_categories=a['categories']))
 for enabled in (False,True):
  subset=[x for x in runs if x['protocol']==kind and x['enabled']==enabled]
  assert len(subset)==2
  groups[kind+'-'+str(int(enabled))]=dict(cpu_pct=statistics.mean(x['cpu_pct'] for x in subset),KiBs=statistics.mean(x['KiBs'] for x in subset),
   cpu_runs=[x['cpu_pct'] for x in subset],throughput_runs=[x['KiBs'] for x in subset])
 subset=[x for x in runs if x['protocol']==kind and x['enabled']]
 paths[kind]={category:statistics.mean(x['categories_pct'][category] for x in subset) for category in subset[0]['categories_pct']}
event_rates={}
for kind in ('spp','ble'):
 subset=[x for x in runs if x['protocol']==kind and x['enabled']]
 event_rates[kind]={k:statistics.mean(x['events'][k]/(x['capacity_ns']/2e9) for x in subset) for k in subset[0]['events']}
accepted={}
for kind in ('spp','ble'):
 subset=[]
 for run in sorted((d/('accepted-'+kind)).glob('rr20-*')):
  c=json.loads((run/'cpu-summary.json').read_text())
  t=c['throughput']
  assert all(t.get(k,0)==0 for k in ('partial','gap_events','duplicates','bad_pattern_frames','bad_patterns','gaps')),t
  subset.append(dict(cpu_pct=c['total_capacity_busy_pct'],KiBs=c['throughput']['KiBs'],throughput=c['throughput']))
 if subset:
  assert len(subset)==2
  accepted[kind]=dict(runs=subset,cpu_pct=statistics.mean(x['cpu_pct'] for x in subset),KiBs=statistics.mean(x['KiBs'] for x in subset))
result=dict(runs=runs,groups=groups,mean_categories_pct=paths,event_rates_per_second=event_rates,accepted=accepted)
(d/'summary.json').write_text(json.dumps(result,indent=2))
print(json.dumps(dict(groups=groups,mean_categories_pct=paths,event_rates_per_second=event_rates,accepted=accepted),indent=2))
