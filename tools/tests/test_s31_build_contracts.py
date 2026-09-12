#!/usr/bin/env python3
"""Exercise build identity invalidation and reject drifting layout contracts."""
import importlib.util
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("layout", ROOT / "tools/check_s31_layout.py")
layout = importlib.util.module_from_spec(spec)
spec.loader.exec_module(layout)

class Layout(unittest.TestCase):
    def test_overlay_lock_supported_by_kernel_profile(self):
        self.assertIn("CONFIG_FILE_LOCKING=y", (ROOT / "linux-esp32-s31/arch/riscv/configs/esp32s31_defconfig").read_text())
        self.assertIn("--enable FILE_LOCKING", (ROOT / "Makefile").read_text())

    def test_live_layout(self):
        layout.check()
        _, sizes = layout.flash_layout()
        self.assertEqual(sizes["PERSIST"], 0x90000)

    def test_drift_is_rejected(self):
        files = ["configs/esp32s31-layout.cfg", "shared/s31_memory_layout.h", "Makefile", "linux-esp32-s31/drivers/platform/esp32s31-radio-smode.c", "linux-esp32-s31/arch/riscv/kernel/vmlinux-xip.lds.S", "linux-esp32-s31/arch/riscv/boot/dts/espressif/esp32s31.dtsi"]
        for target, old, new in [(files[0], "SLOT_KERNEL=0x500000", "SLOT_KERNEL=0x520000"), (files[3], "0x2f071800UL", "0x2f071900UL")]:
            with self.subTest(target=target), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                for name in files:
                    p = root / name
                    p.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(ROOT / name, p)
                p = root / target
                p.write_text(p.read_text().replace(old, new))
                with self.assertRaises(ValueError):
                    layout.check(root)

class FlashGuards(unittest.TestCase):
    def test_oversize_existing_image_fails_before_serial_access(self):
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / "radio.sqfs"
            with image.open("wb") as f:
                f.truncate(layout.flash_layout()[1]["RADIO"] + 1)
            p = subprocess.run(["make", "--no-print-directory", "flash-existing-radio", "RADIO_FS_IMG=" + str(image)], cwd=ROOT, text=True, capture_output=True)
            self.assertNotEqual(p.returncode, 0)
            self.assertIn("exceeds capacity", p.stderr)
            self.assertNotIn("esptool", p.stdout)

    def test_retired_target_has_no_build_side_effect(self):
        p = subprocess.run(["make", "--no-print-directory", "radio-image"], cwd=ROOT, text=True, capture_output=True)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("radio-image is retired", p.stderr)
        self.assertNotIn("Entering directory", p.stdout)

class Stamp(unittest.TestCase):
    def test_noop_and_changed_input_invalidate_make(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "header.h").write_text("one")
            (root / "Makefile").write_text(".PHONY: FORCE\nall: object\nFORCE:\n.stamp: FORCE\n\tpython3 " + str(ROOT / "tools/write_build_stamp.py") + " --output $@ --file header.h --value=$(FLAGS)\nobject: .stamp\n\t@echo rebuilt >> events\n\t@touch $@\n")
            def run(flags):
                subprocess.run(["make", "-s", "FLAGS=" + flags], cwd=root, check=True, capture_output=True)
                return len((root / "events").read_text().splitlines())
            self.assertEqual(run("one"), 1)
            self.assertEqual(run("one"), 1)
            self.assertEqual(run("two"), 2)
            (root / "header.h").write_text("two")
            self.assertEqual(run("two"), 3)
            self.assertEqual(run("two"), 3)

class OverlayAbi(unittest.TestCase):
    def test_fixed_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            include = Path(tmp) / "include/linux"
            include.mkdir(parents=True)
            shutil.copyfile(ROOT / "linux-esp32-s31/include/uapi/linux/esp32s31-overlay.h", include / "esp32s31-overlay.h")
            source = Path(tmp) / "abi.c"
            source.write_text('#include <stddef.h>\n#include <linux/esp32s31-overlay.h>\n_Static_assert(sizeof(struct s31_overlay_item) == 48, "item ABI");\n_Static_assert(offsetof(struct s31_overlay_item, gpios) == 40, "mask ABI");\n_Static_assert(sizeof(struct s31_overlay_list) == 1544, "list ABI");\n_Static_assert(_IOC_SIZE(S31_OVERLAY_IOC_LIST) == 1544, "ioctl ABI");\n')
            subprocess.run(["cc", "-Werror", "-I" + str(include.parent), "-c", str(source), "-o", str(Path(tmp) / "abi.o")], check=True)

if __name__ == "__main__":
    unittest.main()
