from pathlib import Path
import json,gzip,hashlib
b=Path(__file__).resolve().parent;selected=[];hashes=[]
for p in sorted(b.rglob('*')):
 if not p.is_file() or '__pycache__' in p.parts or p.suffix=='.gz' or p.name in ('evidence-files.json','raw-sha256.json'):continue
 data=p.read_bytes();rel=str(p.relative_to(b))
 if p.suffix in ('.raw','.bin','.profile') or len(data)>50000 or b'\r' in data or data.endswith(b'\n\n') or any(l.endswith((b' ',b'\t')) for l in data.splitlines()):
  q=p.with_name(p.name+'.gz');q.write_bytes(gzip.compress(data,mtime=0));assert gzip.decompress(q.read_bytes())==data
  selected.append(str(q.relative_to(b)));hashes.append({'file':rel,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()})
 else:selected.append(rel)
(b/'raw-sha256.json').write_text(json.dumps(hashes,indent=2)+'\n')
selected+=['raw-sha256.json','evidence-files.json']
(b/'evidence-files.json').write_text(json.dumps(selected,indent=2)+'\n')
print(len(selected),'files')
