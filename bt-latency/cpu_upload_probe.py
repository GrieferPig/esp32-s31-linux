import sys,subprocess,hashlib,base64,json
from pathlib import Path
root=Path(__file__).resolve().parent
binary=Path(sys.argv[1]);dest=Path(sys.argv[2]);dest.mkdir(exist_ok=True)
data=binary.read_bytes();encoded=base64.b64encode(data).decode();commands=[": > /tmp/cpu-idle-probe.b64"]
commands += ["printf '%s' '"+encoded[i:i+700]+"' >> /tmp/cpu-idle-probe.b64" for i in range(0,len(encoded),700)]
commands += ['base64 -d /tmp/cpu-idle-probe.b64 > /tmp/cpu-idle-probe; chmod 755 /tmp/cpu-idle-probe; sha256sum /tmp/cpu-idle-probe']
(dest/'commands.json').write_text(json.dumps(commands,indent=2))
with (dest/'upload.raw').open('wb') as f:
 for cmd in commands:
  x=subprocess.run(['python3',str(root/'board_command.py'),cmd,'20'],capture_output=True)
  f.write(x.stdout+x.stderr)
  if x.returncode:raise SystemExit(x.returncode)
assert hashlib.sha256(data).hexdigest().encode() in x.stdout
print('Temporary CPU probe uploaded and SHA256 verified')
