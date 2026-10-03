The release contains a combined image, six slot images, build identity, and
SHA-256 checksums. Verify downloaded files against `SHA256SUMS` first.

## Flash layout

This layout fills the 16 MiB flash with no gaps. Raw ranges are end-exclusive:

| Raw start | Raw end | Capacity | Contents |
|---:|---:|---:|---|
| `0x000000` | `0x002000` | 8 KiB | Mandatory ROM FlashEncryption reservation |
| `0x002000` | `0x00E000` | 48 KiB | SPL |
| `0x00E000` | `0x05E000` | 320 KiB | U-Boot/OpenSBI FIT |
| `0x05E000` | `0x06E000` | 64 KiB | Linux DTB |
| `0x06E000` | `0x1EE000` | 1536 KiB | Radio XIP payload |
| `0x1EE000` | `0x400000` | 2120 KiB | Persistent JFFS2 filesystem |
| `0x400000` | `0xA00000` | 6144 KiB | Linux XIP kernel |
| `0xA00000` | `0x1000000` | 6144 KiB | SquashFS root filesystem |

The full flash maps linearly at physical `0x40000000`. Linux starts at
`0x40400000`, a 4 MiB Sv32 megapage boundary; see the unaligned-XIP failure in
[issue #1](https://github.com/GrieferPig/esp32-s31-linux/issues/1).
There is no HIL scratch partition or destructive flash HIL case.

Changing the installed flash layout requires an external backup, a clean
installation, and restoration of needed files/settings. Whole-flash writes
overwrite persist; verify backups before erasing or writing.

## Clean installation

Erase flash and write `s31_full_flash.bin` at zero. These commands erase all
saved data; back it up first:

```sh
esptool -p BOARD_PORT -b 460800 erase-flash
esptool -p BOARD_PORT -b 460800 write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB 0x0 s31_full_flash.bin
```

A freshly erased persist area is a valid empty JFFS2 filesystem; a separate
persist image is not required for a clean installation. A contiguous
full-image write overwrites persist even when `erase-flash` is omitted.

## Update a board already using this layout

Only when the installed image already uses the compact layout above, download
all six slot images from the same release and write them together without
erasing flash:

```sh
esptool -p BOARD_PORT -b 460800 write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x002000 spl_app.bin 0x00E000 u-boot.itb 0x05E000 esp32s31_generic.dtb \
  0x06E000 radio.bin 0x400000 xipImage 0xA00000 rootfs.sqfs
```

This same-layout slot update preserves persist (`0x1EE000–0x400000`). Keep the
kernel, rootfs/module, and prelinked radio payload from the same build.

Hardware boot and flashing validation is pending. The emulator reaches
read-only recovery because persistent-flash erase fails. Host checks and a
recovery login do not establish hardware validation or working persistence.
