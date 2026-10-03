"""Release-body and publication checks without publishing or accessing hardware."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.release import assets

EXPECTED_BODY = r'''## Linux

```sh
PORT=/dev/ttyUSB0
esptool --chip esp32s31 -p "$PORT" -b 2000000 erase-flash
esptool --chip esp32s31 -p "$PORT" -b 2000000 write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 s31_full_flash.bin
```

## Windows (PowerShell)

```powershell
$PORT="COM3"
esptool --chip esp32s31 -p "$PORT" -b 2000000 erase-flash
esptool --chip esp32s31 -p "$PORT" -b 2000000 write-flash `
  --flash-mode dio --flash-freq 80m --flash-size 16MB `
  0x0 s31_full_flash.bin
```
'''


def publish_script():
    workflow = (ROOT / '.github/workflows/release-images.yml').read_text()
    step = workflow.split('      - name: Publish images to GitHub Release\n', 1)[1]
    step = step.split('\n      - ', 1)[0]
    return textwrap.dedent(step.split('        run: |\n', 1)[1])


class ReleaseNotes(unittest.TestCase):
    def test_body_is_only_requested_linux_and_powershell_instructions(self):
        self.assertEqual((ROOT / 'configs/release-notes.md').read_text(), EXPECTED_BODY)

    def test_bash_syntax_without_running_flash_commands(self):
        linux_commands = EXPECTED_BODY.split('```sh\n', 1)[1].split('```', 1)[0]
        for script in (linux_commands, publish_script()):
            with self.subTest(script=script):
                subprocess.run(['bash', '-n'], input=script, text=True, check=True)

    def run_publication(self, corrupt=False):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        workspace = Path(temporary.name)
        (workspace / 'tools').symlink_to(ROOT / 'tools', target_is_directory=True)
        image_dir = workspace / 'dist/current'
        image_dir.mkdir(parents=True)
        for name in assets.assets():
            (image_dir / name).write_bytes(name.encode())
        assets.checksums(image_dir)
        if corrupt:
            (image_dir / 's31_full_flash.bin').write_bytes(b'corrupt')
        capture = workspace / 'release-argv'
        # Override gh in the test shell: no GitHub operation or esptool is run.
        stub = 'gh() { printf "%s\\0" "$@" > "$CAPTURE"; }\n'
        result = subprocess.run(
            ['bash', '-e', '-o', 'pipefail', '-c', stub + publish_script()],
            cwd=workspace, capture_output=True, text=True,
            env=dict(os.environ, GITHUB_WORKSPACE=str(ROOT), CAPTURE=str(capture),
                     RELEASE_TAG='fixture-tag', RELEASE_TITLE='fixture-title'))
        return result, capture

    def test_publication_passes_only_fixed_body_and_preserves_all_assets(self):
        result, capture = self.run_publication()
        self.assertEqual(result.returncode, 0, result.stderr)
        argv = capture.read_bytes().decode().rstrip('\0').split('\0')
        self.assertEqual(argv, [
            'release', 'create', 'fixture-tag',
            *['dist/current/' + name for name in assets.assets() + ['SHA256SUMS']],
            '--title', 'fixture-title',
            '--notes-file', str(ROOT / 'configs/release-notes.md'),
        ])
        self.assertEqual(Path(argv[-1]).read_text(), EXPECTED_BODY)

    def test_bad_asset_checksum_still_blocks_publication(self):
        result, capture = self.run_publication(corrupt=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(capture.exists())


if __name__ == '__main__':
    unittest.main()
