from pathlib import Path
import subprocess,struct,re,json,hashlib
r=Path('/home/grieferpig/esp32-s31-linux');d=Path(__file__).resolve().parent;p=r/'cache/toolchains/riscv32-esp-linux-musl/bin'
result={}
for name,rel in [('btstack','usr/sbin/s31-btstack-a2dp'),('libc','usr/lib/libc.so'),('positive-ext-test','usr/sbin/s31-ext-test'),('vdso','@vdso')]:
 f=(r/'out/linux/arch/riscv/kernel/vdso/vdso.so') if rel=='@vdso' else (r/'out/buildroot/target'/rel);data=f.read_bytes();assert data[:5]==b'\x7fELF\x01'
 hdr=struct.unpack_from('<16sHHIIIIIHHHHHH',data);assert hdr[2]==243
 section_base,section_size,section_count=hdr[6],hdr[11],hdr[12]
 candidates=[];count=0;lengths={}
 for i in range(section_count):
  sec=struct.unpack_from('<IIIIIIIIII',data,section_base+i*section_size)
  if sec[1]!=1 or not sec[2]&4:continue # PROGBITS, executable
  content=data[sec[4]:sec[4]+sec[5]];pos=0
  while pos+2<=len(content):
   half=int.from_bytes(content[pos:pos+2],'little')
   size=2 if half&3!=3 else 4
   if size==4 and pos+4<=len(content):
    word=int.from_bytes(content[pos:pos+4],'little')
    if word&0x7f==0x2b or word&0x1b==0x1b:
     candidates.append({'section':i,'address':hex(sec[3]+pos),'word':hex(word),
                        'family':'hwloop' if word&0x7f==0x2b else 'pie'})
   count+=1;lengths[size]=lengths.get(size,0)+1;pos+=size
 attrs=subprocess.check_output([str(p/'riscv32-esp-linux-musl-readelf'),'-A',str(f)]).decode()
 dynamic=subprocess.run([str(p/'riscv32-esp-linux-musl-readelf'),'-d',str(f)],capture_output=True).stdout.decode()
 disasm=subprocess.check_output([str(p/'riscv32-esp-linux-musl-objdump'),'-d',str(f)]).decode()
 (d/(name+'.disasm')).write_text(disasm)
 result[name]={'path':str(f),'sha256':hashlib.sha256(data).hexdigest(),'elf_attributes':attrs,
               'dynamic_section':dynamic,'instructions_scanned':count,'instruction_sizes':lengths,
               'vendor_candidates':candidates,'esp_disassembly_lines':[v for v in disasm.splitlines() if 'esp.' in v]}
assert not result['btstack']['vendor_candidates'],result['btstack']['vendor_candidates']
assert not result['libc']['vendor_candidates'],result['libc']['vendor_candidates']
assert not result['vdso']['vendor_candidates'],result['vdso']['vendor_candidates']
assert any(v['family']=='pie' for v in result['positive-ext-test']['vendor_candidates'])
assert any(v['family']=='hwloop' for v in result['positive-ext-test']['vendor_candidates'])
assert result['btstack']['sha256']=='49f8ee469d22c9dc0ca3ac7eff7687a637a16169f8239471374a44be800a8707'
(d/'binary-inspection.json').write_text(json.dumps(result,indent=2))
for name,x in result.items():print(name,'instructions',x['instructions_scanned'],'vendor candidates',len(x['vendor_candidates']),'esp disasm',len(x['esp_disassembly_lines']))
print('App and sole dynamic dependency contain zero PIE/HWLoop family instructions. Positive control detects both.')
