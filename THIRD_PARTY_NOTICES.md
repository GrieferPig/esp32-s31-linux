# Third-Party Notices

## Espressif ESP32-S31 Linux ports

This project contains code derived from Espressif Systems' ESP32-S31 ports:

- [esp-linux-bsp commit a77a06f03068d25799ddd719566df50207f28c5a](https://github.com/espressif/esp-linux-bsp/commit/a77a06f03068d25799ddd719566df50207f28c5a), licensed under the Apache License 2.0. A copy is provided in [`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt).
- [OpenSBI commit a93bce66b102966b0612c0afb5c9872a4737fe68](https://github.com/espressif/opensbi/commit/a93bce66b102966b0612c0afb5c9872a4737fe68), licensed under the BSD 2-Clause License. See [`external/opensbi/COPYING.BSD`](external/opensbi/COPYING.BSD).
- [U-Boot integration/v2024.07-esp32s31](https://github.com/espressif/u-boot/tree/integration/v2024.07-esp32s31), generally licensed under GPL-2.0-or-later, subject to the SPDX identifier in each file. See [`external/u-boot/Licenses/README`](external/u-boot/Licenses/README).
- [Linux integration/v6.18-esp32s31](https://github.com/espressif/linux/tree/integration/v6.18-esp32s31), generally licensed under GPL-2.0-only, subject to the SPDX identifier in each file. See [`external/linux/COPYING`](external/linux/COPYING).

The Espressif-derived code has been modified for dual-hart SMP, hart 0 device-interrupt affinity, direct S-mode timer delivery, XIP OpenSBI, and the local radio architecture. File-level notices identify the portions derived from the BSP and OpenSBI ports.

ESP32 and Espressif are trademarks of Espressif Systems. This project is not an official Espressif product.

## OASIS PKCS #11 interface definitions

`src/userspace/crypto/pkcs11/pkcs11.h`, `pkcs11t.h` and `pkcs11f.h` are unmodified
OASIS PKCS #11 v2.40 Errata 01 interface headers, Copyright OASIS Open 2016.
Their original OASIS IPR Policy notices remain in each file. They come from
[OASIS commit 9ef73f9f44ed8a240b05484427a0851ab36be251](https://github.com/oasis-tcs/pkcs11/tree/9ef73f9f44ed8a240b05484427a0851ab36be251/published/2-40-errata-1).
The accompanying implementation is covered by its own Apache-2.0 file notices.
