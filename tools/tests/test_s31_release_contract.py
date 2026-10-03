"""Paired release, packaging and hardware-free preflight regression tests."""
import argparse
import hashlib
import io
import json
import lzma
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.checks.layout import flash_layout
from tools.device import flash
from tools.device.flash import command
from tools.release import assets, manifest
from tools.build import radio_image


class MatchedRelease(unittest.TestCase):
    def setUp(self):
        self.packed_hash = hashlib.sha256(b'fixture-packed-module').hexdigest()
        extractor = mock.patch.object(manifest, 'packed_module_digest', return_value=self.packed_hash)
        self.extract = extractor.start()
        self.addCleanup(extractor.stop)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / 'out'
        self.images = self.root / 'images'
        self.images.mkdir(parents=True)
        binding = {}
        for key, name in dict(manifest.BINDINGS, radio_imports_sha256='radio/linux-radio-linked-imports.txt').items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(name.encode())
            binding[key] = manifest.digest(path)
        for name in manifest.ARTIFACTS:
            (self.images / name).write_bytes(name.encode())
        for name, native in manifest.NATIVE_IMAGES.items():
            path = self.root / native
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(self.images / name, path)
        report = {'sha256': manifest.digest(self.images / 'radio.bin'), 'binding': binding}
        (self.images / 'radio.json').write_text(json.dumps(report))
        offsets, _ = flash_layout()
        with (self.images / 's31_full_flash.bin').open('wb') as full:
            for slot, name in [('SPL', 'spl_app.bin'), ('UBOOT_ITB', 'u-boot.itb'),
                               ('DTB', 'esp32s31_generic.dtb'), ('RADIO', 'radio.bin'),
                               ('KERNEL', 'xipImage'), ('ROOTFS', 'rootfs.sqfs')]:
                full.seek(offsets['SLOT_' + slot])
                full.write((self.images / name).read_bytes())
        self.data = {'schema_version': manifest.SCHEMA_VERSION,
                     'radio_binding': binding,
                     'source_configs': {name: manifest.digest(ROOT / name) for name in manifest.SOURCE_CONFIGS},
                     'generated_configs': {},
                     'artifacts': {name: manifest.digest(self.images / name) for name in manifest.ARTIFACTS}}
        provenance = self.root / 'reports/rootfs-provenance.json'
        provenance.parent.mkdir()
        manifest.atomic_json(provenance, {'mode': 'native-buildroot',
                                         'module_sha256': binding['kernel_module_sha256'],
                                         'packaged_module_sha256': self.packed_hash,
                                         'rootfs_sha256': self.data['artifacts']['rootfs.sqfs']})
        self.data['build_provenance'] = {'rootfs': manifest.rootfs_provenance(self.root, self.data['artifacts'], binding)}
        self.path = self.images / 'build-manifest.json'
        manifest.atomic_json(self.path, self.data)

    def verify(self):
        return manifest.verify_manifest(self.path, self.root)

    def test_matched_inputs_and_all_slot_preflight(self):
        self.verify()
        argv = command(self.root, '/dev/not-accessed', 460800)
        self.assertIn('0x6e000', argv)
        self.assertNotIn('0x1ee000', argv)  # persist is never written
        self.assertEqual(len(argv), 20)

    def test_mixed_kernel_module_or_payload_rejected(self):
        for name in list(manifest.BINDINGS.values()) + ['radio/linux-radio-linked-imports.txt']:
            with self.subTest(name=name):
                path = self.root / name
                original = path.read_bytes()
                path.write_bytes(original + b'changed')
                with self.assertRaisesRegex(ValueError, 'input mismatch|import contract changed'):
                    self.verify()
                path.write_bytes(original)

    def test_stale_native_kernel_image_or_dtb_rejected(self):
        for name, native in manifest.NATIVE_IMAGES.items():
            path = self.root / native
            original = path.read_bytes()
            path.write_bytes(b'newer native artifact')
            with self.subTest(name=name):
                with self.assertRaisesRegex(ValueError, 'differs from current native kernel output'):
                    self.verify()
                with self.assertRaisesRegex(ValueError, 'differs from current native kernel output'):
                    manifest.verify_native_images(self.root, self.data['artifacts'])
            path.write_bytes(original)

    def package_fixture(self):
        stage = Path(self.tmp.name) / 'package'
        for directory in ('module', 'firmware', 'config', 'overlays'):
            (stage / directory).mkdir(parents=True, exist_ok=True)
        module = self.root / manifest.BINDINGS['kernel_module_sha256']
        (stage / 'module/esp32s31-radio.ko.xz').write_bytes(lzma.compress(module.read_bytes()))
        for name in ('radio.bin', 'radio.json'):
            shutil.copyfile(self.images / name, stage / 'firmware' / name)
        shutil.copyfile(self.path, stage / 'build-manifest.json')
        for name in ('sdkconfig.defaults', 'sdkconfig.radio.defaults'):
            shutil.copyfile(ROOT / 'firmware/radio/idf_deps' / name, stage / 'config' / name)
        shutil.copyfile(ROOT / 'firmware/radio/RADIO_BUNDLE_LICENSES.md', stage / 'RADIO_BUNDLE_LICENSES.md')
        for overlay in ('radio-wifi', 'radio-bluetooth', 'radio-combo'):
            name = 'esp32s31-overlay-' + overlay + '.dtbo'
            source = self.root / 'linux/arch/riscv/boot/dts/espressif' / name
            source.write_bytes(name.encode())
            shutil.copyfile(source, stage / 'overlays' / name)
        return stage

    def test_staged_package_validates_every_copied_input(self):
        stage = self.package_fixture()
        manifest.verify_radio_package(stage, self.root)
        names = ['build-manifest.json', 'firmware/radio.bin', 'firmware/radio.json',
                 'config/sdkconfig.defaults', 'config/sdkconfig.radio.defaults', 'RADIO_BUNDLE_LICENSES.md']
        names += ['overlays/esp32s31-overlay-' + name + '.dtbo' for name in ('radio-wifi', 'radio-bluetooth', 'radio-combo')]
        for name in names:
            path = stage / name
            original = path.read_bytes()
            path.write_bytes(b'mixed copy')
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'staged package'):
                manifest.verify_radio_package(stage, self.root)
            path.write_bytes(original)

    def test_staged_package_rejects_mixed_compressed_module(self):
        stage = self.package_fixture()
        path = stage / 'module/esp32s31-radio.ko.xz'
        for content in (b'not an archive', lzma.compress(b'different module')):
            path.write_bytes(content)
            with self.assertRaisesRegex(ValueError, 'staged package module'):
                manifest.verify_radio_package(stage, self.root)

    def test_changed_component_or_combined_image_rejected(self):
        for name in ('radio.bin', 'rootfs.sqfs', 's31_full_flash.bin'):
            with self.subTest(name=name):
                path = self.images / name
                original = path.read_bytes()
                path.write_bytes(b'changed')
                with self.assertRaisesRegex(ValueError, 'artifact mismatch'):
                    self.verify()
                path.write_bytes(original)

    def test_legacy_manifest_schema_rejected(self):
        for version in (None, 1, 2):
            self.data['schema_version'] = version
            manifest.atomic_json(self.path, self.data)
            with self.subTest(version=version), self.assertRaisesRegex(ValueError, 'legacy manifest'):
                self.verify()

    def test_profile_bearing_manifest_rejected_even_with_new_schema(self):
        self.data['profile'] = 'radio-appliance'
        manifest.atomic_json(self.path, self.data)
        for check_inputs in (True, False):
            with self.subTest(check_inputs=check_inputs), self.assertRaisesRegex(ValueError, 'legacy manifest'):
                manifest.verify_manifest(self.path, self.root, check_inputs=check_inputs)

    def test_old_profile_source_config_rejected(self):
        self.data['source_configs']['configs/profiles/peripherals.mk'] = '0' * 64
        manifest.atomic_json(self.path, self.data)
        with self.assertRaisesRegex(ValueError, 'legacy manifest'):
            self.verify()

    def test_canonical_source_configuration_bindings_cannot_be_omitted(self):
        for name in manifest.SOURCE_CONFIGS:
            original = self.data['source_configs'].pop(name)
            manifest.atomic_json(self.path, self.data)
            for check_inputs in (True, False):
                with self.subTest(name=name, check_inputs=check_inputs), self.assertRaisesRegex(ValueError, 'canonical source'):
                    manifest.verify_manifest(self.path, self.root, check_inputs=check_inputs)
            self.data['source_configs'][name] = original

    def test_changed_canonical_source_configuration_rejected(self):
        for name in manifest.SOURCE_CONFIGS:
            original = self.data['source_configs'][name]
            self.data['source_configs'][name] = '0' * 64
            manifest.atomic_json(self.path, self.data)
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'configuration changed'):
                self.verify()
            self.data['source_configs'][name] = original

    def test_unsafe_source_configuration_paths_rejected(self):
        for name in ('../secret', '/etc/passwd'):
            self.data['source_configs'][name] = '0' * 64
            manifest.atomic_json(self.path, self.data)
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'unsafe configuration path'):
                self.verify()
            del self.data['source_configs'][name]

    def test_generated_configuration_binding_still_enforced(self):
        path = self.root / 'linux/.config'
        path.write_text('CONFIG_MODULES=y\n')
        self.data['generated_configs']['linux/.config'] = manifest.digest(path)
        manifest.atomic_json(self.path, self.data)
        self.verify()
        path.write_text('# CONFIG_MODULES is not set\n')
        with self.assertRaisesRegex(ValueError, 'generated configuration changed'):
            self.verify()

    def create_manifest(self, resolved):
        manifest.atomic_json(self.root / 'reports/resolved-config.json', resolved)
        compiler = self.root / 'bin/cross-gcc'
        compiler.parent.mkdir(exist_ok=True)
        compiler.write_bytes(b'compiler fixture')
        argv = ['manifest.py', '--compiler', str(compiler), '--idf', str(self.root / 'idf'),
                '--output-root', str(self.root)]
        with mock.patch.object(sys, 'argv', argv), mock.patch.object(manifest, 'revision', return_value={'commit': 'fixture'}), \
                mock.patch.object(manifest.subprocess, 'check_output', return_value='fixture gcc\n'), \
                mock.patch('sys.stdout', new_callable=io.StringIO):
            manifest.main()

    def test_new_manifest_binds_full_build_without_selection(self):
        self.create_manifest({'kernel_fragments': [str(ROOT / name) for name in manifest.SOURCE_CONFIGS if name.endswith('.config')]})
        data = self.verify()
        self.assertEqual(data['schema_version'], 3)
        self.assertNotIn('profile', data)
        self.assertTrue(set(manifest.SOURCE_CONFIGS).issubset(data['source_configs']))
        self.assertIn('reports/resolved-config.json', data['generated_configs'])
        self.assertFalse(any(name.startswith('configs/profiles/') for name in data['source_configs']))

    def test_legacy_resolved_configuration_cannot_create_new_manifest(self):
        before = self.path.read_bytes()
        with mock.patch('sys.stderr', new_callable=io.StringIO) as errors, self.assertRaises(SystemExit) as caught:
            self.create_manifest({'profile': 'peripherals', 'kernel_fragments': []})
        self.assertEqual(caught.exception.code, 1)
        self.assertIn('legacy resolved configuration', errors.getvalue())
        self.assertEqual(self.path.read_bytes(), before)

    def test_embedded_old_component_rejected_even_if_hash_updated(self):
        path = self.images / 's31_full_flash.bin'
        with path.open('r+b') as full:
            full.seek(flash_layout()[0]['SLOT_KERNEL'])
            full.write(b'old')
        self.data['artifacts']['s31_full_flash.bin'] = manifest.digest(path)
        manifest.atomic_json(self.path, self.data)
        with self.assertRaisesRegex(ValueError, 'does not contain matched xipImage'):
            self.verify()

    def test_atomic_publication_preserves_previous_good_set_after_failure(self):
        dist = Path(self.tmp.name) / 'dist'
        first = assets.publish(self.images, dist, self.root)
        self.assertEqual((dist / 'current').resolve(), first)
        self.assertEqual(first.name, manifest.digest(self.path)[:16])
        self.assertTrue((first / 'SHA256SUMS').is_file())
        self.assertEqual(assets.publish(self.images, dist, self.root), first)
        (self.images / 'radio.bin').write_bytes(b'bad')
        with self.assertRaises(ValueError):
            assets.publish(self.images, dist, self.root)
        self.assertEqual((dist / 'current').resolve(), first)
        manifest.verify_manifest(first / 'build-manifest.json', self.root, check_inputs=False)

    def test_rootfs_without_paired_module_provenance_rejected(self):
        del self.data['build_provenance']
        manifest.atomic_json(self.path, self.data)
        with self.assertRaisesRegex(ValueError, 'rootfs provenance'):
            self.verify()

    def test_wrong_module_inside_rootfs_is_rejected(self):
        self.extract.return_value = '0' * 64
        with self.assertRaisesRegex(ValueError, 'packed module'):
            self.verify()

    def test_missing_local_rootfs_provenance_stops_verify_publish_and_device(self):
        (self.root / 'reports/rootfs-provenance.json').unlink()
        for operation in (self.verify,
                          lambda: assets.publish(self.images, Path(self.tmp.name) / 'dist', self.root),
                          lambda: command(self.root, '/dev/not-accessed', 460800)):
            with self.assertRaisesRegex(ValueError, 'rootfs provenance missing'):
                operation()

    def test_radio_noop_skips_tools_and_changed_inputs_rebuild(self):
        prefix = self.root / 'toolchain/cross-'
        prefix.parent.mkdir()
        for name in ('gcc', 'ld', 'objcopy'):
            path = Path(str(prefix) + name)
            path.write_text('#!/bin/sh\nexit 1\n')
            path.chmod(0o755)
        args = argparse.Namespace(prefix=str(prefix), kernel=self.root / 'linux/vmlinux',
                                  module=self.root / 'linux/drivers/platform/esp32s31-radio.ko',
                                  payload=self.root / 'radio/linux_radio.localized.o',
                                  imports=self.root / 'radio/linux-radio-linked-imports.txt',
                                  output=self.images / 'radio.bin')
        report = json.loads(args.output.with_suffix('.json').read_text())
        report['build_inputs'] = radio_image.input_identity(args)
        manifest.atomic_json(args.output.with_suffix('.json'), report)
        before = args.output.stat().st_mtime_ns
        with mock.patch.object(radio_image, 'run') as run:
            radio_image.build(args)
            run.assert_not_called()
        self.assertEqual(args.output.stat().st_mtime_ns, before)
        for path in (args.kernel, args.module, args.payload, args.imports, Path(str(prefix) + 'ld')):
            original = path.read_bytes()
            path.write_bytes(original + b'changed')
            with self.subTest(path=path), mock.patch.object(radio_image, 'Elf', side_effect=RuntimeError('rebuild path')):
                with self.assertRaisesRegex(RuntimeError, 'rebuild path'):
                    radio_image.build(args)
            path.write_bytes(original)
        args.output.unlink()
        with mock.patch.object(radio_image, 'Elf', side_effect=RuntimeError('rebuild path')):
            with self.assertRaisesRegex(RuntimeError, 'rebuild path'):
                radio_image.build(args)

    def test_flash_resolves_immutable_publication_instead_of_mutable_outputs(self):
        dist = Path(self.tmp.name) / 'dist'
        published = assets.publish(self.images, dist, self.root)
        (self.root / 'linux/vmlinux').write_bytes(b'new unfinished build')
        argv = command(self.root, '/dev/not-accessed', 460800,
                       artifact_dir=dist / 'current')
        self.assertIn(str(published / 'radio.bin'), argv)
        self.assertNotIn(str(dist / 'current/radio.bin'), argv)

    def test_partial_update_rejected_without_hardware(self):
        result = subprocess.run([sys.executable, str(ROOT / 'tools/device/flash.py'),
                                 '--slot', 'radio', '--output-root', str(self.root)], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('installed companion identity is unknown', result.stderr)

    def test_bundle_is_packaging_only_and_licenses_fail_closed(self):
        result = subprocess.run(['sh', str(ROOT / 'tools/release/radio_bundle.sh'),
                                 '--release', '--output-root', str(self.root)], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('requires --grant', result.stderr)
        script = (ROOT / 'tools/release/radio_bundle.sh').read_text()
        self.assertNotIn('make -C', script)
        self.assertNotIn('--strip-debug', script)


class SingleBuildEntrypoints(unittest.TestCase):
    def test_removed_profile_option_is_an_argparse_error(self):
        for name in ('tools/release/manifest.py', 'tools/release/assets.py', 'tools/device/flash.py'):
            result = subprocess.run([sys.executable, str(ROOT / name), '--profile', 'peripherals'],
                                    capture_output=True, text=True)
            with self.subTest(name=name):
                self.assertEqual(result.returncode, 2)
                self.assertIn('unrecognized arguments: --profile peripherals', result.stderr)

    def test_assets_default_to_flat_output_root(self):
        with mock.patch.object(sys, 'argv', ['assets.py']), mock.patch('sys.stdout', new_callable=io.StringIO) as output:
            assets.main()
        self.assertEqual(output.getvalue().splitlines(),
                         [str(ROOT / 'out/images' / name) for name in assets.assets() + ['SHA256SUMS']])

    def test_manifest_default_to_flat_output_root(self):
        with mock.patch.object(sys, 'argv', ['manifest.py', '--verify', '/fixture/manifest.json']), \
                mock.patch.object(manifest, 'verify_manifest') as verify, \
                mock.patch('sys.stdout', new_callable=io.StringIO):
            manifest.main()
        verify.assert_called_once_with(Path('/fixture/manifest.json'), ROOT / 'out', check_inputs=True)

    def test_flash_default_to_current_immutable_publication(self):
        with mock.patch.object(sys, 'argv', ['flash.py', '--dry-run']), \
                mock.patch.object(flash, 'command', return_value=['esptool']) as prepare, \
                mock.patch.object(flash.subprocess, 'run') as hardware, \
                mock.patch('sys.stdout', new_callable=io.StringIO):
            flash.main()
        prepare.assert_called_once_with(ROOT / 'out', '/dev/ttyUSB0', 460800, 'esptool',
                                        artifact_dir=ROOT / 'dist/current')
        hardware.assert_not_called()

    def test_bundle_rejects_profile_option(self):
        result = subprocess.run(['sh', str(ROOT / 'tools/release/radio_bundle.sh'), '--profile', 'peripherals'],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn('Usage:', result.stderr)

    def test_shell_helpers_do_not_read_profile_environment(self):
        for name in ('tools/release/radio_bundle.sh', 'tools/release/btstack_notices.sh', 'tools/device/backup_flash.sh'):
            with self.subTest(name=name):
                self.assertNotIn('PROFILE', (ROOT / name).read_text())


if __name__ == '__main__':
    unittest.main()
