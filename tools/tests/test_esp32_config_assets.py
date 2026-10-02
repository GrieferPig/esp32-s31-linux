#!/usr/bin/env python3
"""Verify capability gates and repeatable, small timezone installation."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("assets", REPO / "tools/build_esp32_config_assets.py")
ASSETS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ASSETS)


class Capabilities(unittest.TestCase):
    def test_missing_controller_is_not_a_supported_interface(self):
        features = ASSETS.kernel_features({"I2C": "y", "SPI": "y", "USB_GADGET": "y"})
        self.assertEqual(features["i2c"], 0)
        self.assertEqual(features["spi"], 0)
        self.assertEqual(features["usb_acm"], 0)
        self.assertEqual(features["usb_gadget"], 0)

    def test_built_in_only_matches_module_packaging(self):
        config = {"I2C": "y", "I2C_ESP32S31": "m", "ESP32S31_AHB_GDMA": "y"}
        features = ASSETS.kernel_features(config)
        self.assertEqual(features["i2c"], 0)
        self.assertEqual(features["gdma"], 1)
        self.assertEqual(features["ahb_gdma"], 1)
        self.assertEqual(features["axi_gdma"], 0)

    def test_usb_functions_require_controller_role_and_function(self):
        config = {key: "y" for key in ["USB_GADGET", "USB_CONFIGFS", "USB_DWC2", "USB_CONFIGFS_ACM"]}
        self.assertEqual(ASSETS.kernel_features(config)["usb_acm"], 0)
        config["USB_DWC2_DUAL_ROLE"] = "y"
        features = ASSETS.kernel_features(config)
        self.assertEqual(features["usb_acm"], 1)
        self.assertEqual(features["usb_ecm"], 0)

    def test_target_mode_requires_spi_target_support(self):
        config = {"SPI": "y", "SPI_ESP32S31": "y"}
        self.assertEqual(ASSETS.kernel_features(config)["spi_target"], 0)
        config["SPI_SLAVE"] = "y"
        self.assertEqual(ASSETS.kernel_features(config)["spi_target"], 1)

    def test_lean_missing_subsystems_remain_zero(self):
        config = {key: "y" for key in ["GPIOLIB", "GPIO_CDEV", "PINCTRL_ESP32S31",
                                      "SERIAL_ESP32", "USB", "USB_DWC2", "USB_DWC2_HOST"]}
        features = ASSETS.kernel_features(config)
        self.assertEqual(features["gpio"], 1)
        self.assertEqual(features["uart"], 1)
        self.assertEqual(features["usb_host"], 1)
        for key in ["i2c", "spi", "mmc", "sound", "usb_acm", "usb_ecm", "zram", "pwm", "iio", "hwmon"]:
            self.assertEqual(features[key], 0, key)


class ImageAssets(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.target = self.root / "target"
        self.source = self.root / "host/share/zoneinfo/posix"
        self.config = self.root / ".config"
        self.config.write_text("CONFIG_GPIO_CDEV=y\nCONFIG_GPIOLIB=y\nCONFIG_PINCTRL_ESP32S31=y\n")
        self.zones = self.root / "zones.list"
        self.zones.write_text("Etc/UTC\nAmerica/Los_Angeles\n")
        for name in ["Etc/UTC", "America/Los_Angeles", "Asia/Shanghai"]:
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"TZif2\x00" + name.encode())

    def tearDown(self):
        self.tmp.cleanup()

    def install(self):
        ASSETS.install_assets(self.target, self.config, self.source, self.zones)

    def test_selected_real_files_and_localtime_link(self):
        stale = self.target / "usr/share/zoneinfo/right/old-zone"
        stale.parent.mkdir(parents=True)
        stale.write_text("old")
        self.install()
        zone_root = self.target / "usr/share/zoneinfo"
        files = sorted(str(p.relative_to(zone_root)) for p in zone_root.rglob("*") if p.is_file())
        self.assertEqual(files, ["America/Los_Angeles", "Etc/UTC"])
        self.assertFalse((zone_root / "America/Los_Angeles").is_symlink())
        self.assertEqual(zone_root.stat().st_mode & 0o777, 0o755)
        self.assertEqual((self.target / "etc/localtime").readlink(), Path("../usr/share/zoneinfo/Etc/UTC"))
        self.assertIn("gpio=1\n", (self.target / "usr/share/esp32-config/kernel-features").read_text())

    def test_incremental_regeneration_uses_unpruned_host_source(self):
        self.install()
        self.zones.write_text("Etc/UTC\nAsia/Shanghai\n")
        self.install()
        self.assertTrue((self.target / "usr/share/zoneinfo/Asia/Shanghai").is_file())
        self.assertFalse((self.target / "usr/share/zoneinfo/America/Los_Angeles").exists())

    def test_missing_zone_leaves_existing_target_data(self):
        self.install()
        before = (self.target / "usr/share/zoneinfo/Etc/UTC").read_bytes()
        self.zones.write_text("Etc/UTC\nMissing/Zone\n")
        with self.assertRaises(FileNotFoundError):
            self.install()
        self.assertEqual((self.target / "usr/share/zoneinfo/Etc/UTC").read_bytes(), before)

    def test_zone_paths_cannot_escape_source(self):
        self.zones.write_text("Etc/UTC\n../../etc/shadow\n")
        with self.assertRaises(ValueError):
            self.install()

    def test_no_config_does_not_generate_false_capabilities(self):
        self.config.write_text("")
        with self.assertRaises(ValueError):
            self.install()
        self.assertFalse((self.target / "usr/share/esp32-config/kernel-features").exists())


if __name__ == "__main__":
    unittest.main()
