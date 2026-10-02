#!/usr/bin/env python3
"""Integration checks for the real esp32-config dispatcher and shared UI.

Hardware commands use mocks; the configuration modules and backup parser are
real. No test changes the host network, services, hostname, or GPIO state.
"""
from pathlib import Path
import os
import pty
import select
import time
import shlex
import shutil
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
OVERLAY = ROOT / 'buildroot-external/board/esp32-s31/overlay'
LIB = OVERLAY / 'usr/lib/esp32-config'
MAIN = OVERLAY / 'usr/sbin/esp32-config'


class FrontendIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.helper = Path(cls.build.name) / 's31-config-archive'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Os',
                        str(ROOT / 'rootfs/s31_config_archive.c'), '-o', str(cls.helper)],
                       check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.lib = self.base / 'lib'
        shutil.copytree(LIB, self.lib)
        self.conf, self.run, self.bin = (self.base / name for name in ('conf', 'run', 'bin'))
        for path in (self.conf, self.run, self.bin):
            path.mkdir()
        # Redirect the helper's fixed /run/wpa_supplicant mkdir for an entirely
        # unprivileged host test. Defaults and the actual transaction helpers
        # remain shared with production.
        with (self.lib / 'common.sh').open('a') as stream:
            stream.write('''
require_root() { :; }
ensure_config() {
    umask 077
    mkdir -p "$CONF_DIR" "$RUN_DIR"
    create_file_if_missing "$SYSTEM_CONF" 0600 hostname=esp32-test || return 1
    create_file_if_missing "$WIFI_CONF" 0600 enabled=0 interface=wlan0 dhcp=1 || return 1
    create_file_if_missing "$BT_CONF" 0600 enabled=0 index=0 le=1 || return 1
}
''')
        self.trace = self.base / 'trace'
        self.trace.write_text('')
        self.dialogs = self.base / 'dialogs'
        self.dialogs.write_text('')
        self.resolv = self.base / 'resolv.conf'
        self.resolv.write_text('nameserver 192.0.2.1\n')
        shutil.copy2(self.helper, self.bin / self.helper.name)
        self.mock('hostname', 'if [ "$#" = 0 ]; then printf "esp32-test\\n"; else printf "hostname %s\\n" "$*" >>"$TRACE"; fi\n')
        self.mock('ip', '''case "$*" in
  *"address show"*) [ -z "${MOCK_ADDRESS:-}" ] || printf '    inet %s brd 192.0.2.255\n' "$MOCK_ADDRESS" ;;
  *"link show"*) printf '2: wlan0: <BROADCAST,UP>\n' ;;
  *"route show"*) : ;;
  *) printf 'ip %s\n' "$*" >>"$TRACE" ;;
esac
exit 0
''')
        self.mock('wpa_cli', '''case "$*" in
  *status) printf 'wpa_state=%s\n' "${MOCK_WPA_STATE:-DISCONNECTED}" ;;
  *ping) printf 'PONG\n' ;;
  *) printf 'wpa_cli %s\n' "$*" >>"$TRACE" ;;
esac
''')
        for name in ('wpa_supplicant', 'udhcpc', 'ntpd', 'passwd', 'rfkill'):
            self.mock(name, f"printf '{name} %s\\n' \"$*\" >>\"$TRACE\"\n")
        self.mock('sleep', ':\n')
        self.mock('s31-gpio', 'case "$1" in ping) exit 1;; list) printf "No configured GPIO\\n";; check) exit 0;; *) printf "gpio %s\\n" "$*" >>"$TRACE";; esac\n')
        self.mock('s31-overlay', 'case "$1" in status|list|check) exit 0;; *) printf "overlay %s\\n" "$*" >>"$TRACE";; esac\n')
        self.mock('radio-service', 'printf "radio %s\\n" "$*" >>"$TRACE"\n')
        self.mock('bt-service', 'case "$1" in status) exit 1;; *) printf "bluetooth %s\\n" "$*" >>"$TRACE";; esac\n')
        self.mock('dialog', '''printf '%s\n' "$*" >>"$DIALOG_TRACE"
case "$*" in *--menu*) exit 1;; *) exit 0;; esac
''')
        self.env = dict(os.environ,
                        ESP32_CONFIG_LIB_DIR=str(self.lib), ESP32_CONFIG_DIR=str(self.conf),
                        ESP32_CONFIG_RUN_DIR=str(self.run),
                        ESP32_CONFIG_RADIO_SERVICE=str(self.bin / 'radio-service'),
                        ESP32_CONFIG_BT_SERVICE=str(self.bin / 'bt-service'),
                        ESP32_CONFIG_RESOLV_CONF=str(self.resolv),
                        ESP32_CONFIG_LOCALTIME=str(self.base / 'localtime'),
                        ESP32_CONFIG_ASSOC_TIMEOUT='1', ESP32_CONFIG_ADDRESS_TIMEOUT='1',
                        TRACE=str(self.trace), DIALOG_TRACE=str(self.dialogs), TERM='xterm',
                        PATH=str(self.bin) + os.pathsep + os.environ['PATH'])

    def mock(self, name, body):
        path = self.bin / name
        path.write_text('#!/bin/sh\n' + body)
        path.chmod(0o755)

    def command(self, *arguments, stdin=None, **extra):
        return subprocess.run(['sh', str(MAIN), *arguments], env={**self.env, **extra},
                              input=stdin, text=True, capture_output=True, timeout=10)

    def test_help_does_not_create_settings_or_start_services(self):
        result = self.command('--help')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('wifi connect SSID [psk|open]', result.stdout)
        self.assertIn('network static ADDRESS PREFIX GATEWAY_OR_- [DNS ...]', result.stdout)
        self.assertNotIn('ssh', result.stdout.lower())
        self.assertNotIn('dropbear', result.stdout.lower())
        self.assertEqual(list(self.conf.iterdir()), [])
        self.assertEqual(self.trace.read_text(), '')

    def test_cancel_from_main_menu_does_not_apply_settings(self):
        master, slave = pty.openpty()
        try:
            result = subprocess.run(['sh', str(MAIN)], env=self.env,
                                    stdin=slave, stdout=slave, stderr=slave, timeout=10)
            self.assertEqual(result.returncode, 0)
        finally:
            os.close(master)
            os.close(slave)
        self.assertIn('ESP32-S31 Configuration', self.dialogs.read_text())
        self.assertNotIn('Run a short test', self.dialogs.read_text())
        self.assertNotIn('SSH', self.dialogs.read_text())
        self.assertEqual(self.trace.read_text(), '')
        self.assertEqual({p.name for p in self.conf.iterdir()}, {'system.conf', 'wifi.conf', 'bluetooth.conf'})

    def test_status_reports_state_without_applying_it(self):
        result = self.command('status')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('Wi-Fi: Off', result.stdout)
        self.assertIn('IPv4: Automatic', result.stdout)
        self.assertEqual(self.trace.read_text(), '')

    def test_backup_through_dispatcher_preserves_running_services(self):
        backup = self.base / 'device settings.tar'
        result = self.command('maintenance', 'backup', str(backup))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with tarfile.open(backup) as archive:
            self.assertEqual(set(archive.getnames()), {'format', 'system.conf', 'wifi.conf', 'bluetooth.conf'})
        self.assertEqual(self.trace.read_text(), '')

    def test_dispatcher_single_password_connect_preserves_pending_exit(self):
        result = self.command('wifi', 'connect', 'music network', stdin='a' * 64 + '\n',
                              MOCK_WPA_STATE='ASSOCIATING')
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn('still in progress', result.stdout)
        self.assertIn('ssid=' + b'music network'.hex(), (self.conf / 'wpa_supplicant.conf').read_text())
        self.assertNotIn('Repeat', result.stdout)
        self.assertEqual((self.run / 'wifi.state').read_text(), 'pending\n')
        self.assertLess(self.trace.read_text().index('bluetooth stop'), self.trace.read_text().index('wpa_supplicant'))

    def test_invalid_static_address_keeps_saved_policy(self):
        wifi = self.conf / 'wifi.conf'
        original = 'enabled=0\ninterface=wlan0\ndhcp=1\n'
        wifi.write_text(original)
        result = self.command('network', 'static', '192.0.2.4', '24', '198.51.100.1', '1.1.1.1')
        self.assertEqual(result.returncode, 1)
        self.assertEqual(wifi.read_text(), original)
        self.assertEqual(self.trace.read_text(), '')

    def test_apply_all_keeps_pending_and_continues_other_settings(self):
        with (self.lib / 'interfaces.sh').open('a') as stream:
            stream.write('''
system_apply() { printf 'system\n' >>"$TRACE"; return "${SYSTEM_RESULT:-0}"; }
system_extra_apply() { printf 'time\n' >>"$TRACE"; }
radio_reload_and_apply_all() { printf 'radio pending\n' >>"$TRACE"; return 2; }
gpio_apply() { printf 'gpio\n' >>"$TRACE"; }
usb_apply() { printf 'usb\n' >>"$TRACE"; }
storage_apply() { printf 'storage\n' >>"$TRACE"; }
''')
        result = self.command('apply', 'all')
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(self.trace.read_text().splitlines(), ['system', 'time', 'radio pending', 'gpio', 'usb', 'storage'])
        self.trace.write_text('')
        result = self.command('apply', 'all', SYSTEM_RESULT='1')
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.trace.read_text().splitlines()[-1], 'storage')



    def test_partial_password_eof_cancels_without_saving(self):
        master, slave = pty.openpty()
        child = subprocess.Popen(['sh', str(MAIN), 'wifi', 'configure'], env=self.env,
                                 stdin=slave, stdout=slave, stderr=slave)
        try:
            os.write(master, b'music network\n')
            output = b''
            deadline = time.monotonic() + 5
            while b'Passphrase' not in output and time.monotonic() < deadline:
                if select.select([master], [], [], 0.2)[0]:
                    output += os.read(master, 4096)
            self.assertIn(b'Passphrase', output)
            # First EOF releases the partial line; the second terminates read.
            # A valid-length partial key must never become a submitted secret.
            os.write(master, b'a' * 64 + b'\x04\x04')
            self.assertEqual(child.wait(timeout=5), 1)
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()
            os.close(master)
            os.close(slave)
        self.assertFalse((self.conf / 'wpa_supplicant.conf').exists())
        self.assertEqual(self.trace.read_text(), '')

    def test_extra_cli_arguments_do_not_apply_or_disable_network(self):
        wifi = self.conf / 'wifi.conf'
        original = 'enabled=1\ninterface=wlan0\ndhcp=1\n'
        wifi.write_text(original)
        for arguments in [('wifi', 'disable', '--dry-run'), ('wifi', 'forget', '--dry-run'),
                          ('wifi', 'scan', '--dry-run'), ('apply', 'wifi', '--dry-run'),
                          ('stop', '--dry-run')]:
            with self.subTest(arguments=arguments):
                result = self.command(*arguments)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertEqual(wifi.read_text(), original)
                self.assertEqual(self.trace.read_text(), '')

    def test_shared_ui_pending_becomes_notice_without_failure_dialog(self):
        script = '''
. "$ESP32_CONFIG_LIB_DIR/common.sh"
ensure_config
pending() { printf 'Settings saved; association is still pending.\n'; return 2; }
ui_run_action Wi-Fi 'Connecting...' Connected pending
code=$?
printf 'result=%s\nnotice=%s\n' "$code" "$UI_NOTICE"
'''
        result = subprocess.run(['sh', '-u', '-c', script], env=self.env,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('result=2', result.stdout)
        self.assertIn('notice=Settings saved', result.stdout)
        self.assertNotIn('--textbox', self.dialogs.read_text())
        self.assertNotIn('--msgbox', self.dialogs.read_text())


if __name__ == '__main__':
    unittest.main()
