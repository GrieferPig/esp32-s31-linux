# ESP32-S31 radio

`make all` builds Linux, rootfs and the matched flash-XIP radio image. The
Linux module is installed in rootfs; `out/images/radio.bin` contains the prelinked
radio code and its pristine writable-data template. Kernel and radio image
must be built and deployed together. Layout and image CRC checks reject
incompatible layouts or corrupt images before executing radio code. The
`make image` publication and device targets additionally verify the build
manifest and kernel/module/payload binding before using the matched set.

The Wi-Fi frontend is a single-STA mac80211 bridge using native MAC CCMP,
bounded TX aggregation, and Linux RX replay/reorder checks. Runtime mode
selection enables Wi-Fi, Bluetooth, or both. Wi-Fi-only mode uses 16 static and
32 dynamic RX buffers; combo mode uses 10 static and 32 dynamic RX buffers.
The current TX aggregation cap is 16 frames and the RX cache limit is 48 blocks.
No throughput claim follows from a successful build.

The firmware uses the ESP-IDF revision and compiler pinned in
`configs/build-versions.mk`. `make radio-package` packages the matched image,
module, overlays, configuration and licenses. Redistribution requirements
are described in `RADIO_BUNDLE_LICENSES.md`.
