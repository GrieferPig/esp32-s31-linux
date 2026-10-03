#!/usr/bin/env python3

import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest


SCRIPT = (
    Path(__file__).resolve().parents[2]
    / "buildroot-external"
    / "board"
    / "esp32-s31"
    / "overlay"
    / "usr"
    / "bin"
    / "s31-selftest"
)


class S31SelftestTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.write("proc/device-tree/model", b"Espressif ESP32-S31 test\0")
        self.write("sys/devices/system/cpu/online", b"0-1\n")
        self.write("proc/kallsyms", b"c0000000 T _xiprom\n")
        self.write(
            "proc/mtd",
            b'dev:    size   erasesize  name\n'
            b'mtd2: 00630000 00001000 "linux"\n'
            b'mtd3: 000a0000 00001000 "persist"\n',
        )
        self.write("proc/meminfo", b"MemAvailable:       9000 kB\n")
        self.write("proc/mounts", b"overlay / overlay rw 0 0\n")
        self.write(
            "proc/iomem",
            b"40000000-40efffff : 40000000.flash flash@40000000\n",
        )
        self.write(
            "proc/dmesg",
            b"esp32s31-flash-mtd: registered writable XIP Flash MTD window\n",
        )
        self.write("sys/class/net/wlan0/operstate", b"up\n")
        self.write("sys/class/net/wlan0/statistics/rx_packets", b"0\n")
        self.write("sys/class/net/wlan0/statistics/tx_packets", b"0\n")
        (self.root / "sys/class/bluetooth/hci0").mkdir(parents=True)
        self.write(
            "sys/devices/platform/s31/radio_health",
            b"state=2 wifi_rx_dropped=0 wifi_tx_dropped=0 "
            b"hci_rx_dropped=0 hci_tx_dropped=0\n",
        )
        self.write("dev/mtd2", b"x" * (4096 * 64))
        (self.root / "var/lib/s31-selftest").mkdir(parents=True)
        (self.root / "tmp").mkdir(parents=True)

    def tearDown(self):
        self.temp.cleanup()

    def write(self, relative, data):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def run_selftest(self, *args):
        env = os.environ.copy()
        env.update(
            {
                "S31_SELFTEST_ROOT": str(self.root),
                "S31_SELFTEST_WORKDIR": str(self.root / "var/lib/s31-selftest"),
                "S31_SELFTEST_DMESG_FILE": str(self.root / "proc/dmesg"),
                "S31_SELFTEST_TEST_MODE": "1",
                "S31_SELFTEST_NO_TASKSET": "1",
            }
        )
        return subprocess.run(
            ["sh", str(SCRIPT), *args],
            text=True,
            capture_output=True,
            env=env,
            timeout=20,
            check=False,
        )

    def test_quick_human_passes(self):
        result = self.run_selftest("--quick")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS kernel.xip", result.stdout)
        self.assertIn("PASS mode=quick", result.stdout)

    def test_quick_json_is_json_lines(self):
        result = self.run_selftest("--quick", "--json")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(records[-1]["test"], "summary")
        self.assertEqual(records[-1]["status"], "PASS")
        self.assertEqual(records[-1]["mode"], "quick")

    def test_quick_xip_falls_back_when_kallsyms_is_disabled(self):
        (self.root / "proc/kallsyms").unlink()
        result = self.run_selftest("--quick", "--json")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        xip = next(record for record in records if record["test"] == "kernel.xip")
        self.assertEqual(xip["status"], "PASS")
        self.assertIn("kallsyms unavailable", xip["detail"])

    def test_quick_failure_sets_exit_status(self):
        self.write("sys/devices/system/cpu/online", b"0\n")
        result = self.run_selftest("--quick", "--json")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        smp = next(record for record in records if record["test"] == "cpu.smp")
        self.assertEqual(smp["status"], "FAIL")
        self.assertEqual(records[-1]["status"], "FAIL")

    def test_stress_runs_all_workers_and_cleans_up(self):
        result = self.run_selftest("--stress", "--duration", "1", "--json")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        by_name = {record["test"]: record for record in records}
        for name in (
            "stress.cpu0",
            "stress.cpu1",
            "stress.xip-read",
            "stress.persist-write",
            "stress.radio-health",
            "stress.radio-drops",
            "stress.kernel-log",
        ):
            self.assertEqual(by_name[name]["status"], "PASS", result.stdout)
        self.assertFalse((self.root / "var/lib/s31-selftest/stress-probe").exists())

    def test_stress_requires_radio_health(self):
        (self.root / "sys/devices/platform/s31/radio_health").unlink()
        result = self.run_selftest("--stress", "--duration", "1", "--json")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        preflight = next(
            record for record in records if record["test"] == "stress.preflight"
        )
        self.assertEqual(preflight["status"], "FAIL")

    def test_required_radio_traffic_fails_when_idle(self):
        result = self.run_selftest(
            "--stress", "--duration", "1", "--require-radio-traffic", "--json"
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        traffic = next(
            record for record in records if record["test"] == "stress.radio-traffic"
        )
        self.assertEqual(traffic["status"], "FAIL")

    def test_required_radio_traffic_passes_with_packet_progress(self):
        stop = threading.Event()

        def add_packets():
            # The quick preflight performs syncs before capturing the traffic
            # baseline. Keep traffic flowing across that boundary, rather than
            # racing it with one update after an arbitrary 300 ms delay.
            counter = self.root / "sys/class/net/wlan0/statistics/rx_packets"
            pending = counter.with_suffix(".next")
            packets = 0
            while not stop.wait(0.05):
                packets += 25
                pending.write_text(f"{packets}\n")
                pending.replace(counter)  # Never expose a truncated counter.

        updater = threading.Thread(target=add_packets)
        updater.start()
        try:
            result = self.run_selftest(
                "--stress", "--duration", "1", "--require-radio-traffic", "--json"
            )
        finally:
            stop.set()
            updater.join()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        traffic = next(
            record for record in records if record["test"] == "stress.radio-traffic"
        )
        self.assertEqual(traffic["status"], "PASS")

    def test_invalid_duration_is_usage_error(self):
        result = self.run_selftest("--stress", "--duration", "zero")
        self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main()
