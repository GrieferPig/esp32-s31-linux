from pathlib import Path
import gzip,hashlib,json
b=Path(__file__).resolve().parent;files=[];raw=[]
for p in sorted(b.rglob('*')):
 rel=p.relative_to(b)
 if not p.is_file() or any(x=='__pycache__' or x.startswith('scratch') for x in rel.parts[:-1]) or p.suffix=='.gz' or p.name in ('evidence-files.json','raw-sha256.json'):continue
 data=p.read_bytes()
 if p.suffix in ('.raw','.bin') or len(data)>50000 or b'\r' in data or data.endswith(b'\n\n') or any(l.endswith((b' ',b'\t')) for l in data.splitlines()):
  q=p.with_name(p.name+'.gz');q.write_bytes(gzip.compress(data,mtime=0))
  assert gzip.decompress(q.read_bytes())==data
  files.append(str(q.relative_to(b)));raw.append({'file':str(rel),'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()})
 else:files.append(str(rel))
(b/'raw-sha256.json').write_text(json.dumps(raw,indent=2)+'\n')
files+=['raw-sha256.json','evidence-files.json']
(b/'evidence-files.json').write_text(json.dumps(files,indent=2)+'\n')
print(len(files),'files selected')
