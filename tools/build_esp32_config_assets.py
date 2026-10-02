#!/usr/bin/env python3
"""Install esp32-config capabilities and selected IANA TZif files into an image.

The kernel input is the completed build's .config, after profile trimming and
olddefconfig. Time data comes from Buildroot's host-tzdata POSIX output so an
incremental target that was already pruned can be regenerated faithfully.
"""
import argparse
from pathlib import Path
import re
import shutil
import tempfile


def read_kernel_config(path):
    values = {}
    for line in Path(path).read_text().splitlines():
        match = re.fullmatch(r"CONFIG_([A-Z0-9_]+)=(.*)", line)
        if match:
            values[match[1]] = match[2]
    if not values:
        raise ValueError("kernel .config is missing or empty")
    return values


def kernel_features(config):
    # This image installs the separate radio module, but no general set of
    # peripheral modules. Advertise these controllers only when built in.
    def all_on(*names):
        return all(config.get(name) == "y" for name in names)

    def any_on(*names):
        return any(config.get(name) == "y" for name in names)

    gadget = all_on("USB_GADGET", "USB_CONFIGFS", "USB_DWC2") and any_on(
        "USB_DWC2_DUAL_ROLE", "USB_DWC2_PERIPHERAL")
    features = {
        "gpio": all_on("GPIOLIB", "GPIO_CDEV", "PINCTRL_ESP32S31"),
        "uart": all_on("SERIAL_ESP32"),
        "uart_dma": all_on("SERIAL_ESP32", "SERIAL_ESP32_UHCI", "ESP32S31_AHB_GDMA"),
        "i2c": all_on("I2C", "I2C_ESP32S31"),
        "spi": all_on("SPI", "SPI_ESP32S31"),
        "spi_target": all_on("SPI", "SPI_ESP32S31", "SPI_SLAVE"),
        "mmc": all_on("MMC", "MMC_DW", "MMC_DW_PLTFM"),
        "sound": all_on("SND", "SND_SOC", "SND_SOC_ESP32S31_I2S"),
        "usb_host": all_on("USB", "USB_DWC2") and any_on("USB_DWC2_HOST", "USB_DWC2_DUAL_ROLE"),
        "usb_gadget": gadget,
        "usb_acm": gadget and all_on("USB_CONFIGFS_ACM"),
        "usb_ecm": gadget and all_on("USB_CONFIGFS_ECM"),
        "swap": all_on("SWAP"),
        "usb_storage": all_on("USB_STORAGE", "SCSI", "BLK_DEV_SD"),
        "ext4": all_on("EXT4_FS"),
        "fat": all_on("FAT_FS"),
        "vfat": all_on("VFAT_FS"),
        "zram": all_on("SWAP", "ZRAM"),
        "pwm": all_on("PWM") and any_on("PWM_ESP32S31_LEDC", "PWM_ESP32S31_MCPWM", "PWM_ESP32S31_SDM"),
        "counter": all_on("COUNTER", "ESP32S31_PCNT"),
        "can": all_on("CAN", "CAN_CTUCANFD", "CAN_CTUCANFD_PLATFORM"),
        "ethernet": all_on("ETHERNET", "STMMAC_ETH", "STMMAC_PLATFORM", "DWMAC_GENERIC"),
        "iio": all_on("IIO") and any_on("ESP32S31_ADC", "ESP32S31_DAC", "ESP32S31_TOUCH", "ESP32S31_COMPARATOR"),
        "hwmon": all_on("HWMON", "SENSORS_ESP32S31_TSENS"),
        "watchdog": all_on("WATCHDOG", "ESP32S31_WATCHDOG"),
        "timers": any_on("ESP32S31_SYSTEM_TIMERS", "ESP32S31_GPTIMER"),
        "gdma": any_on("ESP32S31_AXI_GDMA", "ESP32S31_AHB_GDMA"),
        "lp": all_on("REMOTEPROC", "ESP32S31_LP_REMOTEPROC"),
    }
    for key, parent, symbol in [
            ("adc", "IIO", "ESP32S31_ADC"), ("dac", "IIO", "ESP32S31_DAC"),
            ("touch", "IIO", "ESP32S31_TOUCH"), ("comparator", "IIO", "ESP32S31_COMPARATOR"),
            ("ledc", "PWM", "PWM_ESP32S31_LEDC"), ("mcpwm", "PWM", "PWM_ESP32S31_MCPWM"),
            ("sdm", "PWM", "PWM_ESP32S31_SDM")]:
        features[key] = all_on(parent, symbol)
    features.update({
        "axi_gdma": all_on("ESP32S31_AXI_GDMA"),
        "ahb_gdma": all_on("ESP32S31_AHB_GDMA"),
        "system_timers": all_on("ESP32S31_SYSTEM_TIMERS"),
        "gptimer": all_on("ESP32S31_GPTIMER"),
    })
    return {name: int(value) for name, value in features.items()}


