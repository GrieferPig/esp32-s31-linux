"""Gapless flash capacities, packaging bounds and cross-layer map contracts.

These are host-side contract checks, not hardware or emulator boot evidence.
"""
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('compact_layout', ROOT / 'tools/checks/layout.py')
layout = importlib.util.module_from_spec(spec)
spec.loader.exec_module(layout)

class CompactLayout(unittest.TestCase):
    def test_exact_gapless_geometry(self):
        slots, sizes = layout.flash_layout()
        self.assertEqual(sum(sizes.values()) + 0x2000, 16 * 1024 * 1024)
        for left, right in zip(layout.SLOTS, layout.SLOTS[1:]):
            self.assertEqual(slots['SLOT_' + left] + sizes[left], slots['SLOT_' + right])
        self.assertEqual(slots['SLOT_ROOTFS'] + sizes['ROOTFS'], slots['FLASH_SIZE'])
        self.assertEqual(sizes['PERSIST'], 2120 * 1024)

    def test_every_slot_rejects_one_byte_oversize_and_empty(self):
        _, sizes = layout.flash_layout()
        with tempfile.TemporaryDirectory() as temp:
            image = Path(temp) / 'payload'
            for key, size in sizes.items():
                for length, success in [(0, False), (size, True), (size + 1, False)]:
                    with self.subTest(slot=key, size=length):
                        with image.open('wb') as output:
                            output.truncate(length)
                        result = subprocess.run(['python3', str(ROOT / 'tools/checks/layout.py'),
                                                 '--image-slot', key, str(image)], capture_output=True)
                        self.assertEqual(result.returncode == 0, success)

    def test_packager_checks_real_slot_boundaries_before_merge(self):
        slots, sizes = layout.flash_layout()
        names = {'SPL': 'spl_app.bin', 'UBOOT_ITB': 'u-boot.itb',
                 'DTB': 'esp32s31_generic.dtb', 'RADIO': 'radio.bin',
                 'KERNEL': 'xipImage', 'ROOTFS': 'rootfs.sqfs'}
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            tool = directory / 'mock-esptool'
            tool.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$ESP_CALLS"\nprintf x > s31_full_flash.bin\n')
            tool.chmod(0o755)
            calls = directory / 'calls'
            env = dict(os.environ, ESP_ESPTOOL=str(tool), ESP_CALLS=str(calls))
            argv = ['bash', str(ROOT / 'tools/build/flash_image.sh'),
                    str(ROOT / 'configs/esp32s31-layout.cfg'), str(directory)]
            for key, name in names.items():
                with (directory / name).open('wb') as output:
                    output.truncate(sizes[key])
            valid = subprocess.run(argv, env=env, capture_output=True)
            self.assertEqual(valid.returncode, 0, valid.stderr)
            args = calls.read_text().splitlines()
            for key, name in names.items():
                self.assertEqual(int(args[args.index(name) - 1], 0), slots['SLOT_' + key])
            self.assertNotIn('persist.jffs2', args)
            for key, name in names.items():
                for length in (0, sizes[key] + 1):
                    with self.subTest(slot=key, size=length):
                        calls.unlink(missing_ok=True)
                        with (directory / name).open('wb') as output:
                            output.truncate(length)
                        failed = subprocess.run(argv, env=env, capture_output=True)
                        self.assertNotEqual(failed.returncode, 0)
                        self.assertFalse(calls.exists(), 'merge must not run after rejected payload')
                with (directory / name).open('wb') as output:
                    output.truncate(sizes[key])

    def test_radio_builder_matches_driver(self):
        driver = layout.defines((ROOT / 'linux-esp32-s31/drivers/platform/esp32s31-radio-xip.h').read_text())
        builder = (ROOT / 'tools/build/radio_image.py').read_text()
        for field, macro in [('BASE', 'S31_XIP_BASE'), ('SLOT_SIZE', 'S31_XIP_SLOT_SIZE')]:
            value = int(re.search(r'^' + field + r' = (0x[0-9A-Fa-f]+)$', builder, re.M)[1], 0)
            self.assertEqual(value, driver[macro])
        self.assertEqual(driver['S31_XIP_BASE'], 0xbe06e000)
        self.assertEqual(driver['S31_XIP_PHYS'], 0x4006e000)
        self.assertEqual(driver['S31_XIP_BASE'] % 0x400000, driver['S31_XIP_PHYS'] % 0x400000)

    def test_mtd_raw_identity_window(self):
        source = (ROOT / 'linux-esp32-s31/drivers/mtd/devices/esp32s31_flash.c').read_text()
        for key, expected in [('XIP_BASE', 0x40000000), ('XIP_SIZE', 0x1000000), ('RAW_OFFSET', 0)]:
            value = re.search(r'#define ESP32S31_FLASH_' + key + r'\s+(0x[0-9A-Fa-f]+)', source)[1]
            self.assertEqual(int(value, 0), expected)
        dts = (ROOT / 'linux-esp32-s31/arch/riscv/boot/dts/espressif/esp32s31.dtsi').read_text()
        self.assertNotIn('hil-scratch', dts)
        self.assertRegex(dts, r'label = "spl";\s*reg = <0x002000 0x00c000>;')

    def test_boot_link_and_linux_megapage_addresses(self):
        config = (ROOT / 'u-boot-esp32-s31/configs/espressif_esp32s31_defconfig').read_text()
        self.assertIn('CONFIG_SPL_OPENSBI_LOAD_ADDR=0x4000e400', config)
        self.assertIn('CONFIG_BOOTCOMMAND="booti 0x40400000 - 0x4005e000"', config)
        for key in ('CONFIG_SPL_SIZE_LIMIT', 'CONFIG_SPL_MAX_SIZE'):
            self.assertIn(key + '=0xc000', config)
        kernel = (ROOT / 'linux-esp32-s31/arch/riscv/configs/esp32s31_defconfig').read_text()
        self.assertIn('CONFIG_XIP_PHYS_ADDR=0x40400000', kernel)
        self.assertIn('CONFIG_MTD_PARTITIONED_MASTER=y', kernel)
        self.assertIn('FW_TEXT_START ?= 0x4000e400', "\n".join(p.read_text() for p in [ROOT / "Makefile", *sorted((ROOT / "mk").glob("*.mk"))]))
        spl = (ROOT / 'u-boot-esp32-s31/board/espressif/esp32s31/spl.c').read_text()
        self.assertRegex(spl, r's31_spl_flash_map\(0x40000000, 0x0+, 0x1000000\)')

if __name__ == '__main__':
    unittest.main()
