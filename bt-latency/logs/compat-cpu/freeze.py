from pathlib import Path
import subprocess,shutil,json,hashlib
r=Path('/home/grieferpig/esp32-s31-linux');base=Path(__file__).resolve().parent;d=base/'symbols';d.mkdir()
nm=r/'cache/toolchains/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-nm'
files={'kernel':r/'out/linux/vmlinux','payload':r/'out/images/radio-xip.elf','module':r/'out/linux/drivers/platform/esp32s31-radio.ko'}
manifest={}
for key,p in files.items():
 (d/(key+'.nm')).write_bytes(subprocess.check_output([str(nm),'-n','-S',str(p)]))
 manifest[key]={'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
shutil.copyfile(r/'out/linux/System.map',d/'System.map')
shutil.copyfile(r/'out/linux/.config',d/'linux.config')
shutil.copyfile(files['module'],d/'module.ko')
(d/'manifest.json').write_text(json.dumps(manifest,indent=2))
files=list((r/'out/radio/s31_rtos').glob('*.o'))+[r/'out/radio/s31_linux_locks.o',r/'out/radio/s31_linux_timer.o',r/'out/linux/drivers/platform/esp32s31-radio-rtos.o',r/'out/linux/drivers/platform/esp32s31-radio-smode.o']
sources={}
for p in files:
 name=p.name+'.nm';(d/name).write_bytes(subprocess.check_output([str(nm),'-n','-S',str(p)]));sources[name]=str(p)
(d/'object-files.json').write_text(json.dumps(sources,indent=2))
for name,src in [('module-layout-source.c','linux-esp32-s31/kernel/module/main.c'),('module-arch-source.c','linux-esp32-s31/arch/riscv/kernel/module-sections.c')]:
 shutil.copyfile(r/src,d/name)
shutil.copyfile(r/'out/images/build-manifest.json',base/'build-manifest.json')
dist=(r/'dist/current').resolve().name
(base/'dist.txt').write_text(dist+'\n')
print(dist,flush=True)
