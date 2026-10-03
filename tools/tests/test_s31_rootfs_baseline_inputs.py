"""Fail-closed full-service restoration and explicit inherited binary provenance."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.build import rootfs_baseline as repack


def entry(mode='-rwxr-xr-x', link=''):
    return dict(mode=mode, uid=0, gid=0, date='2026-10-02', time='02:17:09', link=link)


def full_entries():
    return {name: entry() for name in repack.FULL_RUNTIME}


def busybox_fixture(root):
    root.mkdir(parents=True, exist_ok=True)
    (root / 'include').mkdir()
    config = '\n'.join('CONFIG_' + name + '=y' for name in repack.SERVICE_CONFIG)
    (root / '.config').write_text(config + '\nCONFIG_FEATURE_CROND_DIR="/etc/cron"\n')
    helps = {
        'crond': '\t-f\t Foreground /etc/cron/crontabs', 'klogd': '\t-n\t Foreground',
        'rm': '\t-f\t Never prompt', 'sh': 'Unix shell interpreter',
        'sleep': '[N]... (s)econds', 'start-stop-daemon': '\t-b\t\t-m\t\t-t\t\t-q\t',
        'syslogd': '\t-n\t Foreground',
    }
    names = ''.join(name + '\0' for name in helps)
    usage = ''.join(text + '\0' for text in helps.values())
    (root / 'include/applet_tables.h').write_text('const char applet_names[] ALIGN1 = ' + json.dumps(names).replace('\\u0000', '\\0') + ';\n')
    (root / 'include/usage_compressed.h').write_text('#define UNPACKED_USAGE ' + json.dumps(usage).replace('\\u0000', '\\0') + '\n#define UNPACKED_USAGE_LENGTH 1\n')
    binary = names.encode() + b'\xff' + usage.encode() + b'\xff'
    for option, arity, short in [('stop', 0, 'K'), ('start', 0, 'S'), ('background', 0, 'b'),
                                 ('quiet', 0, 'q'), ('test', 0, 't'), ('make-pidfile', 0, 'm'),
                                 ('pidfile', 1, 'p'), ('exec', 1, 'x')]:
        binary += option.encode() + b'\0' + bytes([arity]) + short.encode()
    entries = {name: entry('lrwxrwxrwx', '../' * (len(Path(name).parts) - 1) + 'bin/busybox') for name in repack.SERVICE_APPLETS}
    for name in ('bin/busybox', 'etc/init.d/rcS', 'etc/inittab', 'etc/cron', 'etc/cron/crontabs', 'var/log', 'run'):
        entries[name] = entry()
    return binary, entries


class BaselineInputs(unittest.TestCase):
    def test_strip_change_invalidates_baseline_repack(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ['baseline', 'module', 'strip', 'output']:
                (root / name).write_bytes(name.encode())
            (root / '.config').write_text('CONFIG_MODULES=y\n')
            (root / 'modules.order').write_text('drivers/platform/esp32s31-radio.o\n')
            ident = {'kernel_config_sha256': repack.sha(root / '.config'), 'modules_order_sha256': repack.sha(root / 'modules.order'), 'full_runtime_contract_verified': True, 'optimization': 'Inherited userspace binaries; optimization flags are not reverified or changed.', 'mode': 'incremental-baseline', 'baseline_sha256': repack.sha(root / 'baseline'), 'module_sha256': repack.sha(root / 'module'), 'repacker_sha256': repack.sha(repack.__file__), 'strip_sha256': repack.sha(root / 'strip'), 'limitation': 'Userspace inherited from explicit baseline; not a clean source build.', 'rootfs_sha256': repack.sha(root / 'output'), 'service_restoration': {}}
            (root / 'report').write_text(json.dumps(ident))
            argv = ['repack', '--kernel-output', str(root)]
            for name in ['baseline', 'module', 'output', 'report', 'staging', 'strip']:
                argv.extend(['--' + name, str(root / name)])
            with patch.object(sys, 'argv', argv), patch.object(repack, 'metadata', return_value=full_entries()) as metadata:
                repack.main(); metadata.assert_called_once(); metadata.reset_mock()
                (root / 'strip').write_bytes(b'changed strip executable')
                metadata.side_effect = RuntimeError('repack started')
                with self.assertRaisesRegex(RuntimeError, 'repack started'):
                    repack.main()

    def test_incomplete_userspace_and_extra_modules_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'modules.order').write_text('drivers/platform/esp32s31-radio.o\n')
            with self.assertRaisesRegex(SystemExit, 'Missing:'):
                repack.verify_full_inputs(root, {})
            repack.verify_full_inputs(root, full_entries())
            entries = full_entries()
            for path in repack.SERVICE_SCRIPTS:
                entries.pop(path)
            repack.verify_full_inputs(root, entries, repack.SERVICE_SCRIPTS)
            entries.pop('usr/bin/candump')
            with self.assertRaisesRegex(SystemExit, 'Missing: usr/bin/candump'):
                repack.verify_full_inputs(root, entries, repack.FULL_RUNTIME)
            (root / 'modules.order').write_text('drivers/platform/esp32s31-radio.o\ndrivers/test.o\n')
            with self.assertRaisesRegex(SystemExit, 'additional or missing modules'):
                repack.verify_full_inputs(root, full_entries())

    def test_runtime_must_be_executable_or_resolve_to_executable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'modules.order').write_text('drivers/platform/esp32s31-radio.o\n')
            entries = full_entries()
            entries['usr/bin/arecord'] = entry('lrwxrwxrwx', 'aplay')
            repack.verify_full_inputs(root, entries)
            entries['usr/bin/aplay'] = entry('-rw-r--r--')
            with self.assertRaisesRegex(SystemExit, 'not executable'):
                repack.verify_full_inputs(root, entries)
            entries['usr/bin/aplay'] = entry('lrwxrwxrwx', 'arecord')
            with self.assertRaisesRegex(SystemExit, 'not executable'):
                repack.verify_full_inputs(root, entries)

    def test_complete_baseline_needs_no_buildbox_or_source(self):
        self.assertEqual(repack.service_restoration(Path('unused'), full_entries(), None, None), ({}, {}))

    def test_restoration_requires_explicit_source_and_busybox(self):
        with self.assertRaisesRegex(SystemExit, 'requires --restore-full-services-from and --busybox-build'):
            repack.service_restoration(Path('unused'), {}, Path('source'), None)

    def test_busybox_config_is_bound_to_actual_applet_usage_and_options(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary, entries = busybox_fixture(root)
            evidence = repack.verify_service_busybox(binary, root, entries)
            self.assertEqual(evidence['busybox_sha256'], repack.digest(binary))
            self.assertIn('not target-runtime tested', evidence['verification'])
            with self.assertRaisesRegex(SystemExit, 'does not match'):
                repack.verify_service_busybox(binary.replace(b'Foreground', b'background', 1), root, entries)
            with self.assertRaisesRegex(SystemExit, 'option: --make-pidfile'):
                repack.verify_service_busybox(binary.replace(b'make-pidfile', b'no-pidfilexx'), root, entries)
            entries.pop('sbin/syslogd')
            with self.assertRaisesRegex(SystemExit, 'applet link: sbin/syslogd'):
                repack.verify_service_busybox(binary, root, entries)

    def test_busybox_missing_config_option_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary, entries = busybox_fixture(root)
            config = root / '.config'
            config.write_text(config.read_text().replace('CONFIG_FEATURE_FANCY_SLEEP=y', '# CONFIG_FEATURE_FANCY_SLEEP is not set'))
            with self.assertRaisesRegex(SystemExit, 'required service options: FEATURE_FANCY_SLEEP'):
                repack.verify_service_busybox(binary, root, entries)

    def test_only_pinned_unmodified_service_sources_are_allowed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'buildroot'
            (source / 'package/busybox').mkdir(parents=True)
            scripts = {path: b'#!/bin/sh\n' for path in repack.SERVICE_SCRIPTS}
            for path, data in scripts.items():
                (source / 'package/busybox' / Path(path).name).write_bytes(data)
            entries = {'etc/init.d': entry('drwxr-xr-x')}
            outputs = ['160000 commit 0 buildroot', 'other-commit']
            with patch.object(repack.subprocess, 'check_output', side_effect=outputs):
                with self.assertRaisesRegex(SystemExit, 'pinned Buildroot commit'):
                    repack.service_restoration(Path('baseline'), entries, source, root)
            outputs = ['160000 commit commit buildroot', 'commit', b'modified']
            with patch.object(repack.subprocess, 'check_output', side_effect=outputs):
                with self.assertRaisesRegex(SystemExit, 'Modified Buildroot service source'):
                    repack.service_restoration(Path('baseline'), entries, source, root)
            outputs = ['160000 commit commit buildroot', 'commit', *scripts.values(), b'binary']
            with patch.object(repack.subprocess, 'check_output', side_effect=outputs), patch.object(repack, 'verify_service_busybox', return_value={'verification': 'verified'}):
                restored, evidence = repack.service_restoration(Path('baseline'), entries, source, root)
            self.assertEqual(restored, scripts)
            self.assertEqual(evidence['buildroot_commit'], 'commit')
            self.assertEqual(set(evidence['scripts']), set(scripts))

    def test_install_preserves_existing_metadata_and_records_new_scripts(self):
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory)
            parent = tree / 'etc/init.d'
            parent.mkdir(parents=True)
            timestamp = 1700000009000000000
            os.utime(parent, ns=(timestamp, timestamp))
            before = {'etc/init.d': entry('drwxr-xr-x'), 'existing': entry()}
            scripts = {repack.SERVICE_SCRIPTS[0]: b'#!/bin/sh\n'}
            after = repack.install_services(tree, scripts, before)
            self.assertEqual(parent.stat().st_mtime_ns, timestamp)
            self.assertEqual(after['existing'], before['existing'])
            self.assertEqual(after['etc/init.d'], before['etc/init.d'])
            path = next(iter(scripts))
            self.assertEqual((tree / path).read_bytes(), scripts[path])
            self.assertEqual((tree / path).stat().st_mode & 0o777, 0o755)
            self.assertEqual((tree / path).stat().st_mtime_ns, timestamp)
            self.assertEqual(after[path], entry())
            with self.assertRaises(FileExistsError):
                repack.install_services(tree, scripts, before)


if __name__ == '__main__':
    unittest.main()
