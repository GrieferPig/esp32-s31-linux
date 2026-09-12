The release contains a combined image, six slot images, build identity, and
SHA-256 checksums. Verify downloaded files against `SHA256SUMS` first.

For a first installation, erase flash and write `s31_full_flash.bin` at zero:

```sh
esptool -p BOARD_PORT -b 460800 erase-flash
esptool -p BOARD_PORT -b 460800 write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB 0x0 s31_full_flash.bin
```

For an update preserving configuration, download all six slot images from the
same release and write them together, without erasing flash:

```sh
esptool -p BOARD_PORT -b 460800 write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x002000 spl_app.bin 0x100000 u-boot.itb 0x300000 esp32s31_generic.dtb \
  0x310000 radio.sqfs 0x500000 xipImage 0xBD0000 rootfs.sqfs
```

The slot update preserves persist (`0xB30000–0xBC0000`) and HIL scratch
(`0xBC0000–0xBD0000`). A contiguous full-image write overwrites both ranges.
A freshly erased persist area is a valid empty JFFS2 filesystem; a separate
persist image is not required for first installation.
