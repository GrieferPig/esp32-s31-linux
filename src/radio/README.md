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

The native CPU0 submitter fills final vendor buffers from retained skb leases.
Its default TX path shares the individual Wi-Fi resource mutex across up to
eight available submissions. Backpressure ends that scope; PP notification
recovery releases it before waiting. Per-frame descriptor/key ownership and
notifications remain independent. The mandatory batch capability rejects the
retired per-frame interface; there is no selectable compatibility path.

The firmware uses the ESP-IDF revision and compiler pinned in
`configs/versions.mk`. `make radio-package` packages the matched image,
module, overlays, configuration and licenses. Redistribution requirements
are described in `RADIO_BUNDLE_LICENSES.md`.

Radio tasks retain their Linux execution context on CPU0, using native PI
waits and direct owned IRQ dispatch. Normal peripheral IRQs run on CPU1.
The runtime preserves FP, ownership and teardown rules; radio code uses no
M-mode transition or HWLoop/PIE state. Private queue-directory locks mask
local IRQs only while admitting and retaining an object.

The pinned SDK/controller set is used as a complete dependency closure. The
compiler resolves its own ILP32F library directory so the build does not depend
on a developer's multilib paths.
