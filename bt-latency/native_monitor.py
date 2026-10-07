#!/usr/bin/env python3
"""Read raw 115200 UART without modem-line toggles or console commands."""
import os,tty,termios,select,time,sys
from pathlib import Path
dest=Path(sys.argv[1]);seconds=float(sys.argv[2])
fd=os.open('/dev/ttyUSB0',os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK);tty.setraw(fd)
a=termios.tcgetattr(fd);a[4]=a[5]=termios.B115200
# Keep closing a console capture from hanging up the USB-UART modem lines.
a[2]=(a[2] | termios.CLOCAL | termios.CREAD) & ~termios.HUPCL
termios.tcsetattr(fd,termios.TCSANOW,a)
end=time.monotonic()+seconds
with dest.open('ab') as out:
 while time.monotonic()<end:
  if select.select([fd],[],[],min(.5,max(0,end-time.monotonic())))[0]:
   data=os.read(fd,65536);out.write(data);out.flush()
os.close(fd)
print(dest.read_text(errors='replace')[-6000:])
