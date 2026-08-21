#!/usr/bin/env python3
"""Run the ESP32-S31 Wi-Fi load matrix through idf.py monitor."""

from __future__ import annotations

import argparse
import csv
import itertools
import os
import re
import selectors
import shlex
import signal
import subprocess
import sys
import time
from pathlib import Path


ANSI_RE = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]")
DONE_RE = re.compile(rb"__S31_DONE_([0-9a-fA-F]+):([0-9]+)")
RESULT_RE = re.compile(
    rb"__S31_RESULT__([^,\r\n]+),(\d+),(\d+),([0-9.]+),(\d+),([0-9.]+)"
)
SCAN_RE = re.compile(
    rb"([0-9a-fA-F:]{17}) freq=(\d+) signal=[^\r\n]* SSID=([^\r\n]+)"
)


class MonitorError(RuntimeError):
    pass


class IdfMonitor:
    def __init__(self, repo: Path, log_path: Path, port: str) -> None:
        self.repo = repo
        self.log_path = log_path
        self.port = port
        self.master_fd: int | None = None
        self.process: subprocess.Popen[bytes] | None = None
        self.selector = selectors.DefaultSelector()
        self.log_file = None

    def start(self) -> None:
        master_fd, slave_fd = os.openpty()
        command = (
            "source /home/grieferpig/.espressif/master/esp-idf/export.sh "
            f">/dev/null && exec idf.py -C bootloader -p {shlex.quote(self.port)} monitor"
        )
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self.log_file = self.log_path.open("ab")
        self.process = subprocess.Popen(
            ["bash", "-lc", command],
            cwd=self.repo,
            stdin=slave_fd,
            stdout=slave_fd,
            stderr=slave_fd,
            start_new_session=True,
            close_fds=True,
        )
        os.close(slave_fd)
        self.master_fd = master_fd
        os.set_blocking(master_fd, False)
        self.selector.register(master_fd, selectors.EVENT_READ)

    def close(self) -> None:
        if self.master_fd is not None:
            try:
                os.write(self.master_fd, b"\x1d")
            except OSError:
                pass
        if self.process is not None and self.process.poll() is None:
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGTERM)
                try:
                    self.process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(self.process.pid, signal.SIGKILL)
                    self.process.wait()
        if self.master_fd is not None:
            try:
                self.selector.unregister(self.master_fd)
            except Exception:
                pass
            os.close(self.master_fd)
            self.master_fd = None
        if self.log_file is not None:
            self.log_file.close()
            self.log_file = None

    def send(self, text: str) -> None:
        if self.master_fd is None:
            raise MonitorError("monitor is not running")
        os.write(self.master_fd, text.encode())

    def read_until(self, pattern: re.Pattern[bytes], timeout: float) -> bytes:
        if self.master_fd is None or self.process is None:
            raise MonitorError("monitor is not running")
        deadline = time.monotonic() + timeout
        window = bytearray()
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise MonitorError(f"idf.py monitor exited with {self.process.returncode}")
            events = self.selector.select(min(1.0, deadline - time.monotonic()))
            for _key, _mask in events:
                try:
                    data = os.read(self.master_fd, 65536)
                except BlockingIOError:
                    continue
                if not data:
                    continue
                assert self.log_file is not None
                self.log_file.write(data)
                self.log_file.flush()
                window.extend(ANSI_RE.sub(b"", data).replace(b"\x00", b""))
                if len(window) > 2 * 1024 * 1024:
                    del window[: len(window) - 1024 * 1024]
                if pattern.search(window):
                    return bytes(window)
        raise MonitorError(f"timeout waiting for {pattern.pattern!r}")

    def login(self, timeout: float = 90) -> None:
        self.read_until(re.compile(rb"login:"), timeout)
        self.send("root\n")
        self.read_until(re.compile(rb"(?:^|[\r\n])[^\r\n]*# "), 10)

    def command(self, command: str, timeout: float) -> tuple[int, bytes]:
        wrapped = (
            f"{command}; s=$?; "
            "printf '\\n__S31_DONE_%08x:%u\\n' $$ \"$s\"\n"
        )
        self.send(wrapped)
        output = self.read_until(DONE_RE, timeout)
        matches = list(DONE_RE.finditer(output))
        if not matches:
            raise MonitorError("command completion marker missing")
        return int(matches[-1].group(2)), output


