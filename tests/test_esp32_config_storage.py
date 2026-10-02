#!/usr/bin/env python3
"""Host contracts for storage/USB configuration; no real disk or USB access."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
LIB = ROOT / "buildroot-external/board/esp32-s31/overlay/usr/lib/esp32-config"


class StorageConfigTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="s31-storage-test-")
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        for name in ("conf", "run", "proc", "sys", "bin", "gadget"):
            (self.base / name).mkdir()
        (self.base / "proc/swaps").write_text("Filename Type Size Used Priority\n")
        (self.base / "proc/mounts").write_text("")
        (self.base / "proc/filesystems").write_text("nodev configfs\n\text4\n\tvfat\n")
        uuid = self.base / "proc/sys/kernel/random/uuid"
        uuid.parent.mkdir(parents=True)
        uuid.write_text("12345678-1234-5678-9abc-123456789abc\n")
        (self.base / "features").write_text("usb_gadget=1\nusb_acm=1\nusb_ecm=1\nzram=0\n")
        self.env = os.environ | {
            "ESP32_CONFIG_DIR": str(self.base / "conf"),
            "ESP32_CONFIG_RUN_DIR": str(self.base / "run"),
            "ESP32_CONFIG_PROC_DIR": str(self.base / "proc"),
            "ESP32_CONFIG_SYS_DIR": str(self.base / "sys"),
            "ESP32_CONFIG_GADGET_DIR": str(self.base / "gadget"),
            "ESP32_CONFIG_FEATURES_FILE": str(self.base / "features"),
            "ESP32_CONFIG_LEGACY_SWAP_FILE": str(self.base / "legacy-swap"),
            "ESP32_CONFIG_LIB_DIR": str(LIB),
            "ESP32_CONFIG_SECURETTY_FILE": str(self.base / "securetty"),
            "TEST_BASE": str(self.base),
            "PATH": str(self.base / "bin") + ":" + os.environ.get("PATH", "/bin:/usr/bin"),
        }
        self.write_program("s31-overlay", r'''
case "$1" in
    status) [ ! -f "$TEST_BASE/usb-overlay" ] || echo 'active: usb-device id=1' ;;
    apply) echo "$*" >>"$TEST_BASE/actions"; touch "$TEST_BASE/usb-overlay"
        mkdir -p "$ESP32_CONFIG_SYS_DIR/class/udc/mock-dwc2" ;;
    remove) echo "$*" >>"$TEST_BASE/actions"; rm -f "$TEST_BASE/usb-overlay"
        rmdir "$ESP32_CONFIG_SYS_DIR/class/udc/mock-dwc2" 2>/dev/null || true ;;
    *) exit 2 ;;
esac
exit 0
''')
        self.write_program("ip", 'echo "ip $*" >>"$TEST_BASE/actions"\n')
        self.write_program("blkid", 'exit 1\n')

    def write_program(self, name, content):
        path = self.base / "bin" / name
        path.write_text("#!/bin/sh\n" + content)
        path.chmod(0o755)

    def run_shell(self, body, expect=0, configfs=False):
        prelude = r'''
set -u
. "$ESP32_CONFIG_LIB_DIR/common.sh"
. "$ESP32_CONFIG_LIB_DIR/storage.sh"
. "$ESP32_CONFIG_LIB_DIR/usb.sh"
swapoff() {
    printf 'swapoff %s\n' "$1" >>"$TEST_BASE/actions"
    [ "${FAIL_SWAPOFF:-0}" != 1 ] || return 1
    awk -v dev="$1" '$1!=dev' "$ESP32_CONFIG_PROC_DIR/swaps" >"$TEST_BASE/swaps.new"
    mv "$TEST_BASE/swaps.new" "$ESP32_CONFIG_PROC_DIR/swaps"
}
swapon() {
    printf 'swapon %s\n' "$1" >>"$TEST_BASE/actions"
    printf '%s partition 32768 0 -2\n' "$1" >>"$ESP32_CONFIG_PROC_DIR/swaps"
}
mkswap() { echo 'UNEXPECTED mkswap' >>"$TEST_BASE/actions"; return 1; }
mount() { echo 'mount '"$*" >>"$TEST_BASE/actions"; return 0; }
umount() {
    echo "umount $*" >>"$TEST_BASE/actions"
    [ "${FAIL_UMOUNT:-0}" != 1 ] || return 1
    awk -v path="$1" '$2!=path' "$ESP32_CONFIG_PROC_DIR/mounts" >"$TEST_BASE/mounts.new"
    mv "$TEST_BASE/mounts.new" "$ESP32_CONFIG_PROC_DIR/mounts"
}
'''
        if configfs:
            prelude += r'''
# Model ConfigFS's implicit directories/attributes with ordinary temp files.
mkdir() {
    command mkdir "$@" || return 1
    for item in "$@"; do
        case "$item" in
            "$ESP32_CONFIG_GADGET_DIR/s31")
                command mkdir -p "$item/functions" "$item/configs" "$item/strings"
                : >"$item/UDC" ;;
            "$ESP32_CONFIG_GADGET_DIR/s31/functions/acm.usb0")
                printf '2\n' >"$item/port_num" ;;
            "$ESP32_CONFIG_GADGET_DIR/s31/functions/ecm.usb0")
                printf 'usb42\n' >"$item/ifname" ;;
        esac
    done
}
rmdir() {
    for item in "$@"; do
        case "$item" in
            "$ESP32_CONFIG_GADGET_DIR/s31"*)
                for attribute in "$item"/*; do
                    [ ! -f "$attribute" ] || rm "$attribute"
                done
                if [ "$item" = "$ESP32_CONFIG_GADGET_DIR/s31/configs/c.1" ]; then
                    command rmdir "$item/strings"
                fi
                if [ "$item" = "$ESP32_CONFIG_GADGET_DIR/s31" ]; then
                    command rmdir "$item/functions" "$item/configs" "$item/strings"
                fi ;;
        esac
    done
    command rmdir "$@"
}
'''
        result = subprocess.run(["/bin/sh", "-c", prelude + "\n" + body],
                                env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode, expect, result.stdout + result.stderr)
        return result

    def actions(self):
        path = self.base / "actions"
        return path.read_text() if path.exists() else ""

    def test_literal_legacy_file_never_executes(self):
        marker = self.base / "should-not-exist"
        (self.base / "legacy-swap").write_text(f'USB_SWAP_DEVICE="$(touch {marker})"\n')
        self.run_shell('storage_validate_swap_config "$CONF_DIR/swap.conf"', expect=1)
        self.assertFalse(marker.exists())
        (self.base / "legacy-swap").write_text('USB_SWAP_DEVICE="/dev/sda2"\nUSB_SWAP_INIT=1\n')
        result = self.run_shell('storage_validate_swap_config "$CONF_DIR/swap.conf" && printf "%s" "$STORAGE_SWAP_DEVICE"')
        self.assertEqual(result.stdout, "/dev/sda2")

    def test_extra_arguments_are_rejected_before_mutation(self):
        for command in ("storage_cli mount", "storage_cli unmount", "storage_cli apply", "storage_cli stop", "storage_cli swap disable", "storage_cli swap stop", "storage_cli swap apply", "usb_cli stop", "usb_cli apply"):
            self.run_shell(command + " --dry-run", expect=2)
        self.assertEqual(self.actions(), "")
        self.assertEqual(list((self.base / "conf").iterdir()), [])

    def test_duplicate_and_unknown_keys_rejected(self):
        cases = {
            "swap.conf": ("enabled=1\nenabled=0\n", "storage_validate_swap_config"),
            "storage.conf": ("device=/dev/sda1\npath=/mnt/data\ncommand=rm\n", "storage_validate_mount_config"),
            "usb.conf": ("mode=host\nmode=serial\n", "usb_validate_config"),
        }
        for name, (text, validator) in cases.items():
            (self.base / "conf" / name).write_text(text)
            self.run_shell(f'{validator} "$CONF_DIR/{name}"', expect=1)

    def test_swap_stop_only_owned_devices(self):
        (self.base / "proc/swaps").write_text("Filename Type Size Used Priority\n/dev/sda1 partition 100 0 -2\n/dev/sdb2 partition 200 0 -3\n")
        (self.base / "run/swaps").write_text("/dev/sda1\n")
        self.run_shell("storage_swap_stop")
        self.assertEqual(self.actions(), "swapoff /dev/sda1\n")
        self.assertIn("/dev/sdb2", (self.base / "proc/swaps").read_text())
        self.assertFalse((self.base / "run/swaps").exists())

    def test_swapoff_failure_preserves_ownership_and_saved_policy(self):
        saved = "enabled=1\ndevice=/dev/sda1\nzram_fallback=0\nzram_size_kib=8192\n"
        (self.base / "conf/swap.conf").write_text(saved)
        (self.base / "proc/swaps").write_text("/dev/sda1 partition 100 50 -2\n")
        (self.base / "run/swaps").write_text("/dev/sda1\n")
        self.run_shell("FAIL_SWAPOFF=1\nstorage_swap_configure 0 /dev/sda1", expect=1)
        self.assertEqual((self.base / "conf/swap.conf").read_text(), saved)
        self.assertEqual((self.base / "run/swaps").read_text(), "/dev/sda1\n")

    def test_auto_swap_skips_partitions_without_signature(self):
        result = self.run_shell(r'''
storage_devices() { printf '/dev/sda1\n/dev/sda2\n'; }
storage_swap_signature() { [ "$1" = /dev/sda2 ]; }
storage_swap_apply 0
''')
        self.assertIn("/dev/sda2", result.stdout)
        self.assertEqual(self.actions(), "swapon /dev/sda2\n")
        self.assertEqual((self.base / "run/swaps").read_text(), "/dev/sda2\n")

    def test_existing_external_swap_not_adopted(self):
        (self.base / "proc/swaps").write_text("/dev/sda1 partition 100 0 -2\n")
        self.run_shell(r'''
storage_devices() { echo /dev/sda1; }
storage_swap_signature() { return 0; }
storage_swap_apply
storage_swap_stop
''')
        self.assertEqual(self.actions(), "")
        self.assertFalse((self.base / "run/swaps").exists())

    def test_usb_device_mode_skips_usb_swap_but_keeps_mmc_swap(self):
        (self.base / "conf/usb.conf").write_text("mode=serial\n")
        self.run_shell("storage_swap_apply 1")
        self.assertEqual(self.actions(), "")
        (self.base / "conf/swap.conf").write_text("enabled=1\ndevice=/dev/mmcblk0p2\n")
        self.run_shell(r'''
storage_resolve_device() { echo /dev/mmcblk0p2; }
storage_swap_signature() { return 0; }
storage_swap_apply 1
''')
        self.assertEqual(self.actions(), "swapon /dev/mmcblk0p2\n")

    def test_apply_policy_does_not_mount_disabled_volume(self):
        (self.base / "conf/swap.conf").write_text("enabled=0\n")
        (self.base / "conf/storage.conf").write_text("enabled=0\ndevice=/dev/sda1\npath=/mnt/music\nreadonly=0\n")
        self.run_shell("storage_apply")
        self.assertEqual(self.actions(), "")

    def test_uuid_ambiguity_rejected(self):
        self.run_shell(r'''
storage_devices() { printf '/dev/sda1\n/dev/sdb1\n'; }
storage_blkid_value() { printf 'aabb-ccdd\n'; }
storage_resolve_device UUID=aabb-ccdd
''', expect=1)

    def test_missing_volume_does_not_fail_boot(self):
        (self.base / "conf/storage.conf").write_text("enabled=1\ndevice=UUID=aabb-ccdd\npath=/mnt/data\nreadonly=0\n")
        self.run_shell('storage_resolve_device() { return 1; }\nstorage_mount_apply 1')
        self.run_shell('storage_resolve_device() { return 1; }\nstorage_mount_apply 0', expect=1)
        self.assertEqual(self.actions(), "")

    def test_lean_image_does_not_offer_filesystem_mounting(self):
        (self.base / "proc/filesystems").write_text("nodev proc\nnodev overlay\n squashfs\n")
        self.run_shell("storage_mount_capable", expect=1)

    def test_mount_path_and_unmount_ownership(self):
        for path in ("/", "/etc/data", "/mnt/../etc", "/mnt/./x", "/mnt/a b", "/mnt", "/mnt/data/"):
            self.run_shell(f"storage_valid_mount_path '{path}'", expect=1)
        self.run_shell("storage_valid_mount_path /mnt/music-card")
        (self.base / "run/storage.mount").write_text("device=/dev/sda1\npath=/mnt/music\n")
        (self.base / "proc/mounts").write_text("/dev/sdb1 /mnt/music ext4 rw 0 0\n")
        self.run_shell("storage_mount_stop", expect=1)
        self.assertEqual(self.actions(), "")
        (self.base / "proc/mounts").write_text("/dev/sda1 /mnt/music ext4 rw 0 0\n")
        self.run_shell("FAIL_UMOUNT=1\nstorage_mount_stop", expect=1)
        self.assertTrue((self.base / "run/storage.mount").exists())
        self.run_shell("storage_mount_stop")
        self.assertFalse((self.base / "run/storage.mount").exists())

    def test_usb_addresses_and_image_capabilities(self):
        for address in ("0.1.2.3/24", "127.0.0.1/8", "224.1.1.1/24", "192.168.7.0/24", "192.168.7.255/24", "1.2.3.4./24", "1.2..3/24", "999.1.2.3/24", "01.2.3.4/24", "1.2.3.4/32"):
            self.run_shell(f"usb_valid_address '{address}'", expect=1)
        self.run_shell("usb_valid_address 192.168.7.2/24")
        self.run_shell("usb_mode_available serial && usb_mode_available network")
        (self.base / "features").write_text("usb_gadget=0\nusb_acm=0\nusb_ecm=0\n")
        self.run_shell("usb_mode_available serial", expect=1)
        self.run_shell("usb_mode_available host")

    def test_usb_refuses_active_host_disks_before_reconfiguration(self):
        (self.base / "proc/mounts").write_text("/dev/sda1 /mnt/music ext4 rw 0 0\n")
        self.run_shell("usb_configure serial 0", expect=1)
        self.assertEqual(self.actions(), "")
        self.assertFalse((self.base / "conf/usb.conf").exists())
        (self.base / "proc/mounts").write_text("")
        (self.base / "proc/swaps").write_text("/dev/sdb2 partition 100 0 -2\n")
        self.run_shell("usb_configure network 0", expect=1)
        self.assertEqual(self.actions(), "")

    def test_usb_serial_uses_actual_acm_port_and_cleans_only_owned_gadget(self):
        (self.base / "gadget/another-gadget").mkdir()
        self.run_shell("usb_configure serial 0\nusb_status", configfs=True)
        self.assertEqual((self.base / "run/usb.serial").read_text(), "ttyGS2\n")
        self.assertEqual((self.base / "gadget/s31/UDC").read_text(), "mock-dwc2\n")
        self.assertIn("mode=serial", (self.base / "conf/usb.conf").read_text())
        self.run_shell("usb_configure host", configfs=True)
        self.assertFalse((self.base / "gadget/s31").exists())
        self.assertTrue((self.base / "gadget/another-gadget").exists())
        self.assertFalse((self.base / "usb-overlay").exists())

    def test_serial_login_adds_only_actual_port_to_existing_allowlist(self):
        path = self.base / "securetty"
        self.run_shell("usb_allow_console ttyGS2")
        self.assertFalse(path.exists())
        path.write_text("console\nttyS0\n")
        self.run_shell("usb_allow_console ttyGS2\nusb_allow_console ttyGS2")
        self.assertEqual(path.read_text(), "console\nttyS0\n\nttyGS2\n")
        self.run_shell("usb_allow_console ttyS1", expect=1)

    def test_usb_network_assigns_address_to_actual_interface(self):
        self.run_shell("usb_configure network 0 192.168.8.2/24", configfs=True)
        self.assertEqual((self.base / "run/usb.network").read_text(), "usb42\n")
        self.assertIn("ip addr add 192.168.8.2/24 dev usb42", self.actions())
        self.assertRegex((self.base / "gadget/s31/functions/ecm.usb0/dev_addr").read_text(), r"^02:31:[0-9a-f:]+\n$")
        previous = self.actions()
        self.run_shell("usb_configure network 0 192.168.8.2/24", configfs=True)
        self.assertEqual(self.actions(), previous, "unchanged settings must not disconnect USB")
        self.run_shell("usb_apply", configfs=True)
        self.assertEqual(self.actions(), previous, "reapplying saved settings must not disconnect USB")


if __name__ == "__main__":
    unittest.main()
