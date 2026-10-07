from pathlib import Path
import gzip,json,hashlib,subprocess
r=Path('/home/grieferpig/esp32-s31-linux');d=Path(__file__).resolve().parent
files=[];entries=[]
for p in sorted(d.rglob('*')):
 if not p.is_file() or '__pycache__' in p.parts or 'scratch' in p.parts:continue
 if p.name=='payload-rebuilt-body.bin':continue
 if p.name=='matrix-wrapper.txt' or p.suffix in ('.raw','.log','.bin','.patch') or (p.suffix in ('.json','.txt') and p.stat().st_size>131072):
  packed=Path(str(p)+'.gz');data=p.read_bytes();packed.write_bytes(gzip.compress(data,mtime=0))
  assert gzip.decompress(packed.read_bytes())==data
  entries.append(dict(path=str(p.relative_to(d)),gzip=str(packed.relative_to(d)),sha256=hashlib.sha256(data).hexdigest(),bytes=len(data)))
  files.append(packed)
 else:files.append(p)
(d/'archive-manifest.json').write_text(json.dumps(entries,indent=2))
files.append(d/'archive-manifest.json')
# A generated .gz may already be in the enumerated list; stage each once.
files=sorted(set(files));subprocess.run(['git','add','-f','--']+[str(p.relative_to(r)) for p in files],cwd=r,check=True)
print('staged',len(files),'files; verified',len(entries),'gzip round trips')
