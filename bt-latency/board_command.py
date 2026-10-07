#!/usr/bin/env python3
"""Run one Linux console command in raw mode; finish on a unique output marker."""
import os,sys,tty,termios,select,time,secrets,re
cmd=sys.argv[1];seconds=float(sys.argv[2]) if len(sys.argv)>2 else 30
fd=os.open('/dev/ttyUSB0',os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK)
tty.setraw(fd)
a=termios.tcgetattr(fd);a[4]=a[5]=termios.B115200;termios.tcsetattr(fd,termios.TCSANOW,a)
# printf assembles the marker so the echoed command cannot match it.
token=secrets.token_hex(8)
marker=('__S31_DONE_'+token+'__').encode()
wire="{ "+cmd+"; }; s31_cmd_status=$?; printf '\\n__S31_DONE_%s__:%s\\n' '"+token+"' \"$s31_cmd_status\""
# This image's BusyBox CONFIG_FEATURE_EDITING_MAX_LEN is1024. Refuse
# oversized lines before sending anything; truncation can leave an open quote.
if len(wire.encode()) >= 1024:
 os.close(fd)
 raise SystemExit('Console command exceeds the1024-byte BusyBox line limit; shorten it')
os.write(fd,(wire+'\r').encode())
out=bytearray();deadline=time.monotonic()+seconds;match=None
while time.monotonic()<deadline:
 if select.select([fd],[],[],min(.5,max(0,deadline-time.monotonic())))[0]:
  data=os.read(fd,65536);out.extend(data)
  sys.stdout.buffer.write(data);sys.stdout.buffer.flush()
  match=re.search(b'\n'+marker+rb':([0-9]+)\n',bytes(out).replace(b'\r',b''))
  if match:break
os.close(fd)
if match is None:
 print('\nHOST: command completion marker timeout',file=sys.stderr)
 raise SystemExit(1)
raise SystemExit(int(match.group(1)))
