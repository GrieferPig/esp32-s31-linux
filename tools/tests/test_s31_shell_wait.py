import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "hil"))
from s31_hil import wait_for_shell

class Console:
    def __init__(self, continuation=False):
        self.continuation = continuation
        self.writes = []
        self.queue = [b"> "] if continuation else [
            b"Stopping network: OK\r\n",
            b"Saving seed for next boot\r\n",
            b"ESP-ROM:esp32s31\r\n",
            b"esp32-s31 login: ",
        ]
    def write(self, data):
        self.writes.append(data)
        if b"\x03" in data:
            if not self.continuation:
                raise RuntimeError("SIGINT cancelled the shutdown script")
            self.continuation = False
            self.queue.append(b"\r\n~ # ")
        if data == b"root\r\n":
            self.queue.append(b"\r\n~ # ")
        if b"printf '__S31_HIL_SHELL_READY__" in data:
            self.queue.append(b"\r\n__S31_HIL_SHELL_READY__\r\n")
    def read(self, timeout):
        return self.queue.pop(0) if self.queue else b""

class BootConsole(Console):
    def __init__(self):
        super().__init__()
        self.booted = False
        self.queue = [b"U-Boot SPL 2024.07\r\n", b"Starting kernel ...\r\n", b"esp32-s31 login: "]
    def write(self, data):
        if not self.booted:
            raise RuntimeError("console input stopped autoboot")
        super().write(data)
    def read(self, timeout):
        data = super().read(timeout)
        if b"login:" in data:
            self.booted = True
        return data

class ShellWaitTest(unittest.TestCase):
    def test_bootloader_is_observed_without_sending_keys(self):
        port = BootConsole()
        self.assertTrue(wait_for_shell(port, 2))

    def test_shutdown_and_boot_are_not_interrupted(self):
        port = Console()
        self.assertTrue(wait_for_shell(port, 2))
        self.assertFalse(any(b"\x03" in b for b in port.writes))
        self.assertIn(b"root\r\n", port.writes)
    def test_observed_continuation_can_be_recovered(self):
        port = Console(continuation=True)
        self.assertTrue(wait_for_shell(port, 2))
        self.assertEqual(sum(b"\x03" in b for b in port.writes), 1)

if __name__ == "__main__":
    unittest.main()
