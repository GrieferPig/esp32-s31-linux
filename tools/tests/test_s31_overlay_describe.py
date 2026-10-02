#!/usr/bin/env python3
"""Test describe/check against real libfdt trees without a dtc executable.

Only the device boundary is mocked. Tree generation, exported metadata,
override patching, policy validation and state parsing use the real C code.
"""
import ctypes
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class Tree:
    def __init__(self, library, name):
        self.fdt = library
        self.buffer = ctypes.create_string_buffer(8192)
        assert self.fdt.fdt_create_empty_tree(self.buffer, len(self.buffer)) == 0
        self.set("/", "espressif,overlay-name", name + "\0")

    def node(self, path):
        value = self.fdt.fdt_path_offset(self.buffer, path.encode())
        assert value >= 0, (path, value)
        return value

    def add(self, parent, name):
        value = self.fdt.fdt_add_subnode(self.buffer, self.node(parent), name.encode())
        assert value >= 0, value
        return (parent.rstrip("/") + "/" + name)

    def set(self, path, property_name, value):
        if isinstance(value, str):
            value = value.encode()
        elif isinstance(value, (list, tuple)):
            value = b"".join(struct.pack(">I", item) for item in value)
        result = self.fdt.fdt_setprop_namelen(self.buffer, self.node(path), property_name.encode(), len(property_name), value, len(value))
        assert result == 0, (property_name, result)

    def delete(self, path, property_name):
        assert self.fdt.fdt_delprop(self.buffer, self.node(path), property_name.encode()) == 0

    def scalar_route(self, parent, route, gpio, kind="matrix-bidirectional", node_name=None):
        path = self.add(parent, node_name or route.replace(".", "-"))
        self.set(path, "espressif,route-name", route + "\0")
        self.set(path, "espressif,route-kind", kind + "\0")
        # A bidirectional pin has an output and input cell for the same GPIO.
        self.set(path, "pinmux", [0x10014400 | gpio, 0x20014400 | gpio])
        return path

    def parameter(self, parent, name, value, allowed, node_name="controller"):
        path = self.add(parent, node_name)
        self.set(path, "espressif,param-name", name + "\0")
        self.set(path, "espressif,param-values", allowed)
        self.set(path, name, [value])
        return path

    def bytes(self):
        assert self.fdt.fdt_pack(self.buffer) == 0
        length = struct.unpack_from(">I", self.buffer.raw, 4)[0]
        return self.buffer.raw[:length]


MOCK = r'''
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/esp32s31-overlay.h>
static int device_writer = -1, current_writes;
static void operation(const char *name) {
    const char *path = getenv("TEST_DEVICE_OPERATIONS");
    FILE *file = path ? fopen(path, "a") : NULL;
    if (file) { fprintf(file, "%s\n", name); fclose(file); }
}
int __wrap_ioctl(int fd, unsigned long op, ...) {
    (void)fd;
    operation("ioctl");
    if (getenv("TEST_MANAGER_MISSING")) { errno = ENODEV; return -1; }
    if ((op == S31_OVERLAY_IOC_REMOVE_NAME || op == S31_OVERLAY_IOC_REMOVE_ALL) &&
        getenv("TEST_REMOVE_MISSING")) { errno = ENOENT; return -1; }
    if (op == S31_OVERLAY_IOC_LIST) {
        va_list args; va_start(args, op);
        struct s31_overlay_list *list = va_arg(args, struct s31_overlay_list *);
        va_end(args);
        const char *name = getenv("TEST_ACTIVE_PROFILE");
        memset(list, 0, sizeof(*list));
        if (name && *name) { list->count = 1; snprintf(list->items[0].name, sizeof(list->items[0].name), "%s", name); }
    }
    return 0;
}
int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args);
    }
    int fd = __real_open(path, flags, mode);
    if (!strcmp(path, "/dev/null") && (flags & O_ACCMODE) != O_RDONLY)
        device_writer = fd;
    return fd;
}
int __real_close(int fd);
int __wrap_close(int fd) {
    if (fd == device_writer) device_writer = -1;
    return __real_close(fd);
}
ssize_t __real_write(int fd, const void *data, size_t size);
ssize_t __wrap_write(int fd, const void *data, size_t size) {
    if (fd == device_writer) {
        operation("apply");
        if (getenv("TEST_APPLY_FAIL")) { errno = EIO; return -1; }
    } else {
        operation("write");
        ++current_writes;
        const char *fail = getenv("TEST_FAIL_CURRENT_WRITE");
        if (fail && current_writes == atoi(fail)) { errno = ENOSPC; return -1; }
    }
    return __real_write(fd, data, size);
}
'''


