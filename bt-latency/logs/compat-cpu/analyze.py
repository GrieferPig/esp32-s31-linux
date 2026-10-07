"""Resolve frozen all-address timer samples. Counts are NOT CPU percentages."""
from pathlib import Path
import re,json,sys,collections,bisect,struct,gzip
base=Path(__file__).resolve().parent
def read(path):
 return path.read_text(errors='replace') if path.exists() else gzip.decompress(Path(str(path)+'.gz').read_bytes()).decode(errors='replace')

def nm(path):
 out=[]
 for line in path.read_text().splitlines():
  m=re.fullmatch(r'([0-9a-f]+) ([0-9a-f]+) ([tT]) (.+)',line)
  if m:out.append((int(m[1],16),int(m[2],16),m[4]))
 return out
kernel=nm(base/'symbols/kernel.nm');payload=nm(base/'symbols/payload.nm')
module_sizes={n:z for a,z,n in nm(base/'symbols/module.nm')}
compat=set()
for p in (base/'symbols').glob('*.o.nm'):
 if p.name.startswith('esp32s31-radio-smode'):continue
 compat.update(n for a,z,n in nm(p))

def analyze(dest):
 raw=read(dest.parent/'setup/module-symbols.raw')
 m=re.search(r'esp32s31_radio \d+ \d+ .* Live 0x([0-9a-f]+)',raw)
 assert m,raw
 load=int(m[1],16)
 # Mirror kernel/module/main.c: executable ALLOC sections in ELF order,
 # init excluded; CONFIG_MODULE_UNLOAD=y retains .exit.text before .text.
 data=(base/'symbols/module.ko').read_bytes()
 assert data[:6]==b'\x7fELF\x01\x01'
 shoff=struct.unpack_from('<I',data,32)[0]
 entsize,nsec,names=struct.unpack_from('<HHH',data,46)
 sections=[struct.unpack_from('<10I',data,shoff+i*entsize) for i in range(nsec)]
 sn=sections[names]; strings=data[sn[4]:sn[4]+sn[5]]
 def cstr(b,o):return b[o:b.index(b'\0',o)].decode()
 offsets={};off=0;layout=[]
 for i,sec in enumerate(sections):
  name=cstr(strings,sec[0])
  if sec[2]&6!=6 or name.startswith('.init'):continue
  align=sec[8] or 1;off=(off+align-1)//align*align
  offsets[i]=off;layout.append(dict(name=name,offset=off,size=sec[5]))
  off+=sec[5]
 # Dynamic .plt is after real text; unresolved PLT PCs stay unresolved.
 module=[]
 for sec in sections:
  if sec[1]!=2:continue
  strsec=sections[sec[6]];strs=data[strsec[4]:strsec[4]+strsec[5]]
  for o in range(sec[4],sec[4]+sec[5],sec[9]):
   name,value,size,info,other,index=struct.unpack_from('<IIIBBH',data,o)
   if info&15==2 and size and index in offsets:
    module.append((load+offsets[index]+value,size,cstr(strs,name)))
 (dest/'module-layout.json').write_text(json.dumps(dict(base=hex(load),sections=layout),indent=2))
 symbols=sorted([(a,z,n,'kernel') for a,z,n in kernel]+[(a,z,n,'payload') for a,z,n in payload]+[(a,z,n,'module') for a,z,n in module])
 addrs=[s[0] for s in symbols]
 tasks={int(p):n for p,n in re.findall(r'(?m)^(\d+) \(([^)]+)\)',read(dest/'tasks.raw'))}
 counts=collections.Counter();taskcounts=collections.Counter();regions=collections.Counter();rows=[]
 samples=lost=0
 for line in (dest/'pc.txt').read_text().splitlines():
  if line.startswith('# cpu='):
   m=re.fullmatch(r'# cpu=(\d+) samples=(\d+) lost=(\d+)',line)
   samples+=int(m[2]);lost+=int(m[3]);continue
  if line.startswith('#'):continue
  cpu,pid,user,pc,hits=line.split();cpu,pid,user,pc,hits=int(cpu),int(pid),int(user),int(pc,16),int(hits)
  name='unresolved';region='unknown';offset=0
  if user:name='userspace';region='user'
  else:
   i=bisect.bisect_right(addrs,pc)-1
   if i>=0:
    a,z,n,rg=symbols[i]
    if a<=pc<a+z:name=n;region=rg;offset=pc-a
  task=tasks.get(pid,'pid'+str(pid))
  counts[(task,region,name)]+=hits
  taskcounts[task]+=hits;regions[region]+=hits
  rows.append(dict(cpu=cpu,pid=pid,task=task,user=user,pc=hex(pc),hits=hits,symbol=name,offset=offset,region=region))
 assert sum(r['hits'] for r in rows)+lost==samples
 categories=collections.Counter()
 for row in rows:
  if row['task']=='btdm' or 's31-radio' in row['task']:
   category=('RTOS compatibility direct PC' if row['symbol'] in compat else
             'other radio module PC' if row['region']=='module' else
             'other radio payload PC' if row['region']=='payload' else
             'generic kernel PC' if row['region']=='kernel' else 'unresolved PC')
   categories[category]+=row['hits']
 result=dict(radio_categories=dict(categories),samples=samples,lost=lost,regions=dict(regions),tasks=dict(taskcounts),hotspots=[dict(task=t,region=rg,symbol=n,hits=h) for (t,rg,n),h in counts.most_common()],rows=rows)
 (dest/'pc-analysis.json').write_text(json.dumps(result,indent=2))
 print(dest,samples,lost,dict(regions))
 for row in result['hotspots'][:20]:print(row)
for dest in map(Path,sys.argv[1:]):analyze(dest)
