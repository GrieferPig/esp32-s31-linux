#!/usr/bin/env python3
"""Pull the frozen kernel profile as bounded gzip/base64 over raw UART."""
import subprocess,sys,base64,gzip,hashlib,re,json
from pathlib import Path
dest=Path(sys.argv[1]);dest.mkdir(parents=True,exist_ok=True)
command="""gzip -c /tmp/s31-cost.txt > /tmp/s31-cost.txt.gz; test $(wc -c < /tmp/s31-cost.txt.gz) -le 307200 && { sha256sum /tmp/s31-cost.txt; printf '\\nPROFILE_%s\\n' BEGIN; dd if=/tmp/s31-cost.txt.gz bs=1K skip=0 count=300 2>/dev/null | base64; printf '\\nPROFILE_%s\\n' END; }"""
(dest/'profile-pull-command.txt').write_text(command+'\n')
r=subprocess.run(['python3',str(Path(__file__).resolve().parents[2]/'board_command.py'),command,'60'],capture_output=True)
raw=r.stdout+r.stderr;(dest/'profile-pull.raw').write_bytes(raw)
if r.returncode:raise SystemExit('profile pull failed')
clean=raw.replace(b'\r',b'')
part=clean.split(b'\nPROFILE_BEGIN\n',1)[1].split(b'\nPROFILE_END\n',1)[0]
packed=base64.b64decode(b''.join(part.split()),validate=True);data=gzip.decompress(packed)
digest=hashlib.sha256(data).hexdigest()
assert re.search(digest.encode()+rb'\s+/tmp/s31-cost.txt',clean),'profile hash mismatch'
(dest/'cost.txt.gz').write_bytes(packed);(dest/'cost.txt').write_bytes(data)
print(json.dumps({'bytes':len(data),'gzip_bytes':len(packed),'sha256':digest}))
