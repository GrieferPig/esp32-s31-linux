from pathlib import Path
import re,json,sys,collections,gzip
for dest in map(Path,sys.argv[1:]):
 source=dest/'cost.txt'
 raw=source.read_text() if source.exists() else gzip.decompress((dest/'cost.txt.gz').read_bytes()).decode()
 for line in raw.splitlines():
  assert re.fullmatch(r'CPU \d+ start=\d+ end=\d+ clock_errors=\d+ state_errors=\d+|COST \d+ \S+ \S+ ns=\d+ entries=\d+|EVENT \d+ \S+ count=\d+',line),line
 windows={}
 for cpu,start,end,clock,state in re.findall(r'CPU (\d+) start=(\d+) end=(\d+) clock_errors=(\d+) state_errors=(\d+)',raw):
  assert int(clock)==int(state)==0,(clock,state)
  windows[int(cpu)]=dict(start=int(start),end=int(end),ns=int(end)-int(start))
 assert set(windows)=={0,1}
 rows=[];totals=collections.Counter();counts=collections.Counter()
 for cpu,actor,cat,ns,entries in re.findall(r'COST (\d+) (\S+) (\S+) ns=(\d+) entries=(\d+)',raw):
  row=dict(cpu=int(cpu),actor=actor,category=cat,ns=int(ns),entries=int(entries));rows.append(row)
  totals[row['cpu']]+=row['ns'];counts[(actor,cat)]+=row['ns']
 assert len(rows)==102 and len({(x['cpu'],x['actor'],x['category']) for x in rows})==102
 for cpu in windows:assert totals[cpu]==windows[cpu]['ns'],(cpu,totals[cpu],windows[cpu])
 enabled=json.loads((dest/'command.json').read_text())['env'].get('S31_COMPAT_PROFILE')=='1'
 if enabled:assert all(29e9<w['ns']<32e9 for w in windows.values()),windows
 capacity_ns=sum(totals.values())
 event_totals=collections.Counter()
 for cpu,name,count in re.findall(r'EVENT (\d+) (\S+) count=(\d+)',raw):event_totals[name]+=int(count)
 results=dict(enabled=enabled,windows=windows,capacity_ns=capacity_ns,rows=rows,
  categories=[dict(actor=a,category=c,ns=ns,total_capacity_pct=100*ns/capacity_ns if capacity_ns else 0) for (a,c),ns in counts.most_common()],
  events=dict(event_totals),parse_errors=0)
 (dest/'cost-analysis.json').write_text(json.dumps(results,indent=2))
 print(dest,'enabled',enabled,'capacity',capacity_ns)
 for row in results['categories'][:15]:print(row)
