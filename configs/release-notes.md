## Linux

```sh
PORT=/dev/ttyUSB0
esptool --chip esp32s31 -p "$PORT" -b 2000000 erase-flash
esptool --chip esp32s31 -p "$PORT" -b 2000000 write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 s31_full_flash.bin
```

## Windows (PowerShell)

```powershell
$PORT="COM3"
esptool --chip esp32s31 -p "$PORT" -b 2000000 erase-flash
esptool --chip esp32s31 -p "$PORT" -b 2000000 write-flash `
  --flash-mode dio --flash-freq 80m --flash-size 16MB `
  0x0 s31_full_flash.bin
```
