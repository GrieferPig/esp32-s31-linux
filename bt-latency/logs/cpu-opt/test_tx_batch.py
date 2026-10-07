#!/usr/bin/env python3
"""Exercise the real kernel batch parser with a bounded mock TX ring."""
from pathlib import Path
import subprocess,tempfile,gzip
root=Path(__file__).resolve().parent
def read_source(p):
 return p.read_text() if p.exists() else gzip.decompress(Path(str(p)+'.gz').read_bytes()).decode()
source=read_source(root/'final/esp32s31-radio-smode.c')
start=source.index('int esp32s31_radio_hci_send_batch(')
end=source.index('EXPORT_SYMBOL_GPL(esp32s31_radio_hci_send_batch);',start)
body=source[start:end]
prefix=r"""
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <stdbool.h>
#include <assert.h>
typedef uint8_t u8;
#define S31_HCI_FRAME_SIZE 1030
static unsigned long s31_tx_batch_calls,s31_tx_batch_frames,s31_tx_batch_partial;
static int capacity, sent, wakes;
static unsigned char ids[16];
static int s31_hci_send(u8 type,const u8 *p,size_t n,bool wake) {
 assert(!wake);assert(type==2 && n==1);
 if(sent==capacity)return -EAGAIN;
 ids[sent++]=p[0];return 0;
}
static void s31_radio_wake(void){wakes++;}
"""
tests=r"""
int main(void){
 unsigned char data[40]={255};
 for(int i=0;i<9;i++){data[1+i*4]=2;data[2+i*4]=0;data[3+i*4]=2;data[4+i*4]=i;}
 capacity=8;
 assert(esp32s31_radio_hci_send_batch(data,33)==33);
 assert(sent==8&&wakes==1&&ids[7]==7);
 sent=wakes=0;
 assert(esp32s31_radio_hci_send_batch(data,37)==-EINVAL);
 assert(sent==0&&wakes==0);
 assert(esp32s31_radio_hci_send_batch(data,32)==-EINVAL);
 assert(sent==0&&wakes==0);
 capacity=0;
 assert(esp32s31_radio_hci_send_batch(data,13)==-EAGAIN);
 assert(sent==0&&wakes==0);
 capacity=2;
 assert(esp32s31_radio_hci_send_batch(data,13)==9);
 assert(sent==2&&wakes==1&&ids[0]==0&&ids[1]==1);
 /* Retry only the unsent suffix with its marker restored. */
 unsigned char tail[]={255,2,0,2,2};capacity=3;
 assert(esp32s31_radio_hci_send_batch(tail,sizeof(tail))==5);
 assert(sent==3&&wakes==2&&ids[2]==2);
 data[1]=0;
 assert(esp32s31_radio_hci_send_batch(data,5)==-EINVAL);
 assert(sent==3&&wakes==2);
 return 0;
}
"""
with tempfile.TemporaryDirectory() as td:
 c=Path(td)/'test.c';exe=Path(td)/'test'
 c.write_text(prefix+body+tests)
 subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: complete batch, record limits, truncated/zero lengths, EAGAIN, partial acceptance, ordered suffix retry')

src=read_source(root/'final/hci_transport_linux.c')
body=src[src.index('void s31_hci_flush_tx_batch(void)'):src.index('static void hci_transport_linux_process_write')]
prefix=r"""
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#define btstack_assert assert
#define DATA_SOURCE_CALLBACK_WRITE 1
static unsigned s31_tx_batch_limit=4,s31_tx_frames,s31_tx_size;
static int hci_socket,s31_tx_waiting_completion,hci_transport_linux_data_source;
static unsigned char s31_tx_buffer[8192];
static int completions,write_enabled;
static ssize_t write_result;
static ssize_t write(int fd,const void *p,size_t n){(void)fd;(void)p;(void)n;return write_result;}
static void btstack_run_loop_enable_data_source_callbacks(int *ds,int x){(void)ds;(void)x;write_enabled=1;}
static void btstack_run_loop_disable_data_source_callbacks(int *ds,int x){(void)ds;(void)x;write_enabled=0;}
static void hci_transport_linux_schedule_packet_sent(void){completions++;}
"""
tests=r"""
int main(void){
 unsigned char batch[]={255,2,0,2,0,2,0,2,1,2,0,2,2};
 memcpy(s31_tx_buffer,batch,sizeof(batch));s31_tx_size=sizeof(batch);s31_tx_frames=3;s31_tx_waiting_completion=1;
 write_result=-1;errno=EAGAIN;s31_hci_flush_tx_batch();
 assert(s31_tx_size==13&&s31_tx_frames==3&&completions==0&&write_enabled);
 write_result=9;s31_hci_flush_tx_batch();
 assert(s31_tx_size==5&&s31_tx_frames==1&&completions==1&&write_enabled);
 assert(s31_tx_buffer[0]==255&&s31_tx_buffer[4]==2);
 write_result=-1;errno=EINTR;s31_hci_flush_tx_batch();
 assert(s31_tx_size==5&&s31_tx_frames==1&&completions==1);
 write_result=5;s31_hci_flush_tx_batch();
 assert(s31_tx_size==1&&s31_tx_frames==0&&completions==1&&!write_enabled);
 return 0;
}
"""
with tempfile.TemporaryDirectory() as td:
 c=Path(td)/'test.c';exe=Path(td)/'test';c.write_text(prefix+body+tests)
 subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: transport EAGAIN/EINTR, accepted-prefix removal, ordered suffix, completion and POLLOUT accounting')
