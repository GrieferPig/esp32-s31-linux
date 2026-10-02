#!/usr/bin/env python3
"""Host checks for literal argument handling and transactional config restore."""
import io
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tarfile
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[2]
LIB = REPO / "buildroot-external/board/esp32-s31/overlay/usr/lib/esp32-config"


class SystemConfiguration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.helper = Path(cls.build.name) / "s31-config-archive"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Os",
                        str(REPO / "rootfs/s31_config_archive.c"), "-o", str(cls.helper)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.conf = self.root / "conf"
        self.run = self.root / "run"
        self.conf.mkdir()
        self.run.mkdir()
        self.env = dict(os.environ, ESP32_CONFIG_DIR=str(self.conf),
                        ESP32_CONFIG_RUN_DIR=str(self.run),
                        ESP32_CONFIG_LOCALTIME=str(self.root / "localtime"),
                        PATH=str(self.helper.parent) + os.pathsep + os.environ["PATH"])
        self.prelude = "set -u\n" + "\n".join(
            ". " + shlex.quote(str(LIB / name)) for name in
            ["common.sh", "network.sh", "bluetooth.sh", "storage.sh", "system.sh", "maintenance.sh"])
        self.prelude += """
require_root() { :; }
ensure_config() {
    umask 077
    mkdir -p "$CONF_DIR" "$RUN_DIR"
    create_file_if_missing "$SYSTEM_CONF" 0600 hostname=esp32-s31
    create_file_if_missing "$WIFI_CONF" 0600 enabled=0 interface=wlan0 dhcp=1
    create_file_if_missing "$BT_CONF" 0600 enabled=0 index=0 le=1
}
"""

    def tearDown(self):
        self.tmp.cleanup()

    def shell(self, code, check=True):
        result = subprocess.run(["/bin/sh", "-c", self.prelude + "\n" + code],
                                env=self.env, text=True, capture_output=True, timeout=15)
        if check:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def archive(self, entries, filename="backup.tar"):
        path = self.root / filename
        with tarfile.open(path, "w", format=tarfile.USTAR_FORMAT) as archive:
            fmt = tarfile.TarInfo("format")
            payload = b"esp32-config-backup 1\n"
            fmt.size = len(payload)
            archive.addfile(fmt, io.BytesIO(payload))
            for name, content, kind in entries:
                member = tarfile.TarInfo(name)
                member.type = kind
                if kind == tarfile.REGTYPE:
                    member.size = len(content)
                    archive.addfile(member, io.BytesIO(content))
                else:
                    member.linkname = content.decode()
                    archive.addfile(member)
        return path

    def unpack(self, path):
        output = self.root / "unpacked"
        output.mkdir(exist_ok=True)
        return subprocess.run([str(self.helper), "unpack", str(path), str(output)],
                              text=True, capture_output=True)

    def test_archive_accepts_flat_ustar(self):
        path = self.archive([("system.conf", b"hostname=desk\n", tarfile.REGTYPE)])
        self.assertEqual(self.unpack(path).returncode, 0)
        self.assertEqual((self.root / "unpacked/system.conf").read_text(), "hostname=desk\n")

    def test_archive_rejects_paths_links_and_extensions(self):
        for index, (name, kind, content) in enumerate([
                ("../outside", tarfile.REGTYPE, b"bad"),
                ("/etc/shadow", tarfile.REGTYPE, b"bad"),
                ("system.conf", tarfile.SYMTYPE, b"/etc/shadow"),
                ("system.conf", tarfile.LNKTYPE, b"/etc/shadow"),
                ("system.conf", tarfile.DIRTYPE, b""),
                ("system.conf", tarfile.XHDTYPE, b"path=/etc/shadow"),
                ("unknown.conf", tarfile.REGTYPE, b"value=1\n")]):
            with self.subTest(name=name, kind=kind):
                path = self.archive([(name, content, kind)], f"invalid{index}.tar")
                shutil.rmtree(self.root / "unpacked", ignore_errors=True)
                self.assertNotEqual(self.unpack(path).returncode, 0)
        self.assertFalse((self.root / "outside").exists())

    def test_archive_rejects_duplicate_members(self):
        path = self.archive([("system.conf", b"hostname=one\n", tarfile.REGTYPE),
                             ("system.conf", b"hostname=two\n", tarfile.REGTYPE)])
        self.assertNotEqual(self.unpack(path).returncode, 0)

    def test_archive_rejects_corruption_binary_and_oversize(self):
        path = self.archive([("system.conf", b"hostname=one\n", tarfile.REGTYPE)])
        content = bytearray(path.read_bytes())
        content[100] ^= 1
        path.write_bytes(content)
        self.assertNotEqual(self.unpack(path).returncode, 0)
        for payload in [b"hostname=a\0bad\n", b"x" * 65537]:
            shutil.rmtree(self.root / "unpacked", ignore_errors=True)
            path = self.archive([("system.conf", payload, tarfile.REGTYPE)])
            self.assertNotEqual(self.unpack(path).returncode, 0)

    def test_backup_restore_preserves_unrelated_files(self):
        backup = self.root / "saved.tar"
        self.shell("ensure_config")
        (self.conf / "notes.txt").write_text("my unrelated data")
        self.shell("maintenance_backup " + shlex.quote(str(backup)))
        self.assertEqual(backup.stat().st_mode & 0o777, 0o600)
        (self.conf / "system.conf").write_text("hostname=changed\n")
        self.shell("maintenance_restore " + shlex.quote(str(backup)))
        self.assertEqual((self.conf / "system.conf").read_text(), "hostname=esp32-s31\n")
        self.assertEqual((self.conf / "notes.txt").read_text(), "my unrelated data")

    def test_invalid_settings_leave_existing_files_unchanged(self):
        self.shell("ensure_config")
        path = self.archive([("system.conf", b"hostname=valid\nexecute=touch /tmp/bad\n", tarfile.REGTYPE)])
        before = {p.name: p.read_bytes() for p in self.conf.iterdir()}
        result = self.shell("maintenance_restore " + shlex.quote(str(path)), check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.conf.iterdir()})

    def test_duplicate_config_keys_are_rejected(self):
        path = self.archive([("system.conf", b"hostname=one\nhostname=two\n", tarfile.REGTYPE)])
        self.assertNotEqual(self.shell("maintenance_restore " + shlex.quote(str(path)), check=False).returncode, 0)

    def test_failed_multi_file_install_rolls_back(self):
        self.shell("ensure_config")
        old_system = (self.conf / "system.conf").read_bytes()
        old_wifi = (self.conf / "wifi.conf").read_bytes()
        stage = self.root / "stage"
        stage.mkdir()
        (stage / "system.conf").write_text("hostname=new\n")
        (stage / "wifi.conf").write_text("enabled=0\n")
        result = self.shell("""
mv() {
    case "$*" in *stage/wifi.conf*) return 1 ;; esac
    command mv "$@"
}
maintenance_install """ + shlex.quote(str(stage)) + " system.conf wifi.conf", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.conf / "system.conf").read_bytes(), old_system)
        self.assertEqual((self.conf / "wifi.conf").read_bytes(), old_wifi)
        self.assertFalse((self.conf / ".install-lock").exists())

    def test_targeted_reset_only_changes_selected_settings(self):
        self.shell("ensure_config")
        (self.conf / "system.conf").write_text("hostname=myboard\n")
        (self.conf / "gpio.conf").write_text("17 high none\n")
        self.shell("maintenance_reset network")
        self.assertFalse((self.conf / "wifi.conf").exists())
        self.assertEqual((self.conf / "system.conf").read_text(), "hostname=myboard\n")
        self.assertEqual((self.conf / "gpio.conf").read_text(), "17 high none\n")

    def test_autostart_arguments_are_literal(self):
        executable = self.root / "program with spaces"
        result = self.root / "received"
        executable.write_text("#!/bin/sh\nprintf '<%s>\\n' \"$@\" >\"$OUTPUT\"\n")
        executable.chmod(0o755)
        marker = self.root / "must-not-exist"
        args = ["two words", "$(touch " + str(marker) + ")", "`touch " + str(marker) + "`", "*", "", "x\\y"]
        code = "system_userapp_configure " + " ".join(shlex.quote(x) for x in [str(executable)] + args)
        code += "\nexport OUTPUT=" + shlex.quote(str(result)) + "\nsystem_userapp_run"
        self.shell(code)
        self.assertFalse(marker.exists())
        self.assertEqual(result.read_text(), "".join("<" + arg + ">\n" for arg in args))

    def test_autostart_cancel_does_not_commit_program(self):
        self.shell("system_ensure_extra")
        before = (self.conf / "autostart.conf").read_bytes()
        counter = self.root / "dialog-count"
        self.shell("""
ui_dialog() {
    count=$(cat """ + shlex.quote(str(counter)) + """ 2>/dev/null || echo 0)
    count=$((count+1)); printf '%s' "$count" >""" + shlex.quote(str(counter)) + """
    case "$count" in 1) echo configure;; 2) echo /bin/echo;; 3) return 1;; *) return 1;; esac
}
system_userapp_menu
""")
        self.assertEqual((self.conf / "autostart.conf").read_bytes(), before)

    def test_explicit_start_works_disabled_and_detaches_from_terminal(self):
        probe = subprocess.Popen(["sleep", "1"])
        try:
            stat = Path(f"/proc/{probe.pid}/stat")
            if not stat.exists() or "(sleep)" not in stat.read_text():
                self.skipTest("sandbox /proc does not reflect child process IDs")
        finally:
            probe.terminate()
            probe.wait()
        frontend = self.root / "esp32-config-test"
        frontend.write_text("#!/bin/sh\n" + self.prelude + "\nsystem_userapp_run\n")
        frontend.chmod(0o755)
        executable = self.root / "worker"
        started = self.root / "started"
        executable.write_text("#!/bin/sh\nprintf started >" + shlex.quote(str(started)) + "\nexec sleep 30\n")
        executable.chmod(0o755)
        self.env["ESP32_CONFIG_COMMAND"] = str(frontend)
        self.shell("system_userapp_configure " + shlex.quote(str(executable)) + "\nsystem_userapp_start boot")
        self.assertFalse(started.exists())
        try:
            self.shell("system_userapp_start")
            for _ in range(100):
                if started.exists():
                    break
                time.sleep(0.01)
            self.assertTrue(started.exists())
            pid = int((self.run / "userapp.owner").read_text().split()[0])
            self.assertEqual(os.getpgid(pid), pid)
            self.assertEqual(os.getsid(pid), pid)
            self.assertIn("enabled=0", (self.conf / "autostart.conf").read_text())
        finally:
            self.shell("system_userapp_stop")

    def test_autostart_invalid_args_do_not_partially_write(self):
        self.shell("system_ensure_extra")
        before = (self.conf / "autostart.conf").read_bytes()
        result = self.shell("system_userapp_configure /bin/echo " + shlex.quote("bad\nargument"), check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.conf / "autostart.conf").read_bytes(), before)

    def test_control_characters_cannot_make_unrestorable_backups(self):
        self.shell("ensure_config")
        (self.conf / "system.conf").write_bytes(b"hostname=esp32-s31\n# bad \x1b control\n")
        destination = self.root / "bad.tar"
        result = self.shell("maintenance_backup " + shlex.quote(str(destination)), check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(destination.exists())

    def test_autostart_argument_size_matches_archive_limits(self):
        self.shell("system_ensure_extra")
        (self.conf / "autostart.args").write_text(("x" * 1024 + "\n") * 64)
        self.assertNotEqual(self.shell("system_validate_autostart_config \"$AUTOSTART_CONF\" \"$AUTOSTART_ARGS\"",
                                      check=False).returncode, 0)

    def test_mutating_cli_rejects_extra_arguments(self):
        self.shell("system_ensure_extra")
        before = {p.name: p.read_bytes() for p in self.conf.iterdir()}
        for command in ["password", "autostart enable", "autostart disable", "autostart start",
                        "autostart stop", "time stop", "time sync", "time apply"]:
            result = self.shell("system_cli " + command + " --dry-run", check=False)
            self.assertEqual(result.returncode, 2, command)
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.conf.iterdir()})

    def test_timezone_selection_keeps_dst(self):
        if not Path("/usr/share/zoneinfo/America/Los_Angeles").exists():
            self.skipTest("Host zoneinfo is not installed")
        self.shell("system_time_configure America/Los_Angeles 0 pool.ntp.org")
        self.assertEqual(os.readlink(self.root / "localtime"), "/usr/share/zoneinfo/America/Los_Angeles")
        result = self.shell("TZ=:/usr/share/zoneinfo/America/Los_Angeles date -d '2026-01-01 12:00:00' +%z; "
                            "TZ=:/usr/share/zoneinfo/America/Los_Angeles date -d '2026-07-01 12:00:00' +%z")
        self.assertEqual(result.stdout.splitlines(), ["-0800", "-0700"])


if __name__ == "__main__":
    unittest.main()