@unittest.skipUnless(shutil.which("cc"), "host C compiler required")
class OverlayDescription(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="s31-overlay-describe-")
        cls.work = Path(cls.temp.name)
        cls.state = cls.work / "state"
        cls.blobs = cls.work / "blobs"
        cls.state.mkdir()
        cls.blobs.mkdir()
        source = (ROOT / "rootfs/s31_overlay.c").read_text()
        for key, value in {"OVERLAY_DEVICE": "/dev/null", "CURRENT_FILE": str(cls.state / "current"),
                           "LOCK_FILE": str(cls.state / "lock"), "CONFIG_DIR": str(cls.state)}.items():
            source = re.sub(r"^#define " + key + r" .*", f'#define {key} "{value}"', source, flags=re.M)
        (cls.work / "overlay.c").write_text(source)
        (cls.work / "mock.c").write_text(MOCK)
        includes = ROOT / "linux-esp32-s31/include/uapi"
        fdt = ROOT / "linux-esp32-s31/scripts/dtc/libfdt"
        subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-D__EXPORTED_HEADERS__", "-I" + str(fdt), "-I" + str(includes),
                        str(cls.work / "overlay.c"), str(cls.work / "mock.c"), *map(str, fdt.glob("*.c")),
                        "-Wl,--wrap=ioctl", "-Wl,--wrap=write", "-Wl,--wrap=open", "-Wl,--wrap=close",
                        "-o", str(cls.work / "overlay")], check=True)
        subprocess.run(["cc", "-O2", "-shared", "-fPIC", "-I" + str(fdt),
                        *map(str, fdt.glob("*.c")), "-o", str(cls.work / "libfdt.so")], check=True)
        cls.fdt = ctypes.CDLL(str(cls.work / "libfdt.so"))
        cls.fdt.fdt_create_empty_tree.argtypes = [ctypes.c_void_p, ctypes.c_int]
        cls.fdt.fdt_path_offset.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        cls.fdt.fdt_add_subnode.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p]
        cls.fdt.fdt_setprop_namelen.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
        cls.fdt.fdt_delprop.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p]
        cls.fdt.fdt_pack.argtypes = [ctypes.c_void_p]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        for directory in (self.state, self.blobs):
            for path in directory.iterdir():
                if path.is_dir(): shutil.rmtree(path)
                else: path.unlink()
        self.env = dict(os.environ, S31_OVERLAY_DIR=str(self.blobs),
                        S31_OVERLAY_PERSIST=str(self.state / "overlays.conf"),
                        TEST_DEVICE_OPERATIONS=str(self.state / "operations"))
        self.write_tree(self.i2c_tree())

    def i2c_tree(self):
        tree = Tree(self.fdt, "i2c0")
        pins = tree.add("/", "pins")
        tree.scalar_route(pins, "i2c0.scl", 35)
        tree.scalar_route(pins, "i2c0.sda", 36)
        tree.parameter("/", "clock-frequency", 100000, [100000, 400000, 1000000])
        return tree

    def write_tree(self, tree, name="i2c0"):
        (self.blobs / f"esp32s31-overlay-{name}.dtbo").write_bytes(tree.bytes())

    def run_cli(self, *args, rc=0):
        result = subprocess.run([str(self.work / "overlay"), *args], env=self.env,
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, rc, result.stdout + result.stderr)
        return result

    def rows(self, profile="i2c0"):
        return [line.split("\t") for line in self.run_cli("describe", profile).stdout.splitlines()]

    def test_defaults_have_no_invented_current_or_saved_values(self):
        rows = self.rows()
        self.assertIn(["active", "0"], rows)
        self.assertIn(["saved", "0"], rows)
        self.assertIn(["current_known", "0"], rows)
        self.assertIn(["route", "i2c0.scl", "35", "-", "-", "matrix-bidirectional"], rows)
        self.assertIn(["parameter", "clock-frequency", "100000", "-", "-", "100000,400000,1000000"], rows)

    def test_current_and_saved_overrides_remain_separate(self):
        self.env["TEST_ACTIVE_PROFILE"] = "i2c0"
        (self.state / "current").write_text("i2c0 i2c0.scl=37 i2c0.sda=38 clock-frequency=400000\n")
        (self.state / "overlays.conf").write_text("overlay.i2c0=i2c0 i2c0.scl=40 i2c0.sda=42 clock-frequency=1000000\n")
        rows = self.rows()
        self.assertIn(["current_known", "1"], rows)
        self.assertIn(["route", "i2c0.scl", "35", "37", "40", "matrix-bidirectional"], rows)
        self.assertIn(["route", "i2c0.sda", "36", "38", "42", "matrix-bidirectional"], rows)
        self.assertIn(["parameter", "clock-frequency", "100000", "400000", "1000000", "100000,400000,1000000"], rows)

    def test_active_without_current_record_does_not_use_saved_as_current(self):
        self.env["TEST_ACTIVE_PROFILE"] = "i2c0"
        (self.state / "overlays.conf").write_text("overlay.i2c0=i2c0 i2c0.scl=37\n")
        rows = self.rows()
        self.assertIn(["active", "1"], rows)
        self.assertIn(["current_known", "0"], rows)
        self.assertIn(["route", "i2c0.scl", "35", "-", "37", "matrix-bidirectional"], rows)

    def test_malformed_record_cannot_claim_to_be_known_current(self):
        self.env["TEST_ACTIVE_PROFILE"] = "i2c0"
        for data in (b"i2c0\0hidden\n", b"i2c0 i2c0.scl=35 i2c0.scl=37\n", b"i2c0\ni2c0\n"):
            (self.state / "current").write_bytes(data)
            self.assertEqual(self.run_cli("describe", "i2c0", rc=1).stdout, "")

    def test_inactive_or_unavailable_manager_does_not_trust_stale_current(self):
        (self.state / "current").write_text("i2c0 i2c0.scl=37\n")
        for absent in (False, True):
            if absent: self.env["TEST_MANAGER_MISSING"] = "1"
            rows = self.rows()
            self.assertIn(["active", "unknown" if absent else "0"], rows)
            self.assertIn(["current_known", "0"], rows)
            self.assertIn(["route", "i2c0.scl", "35", "-", "-", "matrix-bidirectional"], rows)

    def test_plural_routes_keep_their_order_and_individual_kinds(self):
        tree = Tree(self.fdt, "gpspi2")
        pins = tree.add("/", "pins")
        tree.set(pins, "espressif,route-names", "gpspi2.sck\0gpspi2.mosi\0gpspi2.miso\0")
        tree.set(pins, "espressif,route-kinds", "matrix-output\0matrix-output\0matrix-input\0")
        tree.set(pins, "pinmux", [0x10001001, 0x10001102, 0x20001203])
        self.write_tree(tree, "gpspi2")
        self.env["TEST_ACTIVE_PROFILE"] = "gpspi2"
        (self.state / "current").write_text("gpspi2 gpspi2.mosi=10\n")
        routes = [row for row in self.rows("gpspi2") if row[0] == "route"]
        self.assertEqual(routes, [["route", "gpspi2.sck", "1", "1", "-", "matrix-output"],
                                  ["route", "gpspi2.mosi", "2", "10", "-", "matrix-output"],
                                  ["route", "gpspi2.miso", "3", "3", "-", "matrix-input"]])

    def test_fixed_gpio_claims_and_parameter_are_exposed_without_routes(self):
        tree = Tree(self.fdt, "sdmmc0")
        tree.set("/", "espressif,gpio-claims", [20, 21, 22, 23, 24, 25])
        tree.parameter("/", "bus-width", 4, [1, 4])
        self.write_tree(tree, "sdmmc0")
        self.env["TEST_ACTIVE_PROFILE"] = "sdmmc0"
        (self.state / "current").write_text("sdmmc0 bus-width=1\n")
        rows = self.rows("sdmmc0")
        self.assertEqual([row for row in rows if row[0] == "fixed_gpio"], [["fixed_gpio", str(n)] for n in range(20, 26)])
        self.assertFalse(any(row[0] == "route" for row in rows))
        self.assertIn(["parameter", "bus-width", "4", "1", "-", "1,4"], rows)

    def test_matching_duplicate_route_is_one_field_conflict_is_error(self):
        tree = self.i2c_tree()
        tree.scalar_route("/pins", "i2c0.scl", 35, node_name="same-scl")
        self.write_tree(tree)
        self.assertEqual(len([row for row in self.rows() if row[:2] == ["route", "i2c0.scl"]]), 1)
        tree.set("/pins/same-scl", "pinmux", [0x10014425, 0x20014425])
        self.write_tree(tree)
        self.assertEqual(self.run_cli("describe", "i2c0", rc=1).stdout, "")

    def test_check_needs_neither_manager_nor_writable_state_lock(self):
        self.env["TEST_MANAGER_MISSING"] = "1"
        (self.state / "lock").mkdir()
        (self.state / "current").write_text("keep current unchanged\n")
        path = self.state / "candidate.conf"
        path.write_text("# saved selections\r\noverlay.i2c0=i2c0 i2c0.scl=37 clock-frequency=400000\r\n")
        self.run_cli("check", str(path))
        self.assertFalse((self.state / "operations").exists())
        self.assertEqual((self.state / "current").read_text(), "keep current unchanged\n")
        self.assertFalse((self.state / "overlays.conf").exists())

    def test_remove_saved_inactive_profile_clears_policy_when_kernel_reports_absent(self):
        self.env["TEST_REMOVE_MISSING"] = "1"
        (self.state / "overlays.conf").write_text("overlay.i2c0=i2c0 i2c0.scl=37\noverlay.uart1=uart1\n")
        (self.state / "current").write_text("uart1\n")
        self.run_cli("remove", "i2c0")
        self.assertEqual((self.state / "overlays.conf").read_text(), "overlay.uart1=uart1\n")
        self.assertEqual((self.state / "current").read_text(), "uart1\n")

    def operations(self):
        path = self.state / "operations"
        return path.read_text().splitlines() if path.exists() else []

    def test_equivalent_active_values_save_without_hardware_apply(self):
        self.env["TEST_ACTIVE_PROFILE"] = "i2c0"
        (self.state / "current").write_text("uart1\ni2c0 clock-frequency=400000 i2c0.scl=37\n")
        (self.state / "overlays.conf").write_text("overlay.uart1=uart1\n")
        # Different key order and an explicit default route produce the same DTBO.
        self.run_cli("apply", "i2c0", "i2c0.sda=36", "i2c0.scl=37", "clock-frequency=400000")
        self.assertNotIn("apply", self.operations())
        self.assertIn("ioctl", self.operations())
        self.assertEqual((self.state / "overlays.conf").read_text(),
                         "overlay.uart1=uart1\noverlay.i2c0=i2c0 i2c0.sda=36 i2c0.scl=37 clock-frequency=400000\n")
        self.assertIn("i2c0.sda=36", (self.state / "current").read_text())

    def test_equivalent_volatile_apply_does_not_create_saved_setting(self):
        self.env["TEST_ACTIVE_PROFILE"] = "i2c0"
        (self.state / "current").write_text("i2c0 i2c0.scl=35 clock-frequency=100000\n")
        self.run_cli("apply", "i2c0", "--volatile")
        self.assertNotIn("apply", self.operations())
        self.assertFalse((self.state / "overlays.conf").exists())
        self.assertEqual((self.state / "current").read_text(), "i2c0\n")

    def test_changed_or_untrusted_current_requires_real_apply(self):
        cases = (("i2c0 i2c0.scl=35\n", True), ("", True),
                 ("i2c0 i2c0.scl=37\n", False), ("i2c0 i2c0.scl=37 i2c0.scl=35\n", True))
        for current, active in cases:
            with self.subTest(current=current, active=active):
                self.env["TEST_ACTIVE_PROFILE"] = "i2c0" if active else ""
                (self.state / "current").write_text(current)
                (self.state / "operations").unlink(missing_ok=True)
                self.run_cli("apply", "i2c0", "i2c0.scl=37")
                self.assertEqual(self.operations().count("apply"), 1)

    def test_state_commit_failure_cannot_leave_previous_value_marked_current(self):
        self.env["TEST_ACTIVE_PROFILE"] = "i2c0"
        self.env["TEST_FAIL_CURRENT_WRITE"] = "2"
        old_saved = "overlay.i2c0=i2c0 i2c0.scl=35\n"
        (self.state / "overlays.conf").write_text(old_saved)
        (self.state / "current").write_text("uart1\ni2c0 i2c0.scl=35\n")
        result = self.run_cli("apply", "i2c0", "i2c0.scl=37", rc=1)
        self.assertIn("overlay applied, but recording state failed", result.stderr)
        self.assertEqual(self.operations().count("apply"), 1)
        self.assertEqual((self.state / "current").read_text(), "uart1\n")
        self.assertEqual((self.state / "overlays.conf").read_text(), old_saved)
        rows = self.rows()
        self.assertIn(["active", "1"], rows)
        self.assertIn(["current_known", "0"], rows)
        self.assertIn(["route", "i2c0.scl", "35", "-", "35", "matrix-bidirectional"], rows)

    def test_failed_invalidation_aborts_before_hardware_and_keeps_old_record(self):
        self.env["TEST_ACTIVE_PROFILE"] = "i2c0"
        self.env["TEST_FAIL_CURRENT_WRITE"] = "1"
        old = "i2c0 i2c0.scl=35\n"
        (self.state / "current").write_text(old)
        result = self.run_cli("apply", "i2c0", "i2c0.scl=37", rc=1)
        self.assertIn("invalidate current overlay record", result.stderr)
        self.assertNotIn("apply", self.operations())
        self.assertEqual((self.state / "current").read_text(), old)
        self.assertFalse((self.state / "overlays.conf").exists())

    def test_failed_kernel_apply_keeps_affected_current_unknown(self):
        self.env["TEST_ACTIVE_PROFILE"] = "i2c0"
        self.env["TEST_APPLY_FAIL"] = "1"
        (self.state / "current").write_text("uart1\ni2c0 i2c0.scl=35\n")
        self.run_cli("apply", "i2c0", "i2c0.scl=37", rc=1)
        self.assertEqual((self.state / "current").read_text(), "uart1\n")
        self.assertFalse((self.state / "overlays.conf").exists())

    def test_bad_records_and_duplicate_profiles_are_rejected_offline(self):
        path = self.state / "candidate.conf"
        cases = [b"overlay.i2c0=i2c1\n", b"i2c0=i2c0\n", b"overlay.i2c0=\n",
                 b"overlay.i2c0=i2c0\noverlay.i2c0=i2c0 i2c0.scl=37\n",
                 b"overlay.i2c0=i2c0\0 hidden\n", b"overlay.i2c0=i2c0\rhidden\n",
                 b"overlay.i2c0=i2c0 " + b"x" * 512 + b"\n"]
        for data in cases:
            with self.subTest(data=data):
                path.write_bytes(data)
                self.run_cli("check", str(path), rc=1)
                self.assertFalse((self.state / "operations").exists())

    def test_invalid_assignments_cannot_apply_or_pass_check(self):
        path = self.state / "candidate.conf"
        cases = ["i2c0.scl=", "i2c0.scl=-1", "i2c0.scl=4294967332", "i2c0.scl=-4294967260",
                 "i2c0.scl=31", "i2c0.scl=58", "i2c0.scl=62", "clock-frequency=12345",
                 "unknown=3", "i2c0.scl=35 i2c0.scl=37", "clock-frequency=400000 clock-frequency=100000"]
        for assignment in cases:
            with self.subTest(assignment=assignment):
                path.write_text("overlay.i2c0=i2c0 " + assignment + "\n")
                self.run_cli("check", str(path), rc=1)
                self.run_cli("apply", "i2c0", *assignment.split(), rc=1)
                self.assertFalse((self.state / "operations").exists())
                self.assertFalse((self.state / "current").exists())
                self.assertFalse((self.state / "overlays.conf").exists())

    def test_broken_exported_metadata_is_rejected_before_any_rows(self):
        cases = [lambda t: t.set("/pins/i2c0-scl", "espressif,route-name", b"no-nul"),
                 lambda t: t.set("/pins/i2c0-scl", "espressif,route-kind", "bad\tfield\0"),
                 lambda t: t.delete("/pins/i2c0-scl", "pinmux"),
                 lambda t: t.set("/pins/i2c0-scl", "pinmux", [35, 36]),
                 lambda t: t.set("/controller", "clock-frequency", [100000, 400000]),
                 lambda t: t.set("/controller", "espressif,param-values", [400000]),
                 lambda t: t.set("/", "espressif,gpio-claims", b"bad"),
                 lambda t: t.set("/", "espressif,overlay-name", "wrong\0")]
        path = self.state / "candidate.conf"
        path.write_text("overlay.i2c0=i2c0\n")
        for mutate in cases:
            tree = self.i2c_tree()
            mutate(tree)
            self.write_tree(tree)
            self.assertEqual(self.run_cli("describe", "i2c0", rc=1).stdout, "")
            self.run_cli("check", str(path), rc=1)

    def test_truncated_blob_cannot_walk_past_file_allocation(self):
        path = self.blobs / "esp32s31-overlay-i2c0.dtbo"
        data = path.read_bytes()
        candidate = self.state / "candidate.conf"
        candidate.write_text("overlay.i2c0=i2c0\n")
        for broken in (b"x", data[:40], data[:-4], data[:4] + struct.pack(">I", len(data) + 100) + data[8:]):
            path.write_bytes(broken)
            self.assertEqual(self.run_cli("describe", "i2c0", rc=1).stdout, "")
            self.run_cli("check", str(candidate), rc=1)


if __name__ == "__main__":
    unittest.main()
