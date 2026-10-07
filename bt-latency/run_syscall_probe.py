#!/usr/bin/env python3
"""Upload the owned diagnostic to /tmp, verify, execute, and remove it."""
import base64,gzip,hashlib,subprocess,sys,json
from pathlib import Path
root=Path(__file__).resolve().parent;binary=Path(sys.argv[1]);dest=Path(sys.argv[2]);dest.mkdir(parents=True,exist_ok=True)
raw=binary.read_bytes();digest=hashlib.sha256(raw).hexdigest()
packed=base64.b64encode(gzip.compress(raw,mtime=0)).decode()
target='/tmp/s31-syscall-bench';stage=target+'.gz.b64'
commands=['test ! -e '+target+' && test ! -e '+stage+' && : > '+stage]
commands += ["printf '%s\\n' '"+packed[i:i+768]+"' >> "+stage for i in range(0,len(packed),768)]
commands += ['base64 -d '+stage+' | gzip -d > '+target+'; chmod 700 '+target+'; sha256sum '+target]
(dest/'upload-commands.json').write_text(json.dumps(commands,indent=2))
for i,cmd in enumerate(commands):
    r=subprocess.run(['python3',str(root/'board_command.py'),cmd,'20'],capture_output=True)
    (dest/f'upload-{i}.raw').write_bytes(r.stdout+r.stderr)
    if r.returncode:raise SystemExit('upload failed')
if digest.encode() not in r.stdout:raise SystemExit('uploaded digest mismatch')
cmd='taskset 2 '+target
r=subprocess.run(['python3',str(root/'board_command.py'),cmd,'30'],capture_output=True)
(dest/'run-command.txt').write_text(cmd+'\n');(dest/'run.raw').write_bytes(r.stdout+r.stderr)
print(r.stdout.decode(errors='replace'),flush=True)
cleanup='rm -f '+target+' '+stage
c=subprocess.run(['python3',str(root/'board_command.py'),cleanup,'10'],capture_output=True)
(dest/'cleanup.raw').write_bytes(c.stdout+c.stderr)
raise SystemExit(r.returncode or c.returncode)