def read_zones(path):
    zones = []
    for raw in Path(path).read_text().splitlines():
        zone = raw.split("#", 1)[0].strip()
        if not zone:
            continue
        if not re.fullmatch(r"[A-Za-z0-9_+-]+(?:/[A-Za-z0-9_+-]+)*", zone):
            raise ValueError(f"invalid time zone path: {zone}")
        if zone in zones:
            raise ValueError(f"duplicate time zone: {zone}")
        zones.append(zone)
    if "Etc/UTC" not in zones:
        raise ValueError("the retained time zones must include Etc/UTC")
    return zones


def install_timezones(target, source, zones):
    share = target / "usr/share"
    share.mkdir(parents=True, exist_ok=True)
    destination = share / "zoneinfo"
    stage = Path(tempfile.mkdtemp(prefix=".zoneinfo-", dir=share))
    previous = share / ".zoneinfo-previous"
    try:
        # Read and validate every selected source before replacing any data.
        for zone in zones:
            payload = (source / zone).read_bytes()
            if not payload.startswith(b"TZif"):
                raise ValueError(f"not an IANA TZif file: {source / zone}")
            output = stage / zone
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(payload)
            output.chmod(0o644)
        stage.chmod(0o755)
        for directory in stage.rglob("*"):
            if directory.is_dir():
                directory.chmod(0o755)
        if previous.exists():
            shutil.rmtree(previous)
        if destination.exists():
            destination.rename(previous)
        try:
            stage.rename(destination)
        except OSError:
            if previous.exists():
                previous.rename(destination)
            raise
        if previous.exists():
            shutil.rmtree(previous)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    etc = target / "etc"
    etc.mkdir(parents=True, exist_ok=True)
    localtime = etc / "localtime"
    localtime.unlink(missing_ok=True)
    localtime.symlink_to("../usr/share/zoneinfo/Etc/UTC")
    (etc / "timezone").write_text("Etc/UTC\n")


def install_assets(target, kernel_config, timezone_source, zones_file):
    config = read_kernel_config(kernel_config)
    zones = read_zones(zones_file)
    install_timezones(target, timezone_source, zones)
    output = target / "usr/share/esp32-config"
    output.mkdir(parents=True, exist_ok=True)
    features = kernel_features(config)
    (output / "kernel-features").write_text("".join(f"{key}={features[key]}\n" for key in sorted(features)))
    (output / "timezones").write_text("".join(f"{zone}\n" for zone in zones))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target-dir", required=True, type=Path)
    parser.add_argument("--kernel-config", required=True, type=Path)
    parser.add_argument("--timezone-source", required=True, type=Path)
    parser.add_argument("--timezones-list", required=True, type=Path)
    args = parser.parse_args()
    try:
        install_assets(args.target_dir, args.kernel_config, args.timezone_source, args.timezones_list)
    except (ValueError, OSError) as error:
        parser.exit(1, f"esp32-config image assets: {error}\n")


if __name__ == "__main__":
    main()
