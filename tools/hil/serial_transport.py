"""Bounded serial transports shared by the S31 HIL orchestrator."""
from __future__ import annotations
import base64
import json
import os
from pathlib import Path
import queue
import re
import select
import subprocess
import threading
import time

class PosixSerial:
    def __init__(self, path: str, baud: int = 115200) -> None:
        import termios

        self.path = path
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            attrs = termios.tcgetattr(self.fd)
            speed = getattr(termios, f"B{baud}")
            attrs[0] = 0
            attrs[1] = 0
            attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
            attrs[3] = 0
            attrs[4] = speed
            attrs[5] = speed
            attrs[6][termios.VMIN] = 0
            attrs[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
            termios.tcflush(self.fd, termios.TCIOFLUSH)
        except BaseException:
            os.close(self.fd)
            raise
        self.trace = bytearray()

    def write(self, data: bytes) -> None:
        view = memoryview(data)
        deadline = time.monotonic() + 5.0
        while view:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([], [self.fd], [], remaining)[1]:
                raise TimeoutError("serial write timed out")
            try:
                written = os.write(self.fd, view)
            except BlockingIOError:
                continue
            if not written:
                raise OSError("serial write made no progress")
            view = view[written:]

    def read(self, timeout: float) -> bytes:
        ready, _, _ = select.select([self.fd], [], [], timeout)
        if not ready:
            return b""
        try:
            data = os.read(self.fd, 4096)
            self.trace.extend(data)
            del self.trace[:-131072]
            return data
        except BlockingIOError:
            # A USB serial disconnect/reconnect or Linux tty wakeup can race
            # the nonblocking read after select().  Treat it as no data and
            # let the bounded caller retry.
            return b""

    def close(self) -> None:
        os.close(self.fd)


class PySerialPort:
    def __init__(self, path: str, baud: int = 115200) -> None:
        try:
            import serial  # type: ignore
        except ImportError as error:
            raise RuntimeError("pyserial is required for Windows COM ports") from error
        # Configure modem lines before opening: asserting the pyserial
        # defaults can reset the S31 and discard the runtime under test.
        self.port = serial.Serial(port=None, baudrate=baud, timeout=0.1, write_timeout=5.0)
        self.port.dtr = False
        self.port.rts = False
        self.port.port = path
        self.port.open()
        self.trace = bytearray()

    def write(self, data: bytes) -> None:
        self.port.write(data)

    def read(self, timeout: float) -> bytes:
        old_timeout = self.port.timeout
        self.port.timeout = timeout
        try:
            data = self.port.read(self.port.in_waiting or 1)
            self.trace.extend(data)
            del self.trace[:-131072]
            return data
        finally:
            self.port.timeout = old_timeout

    def close(self) -> None:
        self.port.close()


class WindowsSerialWorker:
    """Hold a Windows COM port open while the runner executes under WSL."""

    def __init__(self, path: str, baud: int = 115200) -> None:
        script = subprocess.check_output(
            ("wslpath", "-w", str(Path(__file__).with_name("s31_hil.py").resolve())), text=True
        ).strip()
        command = os.environ.get(
            "S31_HIL_WINDOWS_CMD", "/mnt/c/Windows/System32/cmd.exe"
        )
        self.process = subprocess.Popen(
            (command, "/d", "/c", "python", "-u", script,
             "--serial-worker", path, "--baud", str(baud)),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self.trace = bytearray()
        self._closed = False
        self._windows_pid = None
        self._requests = queue.Queue(maxsize=1)
        self._errors = bytearray()
        self._io_thread = threading.Thread(target=self._exchange, daemon=True)
        self._stderr_thread = threading.Thread(target=self._drain_stderr, daemon=True)
        self._io_thread.start()
        self._stderr_thread.start()
        try:
            response = self._request({"op": "hello"}, 10.0)
            if not response.get("ok"):
                raise RuntimeError(str(response.get("error", "COM worker failed")))
            if response.get("platform") == "nt":
                pid = response.get("pid")
                if type(pid) is not int or pid <= 0:
                    raise RuntimeError("invalid Windows worker PID")
                self._windows_pid = pid
            response = self._request({"op": "open"}, 10.0)
            if not response.get("ok"):
                raise RuntimeError(str(response.get("error", "COM port open failed")))
        except BaseException:
            self._terminate()
            raise

    def _drain_stderr(self):
        while True:
            data = self.process.stderr.read(1)
            if not data:
                return
            self._errors.extend(data.encode("utf-8", "replace"))
            del self._errors[:-8192]

    def _exchange(self):
        while True:
            item = self._requests.get()
            if item is None:
                return
            request, response = item
            try:
                self.process.stdin.write(json.dumps(request, separators=(",", ":")) + "\n")
                self.process.stdin.flush()
                line = self.process.stdout.readline()
                if not line:
                    raise RuntimeError("COM worker exited: " + self._errors.decode("utf-8", "replace"))
                value = json.loads(line)
                if not isinstance(value, dict) or not isinstance(value.get("ok"), bool):
                    raise RuntimeError("malformed COM worker response")
                response.put(value)
            except BaseException as error:
                response.put(error)
                return

    def _terminate(self):
        self._closed = True
        if self.process.poll() is None:
            # Terminating the WSL cmd.exe relay alone leaves python.exe alive.
            # The PID comes from our local worker, before it opens any COM port.
            if self._windows_pid is not None:
                try:
                    subprocess.run(
                        ("/mnt/c/Windows/System32/taskkill.exe", "/PID",
                         str(self._windows_pid), "/T", "/F"),
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                        timeout=2, check=False,
                    )
                except (OSError, subprocess.TimeoutExpired):
                    pass
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
        try:
            self._requests.put_nowait(None)
        except queue.Full:
            pass
        self._io_thread.join(timeout=0.2)
        self._stderr_thread.join(timeout=0.2)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            # Closing a stream whose reader still holds its lock can block.
            if not self._io_thread.is_alive() and not self._stderr_thread.is_alive():
                stream.close()

    def _request(self, request: dict, timeout: float = 6.0) -> dict:
        if self._closed:
            raise RuntimeError("COM worker is closed")
        response = queue.Queue(maxsize=1)
        try:
            self._requests.put_nowait((request, response))
            value = response.get(timeout=timeout)
        except (queue.Empty, queue.Full) as error:
            self._terminate()
            raise TimeoutError("COM worker RPC timed out") from error
        if isinstance(value, BaseException):
            self._terminate()
            raise value
        return value

    def write(self, data: bytes) -> None:
        response = self._request(
            {"op": "write", "data": base64.b64encode(data).decode("ascii")}
        )
        if not response.get("ok"):
            raise RuntimeError(str(response.get("error", "COM write failed")))

    def read(self, timeout: float) -> bytes:
        response = self._request({"op": "read", "timeout": timeout}, timeout + 2.0)
        if not response.get("ok"):
            raise RuntimeError(str(response.get("error", "COM read failed")))
        data = base64.b64decode(str(response.get("data", "")))
        self.trace.extend(data)
        del self.trace[:-131072]
        return data

    def close(self) -> None:
        if self._closed:
            return
        try:
            response = self._request({"op": "close"}, 2.0)
            if not response.get("ok"):
                raise RuntimeError("COM worker close failed")
            self.process.wait(timeout=2)
        finally:
            self._terminate()


def open_serial(path: str, baud: int = 115200):
    if os.name == "posix":
        if re.fullmatch(r"COM[0-9]+", path, re.IGNORECASE):
            return WindowsSerialWorker(path, baud)
        return PosixSerial(path, baud)
    return PySerialPort(path, baud)
