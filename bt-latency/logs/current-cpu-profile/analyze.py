from pathlib import Path
import json
b=Path(__file__).resolve().parent
rows=[]
for protocol in ('ble','spp'):
 for d in sorted((b/protocol).glob('rr20-*')):
  s=json.loads((d/'board-summary.json').read_text())
  p=json.loads((d/'kernel-profile.json').read_text())
  t=json.loads((d/'host-rx.json').read_text())
  for key in ('gaps','gap_events','duplicates','bad_patterns','bad_pattern_frames','partial'):assert t.get(key,0)==0
  assert s['counter_delta']['hci_rx_dropped']==s['counter_delta']['hci_tx_dropped']==0
  tasks=s['tasks'];total=sum(x['total_ticks'] for x in tasks)
  radio=[x for x in tasks if x['name']=='btdm' or 's31-radio' in x['name']]
  row={'protocol':protocol,'run':d.name,'host_KiBs':t['KiBs'],
       'radio_share_accounted_task_pct':100*sum(x['total_ticks'] for x in radio)/total,
       'task_cpu_seconds':[{**x,'cpu_seconds':x['total_ticks']/100} for x in tasks[:6]],
       'worker_passes':s['counter_delta']['worker_passes'],'kernel_samples':p['samples'],
       'kernel_top':p['symbols'][:20]}
  rows.append(row)
(b/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
for x in rows:print(x['protocol'],x['run'],round(x['host_KiBs'],3),round(x['radio_share_accounted_task_pct'],2),[(t['name'],t['cpu_seconds']) for t in x['task_cpu_seconds'][:3]])
