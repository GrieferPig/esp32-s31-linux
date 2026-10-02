"""Negative controls for the follow-up audit's observable failure paths."""
from pathlib import Path
import json
import os
import csv
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tools/hil'))
import build_manifest
import s31_hil as hil
import serial_transport


class ManifestInputs(unittest.TestCase):
    def test_untracked_build_inputs_change_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            subprocess.run(['git', 'init', '-q', tmp], check=True)
            subprocess.run(['git', '-C', tmp, '-c', 'user.name=Test', '-c',
                            'user.email=test@example.invalid', 'commit', '-q',
                            '--allow-empty', '-m', 'base'], check=True)
            names = ['buildroot-external/package/test/test.mk',
                     'buildroot-external/package/test/Config.in',
                     'buildroot-external/board/test/overlay/etc/init.d/S01test',
                     'firmware/lp/sdkconfig.defaults', 'drivers/test/Kconfig',
                     'tools/test/Makefile']
            for name in names:
                with self.subTest(name=name):
                    path = root / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text('first')
                    before = build_manifest.revision(root)
                    path.write_text('second')
                    after = build_manifest.revision(root)
                    self.assertNotEqual(before, after)
                    self.assertIn(name, after['untracked_source_files'])
            output = root / 'build/temporary.c'
            output.parent.mkdir()
            output.write_text('not an input')
            self.assertNotIn('build/temporary.c',
                             build_manifest.revision(root)['untracked_source_files'])


class HilProtocol(unittest.TestCase):
    def test_malformed_and_unknown_results_are_rejected(self):
        for payload in (b'{"test":"summary"}', b'[]', b'{',
                        b'{"board":"esp32-s31","test":"summary",'
                        b'"level":"summary","status":"UNKNOWN"}'):
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                hil.parse_result(b'HIL1 ' + payload)
        self.assertTrue(hil.failed([hil.Result('s31', 'UNKNOWN', 'summary', 'summary')]))

    def test_wrong_board_and_case_fail_collection(self):
        packet = dict(board='esp32-s31', status='PASS', test='summary',
                      level='summary', case='firmware')
        for kwargs in (dict(expected_board='esp32-p4-wifi6-dev-kit'),
                       dict(expected_board='esp32-s31', expected_case='lp-core')):
            port = mock.Mock()
            port.read.return_value = b'HIL1 ' + json.dumps(packet).encode() + b'\n'
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                hil.collect(port, 0.1, **kwargs)

    def test_skip_is_preserved_in_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'results.json'
            hil.save_results(str(path), [hil.Result('s31', 'SKIP', 'peer', 'electrical')])
            record = json.loads(path.read_text())
            self.assertEqual(record['counts'], dict(PASS=0, FAIL=0, SKIP=1))
            self.assertEqual(record['results'][0]['status'], 'SKIP')


class SerialOwnership(unittest.TestCase):
    def test_second_open_failure_closes_first_port(self):
        first = mock.Mock()
        with mock.patch.object(hil, 'open_serial', side_effect=[first, OSError('peer')]):
            with self.assertRaises(OSError):
                hil.run_i2c_peer('s31', 'p4', 1)
        first.close.assert_called_once()

    def test_both_ports_close_even_when_cleanup_raises(self):
        first, second = mock.Mock(), mock.Mock()
        second.close.side_effect = OSError('close failed')
        with mock.patch.object(hil, 'open_serial', side_effect=[first, second]):
            with self.assertRaises(OSError):
                with hil.open_peer_ports('s31', 'p4'):
                    pass
        first.close.assert_called_once()
        second.close.assert_called_once()

    def test_silent_worker_times_out_and_is_reaped(self):
        real_popen = subprocess.Popen
        child = real_popen([sys.executable, '-u', '-c',
                           'import sys,time; sys.stdin.readline(); '
                           'print(\'{"ok":true}\',flush=True); '
                           'sys.stdin.readline(); print(\'{"ok":true}\',flush=True); '
                           'sys.stdin.readline(); time.sleep(60)'],
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True, bufsize=1)
        try:
            with mock.patch.object(serial_transport.subprocess, 'check_output',
                                   return_value='worker.py\n'), \
                 mock.patch.object(serial_transport.subprocess, 'Popen', return_value=child):
                worker = serial_transport.WindowsSerialWorker('COM99')
            started = time.monotonic()
            with self.assertRaises(TimeoutError):
                worker._request({'op': 'read'}, timeout=0.05)
            self.assertLess(time.monotonic() - started, 3)
            self.assertIsNotNone(child.poll())
            worker.close()
        finally:
            if child.poll() is None:
                child.kill()
            child.wait()

    @unittest.skipUnless(os.environ.get('S31_TEST_WINDOWS_SERIAL') == '1',
                         'optional real WSL/Windows subprocess cleanup check')
    def test_windows_child_does_not_survive_rpc_timeout(self):
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / 's31_timeout_probe.py'
            script.write_text(
                'import sys,time,os,json\n'
                'sys.stdin.readline()\n'
                'print(json.dumps(dict(ok=True,pid=os.getpid(),platform=os.name)),flush=True)\n'
                'sys.stdin.readline()\n'
                'print(\'{"ok":true}\',flush=True)\n'
                'sys.stdin.readline(); time.sleep(60)\n')
            windows_script = subprocess.check_output(
                ['wslpath', '-w', str(script)], text=True).strip()
            child = subprocess.Popen(
                ['/mnt/c/Windows/System32/cmd.exe', '/d', '/c', 'python', '-u', windows_script],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, bufsize=1)
            worker = None
            try:
                with mock.patch.object(serial_transport.subprocess, 'check_output', return_value=windows_script), \
                     mock.patch.object(serial_transport.subprocess, 'Popen', return_value=child):
                    worker = serial_transport.WindowsSerialWorker('COM99')
                pid = worker._windows_pid
                self.assertIsNotNone(pid)
                with self.assertRaises(TimeoutError):
                    worker._request({'op': 'read'}, timeout=0.05)
                listing = subprocess.check_output(
                    ['/mnt/c/Windows/System32/tasklist.exe', '/FI', 'PID eq ' + str(pid),
                     '/FO', 'CSV', '/NH'], text=True, timeout=5)
                self.assertFalse(any(len(row) > 1 and row[1] == str(pid)
                                     for row in csv.reader(listing.splitlines())), listing)
            finally:
                if worker is not None:
                    worker.close()
                if child.poll() is None:
                    child.kill()
                child.wait(timeout=5)


