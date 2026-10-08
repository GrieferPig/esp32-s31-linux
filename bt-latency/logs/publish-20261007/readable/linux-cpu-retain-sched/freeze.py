from pathlib import Path
import hashlib,json,shutil
r=Path('/home/grieferpig/esp32-s31-linux');d=Path(__file__).resolve().parent
dist=(r/'dist/current').resolve().name
shutil.copyfile(r/'out/images/build-manifest.json',d/'build-manifest.json')
(d/'dist.txt').write_text(dist+'\n')
files={'kernel':r/'out/linux/vmlinux','module':r/'out/linux/drivers/platform/esp32s31-radio.ko','payload':r/'out/images/radio-xip.elf'}
hashes={key:hashlib.sha256(p.read_bytes()).hexdigest() for key,p in files.items()}
m=json.loads((d/'build-manifest.json').read_text())
assert hashes['kernel']==m['radio_binding']['kernel_vmlinux_sha256']
assert hashes['module']==m['radio_binding']['kernel_module_sha256']
(d/'build-elf-hashes.json').write_text(json.dumps(hashes,indent=2))
shutil.copyfile(r/'out/linux/.config',d/'linux.config')
print(dist)
