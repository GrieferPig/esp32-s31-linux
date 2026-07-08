# Build System for ESP32-S31 Linux

This project uses a unified `Makefile` at the root directory to manage downloading the toolchain, out-of-tree builds for all components, and flashing the firmware to the board. All build artifacts are cleanly separated into the `build/` directory.

## Build Targets

### Default Target
- **`make all`** (or just **`make`**)
  The default target. It sequentially executes the following targets: `download`, `opensbi`, `linux`, `busybox`, and `initramfs`. 

### Download & Toolchain
- **`make download`**
  Updates git submodules recursively, downloads the `riscv32imac-musl` cross-compilation toolchain from GitHub releases (if not already downloaded), and extracts it into the `toolchain/` directory.

### Components (Out-of-Tree Builds)
- **`make opensbi`**
  Builds OpenSBI and dynamically compiles the device tree (DTB) from the Linux source. The FW_JUMP binary and DTB are concatenated and padded to match the bootloader's partition size limit. Output is placed in `build/fw_payload.bin`.
- **`make linux`**
  Builds the Linux kernel (`xipImage`) out-of-tree into `build/linux/`. Outputs `xipImage` and the compiled `esp32s31_generic.dtb` directly to the `build/` root.
- **`make busybox`**
  Configures and compiles Busybox out-of-tree (`build/busybox/`) using `allyesconfig` and a custom configuration fragment.
- **`make initramfs`**
  Compiles custom initramfs tools (`segfault`, `forktest`) via `initramfs/Makefile`, merges them into the Busybox installation in `build/s31-initramfs-root/`, creates a `cpio` archive, and pads it to the initramfs partition size limit (`build/initramfs.cpio`).

### Cleaning
- **`make clean`**
  Removes the `build/` directory and all its contents, effectively cleaning all out-of-tree build artifacts for Linux, OpenSBI, Busybox, and the initramfs tools.
- **`make fullclean`**
  Executes the `clean` target and additionally removes the downloaded `toolchain/` directory, reverting the repository to its freshly-cloned state.

### Flashing
*(Note: These targets dynamically parse the partition table (`bootloader/partitions.csv`) to determine the correct offset for flashing via `esptool`.)*
- **`make flash-opensbi`**
  Flashes the OpenSBI payload (`fw_payload.bin`) to the ESP32-S31.
- **`make flash-linux`**
  Flashes the Linux kernel (`xipImage`) to the ESP32-S31.
- **`make flash-initramfs`**
  Flashes the padded initramfs `cpio` archive to the ESP32-S31.
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
make all

# 2. Source your ESP-IDF environment (required for esptool)
source ~/.espressif/export.sh

# 3. Build and flash the bootloader
make bootloader
make flash-bootloader

# 4. Flash all firmware partitions
make flash-opensbi flash-linux flash-initramfs
```