class BuildOrdering(unittest.TestCase):
    def test_source_update_cannot_share_parallel_invocation(self):
        p = subprocess.run(['make', '-j2', 'download', 'all'], cwd=ROOT,
                           capture_output=True, text=True)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('Run make download separately', p.stderr)
        self.assertNotIn('git submodule update', p.stdout)


class RadioStartup(unittest.TestCase):
    def test_probe_failure_and_loaded_unbound_module_fail(self):
        source = (ROOT / 'buildroot-external/board/esp32-s31/overlay/etc/init.d/S00s31-radio').read_text()
        for loaded, ready in ((False, False), (True, False), (False, True), (True, True)):
            with self.subTest(loaded=loaded, ready=ready), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                def put(name, text='', executable=False):
                    path = root / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(text)
                    if executable:
                        path.chmod(0o755)
                    return path
                script = source
                for prefix in ('/sys', '/proc', '/run', '/dev', '/etc', '/usr/lib', '/usr/sbin'):
                    script = script.replace(prefix + '/', tmp + prefix + '/')
                put('start', script, True)
                (root / 'dev').mkdir()
                (root / 'dev/null').symlink_to('/dev/null')
                put('proc/cmdline')
                put('usr/lib/esp32-config/common.sh', '#!/bin/sh\nensure_config() { :; }\n')
                put('proc/modules', 'esp32s31_radio 1 0 - Live 0\n' if loaded else '')
                put('proc/device-tree/soc/radio/status', 'okay\n')
                put('proc/device-tree/soc/radio/wifi/status', 'okay\n')
                put('proc/device-tree/soc/radio/bluetooth/status', 'disabled\n')
                put('sys/module/esp32s31_radio/parameters/mode', 'wifi\n')
                put('proc/mounts', 'source ' + tmp + '/run/s31-radio squashfs ro 0 0\n')
                put('run/s31-radio/module/esp32s31-radio.ko.xz', 'module')
                put('run/s31-radio/firmware/esp32s31-radio-fw-v1.o.xz', 'payload')
                put('usr/lib/s31-radio/esp32s31-radio.ko.xz', 'module')
                (root / 'sys/class/net/wlan0').mkdir(parents=True)
                if ready:
                    put('sys/bus/platform/drivers/esp32s31-radio/soc:radio/radio_health', 'abi=1 state=2 wifi_init=0\n')
                put('usr/sbin/s31-modload', '#!/bin/sh\nif [ "$1" = --remove ]; then : > ' + tmp + '/proc/modules; else echo "esp32s31_radio 1 0 - Live 0" > ' + tmp + '/proc/modules; fi\n', True)
                put('bin/sleep', '#!/bin/sh\nexit 0\n', True)
                put('bin/sync', '#!/bin/sh\nexit 0\n', True)
                import os
                env = dict(os.environ, S31_RADIO_VOLATILE_MODE='wifi', PATH=tmp + '/bin:' + os.environ['PATH'])
                run = subprocess.run(['sh', str(root / 'start'), 'start'], env=env, capture_output=True, text=True)
                self.assertEqual(run.returncode, 0 if ready else 1, run.stderr)
                if not ready:
                    self.assertEqual((root / 'proc/modules').read_text(), '')


if __name__ == '__main__':
    unittest.main()
