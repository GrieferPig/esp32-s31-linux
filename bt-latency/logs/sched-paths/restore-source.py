from pathlib import Path
import hashlib,json,subprocess,gzip
r=Path('/home/grieferpig/esp32-s31-linux');d=Path(__file__).resolve().parent;k=r/'linux-esp32-s31'
def read(p):
 return p.read_bytes() if p.exists() else gzip.decompress(Path(str(p)+'.gz').read_bytes())
original=json.loads((d/'original-hashes.json').read_text())
for name,expected in original.items():
 data=read(d/'original'/name)
 assert hashlib.sha256(data).hexdigest()==expected,name
 assert subprocess.check_output(['git','show','HEAD:'+name],cwd=k)==data,name
 assert (k/name).read_bytes()==read(d/'diagnostic'/name),name+' changed since diagnostic'
for name in ('include/linux/s31_cost.h','kernel/s31_cost.c'):
 assert (k/name).read_bytes()==read(d/'diagnostic'/name),name
for name in original:(k/name).write_bytes(read(d/'original'/name))
for name in ('include/linux/s31_cost.h','kernel/s31_cost.c'):(k/name).unlink()
assert not subprocess.check_output(['git','status','--porcelain'],cwd=k).strip()
(d/'source-restore.json').write_text(json.dumps(dict(original_hashes_verified=True,diagnostic_snapshot_verified=True,kernel_clean=True),indent=2))
print('Original sources restored; kernel submodule clean')
