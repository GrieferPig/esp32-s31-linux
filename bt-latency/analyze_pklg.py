#!/usr/bin/env python3
"""Strict PacketLogger records; reassemble outbound ACL and report ATT throughput."""
import struct,json,sys
from collections import Counter
from pathlib import Path
d=Path(sys.argv[1]).read_bytes();pos=0;records=0;pending={};notes=[];events=[];commands=Counter()
while pos<len(d):
 if pos+13>len(d):raise ValueError(f'truncated header at {pos}')
 n,sec,us=struct.unpack_from('>III',d,pos)
 if n<9 or pos+n+4>len(d):raise ValueError(f'invalid/truncated record at {pos}: {n}')
 r=d[pos+12:pos+n+4];pos+=n+4;records+=1;t=sec+us/1e6;typ=r[0];p=r[1:]
 if typ==0:
  op=int.from_bytes(p[:2],'little');commands[hex(op)]+=1
  if op in (0x2006,0x2008,0x2009,0x200a,0x2022,0x2032,0x0c1a):events.append(dict(t=t,cmd=hex(op),hex=p.hex()))
 if typ==1 and p and (p[0] in (0x05,0x0f) or (p[0]==0x3e and p[2] in (1,3,7,0xc)) or (p[0]==0x0e and int.from_bytes(p[3:5],'little') in (0x2022,0x2032,0x200a))):events.append(dict(t=t,event=p.hex()))
 if typ!=2:continue
 if len(p)<4:raise ValueError('short ACL')
 hf,l=struct.unpack_from('<HH',p);handle=hf&0xfff;pb=(hf>>12)&3;v=p[4:]
 if len(v)!=l:raise ValueError('ACL length mismatch')
 if pb in (0,2):pending[handle]=bytearray(v)
 elif pb==1:
  if handle not in pending:raise ValueError('orphan continuation')
  pending[handle].extend(v)
 else:continue
 b=pending[handle]
 if len(b)<4:continue
 size,cid=struct.unpack_from('<HH',b)
 if len(b)<size+4:continue
 if len(b)!=size+4:raise ValueError('L2CAP length mismatch')
 if cid==4 and b[4]==0x1b:
  notes.append(dict(t=t,att_bytes=size,value_bytes=size-3,seq=int.from_bytes(b[7:11],'little')))
 del pending[handle]
result=dict(file=sys.argv[1],bytes=len(d),records=records,parse_errors=0,incomplete_acl=len(pending),commands=commands,events=events,notifications=len(notes))
if notes:
 start=notes[0]['t'];stop=notes[-1]['t'];result.update(first_t=start,last_t=stop,span=stop-start,att_bytes=sum(x['att_bytes'] for x in notes),seq_first=notes[0]['seq'],seq_last=notes[-1]['seq'])
 result['windows']=[]
 for offset,duration in [(0,10),(10,10),(20,10),(30,10),(40,10),(5,40)]:
  a=start+offset;b=a+duration
  if b>stop:continue
  group=[x for x in notes if a<=x['t']<b];att=sum(x['att_bytes'] for x in group);val=sum(x['value_bytes'] for x in group)
  result['windows'].append(dict(offset=offset,seconds=duration,notifications=len(group),att_bytes=att,value_bytes=val,att_KiBs=att/duration/1024,value_KiBs=val/duration/1024))
print(json.dumps(result,indent=2))
