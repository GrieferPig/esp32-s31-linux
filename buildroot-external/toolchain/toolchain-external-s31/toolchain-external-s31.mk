# SPDX-License-Identifier: BSD-2-Clause
TOOLCHAIN_EXTERNAL_S31_VERSION = esp32s31-linux-gcc-15.2.0-5
TOOLCHAIN_EXTERNAL_S31_SITE = https://github.com/GrieferPig/crosstool-NG-s31/releases/download/$(TOOLCHAIN_EXTERNAL_S31_VERSION)
TOOLCHAIN_EXTERNAL_S31_SOURCE = riscv32-esp-linux-musl.tar.xz
# This release contains bin/, lib/ and the sysroot directly at archive root.
TOOLCHAIN_EXTERNAL_S31_STRIP_COMPONENTS = 0

$(eval $(toolchain-external-package))
