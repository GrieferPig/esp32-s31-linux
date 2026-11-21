#!/usr/bin/env python3
"""Generate setup capabilities from the completed kernel configuration."""
import argparse
from pathlib import Path
import re


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
    # Report controllers guaranteed to be available without module loading.
    # Buildroot may also install optional modules selected by the user.
    def all_on(*names):
        return all(config.get(name) == "y" for name in names)

    def any_on(*names):
        return any(config.get(name) == "y" for name in names)

    gadget = all_on("USB_GADGET", "USB_CONFIGFS", "USB_DWC2") and any_on(
        "USB_DWC2_DUAL_ROLE", "USB_DWC2_PERIPHERAL")
    features = {
        "uart": all_on("SERIAL_ESP32"),
        "uart_dma": all_on("SERIAL_ESP32", "SERIAL_ESP32_UHCI", "ESP32S31_AHB_GDMA"),
        "i2c": all_on("I2C", "I2C_ESP32S31"),
        "spi": all_on("SPI", "SPI_ESP32S31"),
        "spi_target": all_on("SPI", "SPI_ESP32S31", "SPI_SLAVE"),
        "mmc": all_on("MMC", "MMC_DW", "MMC_DW_PLTFM"),
        "sound": all_on("SND", "SND_SOC", "SND_SOC_ESP32S31_I2S"),
        "usb_gadget": gadget,
        "usb_acm": gadget and all_on("USB_CONFIGFS_ACM"),
        "usb_ecm": gadget and all_on("USB_CONFIGFS_ECM"),
        "counter": all_on("COUNTER", "ESP32S31_PCNT"),
        "can": all_on("CAN", "CAN_CTUCANFD", "CAN_CTUCANFD_PLATFORM"),
        "ethernet": all_on("ETHERNET", "STMMAC_ETH", "STMMAC_PLATFORM", "DWMAC_GENERIC"),
        "hwmon": all_on("HWMON", "SENSORS_ESP32S31_TSENS"),
        "watchdog": all_on("WATCHDOG", "ESP32S31_WATCHDOG"),
        "lp": all_on("REMOTEPROC", "ESP32S31_LP_REMOTEPROC"),
    }
    for key, parent, symbol in [
            ("adc", "IIO", "ESP32S31_ADC"), ("dac", "IIO", "ESP32S31_DAC"),
            ("touch", "IIO", "ESP32S31_TOUCH"), ("comparator", "IIO", "ESP32S31_COMPARATOR"),
            ("ledc", "PWM", "PWM_ESP32S31_LEDC"), ("mcpwm", "PWM", "PWM_ESP32S31_MCPWM"),
            ("sdm", "PWM", "PWM_ESP32S31_SDM")]:
        features[key] = all_on(parent, symbol)
    features.update({
        "ahb_gdma": all_on("ESP32S31_AHB_GDMA"),
        "system_timers": all_on("ESP32S31_SYSTEM_TIMERS"),
        "gptimer": all_on("ESP32S31_GPTIMER"),
    })
    return {name: int(value) for name, value in features.items()}


def install_assets(target, kernel_config):
    config = read_kernel_config(kernel_config)
    output = target / "usr/share/esp32-config"
    output.mkdir(parents=True, exist_ok=True)
    features = kernel_features(config)
    (output / "kernel-features").write_text("".join(f"{key}={features[key]}\n" for key in sorted(features)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target-dir", required=True, type=Path)
    parser.add_argument("--kernel-config", required=True, type=Path)
    args = parser.parse_args()
    try:
        install_assets(args.target_dir, args.kernel_config)
    except (ValueError, OSError) as error:
        parser.exit(1, f"esp32-config image assets: {error}\n")


if __name__ == "__main__":
    main()
