#!/usr/bin/env python3
import os, termios, tty, select, time, sys

cmd = sys.argv[1] if len(sys.argv) > 1 else 'echo ALIVE'
wait = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0
fd = os.open('/dev/ttyUSB0', os.O_RDWR | os.O_NOCTTY)
# Raw: no output post-processing (kills ONLCR \n->\r\n), 115200 8N1
tty.setraw(fd)
attrs = termios.tcgetattr(fd)
attrs[4] = getattr(termios, 'B115200')
attrs[5] = getattr(termios, 'B115200')
termios.tcsetattr(fd, termios.TCSANOW, attrs)
termios.tcflush(fd, termios.TCIOFLUSH)
os.set_blocking(fd, False)

def rd(t):
    deadline = time.monotonic() + t
    out = b''
    while time.monotonic() < deadline:
        r, _, _ = select.select([fd], [], [], 0.5)
        if r:
            try:
                d = os.read(fd, 16384)
            except BlockingIOError:
                continue
            if d:
                out += d
    return out

rd(.2)
os.write(fd, (cmd + '\r').encode())
out = rd(wait)
print(out.decode(errors='replace'))
os.close(fd)
