#!/bin/bash

esptool erase-flash
esptool -b 2000000 write-flash 0x220000 fw_payload.bin 0x2A0000 xipImage 0xA20000 initramfs.cpio

