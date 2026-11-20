# ESP32-P4 JPEG board test

This external module exercises the rev1.3 JPEG decoder through both 2D-DMA
channel pairs on the Waveshare ESP32-P4-WIFI6-DEV-KIT. It uses the kernel's IRQ
domain and SRAM DMA mapping. It builds separately from the boot image and
Buildroot.

Build the module against the exact kernel running on the board. Use the same
`CROSS_COMPILE` as for that kernel. Python 3 with Pillow generates the fixtures
and verifies the expected pixels when the module is built.

```sh
make -C linux-esp32-s31 O="$PWD/out-p4/linux" ARCH=riscv \
    CROSS_COMPILE="$CROSS_COMPILE" \
    M="$PWD/ports/esp32p4-reconstructed/jpeg-lab" \
    MO="$PWD/out-p4/reports/p4-jpeg-lab" modules
```

Copy the module to `/run/p4-jpeg-lab.ko`. Boot the default device tree with an
EN reset and run the following as a root shell script with the PPA and 2D-DMA clients idle.
The module accesses the clock and reset registers directly for this test.

```sh
set -e
ppa=/sys/bus/platform/drivers/esp32p4-ppa
dma=/sys/bus/platform/drivers/esp-dma2d
ppa_dev=50087000.pixel-processor
dma_dev=50088000.dma-controller

restore() {
    if ! test -L "$dma/$dma_dev"; then
        echo "$dma_dev" >"$dma/bind"
    fi
    if ! test -L "$ppa/$ppa_dev"; then
        echo "$ppa_dev" >"$ppa/bind"
    fi
}
trap restore EXIT
echo "$ppa_dev" >"$ppa/unbind"
echo "$dma_dev" >"$dma/unbind"
for pair in 0 1; do
    insmod /run/p4-jpeg-lab.ko pair="$pair"
    rmmod p4_jpeg_lab
done
dmesg | grep p4-jpeg-lab
```

For each pair, expect a 16×16 frame with JPEG DONE and DMA RX EOF, a deliberate
frame-width mismatch with JPEG DE_FRAME_EOF_ERR and DMA RX ERR_EOF, and a
32×24 frame with JPEG DONE and DMA RX EOF. The valid frames must match every
pixel and preserve 64 guard bytes. Repeat after another EN reset.

The rev1.3 TRM chapter 33 defines the decoder setup and the Huffman
`JPEG_DHT_TOTLEN_*` FIFO registers. Write the cumulative symbol counts as in
ESP-IDF’s `jpeg_ll_dht_dc0_write_codeword()` and
`jpeg_ll_dht_ac0_write_codeword()`. The per-length count failed the board test. Chapter 6 defines 2D-DMA descriptor
mode. Keep `DSCR_PORT_EN` clear for JPEG so the DMA engines read the linked
descriptors.
