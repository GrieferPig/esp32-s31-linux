"""Resolve a fresh native Buildroot configuration without downloading or building packages."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
EXTERNAL = ROOT / 'buildroot-external'


class NativePackageGraph(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='s31-package-graph-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.work = Path(cls.temporary.name)
        definition = cls.work / 'defconfig'
        lines = (EXTERNAL / 'configs/esp32s31_rootfs_defconfig').read_text().splitlines()
        lines = [line for line in lines if not line.startswith('BR2_TOOLCHAIN_EXTERNAL_PATH=')]
        # This graph-only check must not depend on a downloaded target toolchain.
        lines.append(f'BR2_TOOLCHAIN_EXTERNAL_PATH="{cls.work}/absent-toolchain"')
        definition.write_text('\n'.join(lines) + '\n')
        env = dict(os.environ)
        for name in ('MAKEFLAGS', 'MFLAGS', 'MAKELEVEL', 'BR2_CONFIG', 'BR2_DEFCONFIG'):
            env.pop(name, None)
        cls.command = ['make', '--no-print-directory', '-s', '-C', str(ROOT / 'buildroot'),
                       'O=' + str(cls.work / 'output'), 'BR2_EXTERNAL=' + str(EXTERNAL),
                       'BR2_WGET=false', 'BR2_CURL=false', 'BR2_GIT=false',
                       'BR2_SVN=false', 'BR2_HG=false']
        subprocess.run(cls.command + ['BR2_DEFCONFIG=' + str(definition), 'defconfig'],
                       env=env, capture_output=True, text=True, check=True, timeout=90)
        names = 'BR2_PACKAGE_BTSTACK_S31 BTSTACK_S31_NAME BTSTACK_S31_DIR BTSTACK_S31_PKGDIR BTSTACK_S31_VERSION CONFIGS_NAME BTSTACK_S31_KCONFIG_VAR BTSTACK_S31_INSTALL_TARGET BTSTACK_S31_TARGET_INSTALL_TARGET PACKAGES'
        result = subprocess.run(cls.command + ['printvars', 'VARS=' + names], env=env,
                                capture_output=True, text=True, check=True, timeout=30)
        cls.values = dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)

    def test_selected_bluetooth_is_registered_in_native_package_graph(self):
        self.assertEqual(self.values.get('BR2_PACKAGE_BTSTACK_S31'), 'y')
        self.assertIn('btstack-s31', self.values['PACKAGES'].split())
        self.assertEqual(self.values.get('BTSTACK_S31_NAME'), 'btstack-s31')
        self.assertNotIn('CONFIGS_NAME', self.values)
        self.assertEqual(self.values['BTSTACK_S31_KCONFIG_VAR'], 'BR2_PACKAGE_BTSTACK_S31')
        self.assertEqual(self.values['BTSTACK_S31_INSTALL_TARGET'], 'YES')
        self.assertTrue(self.values['BTSTACK_S31_TARGET_INSTALL_TARGET'].endswith('/.stamp_target_installed'))

    def test_package_directory_and_pinned_version_survive_shared_includes(self):
        versions = dict(line.split(':=', 1) for line in (ROOT / 'configs/build-versions.mk').read_text().splitlines()
                        if ':=' in line and not line.lstrip().startswith('#'))
        expected = next(value.strip() for name, value in versions.items() if name.strip() == 'BTSTACK_REF')
        self.assertEqual(self.values['BTSTACK_S31_VERSION'], expected)
        self.assertEqual(Path(self.values['BTSTACK_S31_PKGDIR']), EXTERNAL / 'package/btstack-s31')
        self.assertEqual(Path(self.values['BTSTACK_S31_DIR']).name, 'btstack-s31-' + expected)

    def test_full_board_custom_packages_remain_selected(self):
        packages = set(self.values['PACKAGES'].split())
        self.assertTrue({'btstack-s31', 'esp-simd', 's31-tools', 'coremark', 'tzdata'} <= packages)


class PackageInstallation(unittest.TestCase):
    def test_native_install_recipe_keeps_binary_and_boot_service(self):
        with tempfile.TemporaryDirectory(prefix='s31-package-install-') as directory:
            work = Path(directory)
            package = work / 'package'
            (package / 's31-build').mkdir(parents=True)
            binary = package / 's31-build/s31-btstack-a2dp'
            binary.write_bytes(b'fixture executable\n')
            target = work / 'target'
            stamp = package / 'install-stamp'
            makefile = work / 'Makefile'
            makefile.write_text(
                'BR2_EXTERNAL_ESP32_S31_PATH := ' + str(EXTERNAL) + '\n'
                'include ' + str(ROOT / 'configs/build-versions.mk') + '\n'
                'generic-package =\n'
                'INSTALL := install\n'
                'TARGET_DIR := ' + str(target) + '\n'
                'include ' + str(EXTERNAL / 'package/btstack-s31/btstack-s31.mk') + '\n'
                + str(stamp) + ':\n\t$(BTSTACK_S31_INSTALL_TARGET_CMDS)\n')
            subprocess.run(['make', '--no-print-directory', '-s', '-f', str(makefile), str(stamp)],
                           cwd=work, capture_output=True, text=True, check=True)
            installed = target / 'usr/sbin/s31-btstack-a2dp'
            service = target / 'etc/init.d/S40btstack'
            self.assertEqual(installed.read_bytes(), binary.read_bytes())
            self.assertEqual(service.read_bytes(), (EXTERNAL / 'package/btstack-s31/S40btstack').read_bytes())
            for path in (installed, service):
                self.assertEqual(path.stat().st_mode & 0o777, 0o755)


if __name__ == '__main__':
    unittest.main()
