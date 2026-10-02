#!/usr/bin/env python3
"""Exercise network transactions and DHCP/SSID/radio behavior with host mocks.

No radio hardware or root network namespace is used. Run with unittest or
python3 tools/tests/test_esp32_config_network.py.
"""
from pathlib import Path
import hashlib
import os
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
OVERLAY = ROOT / "buildroot-external/board/esp32-s31/overlay"
LIB = OVERLAY / "usr/lib/esp32-config"
HOOK = OVERLAY / "usr/share/udhcpc/default.script"


class NetworkBehavior(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.conf = self.base / "conf"
        self.run = self.base / "run"
        self.bin = self.base / "bin"
        for p in (self.conf, self.run, self.bin):
            p.mkdir()
        self.wifi = self.conf / "wifi.conf"
        self.profile = self.conf / "wpa_supplicant.conf"
        self.wifi.write_text("enabled=0\ninterface=wlan0\ndhcp=1\n")
        (self.conf / "bluetooth.conf").write_text("enabled=1\nindex=0\nle=1\n")
        self.resolv = self.base / "resolver"
        self.resolv.write_text("nameserver 9.9.9.9\n")
        self.trace = self.base / "trace"
        self.trace.write_text("")
        self.env = dict(os.environ, ESP32_CONFIG_DIR=str(self.conf),
                        ESP32_CONFIG_RUN_DIR=str(self.run),
                        ESP32_CONFIG_LIB_DIR=str(LIB),
                        ESP32_CONFIG_RESOLV_CONF=str(self.resolv),
                        ESP32_CONFIG_ASSOC_TIMEOUT="1", ESP32_CONFIG_ADDRESS_TIMEOUT="1",
                        ESP32_CONFIG_SCAN_DELAY="0", TRACE=str(self.trace),
                        PATH=str(self.bin) + ":" + os.environ["PATH"])
        self.mock("ip", 'printf "ip %s\\n" "$*" >>"$TRACE"\ncase "$*" in *"address show"*) [ -z "${MOCK_ADDRESS:-}" ] || printf "    inet %s brd 192.0.2.255\\n" "$MOCK_ADDRESS";; *"link show"*) printf "2: wlan0: <BROADCAST,UP,LOWER_UP>\\n";; esac\nexit 0\n')
        self.mock("wpa_cli", 'case "$*" in *status) printf "wpa_state=%s\\n" "${MOCK_WPA_STATE:-COMPLETED}";; *ping) printf "PONG\\n";; *scan_results) cat "$MOCK_SCAN";; *scan) printf "OK\\n";; esac\n')
        self.mock("wpa_supplicant", 'printf "supplicant %s\\n" "$*" >>"$TRACE"\n')
        self.mock("sleep", ":\n")
        self.mock("udhcpc", 'printf "udhcpc %s\\n" "$*" >>"$TRACE"\n')
        wpa = self.bin / "wpa_passphrase"
        wpa.write_text(f"#!{sys.executable}\n" +
                       "import sys,hashlib,os\nssid=os.fsencode(sys.argv[1]); password=sys.stdin.buffer.readline().rstrip(b'\\n')\n" +
                       "key=hashlib.pbkdf2_hmac('sha1',password,ssid,4096,32).hex()\n" +
                       "print('network={'); print('\\t#psk=\"PLAINTEXT-MUST-NOT-BE-STORED\"'); print('\\tpsk='+key); print('}')\n")
        wpa.chmod(0o755)

    def mock(self, name, body):
        path = self.bin / name
        path.write_text("#!/bin/sh\n" + body)
        path.chmod(0o755)

    def shell(self, body, *, stdin=None, env=None, check=True):
        program = '. "$ESP32_CONFIG_LIB_DIR/common.sh"\n. "$ESP32_CONFIG_LIB_DIR/network.sh"\nensure_config() { :; }\nrequire_root() { :; }\n' + body
        result = subprocess.run(["sh", "-eu", "-c", program], input=stdin,
                                text=True, capture_output=True, env={**self.env, **(env or {})})
        if check:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_invalid_policy_does_not_change_saved_configuration(self):
        original = self.wifi.read_bytes()
        bad = ["0 192.0.2.4 33 192.0.2.1 manual 1.1.1.1",
               "0 192.0.2.4 24 198.51.100.1 manual 1.1.1.1",
               "0 192.0.2.4 24 192.0.2.1 auto 1.1.1.1",
               "1 '' 24 '' manual '1.1.1.1;reboot'",
               "1 '' 24 '' manual 001.2.3.4"]
        for args in bad:
            with self.subTest(args=args):
                self.assertNotEqual(self.shell("network_save_policy " + args, check=False).returncode, 0)
                self.assertEqual(self.wifi.read_bytes(), original)

    def test_static_policy_commits_complete_fields(self):
        self.shell("network_save_policy 0 192.0.2.4 24 192.0.2.1 manual '1.1.1.1 9.9.9.9'")
        saved = dict(line.split("=", 1) for line in self.wifi.read_text().splitlines())
        self.assertEqual(saved["address"], "192.0.2.4")
        self.assertEqual(saved["gateway"], "192.0.2.1")
        self.assertEqual(saved["dns_servers"], "1.1.1.1 9.9.9.9")
        self.assertEqual(saved["enabled"], "0")
        self.assertEqual(self.wifi.stat().st_mode & 0o777, 0o600)

    def test_raw_ssid_and_password_are_preserved_without_plaintext_file(self):
        ssid = '  music "音楽"\\x41\n'
        password = 'one-password-only!'
        self.shell('wifi_save_profile "$SSID" "$PASSWORD"', env={"SSID": ssid, "PASSWORD": password})
        profile = self.profile.read_text()
        expected = hashlib.pbkdf2_hmac("sha1", password.encode(), ssid.encode(), 4096, 32).hex()
        self.assertIn("ssid=" + ssid.encode().hex(), profile)
        self.assertIn("psk=" + expected, profile)
        self.assertNotIn(password, profile)
        self.assertNotIn("PLAINTEXT", profile)
        self.assertFalse(list(self.conf.glob("*.tmp.*")))
        self.shell('wifi_validate_files "$WIFI_CONF" "$WIFI_PROFILE"')

    def test_invalid_password_does_not_replace_either_file(self):
        self.profile.write_text("original-profile\n")
        original = self.wifi.read_bytes()
        result = self.shell("wifi_save_profile 'test' short", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.wifi.read_bytes(), original)
        self.assertEqual(self.profile.read_text(), "original-profile\n")

    def test_open_profile_and_binary_ssid_precomputed_key(self):
        self.shell("wifi_save_hex_profile 410042 '' open")
        self.assertIn("ssid=410042", self.profile.read_text())
        self.assertIn("key_mgmt=NONE", self.profile.read_text())
        self.shell('wifi_validate_files "$WIFI_CONF" "$WIFI_PROFILE"')
        self.shell("wifi_save_hex_profile 410042 " + "a" * 64)
        self.assertIn("psk=" + "a" * 64, self.profile.read_text())

    def test_scan_bytes_are_not_taken_from_display_labels(self):
        entries = [(r'caf\xc3\xa9', b'caf\xc3\xa9', '[WPA2-PSK-CCMP][ESS]', 'psk'),
                   (r'literal\\x41', b'literal\\x41', '[ESS]', 'open'),
                   (r' trailing \n', b' trailing \n', '[WPA2-EAP-CCMP][ESS]', 'unsupported'),
                   (r'zero\x00byte', b'zero\x00byte', '[ESS]', 'open')]
        scan = "bssid / frequency / signal level / flags / ssid\n"
        for index, (raw, _, flags, _) in enumerate(entries):
            scan += f"00:11:22:33:44:{index:02x}\t2412\t-50\t{flags}\t{raw}\n"
        out = self.shell("wifi_scan_parse", stdin=scan).stdout.splitlines()
        self.assertEqual(len(out), 4)
        for line, (_, raw, _, security) in zip(out, entries):
            self.assertEqual(line.split("\t"), [raw.hex(), '-50', security])

    def test_legacy_configure_stdin_is_three_line_save_only(self):
        self.shell("wifi_configure", stdin="network\npassword123\npassword123\n")
        self.assertIn("ssid=6e6574776f726b", self.profile.read_text())
        self.assertEqual(self.trace.read_text(), "")

    def test_new_connect_stdin_accepts_one_password(self):
        self.shell('wifi_connect_saved() { printf "connect:%s\\n" "$1" >>"$TRACE"; }; wifi_cli connect network', stdin="password123\n")
        self.assertIn("connect:0", self.trace.read_text())

    def test_unchanged_enabled_setting_does_not_restart_radio(self):
        self.shell('radio_reload_and_apply_all() { printf "RESTART\\n" >>"$TRACE"; }; wifi_set_enabled 0')
        self.assertEqual(self.trace.read_text(), "")

    def test_scan_cancellation_restores_policy_and_bluetooth_before_wifi(self):
        original = self.wifi.read_bytes()
        self.shell(r'''
radio_service() { printf 'radio:%s:%s\n' "${S31_RADIO_VOLATILE_MODE:-saved}" "$*" >>"$TRACE"; }
bluetooth_service() { printf 'bluetooth:%s\n' "$*" >>"$TRACE"; }
wifi_stop() { printf 'wifi:stop\n' >>"$TRACE"; }
bt_apply() { printf 'bluetooth:apply\n' >>"$TRACE"; }
wifi_apply() { printf 'wifi:apply\n' >>"$TRACE"; }
wifi_scan_begin
wifi_scan_end
''')
        self.assertEqual(self.wifi.read_bytes(), original)
        trace = self.trace.read_text()
        self.assertIn("radio:combo:restart", trace)
        self.assertIn("radio:saved:restart", trace)
        self.assertLess(trace.index("bluetooth:apply"), trace.index("wifi:apply"))
        self.assertFalse(list(self.run.glob("scan.*")))

    def test_association_pending_is_distinct_from_failure(self):
        self.wifi.write_text("enabled=1\ninterface=wlan0\ndhcp=1\n")
        self.profile.write_text("network={}\n")
        result = self.shell('radio_prepare() { :; }; wifi_stop() { :; }; wifi_apply',
                            env={"MOCK_WPA_STATE": "ASSOCIATING"}, check=False)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual((self.run / "wifi.state").read_text(), "pending\n")
        self.assertNotIn("-q", self.trace.read_text())

    def test_manual_dns_survives_dhcp_renewal_and_preserves_symlink(self):
        self.wifi.write_text("enabled=1\ninterface=wlan0\ndhcp=1\ndns_mode=manual\ndns_servers=1.1.1.1 9.9.9.9\n")
        link = self.base / "resolv.conf"
        link.symlink_to(self.resolv)
        result = subprocess.run(["sh", str(HOOK), "renew"], text=True, capture_output=True,
                                env={**self.env, "ESP32_CONFIG_RESOLV_CONF": str(link),
                                     "interface": "wlan0", "ip": "192.0.2.4", "subnet": "255.255.255.0",
                                     "router": "192.0.2.1", "dns": "8.8.8.8", "domain": "example.net"})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(link.is_symlink())
        self.assertEqual(self.resolv.read_text(), "nameserver 1.1.1.1\nnameserver 9.9.9.9\n")

    def test_old_dhcp_callback_cannot_overwrite_static_settings(self):
        self.wifi.write_text("enabled=1\ninterface=wlan0\ndhcp=0\naddress=192.0.2.4\nprefix=24\ndns_mode=manual\n")
        for event in ("deconfig", "bound", "renew"):
            result = subprocess.run(["sh", str(HOOK), event], env={**self.env, "interface": "wlan0"}, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.trace.read_text(), "")
        self.assertEqual(self.resolv.read_text(), "nameserver 9.9.9.9\n")

    def test_backup_validator_rejects_extra_supplicant_directives(self):
        self.shell("wifi_save_profile network password123")
        self.profile.write_text(self.profile.read_text() + "include=/tmp/arbitrary\n")
        self.assertNotEqual(self.shell('wifi_validate_files "$WIFI_CONF" "$WIFI_PROFILE"', check=False).returncode, 0)

    def test_address_dialog_cancel_discards_draft(self):
        original = self.wifi.read_bytes()
        queue = self.base / "queue"
        queue.write_text("mode\n0\naddress\n198.51.100.8\nCANCEL\n")
        self.shell(r'''
ui_dialog() {
  value=$(head -n 1 "$QUEUE")
  sed '1d' "$QUEUE" >"$QUEUE.next"; mv "$QUEUE.next" "$QUEUE"
  [ "$value" != CANCEL ] || return 1
  printf '%s\n' "$value"
}
network_address_menu
''', env={"QUEUE": str(queue)}, check=False)
        self.assertEqual(self.wifi.read_bytes(), original)


    def test_tui_asks_for_password_once_and_saves_then_connects(self):
        self.shell(r'''
ui_dialog() {
  case "$*" in
    *--passwordbox*) printf 'password\n' >>"$TRACE"; printf 'password123\n' ;;
  esac
}
wifi_choose_network() {
  WIFI_SELECTED_HEX=6e6574776f726b
  WIFI_SELECTED_SECURITY=psk
}
wifi_ui_connect() { printf 'connect\n' >>"$TRACE"; }
wifi_setup_wizard
''')
        self.assertEqual(self.trace.read_text().splitlines(), ['password', 'connect'])
        self.assertIn('ssid=6e6574776f726b', self.profile.read_text())

    def test_cancelled_password_does_not_save_network(self):
        original = self.wifi.read_bytes()
        self.shell(r'''
ui_dialog() { return 1; }
wifi_choose_network() {
  WIFI_SELECTED_HEX=6e6574776f726b
  WIFI_SELECTED_SECURITY=psk
}
wifi_setup_wizard
''', check=False)
        self.assertEqual(self.wifi.read_bytes(), original)
        self.assertFalse(self.profile.exists())

    def test_connect_after_scan_keeps_prepared_bluetooth_controller(self):
        self.shell(r'''
radio_service() { printf 'radio:%s:%s\n' "${S31_RADIO_VOLATILE_MODE:-saved}" "$*" >>"$TRACE"; }
bluetooth_service() { printf 'bluetooth:%s\n' "$*" >>"$TRACE"; }
wifi_stop() { :; }
wifi_scan_begin
wifi_save_profile network password123
wifi_scan_commit
[ "$WIFI_SCAN_COMMITTED" = 1 ]
wifi_scan_end
''')
        trace = self.trace.read_text()
        self.assertEqual(trace.count('radio:combo:restart'), 1)
        self.assertNotIn('radio:saved:restart', trace)
        self.assertFalse(list(self.run.glob('scan.*')))

    def test_managed_stop_retains_pidfile_when_process_does_not_exit(self):
        proc = self.base / 'proc' / '123'
        proc.mkdir(parents=True)
        (proc / 'comm').write_text('udhcpc\n')
        (proc / 'stat').write_text(' '.join(['123', '(udhcpc)', 'S'] + ['0'] * 18 + ['99']) + '\n')
        pidfile = self.run / 'udhcpc.pid'
        pidfile.write_text('123\n')
        result = self.shell(r'''
kill() { printf 'kill %s\n' "$*" >>"$TRACE"; return 0; }
stop_managed_pid "$PIDFILE" udhcpc
''', env={'ESP32_CONFIG_PROC_DIR': str(proc.parent), 'PIDFILE': str(pidfile)}, check=False)
        self.assertEqual(result.returncode, 1)
        self.assertTrue(pidfile.exists())
        self.assertIn('pid file was retained', result.stderr)
        self.assertEqual(self.trace.read_text().splitlines().count('kill 123'), 1)

    def test_managed_stop_never_signals_an_unrelated_process(self):
        proc = self.base / 'proc' / '123'
        proc.mkdir(parents=True)
        (proc / 'comm').write_text('unrelated\n')
        pidfile = self.run / 'udhcpc.pid'
        pidfile.write_text('123\n')
        self.shell(r'''
kill() { printf 'kill %s\n' "$*" >>"$TRACE"; return 0; }
stop_managed_pid "$PIDFILE" udhcpc
''', env={'ESP32_CONFIG_PROC_DIR': str(proc.parent), 'PIDFILE': str(pidfile)})
        self.assertEqual(self.trace.read_text().splitlines(), ['kill -0 123'])
        self.assertFalse(pidfile.exists())


if __name__ == "__main__":
    unittest.main()
