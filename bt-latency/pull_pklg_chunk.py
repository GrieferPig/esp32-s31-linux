#!/usr/bin/env python3
import os,termios,tty,select,time,sys,base64,re,json
from console_io import write_paced
from pathlib import Path
source,dest,skip,count=sys.argv[1:]
skip,count=int(skip),int(count)
assert 0<count<=300 and skip>=0
fd=os.open('/dev/ttyUSB0',os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK)
tty.setraw(fd)
a=termios.tcgetattr(fd);a[4]=a[5]=termios.B115200
# Keep closing a console capture from hanging up the USB-UART modem lines.
a[2]=(a[2] | termios.CLOCAL | termios.CREAD) & ~termios.HUPCL
termios.tcsetattr(fd,termios.TCSANOW,a)
cmd=f'dd if={source} bs=1K skip={skip} count={count} 2>/dev/null | base64; echo MISSION_DONE'
write_paced(fd,(cmd+'\r').encode())
raw=b'';deadline=time.monotonic()+58
while time.monotonic()<deadline:
 if select.select([fd],[],[],.2)[0]:
  raw+=os.read(fd,65536)
  if b'\r\nMISSION_DONE\r\n' in raw:break
os.close(fd)
Path(dest+'.raw').write_bytes(raw)
assert b'\r\nMISSION_DONE\r\n' in raw,'transfer incomplete'
lines=[l.strip() for l in raw.splitlines() if re.fullmatch(rb'[A-Za-z0-9+/=]{4,76}',l.strip())]
data=base64.b64decode(b''.join(lines),validate=True);Path(dest).write_bytes(data)
print(json.dumps(dict(command=cmd,bytes=len(data),skip=skip,count=count)),flush=True)
