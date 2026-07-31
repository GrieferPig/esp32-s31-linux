# Build System for ESP32-S31 Linux

This project uses a unified `Makefile` at the root directory to manage downloading the toolchain, out-of-tree builds for all components, and flashing the firmware to the board. All build artifacts are cleanly separated into the `build/` directory.

## Build Targets

### Default Target
- **`make all`** (or just **`make`**)
  The default target. It downloads and verifies the pinned prebuilt toolchain,
  then executes `download`, `opensbi`, `linux`, and `initramfs`. It never
  builds the compiler from source.

### Download & Toolchain
- **`make download`**
  Updates git submodules recursively. `make all` installs the prebuilt S31
  toolchain before running this target.
- **`make toolchain`**
  Downloads the pinned ESP32-S31 Linux GCC release from
  `GrieferPig/crosstool-NG-s31`, verifies its SHA256 checksum, and installs the
  compiler as
  `toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc`.
- **`make toolchain-source`**
  Uses the sibling `../crosstool-NG` checkout and
  `configs/riscv32-esp-linux-musl.config` to rebuild the same GCC 15.2,
  binutils 2.45, and musl toolchain from source. This target is intended for
  toolchain development; normal project builds should use `make toolchain`.

### Components (Out-of-Tree Builds)
- **`make opensbi`**
  Builds OpenSBI and dynamically compiles the device tree (DTB) from the Linux source. The FW_JUMP binary and DTB are concatenated and padded to match the bootloader's partition size limit. Output is placed in `build/fw_payload.bin`.
- **`make linux`**
  Builds the Linux kernel (`xipImage`) out-of-tree into `build/linux/`. Outputs `xipImage` and the compiled `esp32s31_generic.dtb` directly to the `build/` root.
- **`make rootfs`**
  Uses the pinned Buildroot submodule and the ESP32-S31 br2-external tree to
  build a complete S31-optimized RV32IMAFBC/musl userspace with HWLoop and PIE
  assembler support. It includes BusyBox, BlueZ tools,
  Dropbear, iproute2, tcpdump, memtester, CoreMark, and the project
  diagnostics. The generated SquashFS is copied to `build/rootfs.sqfs` and
  padded to the rootfs partition size. BusyBox `wget` supports HTTPS and
  HTTP-to-HTTPS redirects using its size-optimized internal TLS client; this
  client encrypts transfers but does not validate CA certificates.
- **`make initramfs`**
  Compatibility name for `make rootfs`; this is the preferred rootfs build
  command for this project.
- **`make buildroot-menuconfig`**
  Opens Buildroot configuration using the ESP32-S31 defconfig. Persist useful
  changes by updating `buildroot-external/configs/esp32s31_rootfs_defconfig`.
- **`make buildroot-clean`**
  Removes only the Buildroot output tree. Use it after changing toolchain or
  package selections; downloaded source archives are retained.

### Cleaning
- **`make clean`**
  Removes the `build/` directory and all out-of-tree Linux, OpenSBI, CoreMark,
  and Buildroot artifacts.
- **`make fullclean`**
  Executes the `clean` target and additionally removes the downloaded `toolchain/` directory, reverting the repository to its freshly-cloned state.

### Flashing
*(Note: These targets dynamically parse the partition table (`bootloader/partitions.csv`) to determine the correct offset for flashing via `esptool`.)*
- **`make flash-opensbi`**
  Flashes the OpenSBI payload (`fw_payload.bin`) to the ESP32-S31.
- **`make flash-linux`**
  Flashes the Linux kernel (`xipImage`) to the ESP32-S31.
- **`make flash-rootfs`**
  Flashes `build/rootfs.sqfs` to the ESP32-S31 rootfs partition.
- **`make bootloader`**
  Dynamically searches for your ESP-IDF installation (looking for `export.sh` up to 5 levels deep in your home folder), sources the environment, and invokes `idf.py build` inside the `bootloader/` directory.
- **`make flash-bootloader`**
  Similar to the above, but invokes `idf.py flash -p /dev/ttyUSB0 -b 2000000` to flash the bootloader.
- **`make erase`**
  Completely erases the entire flash using `esptool erase_flash`.

### Quick Start Example
```bash
# 1. Clean up and build everything from scratch
make fullclean
make toolchain
make all

# 2. Source your ESP-IDF environment (required for esptool)
source ~/.espressif/export.sh

# 3. Build and flash the bootloader
make bootloader
make flash-bootloader

# 4. Flash all firmware partitions
make flash-opensbi flash-linux flash-rootfs
```
