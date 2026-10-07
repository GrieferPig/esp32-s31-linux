#!/usr/bin/env python3
"""Validate and measure the S31 fixed-frame RFCOMM stream; raw bytes retained."""
import socket,sys,time,select,os,json
from pathlib import Path
addr,ch,seconds=sys.argv[1],int(sys.argv[2]),float(sys.argv[3])
frame_size=int(os.environ.get('S31_SPP_FRAME_SIZE','1012'))
sock=socket.socket(socket.AF_BLUETOOTH,socket.SOCK_STREAM,socket.BTPROTO_RFCOMM)
sock.settimeout(15);t=time.monotonic();sock.connect((addr,ch));print(f'connected {time.monotonic()-t:.6f}s',flush=True)
if os.environ.get('S31_SPP_START_CONTROL'):
 control=b'S31'+frame_size.to_bytes(2,'little')
 if os.environ.get('S31_SPP_PACKET_MASK'):
  control+=int(os.environ['S31_SPP_PACKET_MASK'],0).to_bytes(2,'little')
 sock.sendall(control)
sock.setblocking(False);start=time.monotonic();deadline=start+seconds
raw=bytearray();chunks=[];reason='deadline'
while time.monotonic()<deadline:
 if not select.select([sock],[],[],min(5,max(0,deadline-time.monotonic())))[0]:
  reason='deadline' if time.monotonic()>=deadline else 'idle_5s';break
 part=sock.recv(65536)
 if not part:reason='eof';break
 if not chunks and os.environ.get('S31_STREAM_STARTED_FILE'):
  Path(os.environ['S31_STREAM_STARTED_FILE']).write_text(json.dumps({'first_host_packet':time.monotonic()}))
 raw.extend(part);chunks.append((time.monotonic()-start,len(part)))
duration=time.monotonic()-start;sock.close()
dest=Path(os.environ.get('S31_BENCH_RAW','/tmp/spp-rx.bin'));dest.write_bytes(raw)
frames=len(raw)//frame_size;gaps=dups=bad=0;last=None
for i in range(frames):
 b=raw[i*frame_size:(i+1)*frame_size];seq=int.from_bytes(b[:4],'little')
 if last is not None:
  if seq==last:dups+=1
  elif seq!=((last+1)&0xffffffff):gaps+=1
 if any(b[k]!=((k^seq)&255) for k in range(4,frame_size)):bad+=1
 last=seq
report=dict(bytes=len(raw),duration=duration,KiBs=len(raw)/duration/1024,frames=frames,frame_size=frame_size,partial=len(raw)%frame_size,gap_events=gaps,duplicates=dups,bad_pattern_frames=bad,last_seq=last,stop=reason,chunks=chunks)
print(json.dumps({k:v for k,v in report.items() if k!='chunks'}),flush=True)
dest.with_suffix('.json').write_text(json.dumps(report,indent=2))
sys.exit(0 if reason=='deadline' and not(gaps or dups or bad) else 1)
