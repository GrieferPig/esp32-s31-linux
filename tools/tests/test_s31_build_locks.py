"""Exercise the public Make facade using inert native-command substitutes."""
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]


class BuildLocks(unittest.TestCase):
    def test_aliases_and_device_consumers_share_the_build_lock(self):
        required = {'flash-image', 'radio-fs', 'initramfs', 'bootloader', 'radio-module', 'coremark', 'buildroot-menuconfig', 'flash-existing-all', 'build-manifest'}
        text = (ROOT / 'Makefile').read_text()
        declaration = next(line for line in text.splitlines() if line.startswith('LOCK_GOALS :='))
        self.assertTrue(required <= set(declaration.split()))

    def test_all_build_and_device_commands_serialize(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            helper = tmp / 'native'
            helper.write_text('#!/bin/sh\necho started >> "$EVENT"\nwhile [ ! -e "$RELEASE" ]; do sleep 0.05; done\n')
            helper.chmod(0o755)
            processes = []
            def launch(name, target):
                env = dict(os.environ, EVENT=str(tmp / (name + '.event')), RELEASE=str(tmp / (name + '.release')))
                for key in ['MAKEFLAGS', 'MFLAGS', 'MAKELEVEL', 'S31_BUILD_LOCKED']:
                    env.pop(key, None)
                p = subprocess.Popen(['make', '--no-print-directory', '-f', str(ROOT / 'Makefile'), target, 'MAKE='+str(helper), 'OUT_ROOT='+str(tmp / 'out')], cwd=ROOT, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
                processes.append((name,p)); return p
            def wait_event(name):
                deadline=time.monotonic()+5
                while not (tmp / (name+'.event')).exists():
                    if time.monotonic()>deadline:self.fail('inert native command did not start: '+name)
                    time.sleep(.02)
            try:
                launch('one','radio-fs'); wait_event('one')
                launch('two','flash-existing-all')
                self.assertFalse((tmp/'two.event').exists(), 'command escaped the build lock')
                (tmp/'one.release').touch(); wait_event('two')
            finally:
                for name,p in processes:(tmp/(name+'.release')).touch()
                for name,p in processes:
                    _,err=p.communicate(timeout=10)
                    self.assertEqual(p.returncode,0,err.decode())

    def test_build_modules_and_helpers_are_not_ignored(self):
        paths = [p.relative_to(ROOT).as_posix() for directory in ['mk', 'tools/build'] for p in (ROOT/directory).iterdir() if p.is_file()]
        result = subprocess.run(['git', 'check-ignore', *paths], cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(result.stdout, '')

    def test_prepare_cannot_mix_with_build(self):
        for target in ['download','fetch','fetch-rootfs']:
            result=subprocess.run(['make','-f',str(ROOT/'Makefile'),target,'image'],cwd=ROOT,capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('standalone preparation',result.stderr)
            self.assertNotIn('git submodule',result.stdout)

    def test_retired_selection_is_rejected(self):
        for assignment in ["PROFILE=peripherals", "PROFILE=radio-appliance", "S31_LEAN_RADIO=1", "S31_WIFI_ONLY=1"]:
            result = subprocess.run(["make", "help", assignment], cwd=ROOT, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("is removed", result.stderr)

    def test_full_board_has_one_output_and_no_selection_directory(self):
        self.assertFalse((ROOT / "configs/profiles").exists())
        self.assertFalse((ROOT / "configs/kernel/radio-appliance.config").exists())
        result = subprocess.run(["make", "-s", "help"], cwd=ROOT, capture_output=True, text=True, check=True)
        self.assertIn("Full board configuration", result.stdout)
        self.assertNotIn("out/<", result.stdout)
