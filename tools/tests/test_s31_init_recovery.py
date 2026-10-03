#!/usr/bin/env python3
"""Exercise early init failures using temporary paths and inert mount stubs."""
import os
import re
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
INIT = ROOT / 'buildroot-external/board/esp32-s31/overlay/init'


class InitRecovery(unittest.TestCase):
    def run_init(self, failure, volatile_failure=''):
        with tempfile.TemporaryDirectory(prefix='s31-init-test-') as tmp:
            root = Path(tmp) / 'root'
            bindir = Path(tmp) / 'bin'
            bindir.mkdir()
            for directory in ['dev/pts', 'proc', 'sys/class/mtd/mtd5', 'mnt',
                              'run', 'tmp', 'var/log', 'etc', 'usr/lib/s31-overlays']:
                (root / directory).mkdir(parents=True, exist_ok=True)
            if failure != 'missing-persist':
                (root / 'sys/class/mtd/mtd5/name').write_text('persist\n')
                (root / 'dev/mtdblock5').touch()
            log = Path(tmp) / 'commands.log'
            stubs = {
                'mount': '''#!/bin/sh
printf 'mount %s\\n' "$*" >>"$TEST_LOG"
for last do :; done
[ "$last" != "$FAIL_MOUNT" ] && [ "$last" != "$FAIL_VOLATILE" ]
''',
                'mountpoint': '#!/bin/sh\nexit 1\n',
                'pivot_root': '#!/bin/sh\nexit 1\n',
                'mknod': '#!/bin/sh\nexit 0\n',
                'sleep': '#!/bin/sh\nexit 0\n',
                'fake-init': '''#!/bin/sh
printf 'init\\n' >>"$TEST_LOG"
exit 23
''',
            }
            for name, text in stubs.items():
                path = bindir / name
                path.write_text(text)
                path.chmod(0o755)
            # Every filesystem access goes to the fixture. The only executable
            # absolute handoff is replaced explicitly; real mounts never run.
            source = INIT.read_text().replace('exec /sbin/init', 'exec fake-init')
            source = source.replace('[ -b "/dev/mtdblock${mtd_name}" ]',
                                    '[ -f "/dev/mtdblock${mtd_name}" ]')
            source = re.sub(r'/(dev|proc|sys|mnt|run|tmp|var|etc|usr)(?=/|[\s\"\'])',
                            lambda match: str(root / match.group(1)), source)
            script = Path(tmp) / 'init-test.sh'
            script.write_text(source)
            mount_path = {
                'missing-persist': '',
                'staging': str(root / 'mnt'),
                'persist': str(root / 'mnt/store'),
                'overlay': str(root / 'mnt/newroot'),
                'pivot': '',
            }[failure]
            env = dict(os.environ, PATH=f'{bindir}:/usr/bin:/bin', TEST_LOG=str(log),
                       FAIL_MOUNT=mount_path,
                       FAIL_VOLATILE=str(root / volatile_failure) if volatile_failure else '')
            result = subprocess.run(['/bin/sh', str(script)], env=env,
                                    text=True, capture_output=True, timeout=10)
            lines = log.read_text().splitlines()
            return result, lines, str(root)

    def test_all_early_failures_prepare_volatile_mounts_before_init(self):
        for failure in ['missing-persist', 'staging', 'persist', 'overlay', 'pivot']:
            with self.subTest(failure=failure):
                result, lines, root = self.run_init(failure)
                self.assertEqual(result.returncode, 23, result.stderr)
                expected = {'missing-persist': 'persist MTD block device not found',
                            'staging': 'failed to allocate staging tmpfs',
                            'persist': 'failed to mount persist as JFFS2',
                            'overlay': 'failed to mount overlayfs',
                            'pivot': 'pivot_root failed'}[failure]
                self.assertIn(expected, result.stderr)
                self.assertIn('persistent settings unavailable', result.stderr)
                self.assertEqual(lines[-1], 'init')
                for path in ['run', 'tmp', 'var/log']:
                    matches = [line for line in lines if line.startswith('mount -t tmpfs ') and
                               line.endswith(' ' + root + '/' + path)]
                    self.assertEqual(len(matches), 1, lines)

    def test_volatile_failure_is_visible_bounded_and_does_not_block_console(self):
        result, lines, root = self.run_init('persist', 'run')
        self.assertEqual(result.returncode, 23, result.stderr)
        self.assertIn('failed to mount volatile ' + root + '/run', result.stderr)
        self.assertIn('some volatile directories are unavailable', result.stderr)
        self.assertEqual(sum(line.endswith(' ' + root + '/run') for line in lines), 1)
        self.assertTrue(any(line.endswith(' ' + root + '/tmp') for line in lines))
        self.assertTrue(any(line.endswith(' ' + root + '/var/log') for line in lines))
        self.assertEqual(lines[-1], 'init')

    def test_shell_syntax(self):
        subprocess.run(['/bin/sh', '-n', str(INIT)], check=True)


if __name__ == '__main__':
    unittest.main()