def q(value: str) -> str:
    return shlex.quote(value)


def setup_wifi(ssid: str, password: str, bssid: str, frequency: int) -> str:
    return " ".join(
        [
            "ip link set wlan0 up;",
            f"wifi-connect wlan0 {q(ssid)} {q(password)} {q(bssid)} {frequency};",
            "ok=0;",
            "for n in $(seq 1 30); do",
            "iw dev wlan0 link 2>/dev/null | grep -q '^Connected to ' && ok=1 && break;",
            "sleep 1;",
            "done;",
            "if [ \"$ok\" -eq 1 ]; then",
            "udhcpc -i wlan0 -q -n -t 10 -T 2 && ip -4 addr show dev wlan0;",
            "else false; fi",
        ]
    )


def case_command(
    case_id: str,
    payload_url: str,
    hart0: int,
    hart1: int,
    bt_scan: int,
    uart_busy: int,
    http_timeout: int,
    settle_seconds: float,
) -> str:
    commands = [
        "d=/tmp/s31-matrix; mkdir -p $d;",
        "rm -f $d/run0 $d/run1 $d/pid0 $d/pid1;",
        "l0=; l1=; up=; bt=0;",
    ]
    if hart0:
        commands.extend(
            [
                ": >$d/run0;",
                "(while [ -e $d/run0 ]; do taskset 1 /usr/bin/coremark >/dev/null 2>&1 & c=$!; echo $c >$d/pid0; wait $c 2>/dev/null; done) & l0=$!;",
            ]
        )
    if hart1:
        commands.extend(
            [
                ": >$d/run1;",
                "(while [ -e $d/run1 ]; do taskset 2 /usr/bin/coremark >/dev/null 2>&1 & c=$!; echo $c >$d/pid1; wait $c 2>/dev/null; done) & l1=$!;",
            ]
        )
    if bt_scan:
        commands.append("bluetoothctl --timeout 5 scan on >$d/bt.log 2>&1 && bt=1;")
    if uart_busy:
        commands.append("yes S31-UART-BUSY >/dev/ttyS0 & up=$!;")
    commands.extend(
        [
            f"sleep {settle_seconds:g};",
            "rb=$(cat /sys/class/net/wlan0/statistics/rx_bytes);",
            "ts=$(awk '{print $1}' /proc/uptime);",
            f"wget -T {http_timeout} -t 1 -O /dev/null {q(payload_url)} >$d/wget.log 2>&1; ws=$?;",
            "te=$(awk '{print $1}' /proc/uptime);",
            "ra=$(cat /sys/class/net/wlan0/statistics/rx_bytes);",
            "[ -n \"$up\" ] && kill $up 2>/dev/null;",
            "[ \"$bt\" -eq 1 ] && bluetoothctl --timeout 5 scan off >>$d/bt.log 2>&1;",
            "rm -f $d/run0 $d/run1;",
            "[ -s $d/pid0 ] && kill $(cat $d/pid0) 2>/dev/null;",
            "[ -s $d/pid1 ] && kill $(cat $d/pid1) 2>/dev/null;",
            "[ -n \"$l0\" ] && kill $l0 2>/dev/null;",
            "[ -n \"$l1\" ] && kill $l1 2>/dev/null;",
            "m=$(awk -v s=$ts -v e=$te -v b=$rb -v a=$ra 'BEGIN { t=e-s; n=a-b; r=(t>0?n*8/t/1000000:0); printf \"%.2f,%d,%.3f\",t,n,r }');",
            f"printf '\\n__S31_RESULT__%s,%u,%u,%s\\n' {q(case_id)} $bt $ws \"$m\";",
            "true",
        ]
    )
    return " ".join(commands)


def parse_result(output: bytes) -> dict[str, str | int | float]:
    matches = list(RESULT_RE.finditer(output))
    if not matches:
        raise MonitorError("case result marker missing")
    match = matches[-1]
    return {
        "case": match.group(1).decode(errors="replace"),
        "bt_active": int(match.group(2)),
        "wget_status": int(match.group(3)),
        "elapsed_s": float(match.group(4)),
        "rx_bytes": int(match.group(5)),
        "mbps": float(match.group(6)),
    }


