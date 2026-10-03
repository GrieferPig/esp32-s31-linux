#!/usr/bin/env python3
"""Offline checks for board-owned size policy without patching upstream builds."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SizePolicy(unittest.TestCase):
    def test_buildroot_native_optimization(self):
        config = (ROOT / 'buildroot-external/configs/esp32s31_rootfs_defconfig').read_text()
        self.assertIn('BR2_OPTIMIZE_S=y', config)
        wrapper = re.search(r'^BR2_TARGET_OPTIMIZATION="(.*)"$', config, re.M)[1]
        self.assertNotRegex(wrapper, r'(^| )-O\S+')
        self.assertIn('-mabi=ilp32', wrapper)
        # Evaluate Buildroot's real native choice-to-flag mapping without a build.
        source = (ROOT / 'buildroot/package/Makefile.in').read_text()
        mapping = source[source.index('ifeq ($(BR2_OPTIMIZE_0),y)'):source.index('ifeq ($(BR2_DEBUG_1),y)')]
        with tempfile.TemporaryDirectory() as directory:
            makefile = Path(directory) / 'Makefile'
            makefile.write_text('BR2_OPTIMIZE_S=y\n' + mapping + '\nall:\n\t@echo $(TARGET_OPTIMIZATION)\n')
            result = subprocess.check_output(['make', '-s', '-f', str(makefile)], text=True)
        self.assertEqual(result.strip(), '-Os')

    def test_idf_native_size_choices(self):
        for relative in ('firmware/lp/sdkconfig.defaults', 'firmware/radio/idf_deps/sdkconfig.defaults', 'firmware/radio/idf_deps/sdkconfig.radio.defaults'):
            text = (ROOT / relative).read_text()
            self.assertIn('CONFIG_COMPILER_OPTIMIZATION_SIZE=y', text)
            self.assertNotIn('\nCONFIG_COMPILER_OPTIMIZATION_PERF=y', text)

    def test_radio_owned_flags(self):
        text = (ROOT / 'firmware/radio/Makefile').read_text()
        flags = re.search(r'^ESP_CFLAGS\s*:=\s*(.*(?:\\\n.*)*)', text, re.M)[1]
        self.assertEqual(re.findall(r'(?<!\S)-O\S+', flags)[-1], '-Os')
        self.assertIn('-mabi=ilp32f', flags)

    def test_coremark_native_recipe_preserves_threads(self):
        text = (ROOT / 'buildroot-external/package/coremark/coremark.mk').read_text()
        self.assertIn('PORT_CFLAGS="$(TARGET_CFLAGS)"', text)
        self.assertIn('XCFLAGS="-DMULTITHREAD=2 -DUSE_PTHREAD=1 -pthread"', text)
        recipe = text.split('define COREMARK_BUILD_CMDS', 1)[1]
        self.assertNotRegex(recipe, r'-O[023]|-funroll')
        self.assertFalse((ROOT / 'buildroot-external/patches/coremark/0001-linux-enable-two-thread-pthread.patch').exists())

    def test_kernel_and_boot_supported_size_controls(self):
        kernel = (ROOT / 'configs/kernel/common.config').read_text()
        self.assertIn('CONFIG_CC_OPTIMIZE_FOR_SIZE=y', kernel)
        self.assertIn('CONFIG_TRIM_UNUSED_KSYMS=y', kernel)
        self.assertNotIn('CONFIG_UNUSED_KSYMS_WHITELIST=""', kernel)
        self.assertIn('# CONFIG_CC_OPTIMIZE_FOR_PERFORMANCE is not set', kernel)
        boot = (ROOT / 'configs/uboot.config').read_text()
        self.assertIn('CONFIG_CC_OPTIMIZE_FOR_SIZE=y', boot)
        rules = (ROOT / 'mk/linux.mk').read_text()
        self.assertIn('CFLAGS_$(o).o=-Os', rules)
        self.assertIn('$(S31_SIZE_OBJECT_FLAGS)', rules)
        self.assertIn('--ksyms-whitelist "$(S31_RADIO_KSYMS)"', rules)
        radio = (ROOT / 'mk/radio.mk').read_text()
        self.assertIn('S31_RADIO_KSYMS := $(GENERATED_DIR)/radio-kernel-symbols.txt', radio)
        self.assertIn('--payload "$(RADIO_OUT)/linux_radio.localized.o"', radio)
        crypto = (ROOT / 'linux-esp32-s31/crypto/Makefile').read_text()
        self.assertIn('CFLAGS_jitterentropy.o = -O0', crypto)
        opensbi = (ROOT / 'mk/opensbi-size.mk').read_text()
        self.assertIn('override CFLAGS += -Os', opensbi)
        self.assertIn('sha256sum $(ROOT)/mk/opensbi-size.mk', (ROOT / 'mk/boot.mk').read_text())
        self.assertIn('-f Makefile -f $(ROOT)/mk/opensbi-size.mk', (ROOT / 'mk/boot.mk').read_text())

    def test_boot_fragment_is_applied_by_native_configuration_helper(self):
        text = (ROOT / 'tools/build/configure.py').read_text()
        self.assertIn("if a.kind == 'linux' or a.fragment:", text)


if __name__ == '__main__':
    unittest.main()
