#!/usr/bin/env python3
"""Flash a complete native benchmark set strictly below Linux persist."""
import argparse,json,hashlib,subprocess,sys,math
from pathlib import Path
root=Path(__file__).resolve().parent
ap=argparse.ArgumentParser()
ap.add_argument('kind',choices=['ble','spp'])
ap.add_argument('--port',default='/dev/ttyUSB0')
ap.add_argument('--baud',default='921600')
ap.add_argument('--execute',action='store_true')
ap.add_argument('--build-dir',type=Path,help='Explicit matched build directory for a control variant')
ap.add_argument('--log-dir',type=Path,help='Evidence directory; defaults to logs/native-idf')
a=ap.parse_args()
build=(a.build_dir or root/a.kind/'build').resolve()
args=json.loads((build/'flasher_args.json').read_text())
files={int(offset,0):(build/path).resolve() for offset,path in args['flash_files'].items()}
assert set(files)=={0x2000,0x10000,0x20000},files
nvs=build/'benchmark-nvs-empty.bin';nvs.write_bytes(b'\xff'*0x6000);files[0x11000]=nvs
records=[];last_end=0x2000
for offset,p in sorted(files.items()):
 assert p.is_relative_to(build),p
 data=p.read_bytes();assert data
 end=(offset+len(data)+4095)&~4095
 assert offset%4096==0 and last_end<=offset and end<=0x1ee000,(hex(offset),hex(end))
 last_end=end
 records.append(dict(offset=hex(offset),file=str(p),size=len(data),erase_end=hex(end),sha256=hashlib.sha256(data).hexdigest()))
# IDF parser independently validates the table, all partitions below persist.
partition_data=(build/'partition_table/partition-table.bin').read_bytes()
sys.path.insert(0,'/home/grieferpig/esp-idf/components/partition_table')
import gen_esp32part
table=gen_esp32part.PartitionTable.from_binary(partition_data)
for entry in table:
 assert 0x11000<=entry.offset and entry.offset+entry.size<=0x1ee000,(entry.name,entry.offset,entry.size)
 assert entry.name in ['nvs','factory'],entry.name
print(json.dumps(records,indent=2),flush=True)
log=a.log_dir or root.parent/'logs/native-idf'
log.mkdir(parents=True,exist_ok=True)
(log/f'{a.kind}-flash-manifest.json').write_text(json.dumps(records,indent=2))
base=[sys.executable,'-m','esptool','--chip','esp32s31','--port',a.port,'--baud',a.baud,'--no-stub']
argv=base+['write-flash','--flash-mode','dio','--flash-size','16MB','--flash-freq','80m']
for offset,p in sorted(files.items()):argv.extend([hex(offset),str(p)])
print('COMMAND',json.dumps(argv),flush=True)
if a.execute:
 subprocess.run(argv,check=True)
 verify=base+['verify-flash']
 for offset,p in sorted(files.items()):verify.extend([hex(offset),str(p)])
 subprocess.run(verify,check=True)
