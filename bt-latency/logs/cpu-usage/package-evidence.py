"""Losslessly package owned evidence; exclude build and scratch trees."""
from pathlib import Path
import gzip,hashlib,json
b=Path(__file__).resolve().parent
for kind in ('spp','ble'):
 v=b/('native-'+kind)
 manifest=json.loads((v/(kind+'-flash-manifest.json')).read_text())
 out=v/'flashed-images';out.mkdir(exist_ok=True)
 for item in manifest:
  src=Path(item['file']);data=src.read_bytes()
  assert hashlib.sha256(data).hexdigest()==item['sha256']
  (out/(item['offset']+'-'+src.name+'.gz')).write_bytes(gzip.compress(data,mtime=0))
selected=[];raw_hashes=[]
for p in sorted(b.rglob('*')):
 rel=p.relative_to(b)
 if not p.is_file() or any(x in rel.parts for x in ('build','scratch','__pycache__')):continue
 if p.name in ('evidence-files.json','raw-sha256.json'):continue
 if p.suffix=='.gz':continue
 data=p.read_bytes()
 compress=len(data)>50000 or data.endswith(b'\n\n') or p.suffix in ('.raw','.bin') or p.name in ('sdkconfig','cpu_idle_probe') or b'\r' in data or any(line.endswith((b' ',b'\t')) for line in data.splitlines())
 if compress:
  target=p.with_name(p.name+'.gz')
  target.write_bytes(gzip.compress(data,mtime=0))
  selected.append(str(target.relative_to(b)))
  raw_hashes.append({'file':str(rel),'sha256':hashlib.sha256(data).hexdigest(),'bytes':len(data)})
 else:selected.append(str(rel))
for p in sorted(b.glob('native-*/flashed-images/*.gz')):selected.append(str(p.relative_to(b)))
(b/'raw-sha256.json').write_text(json.dumps(raw_hashes,indent=2)+'\n')
selected+=['raw-sha256.json','evidence-files.json']
(b/'evidence-files.json').write_text(json.dumps(sorted(set(selected)),indent=2)+'\n')
print(len(set(selected)),'evidence files selected')
