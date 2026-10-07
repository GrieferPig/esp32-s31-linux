from pathlib import Path
import json,os
r=Path('/home/grieferpig/esp32-s31-linux');d=Path(__file__).resolve().parent
p=r/'dist/current';assert p.is_symlink()
old=os.readlink(p);assert p.resolve().name=='559a02aabaf6fdca',old
assert (r/'dist/e168e8c0f293a4d4/build-manifest.json').is_file()
temp=r/'dist/.sched-paths-current';assert not temp.exists() and not temp.is_symlink()
temp.symlink_to('e168e8c0f293a4d4');os.replace(temp,p)
assert p.resolve().name=='e168e8c0f293a4d4'
(d/'dist-current-restore.json').write_text(json.dumps(dict(previous=old,current=os.readlink(p)),indent=2))
print('dist/current restored to accepted image')
