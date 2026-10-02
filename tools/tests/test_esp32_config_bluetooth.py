#!/usr/bin/env python3
"""Exercise Bluetooth configuration, service ownership and patched C behavior.

Shell/service tests use temporary policy and a tiny host executable. Patch tests
use the pristine pinned BTstack source from make btstack-source, or the directory
named by S31_BTSTACK_TEST_SOURCE. No HCI hardware is accessed.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULES = ROOT / "buildroot-external/board/esp32-s31/overlay/usr/lib/esp32-config"
PACKAGE = ROOT / "buildroot-external/package/btstack-s31"


def command(args, **kwargs):
    return subprocess.run(args, capture_output=True, text=True, timeout=40, **kwargs)


class BluetoothPolicy(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.work = Path(self.temp.name)
        self.conf = self.work / "bluetooth.conf"
        self.conf.write_text("enabled=0\nindex=0\nle=1\n")
        self.env = dict(os.environ, ESP32_CONFIG_DIR=str(self.work),
                        ESP32_CONFIG_RUN_DIR=str(self.work / "run"))
        self.preamble = '''
set -eu
. "$1/common.sh"
. "$1/bluetooth.sh"
shift
require_root() { :; }
ensure_config() { :; }
bluetooth_service() { printf '%s\n' "$1" >>"$CONF_DIR/service-calls"; }
radio_reload_and_apply_all() { printf 'reload\n' >>"$CONF_DIR/service-calls"; }
'''

    def tearDown(self):
        self.temp.cleanup()

    def shell(self, script, *args):
        return command(["sh", "-c", self.preamble + script, "test", str(MODULES), *args], env=self.env)

    def test_literal_and_utf8_name_round_trip(self):
        for name in (r"Desk \radio '$USER' $(true)", "音乐播放器", "A" * 29):
            with self.subTest(name=name):
                result = self.shell('bt_set_name "$1"; bt_name', name)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.rstrip("\n"), name)
                self.assertIn("name=" + name + "\n", self.conf.read_text())
        self.assertFalse((self.work / "service-calls").exists())

    def test_invalid_name_never_mutates_settings(self):
        original = self.conf.read_bytes()
        for name in ("", "A" * 30, "音乐播放器" * 2, "a\nb", "a\rb", "a\tb", "a\x7fb"):
            with self.subTest(name=name):
                result = self.shell('bt_set_name "$1"', name)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.conf.read_bytes(), original)

    def test_toggle_is_idempotent_and_rename_only_restarts_bt(self):
        result = self.shell('bt_set_enabled 0; bt_set_enabled 1; bt_set_enabled 1; '
                            'bt_set_name "Desk radio"; bt_set_name "Desk radio"')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.work / "service-calls").read_text().splitlines(), ["reload", "restart"])

    def test_validation_rejects_unknown_duplicate_and_unimplemented_policy(self):
        for settings in ("enabled=2\n", "index=1\n", "le=0\n", "name=\n", "x=y\n",
                         "enabled=1\nenabled=0\n", "enabled=0\nname=a\rb\n"):
            self.conf.write_text(settings)
            result = self.shell('bt_validate_file "$BT_CONF"')
            self.assertNotEqual(result.returncode, 0, settings)
        self.conf.write_text("enabled=1\nindex=0\nle=1\nname=Kitchen radio\n")
        result = self.shell('bt_validate_file "$BT_CONF"')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_pairing_reset_requires_enabled_service(self):
        result = self.shell('bt_clear_pairings')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.work / "service-calls").exists())
        self.conf.write_text("enabled=1\nindex=0\nle=1\n")
        result = self.shell('bt_clear_pairings')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.work / "service-calls").read_text().splitlines(), ["status", "clear-pairings"])


FAKE_SERVICE = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
static volatile sig_atomic_t stopped;
static void stop(int sig) { (void)sig; stopped = 1; }
int main(int argc, char **argv) {
    int reset = 0;
    for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], "-r")) reset = 1;
    signal(SIGINT, stop); signal(SIGTERM, stop);
    /* Some container sandboxes expose the outer /proc but use inner PIDs.
     * Provide a proc view for this real child, with its actual executable. */
    char directory[1024], exepath[1050];
    snprintf(directory, sizeof(directory), "%s/%d", getenv("S31_BTSTACK_PROC_DIR"), getpid());
    mkdir(directory, 0700);
    snprintf(exepath, sizeof(exepath), "%s/exe", directory);
    if (symlink(argv[0], exepath) != 0) return 2;
    FILE *events = fopen(getenv("BT_TEST_EVENTS"), "a");
    fprintf(events, "start:%s:%d\n", getenv("S31_BTSTACK_NAME"), reset);
    fclose(events);
    if (reset) {
        const char *failure = getenv("BT_TEST_RESET_FAIL");
        printf("S31_PAIRING_RESET=%s\n", failure ? "failed" : "ok");
        fflush(stdout);
    }
    while (!stopped) pause();
    events = fopen(getenv("BT_TEST_EVENTS"), "a");
    fprintf(events, "stop:%d\n", reset); fclose(events);
    unlink(exepath); rmdir(directory);
    return 0;
}
'''


@unittest.skipUnless(shutil.which("cc"), "host C compiler required")
class BluetoothService(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.work = Path(self.temp.name)
        source = self.work / "fake.c"
        source.write_text(FAKE_SERVICE)
        self.binary = self.work / "fake-btstack"
        subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(self.binary)], check=True)
        self.conf = self.work / "bluetooth.conf"
        self.conf.write_text("enabled=1\nname=Desk \\radio\n")
        (self.work / "cmdline").write_text("")
        (self.work / "mode").write_text("combo\n")
        (self.work / "proc").mkdir()
        self.env = dict(os.environ, S31_BTSTACK_CONFIG=str(self.conf),
                        S31_BTSTACK_RUN_DIR=str(self.work), S31_BTSTACK_DATA_DIR=str(self.work / "data"),
                        S31_BTSTACK_EXECUTABLE=str(self.binary), S31_HCI_DEVICE="/dev/null",
                        S31_BTSTACK_MODE_FILE=str(self.work / "mode"), S31_BTSTACK_CMDLINE=str(self.work / "cmdline"),
                        S31_BTSTACK_PROC_DIR=str(self.work / "proc"),
                        BT_TEST_EVENTS=str(self.work / "events"))

    def tearDown(self):
        self.service("stop")
        self.temp.cleanup()

    def service(self, action):
        return command(["sh", str(PACKAGE / "S40btstack"), action], env=self.env)

    def test_reset_stops_owner_runs_reset_once_and_restarts_normally(self):
        self.assertEqual(self.service("start").returncode, 0)
        self.assertEqual(self.service("start").returncode, 0)
        result = self.service("clear-pairings")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.work / "events").read_text().splitlines(), [
            r"start:Desk \radio:0", "stop:0", r"start:Desk \radio:1", "stop:1", r"start:Desk \radio:0"])
        self.assertIn("S31_PAIRING_RESET=ok", (self.work / "s31-btstack-pairing-reset.log").read_text())
        self.assertNotIn("S31_PAIRING_RESET", (self.work / "s31-btstack-a2dp.log").read_text())

    def test_reset_failure_restores_service_and_reports_failure(self):
        self.env["BT_TEST_RESET_FAIL"] = "1"
        self.assertEqual(self.service("start").returncode, 0)
        result = self.service("clear-pairings")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not confirmed", result.stderr)
        self.assertEqual(self.service("status").returncode, 0)
        self.assertTrue((self.work / "events").read_text().endswith(r"start:Desk \radio:0" + "\n"))

    def test_disabled_start_and_invalid_name_do_not_spawn(self):
        self.conf.write_text("enabled=0\n")
        self.assertEqual(self.service("start").returncode, 0)
        self.assertFalse((self.work / "events").exists())
        self.conf.write_text("enabled=1\nname=" + "x" * 30 + "\n")
        self.assertNotEqual(self.service("start").returncode, 0)
        self.assertFalse((self.work / "events").exists())


class PatchedBluetooth(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        pristine = Path(os.environ.get("S31_BTSTACK_TEST_SOURCE", ROOT / "build/btstack-source"))
        if not (pristine / "example/a2dp_sink_demo.c").exists():
            raise unittest.SkipTest("run make btstack-source, or set S31_BTSTACK_TEST_SOURCE to pristine pinned source")
        cls.temp = tempfile.TemporaryDirectory()
        cls.work = Path(cls.temp.name)
        for name in ("example/a2dp_sink_demo.c", "port/linux/main.c", "platform/linux/hci_transport_linux.c", "src/hci.c", "src/hci.h"):
            target = cls.work / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(pristine / name, target)
        for patch in sorted(PACKAGE.glob("*.patch")):
            result = command(["patch", "--batch", "-p1", "-i", str(patch)], cwd=cls.work)
            if result.returncode:
                raise AssertionError(f"{patch.name}: {result.stdout}\n{result.stderr}")

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "temp"):
            cls.temp.cleanup()

    def compile_run(self, source, *args):
        path = self.work / "test.c"
        path.write_text(source)
        result = command(["cc", "-std=c99", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                          "-Wno-unused-parameter", str(path), "-o", str(self.work / "test")])
        self.assertEqual(result.returncode, 0, result.stderr)
        result = command([str(self.work / "test"), *args])
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_complete_ble_name_matches_gap_at_maximum_length(self):
        source = (self.work / "example/a2dp_sink_demo.c").read_text()
        start = source.index("#define S31_BTSTACK_NAME_MAX")
        end = source.index("\nstatic int setup_demo(void)", start)
        functions = source[start:end]
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ENABLE_BLE
#define BLUETOOTH_DATA_TYPE_FLAGS 1
#define BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS 3
#define BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME 9
#define IO_CAPABILITY_NO_INPUT_NO_OUTPUT 3
#define ORG_BLUETOOTH_SERVICE_GENERIC_ACCESS 0x1800
#define ORG_BLUETOOTH_SERVICE_GENERIC_ATTRIBUTE 0x1801
#define ORG_BLUETOOTH_CHARACTERISTIC_GAP_DEVICE_NAME 0x2a00
#define ATT_PROPERTY_READ 2
#define ATT_SECURITY_NONE 0
typedef uint8_t bd_addr_t[6];
static char gap_name[30], scan_name[30];
static uint8_t scan_size, adv_size;
#define sm_set_io_capabilities(...) ((void)0)
#define sm_set_authentication_requirements(...) ((void)0)
#define att_db_util_init(...) ((void)0)
#define att_db_util_add_service_uuid16(...) ((void)0)
#define att_db_util_get_address() NULL
#define att_server_init(...) ((void)0)
#define gap_advertisements_enable(...) ((void)0)
static void gap_advertisements_set_params(int a,int b,int c,int d,bd_addr_t addr,int e,int f) { (void)addr; }
static void att_db_util_add_characteristic_uuid16(int uuid,int a,int b,int c,uint8_t *data,int len) {
    if (uuid == 0x2a00) { assert(len <= 29); memcpy(gap_name,data,len); gap_name[len] = 0; }
}
static void gap_advertisements_set_data(uint8_t len,uint8_t *data) { adv_size = len; assert(len <= 31); }
static void gap_scan_response_set_data(uint8_t len,uint8_t *data) {
    assert(len <= 31 && data[0] == len-1 && data[1] == 9);
    scan_size = len; memcpy(scan_name, data+2,len-2); scan_name[len-2] = 0;
}
''' + functions + r'''
int main(int argc,char **argv) {
    assert(argc == 3);
    setenv("S31_BTSTACK_NAME", argv[1], 1);
    s31_load_device_name(); s31_ble_setup();
    assert(strcmp(gap_name, argv[2]) == 0);
    assert(strcmp(scan_name, gap_name) == 0);
    assert(scan_size == strlen(gap_name)+2 && adv_size == 9);
    return 0;
}
'''
        for name, expected in (("n" * 29, "n" * 29), ("音乐播放器", "音乐播放器"),
                               (r"name \$HOME", r"name \$HOME"), ("n" * 30, "S31 Radio"),
                               ("a\nb", "S31 Radio"), ("", "S31 Radio")):
            self.compile_run(harness, name, expected)

    def test_reset_flag_is_consumed_before_later_working_event(self):
        source = (self.work / "port/linux/main.c").read_text()
        handler = re.search(r"^static void packet_handler \([^;{]*\{.*?^}", source, re.M | re.S).group()
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define HCI_EVENT_PACKET 4
#define BTSTACK_EVENT_STATE 0x60
#define HCI_STATE_WORKING 2
#define HCI_STATE_OFF 0
#define HCI_EVENT_COMMAND_COMPLETE 14
#define HCI_OPCODE_HCI_READ_LOCAL_VERSION_INFORMATION 1
#define ENABLE_CLASSIC
#define ENABLE_BLE
#define TLV_DB_PATH_PREFIX "/var/lib/btstack/btstack_"
#define TLV_DB_PATH_POSTFIX ".tlv"
typedef uint8_t bd_addr_t[6];
static bool tlv_reset, shutdown_triggered;
static char tlv_db_path[100];
static void *tlv_impl;
static int tlv_context, unlinks, init_count, key_count, mode;
#define hci_event_packet_get_type(p) ((p)[0])
#define btstack_event_state_get_state(p) ((p)[1])
#define gap_local_bd_addr(addr) memset((addr), 0, 6)
#define bd_addr_to_str(addr) "00:00:00:00:00:00"
#define bd_addr_to_str_with_delimiter(addr,delimiter) "00-00-00-00-00-00"
#define btstack_strcpy(dest,size,src) strcpy((dest),(src))
#define btstack_strcat(dest,size,src) strcat((dest),(src))
static int fake_unlink(const char *path) {
    ++unlinks;
    if (mode == 2) { errno = EACCES; return -1; }
    key_count = 0;
    if (mode == 1) { errno = ENOENT; return -1; }
    return 0;
}
#define unlink fake_unlink
static void *btstack_tlv_posix_init_instance(int *ctx,const char *path) { ++init_count; return ctx; }
#define btstack_tlv_set_instance(...) ((void)0)
#define btstack_link_key_db_tlv_get_instance(...) NULL
#define hci_set_link_key_db(...) ((void)0)
#define le_device_db_tlv_configure(...) ((void)0)
#define btstack_tlv_posix_deinit(...) ((void)0)
#define btstack_stdin_reset(...) ((void)0)
#define log_info(...) ((void)0)
#define hci_event_command_complete_get_command_opcode(p) 1
#define local_version_information_handler(...) ((void)0)
''' + handler + r'''
int main(int argc,char **argv) {
    assert(argc == 2); mode = atoi(argv[1]);
    uint8_t event[] = {BTSTACK_EVENT_STATE,HCI_STATE_WORKING};
    tlv_reset = true; key_count = 2;
    packet_handler(HCI_EVENT_PACKET,0,event,sizeof(event));
    assert(unlinks == 1 && !tlv_reset && init_count == 1);
    assert(key_count == (mode == 2 ? 2 : 0));
    key_count = 1;
    packet_handler(HCI_EVENT_PACKET,0,event,sizeof(event));
    assert(unlinks == 1 && key_count == 1 && init_count == 2);
    return 0;
}
'''
        for mode in ("0", "1", "2"):
            output = self.compile_run(harness, mode)
            marker = "S31_PAIRING_RESET=" + ("failed" if mode == "2" else "ok")
            self.assertEqual(output.splitlines().count(marker), 1)


if __name__ == "__main__":
    unittest.main()
