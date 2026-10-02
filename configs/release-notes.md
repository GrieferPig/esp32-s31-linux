The release contains a combined image, six slot images, build identity, and
SHA-256 checksums. Verify downloaded files against `SHA256SUMS` first.

For a firmware update, download all six slot images from this release to the
same directory. The command writes only the firmware slots and preserves the
persist and HIL scratch areas. Set `PORT` to the board's serial port.

## Flash command
```bash
PORT=/dev/ttyUSB0
python -m esptool --chip esp32s31 -p "$PORT" -b 460800 write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x002000 spl_app.bin 0x100000 u-boot.itb 0x300000 esp32s31_generic.dtb \
  0x310000 radio.bin 0x500000 xipImage 0xBD0000 rootfs.sqfs
```

The slot update preserves persist (`0xB30000–0xBC0000`) and HIL scratch
(`0xBC0000–0xBD0000`). A contiguous full-image write overwrites both ranges.
The combined image `s31_full_flash.bin` is also provided for first installations;
erasing flash first removes existing persist data.
