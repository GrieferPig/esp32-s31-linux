"""Keep the removed flash erase/program HIL path unreachable, without hardware."""
from contextlib import redirect_stderr, redirect_stdout
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
AGENT = ROOT / "buildroot-external/board/esp32-s31/overlay/usr/bin/s31-hil-agent"
IO_SOURCE = ROOT / "rootfs/s31_hil_io.c"
sys.path.insert(0, str(ROOT / "tools/hil"))
import s31_hil as hil


class HilFlashSafety(unittest.TestCase):
    def test_host_rejects_removed_case_before_opening_ports(self):
        with mock.patch.object(sys, "argv", ["s31_hil.py", "--case", "mtd"]), \
                mock.patch.object(hil, "open_serial") as open_serial, \
                redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as exc:
            hil.main()
        self.assertEqual(exc.exception.code, 2)
        open_serial.assert_not_called()

    def test_host_all_has_no_flash_case(self):
        with mock.patch.object(sys, "argv", ["s31_hil.py", "--board", "s31",
                                            "--s31-port", "fake", "--case", "all"]), \
                mock.patch.object(hil, "run_s31", return_value=[]) as run_s31, \
                mock.patch.object(hil, "run_c6_wifi_recover", return_value=[]), \
                redirect_stdout(io.StringIO()):
            self.assertEqual(hil.main(), 0)
        self.assertEqual([call.args[1] for call in run_s31.call_args_list],
                         ["firmware", "sdmmc", "usb-drive", "lp-core", "smp-irq-dma"])

    def test_board_agent_rejects_removed_case(self):
        result = subprocess.run(["sh", str(AGENT), "--case", "mtd"],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 2)
        self.assertNotIn("HIL1", result.stdout)
        self.assertNotIn("|mtd|", result.stderr)

    def test_board_all_dispatch_retains_other_cases(self):
        source = AGENT.read_text()
        cases = ["firmware", "peer", "sdmmc", "ethernet", "usb_drive",
                 "lp_core", "smp_irq_dma"]
        # Stub all board operations, then run the real argument parser/dispatch.
        # The extra removed-case stub detects an accidental aggregate call.
        stubs = "".join(f'{case}_tests() {{ echo "CASE={case}"; }}\n'
                        for case in cases + ["mtd"])
        source = source.replace('case "$CASE" in\n', stubs + 'case "$CASE" in\n')
        result = subprocess.run(["sh", "-s", "--", "--case", "all"],
                                input=source, capture_output=True, text=True,
                                timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([line.removeprefix("CASE=") for line in result.stdout.splitlines()
                          if line.startswith("CASE=")], cases)

    def test_only_read_only_flash_inventory_remains(self):
        agent = AGENT.read_text()
        helper = IO_SOURCE.read_text()
        self.assertIn("mtd.inventory", agent)
        self.assertIn("/proc/mtd", agent)
        for token in ("hil-scratch", "mtd_tests", "mtd-test", "/dev/mtd"):
            self.assertNotIn(token, agent)
        for token in ("mtd_test", "mtd-test", "MEMERASE", "MEMGETINFO", "mtd-user.h"):
            self.assertNotIn(token, helper)

    def test_compiled_helper_rejects_removed_command_without_opening(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            guard = tmp / "no_open.c"
            # A regressed command must fail before touching any real device/file.
            guard.write_text('#include <unistd.h>\n'
                             'int __wrap_open(const char *path, int flags, ...)\n'
                             '{ (void)path; (void)flags; _exit(99); }\n')
            # Host glibc omits the UART loopback constant from sys/ioctl.h;
            # use the Linux UAPI value for this no-device command-path test.
            compatibility = tmp / "host_compat.h"
            compatibility.write_text('#include <sys/ioctl.h>\n'
                                     '#ifndef TIOCM_LOOP\n'
                                     '#define TIOCM_LOOP 0x8000\n'
                                     '#endif\n')
            binary = tmp / "s31-hil-io"
            subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-include", str(compatibility), str(IO_SOURCE),
                            str(guard), "-Wl,--wrap=open",
                            "-o", str(binary)], check=True, capture_output=True)
            result = subprocess.run([str(binary), "mtd-test", str(tmp / "unused"), "4"],
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertIn("usage:", result.stderr)
            self.assertNotIn("mtd-test", result.stderr)


if __name__ == "__main__":
    unittest.main()
