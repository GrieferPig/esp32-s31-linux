#!/usr/bin/env python3
"""Decode a Linux /proc/profile sample against its exact build System.map."""
import sys,struct,bisect,json,gzip
from pathlib import Path
def read(path):
    p=Path(path)
    if not p.exists() and Path(str(p)+'.gz').exists():p=Path(str(p)+'.gz')
    return gzip.decompress(p.read_bytes()) if p.suffix=='.gz' else p.read_bytes()
raw=read(sys.argv[1])
words=struct.unpack('<'+'I'*(len(raw)//4),raw)
step=words[0]
symbols=[]
for line in read(sys.argv[2]).decode().splitlines():
    addr,kind,name=line.split()[:3]
    if kind.lower() in ('t','w'):
        symbols.append((int(addr,16),name))
symbols.sort()
base=next(a for a,n in symbols if n=='_stext')
end=next(a for a,n in symbols if n=='_etext')
assert len(words)==1+(end-base)//step,(len(words),base,end,step)
addresses=[a for a,n in symbols];counts={};bins=[]
for i,hits in enumerate(words[1:]):
    if not hits:continue
    pc=base+i*step
    first=bisect.bisect_right(addresses,pc)-1
    last=bisect.bisect_left(addresses,pc+step)
    names=list(dict.fromkeys(n for a,n in symbols[first:last]))
    name=names[0] if len(names)==1 else '[ambiguous '+hex(pc)+': '+' / '.join(names)+']'
    counts[name]=counts.get(name,0)+hits
    bins.append({'pc':hex(pc),'hits':hits,'candidate_symbols':names})
report={'step_bytes':step,'samples':sum(counts.values()),
        'limitation':'Built-in kernel text only; excludes userspace, loadable modules, and low-address radio payload. Bins crossing symbol boundaries list every candidate; IRQ-off work can be charged at re-enable.',
        'symbols':sorted(counts.items(),key=lambda x:-x[1]),
        'bins':sorted(bins,key=lambda x:-x['hits'])}
print(json.dumps(report,indent=2))
