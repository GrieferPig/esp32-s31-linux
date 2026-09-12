#!/usr/bin/env python3
"""Exercise actual overlay CLI bookkeeping, with only device I/O mocked."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class OverlayPersistence(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="s31-overlay-test-")
        cls.work = Path(cls.temp.name)
        cls.state = cls.work / "state"
        cls.state.mkdir()
        source = (ROOT / "rootfs/s31_overlay.c").read_text()
        for key, value in {
            "OVERLAY_DEVICE": "/dev/null",
            "CURRENT_FILE": str(cls.state / "current"),
            "LOCK_FILE": str(cls.state / "lock"),
            "CONFIG_DIR": str(cls.state),
        }.items():
            source = re.sub(r"^#define " + key + r" .*", f'#define {key} "{value}"', source, flags=re.M)
        (cls.work / "overlay.c").write_text(source)
        # Only the kernel acceptance boundary is mocked. Persistence, locking,
        # parsing and CLI status propagation are the actual target code.
        (cls.work / "mock.c").write_text("int __wrap_ioctl(int fd, unsigned long op, ...) { return 0; }\n")
        include = cls.work / "include/linux"
        include.mkdir(parents=True)
        (include / "esp32s31-overlay.h").write_bytes(
            (ROOT / "linux-esp32-s31/include/uapi/linux/esp32s31-overlay.h").read_bytes())
        fdt = ROOT / "linux-esp32-s31/scripts/dtc/libfdt"
        subprocess.run(["cc", "-O2", "-Wall", "-Werror", "-I" + str(fdt),
                        "-I" + str(include.parent), str(cls.work / "overlay.c"),
                        str(cls.work / "mock.c"), *map(str, fdt.glob("*.c")),
                        "-Wl,--wrap=ioctl", "-o", str(cls.work / "overlay")], check=True)
        cls.blobs = cls.work / "blobs"
        cls.blobs.mkdir()
        for name in ("uart1", "uart2", "i2c0", "i2c1", "gpspi2", "gpspi3"):
            # No route patching requested: load_blob forwards opaque DTBO bytes.
            (cls.blobs / f"esp32s31-overlay-{name}.dtbo").write_bytes(b"mock DTBO")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        for p in self.state.iterdir():
            p.unlink()
        self.env = dict(os.environ, S31_OVERLAY_DIR=str(self.blobs),
                        S31_OVERLAY_PERSIST=str(self.state / "overlays.conf"))

    def run_cli(self, *args, rc=0):
        p = subprocess.run([str(self.work / "overlay"), *args], env=self.env, capture_output=True, text=True)
        self.assertEqual(p.returncode, rc, p.stderr)
        return p

    def persisted(self):
        p = self.state / "overlays.conf"
        return p.read_text() if p.exists() else ""

    def test_volatile_apply_never_enters_later_persistent_apply(self):
        self.run_cli("apply", "uart1", "--volatile")
        self.run_cli("apply", "uart2")
        self.assertEqual(self.persisted(), "overlay.uart2=uart2\n")
        self.assertEqual((self.state / "current").read_text(), "uart1\nuart2\n")

    def test_volatile_remove_does_not_delete_desired_selection(self):
        self.run_cli("apply", "uart1")
        self.run_cli("remove", "uart1", "--volatile")
        self.run_cli("apply", "uart2")
        self.assertEqual(self.persisted(), "overlay.uart1=uart1\noverlay.uart2=uart2\n")
        self.assertEqual((self.state / "current").read_text(), "uart2\n")

    def test_normal_remove_preserves_unrelated_desired_entries(self):
        self.run_cli("apply", "uart1")
        self.run_cli("apply", "uart2")
        self.run_cli("remove", "uart1", "--volatile")
        self.run_cli("remove", "uart2")
        self.assertEqual(self.persisted(), "overlay.uart1=uart1\n")

    def test_persistent_remove_all_clears_desired_set(self):
        self.run_cli("apply", "uart1")
        self.run_cli("remove", "--all")
        self.assertEqual(self.persisted(), "")

    def test_parallel_apply_does_not_lose_updates(self):
        names = ("uart1", "uart2", "i2c0", "i2c1", "gpspi2", "gpspi3")
        jobs = [subprocess.Popen([str(self.work / "overlay"), "apply", name], env=self.env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE) for name in names]
        for job in jobs:
            stdout, stderr = job.communicate(timeout=10)
            self.assertEqual(job.returncode, 0, stderr)
        self.assertEqual(set(self.persisted().splitlines()), {f"overlay.{n}={n}" for n in names})

    def test_corrupt_persist_fails_before_active_state_changes(self):
        (self.state / "overlays.conf").write_text("overlay.uart1=uart2\n")
        self.run_cli("apply", "uart2", rc=1)
        self.assertFalse((self.state / "current").exists())

    def test_save_failure_reports_that_overlay_is_already_active(self):
        (self.state / "overlays.conf.tmp").mkdir()
        try:
            p = self.run_cli("apply", "uart1", rc=1)
            self.assertIn("overlay applied", p.stderr)
            self.assertEqual((self.state / "current").read_text(), "uart1\n")
            self.assertEqual(self.persisted(), "")
        finally:
            (self.state / "overlays.conf.tmp").rmdir()


if __name__ == "__main__":
    unittest.main()
