## Flash command
```bash
PORT=/dev/ttyUSB0
python -m esptool --chip esp32s31 -p "$PORT" -b 460800 write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 s31_full_flash.bin
```
