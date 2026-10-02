#!/usr/bin/env python3
"""Fresh installations enable Wi-Fi and Bluetooth without overriding saved choices."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
COMMON = REPO / "buildroot-external/board/esp32-s31/overlay/usr/lib/esp32-config/common.sh"


class NetworkDefaults(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.conf = self.root / "conf"
        self.run = self.root / "run"
        self.common = self.root / "common.sh"
        # Keep ensure_config's required runtime directory inside this test's sandbox.
        source = COMMON.read_text().replace("/run/wpa_supplicant", str(self.root / "wpa_supplicant"))
        self.common.write_text(source)
        self.env = dict(os.environ, ESP32_CONFIG_DIR=str(self.conf),
                        ESP32_CONFIG_RUN_DIR=str(self.run))

    def ensure_config(self):
        return subprocess.run(
            ["/bin/sh", "-c", f'. "{self.common}"; ensure_config'],
            env=self.env, text=True, capture_output=True, check=True)

    def test_fresh_config_enables_both_radios(self):
        self.ensure_config()
        self.assertIn("enabled=1\n", (self.conf / "wifi.conf").read_text())
        self.assertIn("enabled=1\n", (self.conf / "bluetooth.conf").read_text())

    def test_existing_disabled_settings_are_preserved(self):
        self.conf.mkdir()
        (self.conf / "wifi.conf").write_text("enabled=0\ninterface=wlan0\ndhcp=1\n")
        (self.conf / "bluetooth.conf").write_text("enabled=0\nindex=0\nle=1\n")
        self.ensure_config()
        self.assertTrue((self.conf / "wifi.conf").read_text().startswith("enabled=0\n"))
        self.assertTrue((self.conf / "bluetooth.conf").read_text().startswith("enabled=0\n"))

    def test_early_radio_start_initializes_fresh_config_before_selection(self):
        source = (REPO / "buildroot-external/board/esp32-s31/overlay/etc/init.d/S00s31-radio").read_text()
        script = source
        for prefix in ("/sys", "/proc", "/run", "/dev", "/etc", "/usr/lib", "/usr/sbin"):
            script = script.replace(prefix + "/", str(self.root) + prefix + "/")
        start = self.root / "start-radio"
        start.write_text(script)
        start.chmod(0o755)
        for name in ("proc/cmdline", "proc/modules"):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        dev_null = self.root / "dev/null"
        dev_null.parent.mkdir(parents=True, exist_ok=True)
        dev_null.symlink_to("/dev/null")
        common = self.root / "usr/lib/esp32-config/common.sh"
        common.parent.mkdir(parents=True, exist_ok=True)
        common.write_text(self.common.read_text())
        overlay_log = self.root / "overlay.log"
        overlay = self.root / "usr/sbin/s31-overlay"
        overlay.parent.mkdir(parents=True, exist_ok=True)
        overlay.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >>"$OVERLAY_LOG"\n')
        overlay.chmod(0o755)
        env = dict(self.env, OVERLAY_LOG=str(overlay_log))
        result = subprocess.run(["/bin/sh", str(start), "start"], env=env,
                                text=True, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("enabled=1\n", (self.conf / "wifi.conf").read_text())
        self.assertIn("enabled=1\n", (self.conf / "bluetooth.conf").read_text())
        self.assertIn("apply radio-combo", overlay_log.read_text())


if __name__ == "__main__":
    unittest.main()