def parse_scan(output: bytes, ssid: str) -> tuple[str, int]:
    candidates = []
    for match in SCAN_RE.finditer(output):
        found_ssid = match.group(3).decode(errors="replace").strip()
        if found_ssid == ssid:
            candidates.append((match.group(1).decode(), int(match.group(2))))
    if not candidates:
        raise MonitorError(f"SSID {ssid!r} not found in Wi-Fi scan")
    return candidates[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ssid", default="win")
    parser.add_argument("--password", default="99003231")
    parser.add_argument("--payload", default="http://192.168.1.8:8081/payload")
    parser.add_argument("--port", default="/dev/ttyUSB0")
    parser.add_argument("--http-timeout", type=int, default=30)
    parser.add_argument("--settle", type=float, default=2.0)
    parser.add_argument("--case-timeout", type=float, default=90.0)
    parser.add_argument("--boot-timeout", type=float, default=120.0)
    parser.add_argument("--boot-attempts", type=int, default=3)
    parser.add_argument("--output", type=Path, default=Path("build/s31-wifi-matrix.csv"))
    parser.add_argument("--log", type=Path, default=Path("build/s31-wifi-matrix-monitor.log"))
    args = parser.parse_args()

    repo = Path(__file__).resolve().parents[1]
    output_path = args.output if args.output.is_absolute() else repo / args.output
    log_path = args.log if args.log.is_absolute() else repo / args.log
    output_path.parent.mkdir(parents=True, exist_ok=True)
    rows: list[dict[str, str | int | float]] = []
    monitor: IdfMonitor | None = None

    try:
        for attempt in range(1, args.boot_attempts + 1):
            print(
                f"Starting idf.py monitor and waiting for Linux login "
                f"(attempt {attempt}/{args.boot_attempts})",
                flush=True,
            )
            monitor = IdfMonitor(repo, log_path, args.port)
            monitor.start()
            try:
                monitor.login(args.boot_timeout)
                break
            except MonitorError:
                monitor.close()
                monitor = None
                if attempt == args.boot_attempts:
                    raise
        assert monitor is not None
        status, scan_output = monitor.command(
            "ip link set wlan0 up; wifi-scan wlan0", 45
        )
        if status:
            raise MonitorError(f"Wi-Fi scan failed with status {status}")
        bssid, frequency = parse_scan(scan_output, args.ssid)
        print(f"Using {args.ssid} at {bssid} / {frequency} MHz", flush=True)
        status, _ = monitor.command(
            setup_wifi(args.ssid, args.password, bssid, frequency), 90
        )
        if status:
            raise MonitorError(f"Wi-Fi setup failed with status {status}")

        combinations = itertools.product((0, 1), repeat=4)
        for index, (hart0, hart1, bt_scan, uart_busy) in enumerate(combinations, 1):
            case_id = f"h0{hart0}-h1{hart1}-bt{bt_scan}-uart{uart_busy}"
            print(f"[{index:02d}/16] {case_id}", flush=True)
            command = case_command(
                case_id,
                args.payload,
                hart0,
                hart1,
                bt_scan,
                uart_busy,
                args.http_timeout,
                args.settle,
            )
            _status, raw = monitor.command(command, args.case_timeout)
            result = parse_result(raw)
            result.update(
                {
                    "hart0_coremark": hart0,
                    "hart1_coremark": hart1,
                    "bt_scan": bt_scan,
                    "uart_busy": uart_busy,
                }
            )
            rows.append(result)
            with output_path.open("w", newline="") as csv_file:
                writer = csv.DictWriter(
                    csv_file,
                    fieldnames=[
                        "case",
                        "hart0_coremark",
                        "hart1_coremark",
                        "bt_scan",
                        "uart_busy",
                        "bt_active",
                        "wget_status",
                        "elapsed_s",
                        "rx_bytes",
                        "mbps",
                    ],
                )
                writer.writeheader()
                writer.writerows(rows)
            print(
                f"         wget={result['wget_status']} bt={result['bt_active']} "
                f"time={result['elapsed_s']:.2f}s rx={result['rx_bytes']} "
                f"speed={result['mbps']:.3f} Mbit/s",
                flush=True,
            )
    except (MonitorError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    finally:
        if monitor is not None:
            monitor.close()

    print(f"CSV: {output_path}")
    print(f"Monitor log: {log_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
