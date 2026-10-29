# ESP32-S31 radio bundle licensing boundary

`esp32s31-radio.ko` combines the Linux Wi-Fi/HCI frontends, the S31
compatibility runtime, and a narrow loader. The locally generated ESP-IDF
radio dependency closure remains a separate, versioned
payload derived from `esp32s31-radio-fw-v1.o`. The default `radio.bin` is
prelinked for direct Flash XIP; its writable template is copied to a kernel
RAM arena and module imports are bound at runtime. It is not statically linked
into the kernel module.

The deployable module, radio overlays, build configuration and this notice are
stored together with the payload in the engineering/release package. In XIP
mode, the module and overlays reside in the root filesystem and the flat
payload occupies the dedicated `radio-bundle` MTD partition.

That separation is useful for deployment and for keeping exact source
identities together, but it is only an engineering boundary and is not a
legal conclusion. The module imports GPL-only kernel symbols and is therefore
marked `GPL v2` for Linux module-loader compatibility. The external payload
can include Apache-2.0 and Espressif-supplied components whose redistribution
and runtime-combination terms must be reviewed separately.

The default package mode is consequently `engineering-only`. Public release
mode is intentionally gated on both:

1. written permission or a compatible licensing grant for the complete radio
   payload; and
2. an exact corresponding-source archive for the redistributable payload and
   the module sources.

Keep the grant, source archive, toolchain identity, ESP-IDF commit, generated
sdkconfig, module, overlays, notices and checksums together for each release.
