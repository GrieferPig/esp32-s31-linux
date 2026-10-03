"""Keep native env-shebang tools out of ESP-IDF's private Python environment."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
WRAPPER = ROOT / 'tools/build/host_python.py'


class HostPython(unittest.TestCase):
    def test_env_shebang_uses_selected_python_and_preserves_other_tools(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            idf = work / 'idf-bin'
            idf.mkdir()
            for name in ('python', 'python3'):
                path = idf / name
                path.write_text('#!/bin/sh\necho wrong-IDF-interpreter >&2\nexit 91\n')
                path.chmod(0o755)
            tool = idf / 'native-helper'
            tool.write_text('#!/bin/sh\necho preserved-host-tool\n')
            tool.chmod(0o755)
            command = work / 'binman-fixture'
            command.write_text('#!/usr/bin/env python3\nimport json, os, subprocess, sys\n'
                               'print(json.dumps([os.path.realpath(sys.executable), '
                               'subprocess.check_output(["native-helper"], text=True).strip()]))\n')
            command.chmod(0o755)
            shim = work / 'output/.host-python'
            argv = [sys.executable, str(WRAPPER), '--python', sys.executable,
                    '--shim', str(shim), '--', str(command)]
            env = dict(os.environ, PATH=str(idf) + os.pathsep + os.environ['PATH'])
            first = subprocess.run(argv, env=env, text=True, capture_output=True, check=True)
            self.assertEqual(json.loads(first.stdout),
                             [os.path.realpath(sys.executable), 'preserved-host-tool'])
            before = (shim / 'python3').lstat().st_mtime_ns
            subprocess.run(argv, env=env, capture_output=True, check=True)
            self.assertEqual((shim / 'python3').lstat().st_mtime_ns, before)

    def test_missing_interpreter_fails_before_native_command(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            marker = work / 'executed'
            result = subprocess.run([sys.executable, str(WRAPPER), '--python',
                                     str(work / 'missing-python'), '--shim', str(work / 'shim'),
                                     '--', 'touch', str(marker)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn('host Python is not executable', result.stderr)
            self.assertFalse(marker.exists())

    def test_uboot_native_build_uses_interpreter_wrapper(self):
        recipe = (ROOT / 'mk/boot.mk').read_text()
        self.assertIn('python3 $(ROOT)/tools/build/host_python.py', recipe)
        self.assertIn('--python "$(HOST_PYTHON)" --shim "$(UBOOT_OUT)/.host-python" --', recipe)
        self.assertIn('PYTHON="$(HOST_PYTHON)" PYTHON3="$(HOST_PYTHON)"', recipe)


if __name__ == '__main__':
    unittest.main()
