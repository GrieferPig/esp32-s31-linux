#!/usr/bin/env python3
"""Normalize native task runtime and Linux /proc/stat to two-core capacity."""
import json,re,sys
from pathlib import Path
def native(dest):
 s=(dest/'cpu-uart.raw').read_text(errors='replace')
 windows=[json.loads(v) for v in re.findall(r'CPU_WINDOW (\{[^\r\n]+\})',s)]
 if len(windows)!=1:raise ValueError('Expected exactly one native CPU window')
 w=windows[0];dt=(w['end_us']-w['start_us'])&0xffffffff
 assert 29e6<dt<32e6,w
 snapshots={0:{},1:{}}
 for line in re.findall(r'CPU_TASK (\{[^\r\n]+\})',s):
  x=json.loads(line);snapshots[x['phase']][x['id']]=x
 assert len(snapshots[0])==w['before_count'] and len(snapshots[1])==w['after_count']
 tasks=[]
 for uid,x in snapshots[1].items():
  if uid not in snapshots[0]:continue
  old=snapshots[0][uid]
  assert x['name']==old['name']
  ticks=(x['runtime_us']-old['runtime_us'])&0xffffffff
  tasks.append({'name':x['name'],'runtime_us':ticks,'one_core_pct':100*ticks/dt})
 idle={int(x['name'][-1]):x for x in tasks if x['name'] in ('IDLE0','IDLE1')}
 assert set(idle)=={0,1},tasks
 cores=[100-idle[i]['one_core_pct'] for i in range(2)]
 coverage=sum(x['runtime_us'] for x in tasks)/(2*dt)*100
 assert 98<coverage<102,coverage
 return {'platform':'esp-idf','window_seconds':dt/1e6,'per_core_busy_pct':cores,'total_capacity_busy_pct':sum(cores)/2,'busy_core_equivalents':sum(cores)/100,'task_accounting_coverage_pct':coverage,'tasks':sorted(tasks,key=lambda v:-v['runtime_us']),'window':w}
def linux(dest):
 s=(dest/'cpu-idle.raw').read_text(errors='replace').replace('\r','')
 matches=list(re.finditer(r'CPU_SNAPSHOT phase=(before|after) begin_ns=(\d+) end_ns=(\d+)',s))
 assert len(matches)==2
 samples={}
 for i,m in enumerate(matches):
  block=s[m.end():matches[i+1].start() if i+1<len(matches) else len(s)]
  samples[m[1]]={'begin':int(m[2]),'end':int(m[3]),'cpu':{k:list(map(int,v.split()))[:8] for k,v in re.findall(r'^(cpu\d*) +(.+)$',block,re.M)}}
 a,b=samples['before'],samples['after']
 lo=(b['begin']-a['end'])/1e9;hi=(b['end']-a['begin'])/1e9;wall=(lo+hi)/2
 assert 29<wall<32,wall
 widths=[(x['end']-x['begin'])/1e6 for x in (a,b)]
 assert max(widths)<100,widths
 delta={k:[y-x for x,y in zip(a['cpu'][k],b['cpu'][k])] for k in a['cpu']}
 def idle(v):return (v[3]+v[4])/100
 cores=[100*(1-idle(delta['cpu'+str(i)])/wall) for i in range(2)]
 total_idle=idle(delta['cpu']);busy=100*(1-total_idle/(2*wall))
 bounds=[100*(1-(total_idle+.01)/(2*lo)),100*(1-max(0,total_idle-.01)/(2*hi))]
 return {'platform':'linux','method':'wall minus NO_HZ idle/iowait residency','window_seconds':wall,'snapshot_width_ms':widths,'per_core_busy_pct':cores,'total_capacity_busy_pct':busy,'busy_core_equivalents':busy/50,'busy_pct_measurement_bounds':bounds,'tick_accounting_coverage_pct':sum(delta['cpu'])/(wall*200)*100,'cpu_tick_deltas':delta,'tasks':[]}
if __name__=='__main__':
 platform=sys.argv[1];dest=Path(sys.argv[2])
 result=(native if platform=='native' else linux)(dest)
 throughput=json.loads((dest/'host-rx.json').read_text())
 result['throughput']={k:v for k,v in throughput.items() if k not in ('arrivals','chunks')}
 if platform=='linux':
  trigger=json.loads((dest/'stream-start.json').read_text())['first_host_packet']
  timing=json.loads((dest/'cpu-sample-host-time.json').read_text())
  result['observer_bounds_since_first_packet_s']=[timing['start']-trigger,timing['end']-trigger]
  assert timing['start']-trigger>=4.5
  assert timing['end']-trigger<throughput['duration']-1

 (dest/'cpu-summary.json').write_text(json.dumps(result,indent=2))
 print(json.dumps({**result,'tasks':result['tasks'][:6]},indent=2))
