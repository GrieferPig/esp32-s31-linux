#!/usr/bin/env python3
"""Host-side runner for the ESP32-S31/P4 hardware-in-the-loop agents.

The runner deliberately treats probe, electrical, data and destructive
results as different evidence levels.  It uses only the Python standard
library on POSIX hosts; pyserial is used as a fallback for Windows COM ports.
"""

from __future__ import annotations

import argparse
import base64
import glob
import json
import os
import re
import sys
import threading
import time
from contextlib import ExitStack, contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


HIL_PREFIX = b"HIL1 "


@dataclass
class Result:
    board: str
    status: str
    test: str
    level: str
    detail: str = ""
    raw: dict | None = None


# Re-export transport classes for existing local HIL scripts.
from serial_transport import PosixSerial, PySerialPort, WindowsSerialWorker, open_serial


@contextmanager
def open_peer_ports(s31_path: str, p4_path: str):
    with ExitStack() as cleanup:
        s31 = open_serial(s31_path)
        cleanup.callback(s31.close)
        p4 = open_serial(p4_path)
        cleanup.callback(p4.close)
        yield s31, p4


def radio_trace_tail(port, limit: int = 16000) -> str:
    text = bytes(getattr(port, "trace", b"")).decode("utf-8", "replace")
    keywords = (
        "[S31]", "esp32s31-wifi", "rcu:", "stall", "CPU:", "PID:",
        "epc:", " ra:", "Call Trace", "sha_", "aes_", "mbedtls",
    )
    lines = [line.strip("\r") for line in text.splitlines()
             if any(keyword in line for keyword in keywords)]
    return "\n".join(lines)[-limit:]


def raw_serial_trace_tail(port, limit: int = 24000) -> str:
    return bytes(getattr(port, "trace", b"")).decode(
        "utf-8", "replace"
    )[-limit:]


def serial_worker(port_path: str, baud: int) -> int:
    """JSON-line serial bridge used by a WSL parent process."""
    # Identify the Windows child before opening a potentially stuck COM port.
    port = None
    try:
        for line in sys.stdin:
            try:
                request = json.loads(line)
                operation = request.get("op")
                if operation == "hello":
                    response = {"ok": True, "pid": os.getpid(), "platform": os.name}
                elif operation == "open":
                    if port is not None:
                        raise RuntimeError("COM port is already open")
                    port = PySerialPort(port_path, baud)
                    response = {"ok": True}
                elif operation == "write":
                    port.write(base64.b64decode(str(request.get("data", ""))))
                    response = {"ok": True}
                elif operation == "read":
                    data = port.read(float(request.get("timeout", 0.1)))
                    response = {
                        "ok": True,
                        "data": base64.b64encode(data).decode("ascii"),
                    }
                elif operation == "close":
                    print('{"ok":true}', flush=True)
                    return 0
                else:
                    response = {"ok": False, "error": "unknown operation"}
            except Exception as error:  # Keep bridge errors machine-readable.
                response = {"ok": False, "error": str(error)}
            print(json.dumps(response, separators=(",", ":")), flush=True)
    finally:
        if port is not None:
            port.close()
    return 0


def autodetect_port(kind: str) -> str | None:
    patterns = {
        "s31": ("*CP2102N*", "*Silicon_Labs*"),
        "p4": ("*CH343*", "*1a86_55d3*"),
    }
    for pattern in patterns[kind]:
        matches = sorted(glob.glob(f"/dev/serial/by-id/{pattern}"))
        if matches:
            return matches[0]
    if kind == "s31" and Path("/dev/ttyUSB0").exists():
        return "/dev/ttyUSB0"
    return None


def parse_result(line: bytes) -> Result | None:
    marker = line.find(HIL_PREFIX)
    if marker < 0:
        return None
    payload = line[marker + len(HIL_PREFIX) :].strip()
    try:
        raw = json.loads(payload)
    except json.JSONDecodeError as error:
        raise ValueError("malformed HIL JSON") from error
    if not isinstance(raw, dict) or any(
        not isinstance(raw.get(key), str) or not raw[key]
        for key in ("board", "status", "test", "level")
    ) or raw["status"] not in {"PASS", "FAIL", "SKIP"}:
        raise ValueError("invalid HIL result fields/status")
    if raw["test"] == "summary" and raw["level"] != "summary":
        raise ValueError("invalid HIL summary level")
    return Result(
        board=str(raw.get("board", "unknown")),
        status=str(raw.get("status", "UNKNOWN")),
        test=str(raw.get("test", "unknown")),
        level=str(raw.get("level", "unknown")),
        detail=str(raw.get("detail", "")),
        raw=raw,
    )


def collect(port, timeout: float, stop_on_summary: bool = True, expected_board: str | None = None, expected_case: str | None = None) -> list[Result]:
    deadline = time.monotonic() + timeout
    pending = bytearray()
    results: list[Result] = []
    while time.monotonic() < deadline:
        chunk = port.read(min(0.25, max(0.0, deadline - time.monotonic())))
        if not chunk:
            continue
        pending.extend(chunk)
        while b"\n" in pending:
            line, _, rest = pending.partition(b"\n")
            pending = bytearray(rest)
            result = parse_result(line)
            if result is None:
                continue
            if expected_board is not None and result.board != expected_board:
                raise ValueError("unexpected HIL board: " + result.board)
            if result.test == "summary" and expected_case is not None and result.raw.get("case") != expected_case:
                raise ValueError("unexpected HIL case summary")
            results.append(result)
            print(
                f"{result.status:4} {result.level:11} {result.test}: "
                f"{result.detail}"
            )
            if stop_on_summary and result.test == "summary":
                return results
    if stop_on_summary:
        raise RuntimeError(
            f"timed out after {timeout:g}s waiting for HIL summary"
        )
    return results


def collect_until_test(port, test: str, timeout: float) -> list[Result]:
    deadline = time.monotonic() + timeout
    pending = bytearray()
    results: list[Result] = []
    while time.monotonic() < deadline:
        chunk = port.read(min(0.25, max(0.0, deadline - time.monotonic())))
        if not chunk:
            continue
        pending.extend(chunk)
        while b"\n" in pending:
            line, _, rest = pending.partition(b"\n")
            pending = bytearray(rest)
            result = parse_result(line)
            if result is None:
                continue
            results.append(result)
            print(
                f"{result.status:4} {result.level:11} {result.test}: "
                f"{result.detail}"
            )
            if result.test == test:
                return results
    raise RuntimeError(f"timed out waiting for {test}")


def p4_command(port, command: str, expected_test: str,
               timeout: float = 5.0) -> Result:
    port.write(("\r\nhil " + command + "\r\n").encode("ascii"))
    results = collect_until_test(port, expected_test, timeout)
    result = next(result for result in reversed(results) if result.test == expected_test)
    if result.board != "esp32-p4-wifi6-dev-kit":
        raise ValueError("unexpected peer board: " + result.board)
    return result


def paced_serial_write(port, data: bytes, chunk_size: int = 4,
                       inter_chunk_delay: float = 0.010) -> None:
    """Avoid overrunning the small target-console RX FIFO on long commands."""
    for offset in range(0, len(data), chunk_size):
        port.write(data[offset:offset + chunk_size])
        if offset + chunk_size < len(data):
            time.sleep(inter_chunk_delay)


def s31_shell_command(port, command: str, timeout: float = 10.0) -> tuple[int, str]:
    marker = f"S31HIL{time.time_ns():x}"
    wrapped = (
        f"printf '{marker}_BEGIN\\n'; {command}; "
        f"_hil_rc=$?; printf '{marker}_END:%u\\n' \"$_hil_rc\"\r\n"
    )
    paced_serial_write(port, wrapped.encode("ascii"))
    deadline = time.monotonic() + timeout
    pending = bytearray()
    output: list[str] = []
    active = False
    while time.monotonic() < deadline:
        chunk = port.read(min(0.25, max(0.0, deadline - time.monotonic())))
        if not chunk:
            continue
        pending.extend(chunk)
        while b"\n" in pending:
            line, _, rest = pending.partition(b"\n")
            pending = bytearray(rest)
            text_line = line.decode("utf-8", "replace").strip("\r")
            if text_line == marker + "_BEGIN":
                active = True
                continue
            # The serial console may echo a long wrapped command.  Its second
            # visual line can start with the end-marker format string, but it
            # is not the marker emitted by printf.  Accept only the complete
            # numeric marker line so command echo cannot finish a test early.
            if re.fullmatch(re.escape(marker) + r"_END:[0-9]+", text_line):
                return int(text_line.rsplit(":", 1)[1]), "\n".join(output)
            if active:
                output.append(text_line)
    # A dropped console byte can leave BusyBox ash waiting at its continuation
    # prompt. Abort that incomplete line so diagnostics and mandatory cleanup
    # can start from a fresh primary prompt.
    port.write(b"\x03\r\n")
    time.sleep(0.1)
    raise RuntimeError(f"S31 command timed out: {command}")


def host_result(status: str, test: str, level: str, detail: str) -> Result:
    raw = {
        "board": "s31-p4-fixture",
        "status": status,
        "test": test,
        "level": level,
        "detail": detail,
    }
    result = Result(**raw, raw=raw)
    print(f"{status:4} {level:11} {test}: {detail}")
    return result


def run_gpio_peer(s31_path: str, p4_path: str, timeout: float) -> list[Result]:
    with open_peer_ports(s31_path, p4_path) as (s31, p4):
        results: list[Result] = []
        try:
            if not wait_for_shell(s31, min(timeout, 20.0)):
                raise RuntimeError("S31 shell prompt was not detected")
            hello = p4_command(p4, "hello", "rpc.hello", 10.0)
            match = re.search(r"token=([0-9a-fA-F]{8})", hello.detail)
            if match is None:
                raise RuntimeError("P4 arm token missing")
            token = match.group(1)

            patterns = (0x0, 0xf, 0x1, 0x2, 0x4, 0x8, 0xe, 0xd, 0xb, 0x7, 0xa, 0x5)
            forward_ok = True
            forward_detail = ""
            for pattern in patterns:
                p4_command(p4, f"arm {token}", "safety.arm")
                for lane in range(4):
                    p4_command(p4, f"lane-write {lane} {(pattern >> lane) & 1}",
                               "lane.write")
                rc, output = s31_shell_command(
                    s31, "gpioget --unquoted -c gpiochip0 42 43 44 45"
                )
                samples = re.findall(r"=(inactive|active|[01])(?:\s|$)", output)
                values = [1 if value in ("active", "1") else 0 for value in samples]
                observed = sum((value & 1) << lane for lane, value in enumerate(values[-4:]))
                if rc or len(values) < 4 or observed != pattern:
                    forward_ok = False
                    forward_detail = f"pattern=0x{pattern:x} observed=0x{observed:x} output={output!r}"
                    break
            results.append(host_result(
                "PASS" if forward_ok else "FAIL", "peer.gpio.p4-to-s31",
                "electrical", "12 patterns matched" if forward_ok else forward_detail,
            ))
            p4_command(p4, "disarm", "safety.disarm")

            reverse_ok = True
            reverse_detail = ""
            for pattern in patterns:
                assignments = " ".join(
                    f"{42 + lane}={(pattern >> lane) & 1}" for lane in range(4)
                )
                rc, output = s31_shell_command(
                    s31, f"killall gpioset 2>/dev/null || true; gpioset -z -c gpiochip0 {assignments}"
                )
                if rc:
                    reverse_ok = False
                    reverse_detail = output
                    break
                observed = 0
                for lane in range(4):
                    p4_command(p4, f"lane-input {lane} none", "lane.input")
                    sample = p4_command(p4, f"lane-read {lane}", "lane.read")
                    level_match = re.search(r"level=([01])", sample.detail)
                    if level_match:
                        observed |= int(level_match.group(1)) << lane
                if observed != pattern:
                    reverse_ok = False
                    reverse_detail = f"pattern=0x{pattern:x} observed=0x{observed:x}"
                    break
            s31_shell_command(
                s31,
                "killall gpioset 2>/dev/null || true; "
                "gpioget -c gpiochip0 42 43 44 45 >/dev/null",
            )
            results.append(host_result(
                "PASS" if reverse_ok else "FAIL", "peer.gpio.s31-to-p4",
                "electrical", "12 patterns matched" if reverse_ok else reverse_detail,
            ))

            # Establish a driven-low baseline before requesting the IRQ.  Starting
            # gpiomon while the lane is floating races its initial line sample and
            # can legitimately consume the first transition before the request is
            # active.
            p4_command(p4, f"arm {token}", "safety.arm")
            p4_command(p4, "lane-write 0 0", "lane.write")
            rc, output = s31_shell_command(
                s31,
                "rm -f /tmp/s31-hil-gpio-events /tmp/s31-hil-gpio-error; "
                "gpiomon --idle-timeout 3s -c gpiochip0 -n 4 -F '%e' 42 "
                ">/tmp/s31-hil-gpio-events 2>/tmp/s31-hil-gpio-error & sleep 1",
            )
            for level in (1, 0, 1, 0):
                p4_command(p4, f"lane-write 0 {level}", "lane.write")
                time.sleep(0.1)
            p4_command(p4, "disarm", "safety.disarm")
            time.sleep(0.5)
            rc, output = s31_shell_command(
                s31,
                "cat /tmp/s31-hil-gpio-events; cat /tmp/s31-hil-gpio-error >&2; "
                "killall gpiomon 2>/dev/null || true; "
                "rm -f /tmp/s31-hil-gpio-events /tmp/s31-hil-gpio-error",
            )
            edges = [value for value in output.splitlines() if value in ("1", "2")]
            irq_ok = rc == 0 and len(edges) == 4 and edges == ["1", "2", "1", "2"]
            results.append(host_result(
                "PASS" if irq_ok else "FAIL", "peer.gpio.irq",
                "electrical", f"edges={','.join(edges) or 'none'}",
            ))

            # Pull both ways after every output owner has exited. Matching levels
            # prove that neither board is still actively driving the lane.
            high_z = True
            for lane in range(4):
                p4_command(p4, f"lane-input {lane} down", "lane.input")
                down = p4_command(p4, f"lane-read {lane}", "lane.read")
                p4_command(p4, f"lane-input {lane} up", "lane.input")
                up = p4_command(p4, f"lane-read {lane}", "lane.read")
                high_z &= "level=0" in down.detail and "level=1" in up.detail
            p4_command(p4, "reset-lines", "safety.disarm")
            results.append(host_result(
                "PASS" if high_z else "FAIL", "peer.gpio.cleanup",
                "safety", "all four lanes follow P4 pulls and end high-Z",
            ))
        finally:
            try:
                s31_shell_command(
                    s31,
                    "killall gpioset gpiomon 2>/dev/null || true; "
                    "gpioget -c gpiochip0 42 43 44 45 >/dev/null; "
                    "rm -f /tmp/s31-hil-gpio-events /tmp/s31-hil-gpio-error",
                    2.0,
                )
            except Exception:
                pass
            try:
                p4_command(p4, "reset-lines", "safety.disarm", 2.0)
            except Exception:
                pass
        summary_status = "FAIL" if failed(results) else "PASS"
        results.append(host_result(summary_status, "summary", "summary", "GPIO peer case"))
        return results


def run_power_wake_peer(s31_path: str, p4_path: str, timeout: float,
                        s31_gpio: int, p4_gpio: int,
                        active_high: bool) -> list[Result]:
    """Prove an APPWR retention cycle is ended by a real LP GPIO level."""
    with open_peer_ports(s31_path, p4_path) as (s31, p4):
        results: list[Result] = []
        inactive = 0 if active_high else 1
        pull = 2 if active_high else 1
        try:
            if not wait_for_shell(s31, min(timeout, 20.0)):
                raise RuntimeError("S31 shell prompt was not detected")
            hello = p4_command(p4, "hello", "rpc.hello", 10.0)
            match = re.search(r"token=([0-9a-fA-F]{8})", hello.detail)
            if match is None:
                raise RuntimeError("P4 arm token missing")
            token = match.group(1)

            continuity_levels: list[int] = []
            continuity_bank: list[list[int]] = []
            for level in (inactive, 1 - inactive):
                p4_command(p4, f"arm {token}", "safety.arm")
                p4_command(p4, f"gpio-write {p4_gpio} {level}", "gpio.write")
                rc, output = s31_shell_command(
                    s31, "gpioget --unquoted -c gpiochip0 0 1 2 3 4 5 6 7"
                )
                samples = re.findall(r"=(inactive|active|[01])(?:\s|$)", output)
                levels = [1 if sample in ("active", "1") else 0
                          for sample in samples[-8:]]
                continuity_bank.append(levels)
                continuity_levels.append(levels[s31_gpio]
                                         if rc == 0 and len(levels) == 8 else -1)
            p4_command(p4, "disarm", "safety.disarm")
            continuity_ok = continuity_levels == [inactive, 1 - inactive]
            changed = ([pin for pin in range(8)
                        if len(continuity_bank) == 2 and
                        len(continuity_bank[0]) == 8 and
                        len(continuity_bank[1]) == 8 and
                        continuity_bank[0][pin] != continuity_bank[1][pin]])
            results.append(host_result(
                "PASS" if continuity_ok else "FAIL",
                "power.lp-gpio-continuity", "electrical",
                f"P4 GPIO{p4_gpio} levels={[inactive, 1 - inactive]} "
                f"observed on S31 GPIO{s31_gpio}={continuity_levels}; "
                f"changed LP GPIOs={changed}",
            ))
            if not continuity_ok:
                results.append(host_result(
                    "FAIL", "summary", "summary",
                    "dedicated LP wake wire failed continuity; suspend not entered",
                ))
                return results

            # A level which is already active while ARM is processed must fail
            # before APPWR power-down.  Besides avoiding a wake-loop, this proves
            # that the later delayed assertion is what ends suspend.
            parameters = "/sys/module/esp32s31_lp/parameters"
            p4_command(p4, f"arm {token}", "safety.arm")
            p4_command(p4, f"gpio-write {p4_gpio} {1 - inactive}", "gpio.write")
            active_hold_stop = threading.Event()
            active_hold_errors: list[Exception] = []

            def renew_active_level() -> None:
                while not active_hold_stop.wait(4.0):
                    try:
                        p4_command(p4, f"arm {token}", "safety.arm", 5.0)
                        p4_command(p4, f"gpio-write {p4_gpio} {1 - inactive}",
                                   "gpio.write", 5.0)
                    except Exception as error:
                        active_hold_errors.append(error)
                        return

            active_hold_thread = threading.Thread(
                target=renew_active_level, daemon=True)
            active_hold_thread.start()
            try:
                rc, output = s31_shell_command(
                    s31,
                    f"echo {s31_gpio} > {parameters}/mem_wake_gpio; "
                    f"echo {'Y' if active_high else 'N'} > "
                    f"{parameters}/mem_gpio_active_high; "
                    f"echo {pull} > {parameters}/mem_gpio_pull; "
                    f"echo 12000 > {parameters}/mem_wake_ms; "
                    "echo mem > /sys/power/state",
                    min(max(timeout, 15.0), 45.0),
                )
            finally:
                active_hold_stop.set()
                active_hold_thread.join(6.0)
            p4_command(p4, "disarm", "safety.disarm")
            if active_hold_errors:
                raise active_hold_errors[-1]
            results.append(host_result(
                "PASS" if rc != 0 else "FAIL", "power.already-active-rejected",
                "safety", f"suspend return code={rc}; LP refused an active level"
                if rc != 0 else "suspend unexpectedly accepted an active level",
            ))
            if rc == 0:
                results.append(host_result(
                    "FAIL", "summary", "summary",
                    "LP accepted an already-active wake level",
                ))
                return results

            rc, output = s31_shell_command(
                s31,
                "test -w /sys/module/esp32s31_lp/parameters/mem_wake_gpio && "
                "printf 'ONLINE=%s\\n' \"$(cat /sys/devices/system/cpu/online)\" && "
                "dd if=/dev/urandom of=/tmp/s31-hil-retention.bin bs=1024 count=128 2>/dev/null && "
                "printf 'RAM_BEFORE='; cksum /tmp/s31-hil-retention.bin && "
                "if [ -b /dev/sda ]; then printf 'USB_BEFORE='; "
                "dd if=/dev/sda bs=4096 count=1 2>/dev/null | cksum; fi",
                20.0,
            )
            if rc:
                results.append(host_result(
                    "FAIL", "power.precondition", "safety",
                    "LP sleep interface missing or retention setup failed: " +
                    output[-512:],
                ))
                results.append(host_result(
                    "FAIL", "summary", "summary",
                    "powered-wake precondition failed before any GPIO output",
                ))
                return results
            ram_before = re.search(r"RAM_BEFORE=(\d+\s+\d+)", output)
            usb_before = re.search(r"USB_BEFORE=(\d+\s+\d+)", output)
            results.append(host_result(
                "PASS", "power.precondition", "probe",
                f"S31 LP GPIO{s31_gpio}, P4 GPIO{p4_gpio}, CPUs online; "
                "retention and optional USB baselines captured",
            ))

            rc, output = s31_shell_command(
                s31,
                f"echo {s31_gpio} > {parameters}/mem_wake_gpio && "
                f"echo {'Y' if active_high else 'N'} > {parameters}/mem_gpio_active_high && "
                f"echo {pull} > {parameters}/mem_gpio_pull && "
                f"echo 15000 > {parameters}/mem_wake_ms",
            )
            if rc:
                raise RuntimeError("failed to configure LP GPIO wake: " + output)

            results.append(p4_command(p4, f"arm {token}", "safety.arm"))
            drive_results: list[Result] = []
            drive_errors: list[Exception] = []

            def drive_wake_level() -> None:
                try:
                    time.sleep(4.0)
                    # Linux device suspend duration is workload-dependent.  Renew
                    # the P4's ten-second safety window immediately before the
                    # held assertion so the fixture cannot return high-Z while
                    # the radio or USB stack is still quiescing.
                    p4_command(p4, f"arm {token}", "safety.arm", 5.0)
                    drive_results.append(p4_command(
                        p4, f"gpio-write {p4_gpio} {1 - inactive}",
                        "gpio.write", 5.0,
                    ))
                except Exception as error:
                    drive_errors.append(error)

            # Keep the wake level asserted until resume instead of generating a
            # short pulse which can fall wholly inside Linux's variable device-
            # suspend interval.  P4 still auto-disarms ten seconds after arm.
            drive_thread = threading.Thread(target=drive_wake_level, daemon=True)
            drive_thread.start()
            started = time.monotonic()
            usb_after = (
                "for _hil_usb_wait in 1 2 3 4 5 6 7 8 9 10; do "
                "[ -b /dev/sda ] && break; sleep 0.5; done; "
                "if [ -b /dev/sda ]; then printf 'USB_AFTER='; "
                "dd if=/dev/sda bs=4096 count=1 2>/dev/null | cksum; fi; "
                if usb_before is not None else ""
            )
            rc, output = s31_shell_command(
                s31,
                "echo mem > /sys/power/state; _suspend_rc=$?; "
                "printf 'SUSPEND_RC=%u\\n' \"$_suspend_rc\"; "
                "printf 'ONLINE=%s\\n' \"$(cat /sys/devices/system/cpu/online)\"; "
                "printf 'RAM_AFTER='; cksum /tmp/s31-hil-retention.bin; "
                + usb_after +
                "dmesg | grep 'resume state=' | tail -n 1; "
                "test \"$_suspend_rc\" -eq 0",
                max(timeout, 15.0),
            )
            elapsed = time.monotonic() - started
            drive_thread.join(6.0)
            if drive_results:
                results.append(host_result(
                    drive_results[-1].status, "peer.gpio-wake-drive",
                    "electrical", "asserted active level after 4000 ms; " +
                    drive_results[-1].detail,
                ))
            else:
                results.append(host_result(
                    "FAIL", "peer.gpio-wake-drive", "electrical",
                    str(drive_errors[-1]) if drive_errors else
                    "wake-drive thread did not complete",
                ))
            results.append(p4_command(p4, "status", "rpc.hello", 5.0))

            reason_matches = re.findall(
                r"resume state=\d+ reason=(0x[0-9a-fA-F]+)", output
            )
            wake_reason = int(reason_matches[-1], 16) if reason_matches else 0
            online = re.search(r"ONLINE=([^\r\n]+)", output)
            ram_after = re.search(r"RAM_AFTER=(\d+\s+\d+)", output)
            usb_after = re.search(r"USB_AFTER=(\d+\s+\d+)", output)
            gpio_woke = bool(wake_reason & 0x2)
            retained = (ram_before is not None and ram_after is not None and
                        ram_before.group(1) == ram_after.group(1))
            usb_ok = (usb_before is None or
                      (usb_after is not None and
                       usb_before.group(1) == usb_after.group(1)))
            passed = (rc == 0 and gpio_woke and
                      online is not None and online.group(1).strip() == "0-1" and
                      retained and usb_ok and elapsed < 14.5)
            detail = (
                f"rc={rc} elapsed={elapsed:.3f}s reason=0x{wake_reason:x} "
                f"online={online.group(1).strip() if online else 'missing'} "
                f"ram_retained={retained} usb_readback={usb_ok}"
            )
            results.append(host_result(
                "PASS" if passed else "FAIL", "power.lp-gpio-wake",
                "electrical", detail,
            ))
        finally:
            try:
                p4_command(p4, "disarm", "safety.disarm", 3.0)
            except Exception:
                pass
            try:
                s31_shell_command(
                    s31,
                    "echo -1 > /sys/module/esp32s31_lp/parameters/mem_wake_gpio; "
                    "echo 1000 > /sys/module/esp32s31_lp/parameters/mem_wake_ms; "
                    "rm -f /tmp/s31-hil-retention.bin",
                    5.0,
                )
            except Exception:
                pass
        results.append(host_result(
            "FAIL" if failed(results) else "PASS", "summary", "summary",
            "APPWR retention wake by external LP GPIO with bounded timer fallback",
        ))
        return results


def run_uart_peer(s31_path: str, p4_path: str, timeout: float) -> list[Result]:
    results: list[Result] = []
    with open_peer_ports(s31_path, p4_path) as (s31, p4):
        overlays = (("uart1", "/dev/ttyS1"), ("uart2", "/dev/ttyS2"),
                    ("uart3", "/dev/ttyS3"), ("uart3-dma", "/dev/ttyS3"))
        try:
            if not wait_for_shell(s31, min(timeout, 20.0)):
                raise RuntimeError("S31 shell prompt was not detected")
            hello = p4_command(p4, "hello", "rpc.hello")
            results.append(hello)
            match = re.search(r"token=([0-9a-fA-F]+)", hello.detail)
            if match is None:
                raise RuntimeError("P4 arm token missing")
            token = match.group(1)

            for overlay, device in overlays:
                for stale, _ in overlays:
                    s31_shell_command(
                        s31, f"s31-overlay remove {stale} --volatile >/dev/null 2>&1 || true"
                    )
                external = overlay in ("uart1", "uart2")
                parameters = (
                    f" {overlay}.tx=42 {overlay}.rx=43" if external else ""
                )
                rc, output = s31_shell_command(
                    s31,
                    f"s31-overlay apply {overlay}{parameters} --volatile",
                )
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", f"peer.{overlay}.overlay",
                    "probe", output.splitlines()[-1] if output else "applied",
                ))
                if rc:
                    continue
                cases = ((115200, 257), (460800, 4096), (921600, 8192))
                if overlay == "uart3-dma":
                    cases = ((921600, 8192),)
                for baud, length in cases:
                    if external:
                        results.append(p4_command(p4, f"arm {token}", "safety.arm"))
                        results.append(p4_command(
                            p4, f"uart-echo-start {baud}", "peer.uart-start"
                        ))
                        time.sleep(0.2)
                    operation = "uart" if external else "uart-loopback"
                    rc, output = s31_shell_command(
                        s31, f"s31-hil-io {operation} {device} {baud} {length}",
                        min(timeout, 20.0),
                    )
                    expected = "PASS uart" if external else "PASS uart-loopback"
                    output_lines = output.splitlines()
                    detail = (output_lines[-1] if rc == 0 else
                              " | ".join(output_lines[-2:])) if output_lines else "no output"
                    results.append(host_result(
                        "PASS" if rc == 0 and expected in output else "FAIL",
                        f"peer.{overlay}.payload-{baud}",
                        "data" if external else "controller",
                        detail,
                    ))
                    if external:
                        results.append(p4_command(p4, "uart-report", "peer.uart-report"))
                        results.append(p4_command(p4, "peer-stop", "peer.stop"))
                rc, output = s31_shell_command(
                    s31, f"s31-overlay remove {overlay} --volatile"
                )
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", f"peer.{overlay}.cleanup",
                    "safety", output.splitlines()[-1] if output else "removed",
                ))
        finally:
            try:
                p4_command(p4, "peer-stop", "peer.stop")
            except Exception:
                pass
            for overlay, _ in overlays:
                try:
                    s31_shell_command(
                        s31, f"s31-overlay remove {overlay} --volatile >/dev/null 2>&1 || true",
                        3.0,
                    )
                except Exception:
                    pass
        ok = not failed(results)
        results.append(host_result("PASS" if ok else "FAIL", "summary", "summary",
                                   "UART1/2 external and UART3 internal-loopback case"))
        return results


def run_spi_peer(s31_path: str, p4_path: str, timeout: float,
                 stress: bool = False, stress_speed: int = 5000000,
                 stress_length: int = 4096,
                 stress_modes: tuple[int, ...] = (0, 1, 2, 3)) -> list[Result]:
    results: list[Result] = []
    with open_peer_ports(s31_path, p4_path) as (s31, p4):
        run_error: Exception | None = None
        overlays = (("gpspi2", "/dev/spidev2.0"),
                    ("gpspi3", "/dev/spidev3.0"))
        try:
            if not wait_for_shell(s31, min(timeout, 20.0)):
                raise RuntimeError("S31 shell prompt was not detected")
            hello = p4_command(p4, "hello", "rpc.hello")
            results.append(hello)
            match = re.search(r"token=([0-9a-fA-F]+)", hello.detail)
            if match is None:
                raise RuntimeError("P4 arm token missing")
            token = match.group(1)

            for overlay, device in overlays:
                for stale, _ in overlays:
                    s31_shell_command(
                        s31, f"s31-overlay remove {stale} --volatile >/dev/null 2>&1 || true"
                    )
                rc, output = s31_shell_command(
                    s31,
                    f"s31-overlay apply {overlay} {overlay}.sclk=42 {overlay}.mosi=43 "
                    f"{overlay}.miso=45 --volatile",
                )
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", f"peer.{overlay}.overlay",
                    "probe", output.splitlines()[-1] if output else "applied",
                ))
                if rc:
                    continue
                # The standard case verifies every mode with a short exact payload
                # on the loose jumper fixture.  Long DMA characterization remains
                # available as spi-stress and is reported independently.
                cases = (((0, 500000, 8), (1, 500000, 8),
                          (2, 500000, 8), (3, 500000, 8)) if not stress else
                         tuple((mode, stress_speed, stress_length)
                               for mode in stress_modes))
                for mode, speed, length in cases:
                    results.append(p4_command(p4, f"arm {token}", "safety.arm"))
                    # P4 rev 1.3 slave DMA needs the opposite transmit sampling
                    # edge for CPHA modes so the first MISO byte is stable before
                    # the S31 master clocks the long transfer.
                    edge_args = " -1 1 -1" if mode in (1, 2) else ""
                    results.append(p4_command(
                        p4, f"spi-start {mode} {length} {speed}{edge_args}",
                                              "peer.spi-start"))
                    time.sleep(0.2)
                    rc, output = s31_shell_command(
                        s31, f"s31-hil-io spi {device} {mode} {speed} {length}",
                        min(timeout, 20.0),
                    )
                    results.append(host_result(
                        "PASS" if rc == 0 and "PASS spi" in output else "FAIL",
                        f"peer.{overlay}.mode{mode}-{length}", "data",
                        " | ".join(output.splitlines()[-3:]) if output else
                        "no output",
                    ))
                    # P4 completes the DMA transaction in its SPI worker after CS
                    # rises.  Give that task a bounded scheduling window before
                    # reading its cross-core counters and CRC.
                    time.sleep(0.1)
                    results.append(p4_command(p4, "spi-report", "peer.spi-report"))
                    results.append(p4_command(p4, "peer-stop", "peer.stop"))
                rc, output = s31_shell_command(
                    s31, f"s31-overlay remove {overlay} --volatile"
                )
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", f"peer.{overlay}.cleanup",
                    "safety", output.splitlines()[-1] if output else "removed",
                ))
        except Exception as error:
            run_error = error
            results.append(host_result(
                "FAIL", "peer.spi-runtime", "firmware",
                f"{type(error).__name__}: {error}",
            ))
        finally:
            try:
                stop = p4_command(p4, "peer-stop", "peer.stop")
                if run_error is not None:
                    results.append(stop)
            except Exception as error:
                if run_error is not None:
                    results.append(host_result(
                        "FAIL", "peer.stop", "safety",
                        f"cleanup failed: {type(error).__name__}: {error}",
                    ))
            for overlay, _ in overlays:
                try:
                    rc, output = s31_shell_command(
                        s31, f"s31-overlay remove {overlay} --volatile >/dev/null 2>&1 || true",
                        3.0,
                    )
                    if run_error is not None:
                        results.append(host_result(
                            "PASS" if rc == 0 else "FAIL",
                            f"peer.{overlay}.emergency-cleanup", "safety",
                            output.splitlines()[-1] if output else
                            "best-effort overlay removal completed",
                        ))
                except Exception as error:
                    if run_error is not None:
                        results.append(host_result(
                            "FAIL", f"peer.{overlay}.emergency-cleanup", "safety",
                            f"cleanup failed: {type(error).__name__}: {error}",
                        ))
        ok = not failed(results)
        results.append(host_result("PASS" if ok else "FAIL", "summary", "summary",
                                   "GPSPI2/3 " +
                                   ("stress case" if stress else "smoke case")))
        return results


def run_spi_stress_peer(s31_path: str, p4_path: str,
                        timeout: float, speed: int = 5000000,
                        length: int = 4096,
                        modes: tuple[int, ...] = (0, 1, 2, 3)) -> list[Result]:
    return run_spi_peer(s31_path, p4_path, timeout, stress=True,
                        stress_speed=speed, stress_length=length,
                        stress_modes=modes)


def run_spi_target_peer(s31_path: str, p4_path: str,
                        timeout: float) -> list[Result]:
    """Exercise both S31 GPSPI instances as targets with the P4 as master."""
    results: list[Result] = []
    with open_peer_ports(s31_path, p4_path) as (s31, p4):
        overlays = (("gpspi2-target", "/dev/spidev2.0"),
                    ("gpspi3-target", "/dev/spidev3.0"))
        try:
            if not wait_for_shell(s31, min(timeout, 20.0)):
                raise RuntimeError("S31 shell prompt was not detected")
            hello = p4_command(p4, "hello", "rpc.hello")
            results.append(hello)
            match = re.search(r"token=([0-9a-fA-F]+)", hello.detail)
            if match is None:
                raise RuntimeError("P4 arm token missing")
            token = match.group(1)

            for overlay, device in overlays:
                for stale, _ in overlays:
                    s31_shell_command(
                        s31, f"s31-overlay remove {stale} --volatile >/dev/null 2>&1 || true"
                    )
                rc, output = s31_shell_command(
                    s31,
                    f"s31-overlay apply {overlay} {overlay}.sclk=42 "
                    f"{overlay}.mosi=43 {overlay}.miso=45 "
                    f"{overlay}.cs0=44 --volatile",
                    15.0,
                )
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", f"peer.{overlay}.overlay",
                    "probe", output.splitlines()[-1] if output else "applied",
                ))
                if rc:
                    continue
                rc, output = s31_shell_command(s31, f"test -c {device}")
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", f"peer.{overlay}.device",
                    "probe", f"resolved target device {device}" if rc == 0 else
                    f"target device {device} missing",
                ))
                if rc:
                    continue

                for mode in range(4):
                    length = 4096
                    speed = 100000
                    results.append(p4_command(p4, f"arm {token}", "safety.arm"))
                    results.append(p4_command(
                        p4, f"spi-master-start {mode} {length} {speed}",
                        "peer.spi-master-start",
                    ))
                    rc, output = s31_shell_command(
                        s31, f"s31-hil-io spi {device} {mode} {speed} {length}",
                        min(timeout, 25.0),
                    )
                    results.append(host_result(
                        "PASS" if rc == 0 and "PASS spi" in output else "FAIL",
                        f"peer.{overlay}.mode{mode}-{length}", "data",
                        " | ".join(output.splitlines()[-3:]) if output else
                        "no output",
                    ))
                    results.extend(collect_until_test(
                        p4, "peer.spi-master-data", min(timeout, 10.0)
                    ))
                    results.append(p4_command(p4, "peer-stop", "peer.stop"))
                rc, output = s31_shell_command(
                    s31, f"s31-overlay remove {overlay} --volatile"
                )
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", f"peer.{overlay}.cleanup",
                    "safety", output.splitlines()[-1] if output else "removed",
                ))
        finally:
            try:
                p4_command(p4, "peer-stop", "peer.stop")
            except Exception:
                pass
            for overlay, _ in overlays:
                try:
                    s31_shell_command(
                        s31, f"s31-overlay remove {overlay} --volatile >/dev/null 2>&1 || true",
                        5.0,
                    )
                except Exception:
                    pass
        results.append(host_result(
            "FAIL" if failed(results) else "PASS", "summary", "summary",
            "GPSPI2/3 target modes 0-3 with 4096-byte bidirectional transfers",
        ))
        return results


def run_i2s_peer(s31_path: str, p4_path: str, timeout: float,
                 stress: bool = False, rate: int = 8000) -> list[Result]:
    results: list[Result] = []
    with open_peer_ports(s31_path, p4_path) as (s31, p4):
        overlays = ("i2s0", "i2s1")
        # A live slave joins an already-running frame clock. Supply guard data
        # beyond the checked window so initial frame synchronization cannot turn
        # a valid final period into an apparent missing tail. Never resync inside
        # the playback verification window.
        fixture_length = 32768 if stress else 16384
        playback_length = 16384 if stress else 64
        capture_length = 32768 if stress else 16384
        capture_minimum = 16384 if stress else 1024
        try:
            if not wait_for_shell(s31, min(timeout, 20.0)):
                raise RuntimeError("S31 shell prompt was not detected")
            hello = p4_command(p4, "hello", "rpc.hello")
            results.append(hello)
            match = re.search(r"token=([0-9a-fA-F]+)", hello.detail)
            if match is None:
                raise RuntimeError("P4 arm token missing")
            token = match.group(1)
            for overlay in overlays:
                for stale in overlays:
                    s31_shell_command(
                        s31,
                        f"s31-overlay remove {stale} --volatile >/dev/null 2>&1 || true",
                    )
                rc, output = s31_shell_command(
                    s31,
                    f"s31-overlay apply {overlay} "
                    f"{overlay}.tx-bck-in=42 {overlay}.bck-in=42 "
                    f"{overlay}.tx-ws-in=43 {overlay}.ws-in=43 "
                    f"{overlay}.data-out=44 {overlay}.data-in=45 --volatile",
                    15.0,
                )
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", f"peer.{overlay}.overlay",
                    "probe", output.splitlines()[-1] if output else "applied",
                ))
                if rc:
                    continue

                rc, output = s31_shell_command(
                    s31,
                    f"s31-hil-io pattern-write /tmp/{overlay}-tx.raw {fixture_length}"
                )
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", f"peer.{overlay}.fixture",
                    "data", output.splitlines()[-1] if output else "no output",
                ))
                if rc != 0:
                    # Do not diagnose the wire using a missing or truncated fixture.
                    s31_shell_command(s31, f"s31-overlay remove {overlay} --volatile")
                    continue
                results.append(p4_command(p4, f"arm {token}", "safety.arm"))
                # Start the P4 master clock first.  Its receiver discards idle
                # prefix bytes until it locks to the 16-byte deterministic test
                # pattern, so Linux playback can enter START without a clockless
                # slave-side readiness deadlock.
                results.append(p4_command(
                    p4, f"i2s-rx-master-start {playback_length} {rate}",
                                          "peer.i2s-start"))
                rc, output = s31_shell_command(
                    s31,
                    f"rm -f /tmp/{overlay}-aplay.log; "
                    f"aplay -D hw:0,0 -t raw -f S16_LE -c 2 -r {rate} "
                    f"--buffer-size=8064 --period-size=1008 "
                    f"/tmp/{overlay}-tx.raw > /tmp/{overlay}-aplay.log 2>&1 & "
                    f"echo $! >/tmp/{overlay}-aplay.pid",
                    5.0,
                )
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", f"peer.{overlay}.playback-ready",
                    "data", output.splitlines()[-1] if output else "playback queued",
                ))
                rc, output = s31_shell_command(
                    s31,
                    f"wait $(cat /tmp/{overlay}-aplay.pid); playback_rc=$?; "
                    f"cat /tmp/{overlay}-aplay.log; test $playback_rc -eq 0",
                    15.0,
                )
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", f"peer.{overlay}.playback",
                    "data", output.splitlines()[-1] if output else "playback complete",
                ))
                rc, output = s31_shell_command(s31, "dmesg | tail -n 16", 5.0)
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", f"peer.{overlay}.playback-dma",
                    "diagnostic", " | ".join(output.splitlines()[-8:]) if output else
                    "no kernel diagnostics",
                ))
                time.sleep(0.2)
                results.append(p4_command(p4, "i2s-wait-report",
                                          "peer.i2s-report", 15.0))
                results.append(p4_command(p4, "peer-stop", "peer.stop"))

                results.append(p4_command(p4, f"arm {token}", "safety.arm"))
                rc, output = s31_shell_command(
                    s31,
                    f"rm -f /tmp/{overlay}-rx.raw /tmp/{overlay}-arecord.log; "
                    f"arecord -D hw:0,0 -t raw -f S16_LE -c 2 -r {rate} "
                    f"--buffer-size=8064 --period-size=1008 --samples={capture_length // 4} "
                    f"/tmp/{overlay}-rx.raw "
                    f">/tmp/{overlay}-arecord.log 2>&1 & "
                    f"echo $! >/tmp/{overlay}-arecord.pid",
                    5.0,
                )
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", f"peer.{overlay}.capture-ready",
                    "data", output.splitlines()[-1] if output else "capture started",
                ))
                # Let arecord configure and queue its slave DMA before P4 begins
                # the finite master transfer.
                time.sleep(0.25)
                results.append(p4_command(p4, f"i2s-tx-start {capture_length} {rate}",
                                          "peer.i2s-start"))
                rc, output = s31_shell_command(
                    s31,
                    f"wait $(cat /tmp/{overlay}-arecord.pid); capture_rc=$?; "
                    f"cat /tmp/{overlay}-arecord.log; "
                    f"test $capture_rc -eq 0 && od -An -tx1 -N32 /tmp/{overlay}-rx.raw && "
                    f"s31-hil-io pattern-stream-check /tmp/{overlay}-rx.raw "
                    f"{capture_length} {capture_minimum}",
                    15.0,
                )
                results.append(host_result(
                    "PASS" if rc == 0 and "PASS pattern-stream-check" in output else "FAIL",
                    f"peer.{overlay}.capture", "data",
                    " | ".join(output.splitlines()[-4:]) if output else "no output",
                ))
                rc, output = s31_shell_command(s31, "dmesg | tail -n 16", 5.0)
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", f"peer.{overlay}.capture-dma",
                    "diagnostic", " | ".join(output.splitlines()[-8:]) if output else
                    "no kernel diagnostics",
                ))
                time.sleep(0.2)
                results.append(p4_command(p4, "i2s-report", "peer.i2s-report"))
                results.append(p4_command(p4, "peer-stop", "peer.stop"))
                rc, output = s31_shell_command(
                    s31,
                    f"rm -f /tmp/{overlay}-tx.raw /tmp/{overlay}-rx.raw; "
                    f"s31-overlay remove {overlay} --volatile",
                )
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", f"peer.{overlay}.cleanup",
                    "safety", output.splitlines()[-1] if output else "removed",
                ))
        finally:
            try:
                p4_command(p4, "peer-stop", "peer.stop")
            except Exception:
                pass
            for overlay in overlays:
                try:
                    s31_shell_command(
                        s31,
                        f"rm -f /tmp/{overlay}-tx.raw /tmp/{overlay}-rx.raw; "
                        f"s31-overlay remove {overlay} --volatile >/dev/null 2>&1 || true",
                        5.0,
                    )
                except Exception:
                    pass
        ok = not failed(results)
        results.append(host_result("PASS" if ok else "FAIL", "summary", "summary",
                                   "I2S0/1 bidirectional PCM " +
                                   ("stress case" if stress else "smoke case")))
        return results


def run_i2c_peer(s31_path: str, p4_path: str, timeout: float,
                 speed_hz: int = 100000) -> list[Result]:
    results: list[Result] = []
    with open_peer_ports(s31_path, p4_path) as (s31, p4):
        # Exercise I2C1 first: it has a distinct interrupt source and catches
        # resource-pressure failures which can otherwise be mistaken for fixed
        # adapter numbering after I2C0 has already run.
        overlays = ("i2c1", "i2c0")
        try:
            if not wait_for_shell(s31, min(timeout, 20.0)):
                raise RuntimeError("S31 shell prompt was not detected")
            hello = p4_command(p4, "hello", "rpc.hello")
            results.append(hello)
            match = re.search(r"token=([0-9a-fA-F]+)", hello.detail)
            if match is None:
                raise RuntimeError("P4 arm token missing")
            token = match.group(1)

            for overlay in overlays:
                for stale in overlays:
                    s31_shell_command(
                        s31, f"s31-overlay remove {stale} --volatile >/dev/null 2>&1 || true"
                    )
                rc, output = s31_shell_command(
                    s31,
                    f"s31-overlay apply {overlay} {overlay}.scl=42 "
                    f"{overlay}.sda=43 clock-frequency={speed_hz} --volatile",
                )
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", f"peer.{overlay}.overlay",
                    "probe", output.splitlines()[-1] if output else "applied",
                ))
                if rc:
                    continue
                rc, output = s31_shell_command(
                    s31,
                    "for node in /dev/i2c-*; do "
                    "[ ! -c \"$node\" ] || printf '%s\\n' \"$node\"; done",
                )
                devices = re.findall(r"^/dev/i2c-[0-9]+$", output, re.MULTILINE)
                if rc or len(devices) != 1:
                    _, diagnostic = s31_shell_command(
                        s31,
                        "dmesg | grep -E 'esp32s31-i2c|i2c@|i2c-[0-9]' | tail -n 8",
                    )
                    results.append(host_result(
                        "FAIL", f"peer.{overlay}.adapter", "probe",
                        f"expected one active I2C adapter, found {devices}; "
                        f"kernel={diagnostic[-240:]}",
                    ))
                    continue
                device = devices[0]
                results.append(host_result(
                    "PASS", f"peer.{overlay}.adapter", "probe",
                    f"resolved active adapter {device}",
                ))
                for attempt in ("initial", "recovery"):
                    results.append(p4_command(p4, f"arm {token}", "safety.arm"))
                    results.append(p4_command(p4, "i2c-start", "peer.i2c-start"))
                    time.sleep(0.1)
                    rc, output = s31_shell_command(
                        s31, f"s31-hil-io i2c {device}", min(timeout, 15.0)
                    )
                    results.append(host_result(
                        "PASS" if rc == 0 and "PASS i2c" in output else "FAIL",
                        f"peer.{overlay}.{attempt}",
                        "recovery" if attempt == "recovery" else "data",
                        output.splitlines()[-1] if output else "no output",
                    ))
                    results.append(p4_command(p4, "i2c-report", "peer.i2c-report"))
                    results.append(p4_command(p4, "peer-stop", "peer.stop"))
                rc, output = s31_shell_command(
                    s31, f"s31-overlay remove {overlay} --volatile"
                )
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", f"peer.{overlay}.cleanup",
                    "safety", output.splitlines()[-1] if output else "removed",
                ))
        finally:
            try:
                p4_command(p4, "peer-stop", "peer.stop")
            except Exception:
                pass
            for overlay in overlays:
                try:
                    s31_shell_command(
                        s31, f"s31-overlay remove {overlay} --volatile >/dev/null 2>&1 || true",
                        3.0,
                    )
                except Exception:
                    pass
        results.append(host_result(
            "FAIL" if failed(results) else "PASS", "summary", "summary",
            f"I2C0/1 {speed_hz} Hz register, repeated-start, NACK, and recovery case",
        ))
        return results


def run_ethernet_peer(s31_path: str, p4_path: str,
                      timeout: float) -> list[Result]:
    results: list[Result] = []
    with open_peer_ports(s31_path, p4_path) as (s31, p4):
        overlay_active = False
        try:
            if not wait_for_shell(s31, min(timeout, 20.0)):
                raise RuntimeError("S31 shell prompt was not detected")
            results.append(p4_command(p4, "hello", "rpc.hello"))
            results.append(p4_command(p4, "ethernet-start",
                                      "peer.ethernet-start", 15.0))
            rc, output = s31_shell_command(
                s31,
                "s31-overlay remove gmac --volatile >/dev/null 2>&1 || true; "
                "s31-overlay apply gmac --volatile",
                min(timeout, 20.0),
            )
            overlay_active = rc == 0
            results.append(host_result(
                "PASS" if rc == 0 else "FAIL", "peer.ethernet.overlay", "probe",
                output.splitlines()[-1] if output else "applied",
            ))
            if rc == 0:
                rc, output = s31_shell_command(
                    s31,
                    "ip link set eth0 up; i=0; "
                    "while [ \"$(cat /sys/class/net/eth0/carrier 2>/dev/null)\" != 1 ] "
                    "&& [ $i -lt 12 ]; do sleep 1; i=$((i+1)); done; "
                    "test \"$(cat /sys/class/net/eth0/carrier)\" = 1",
                    18.0,
                )
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", "peer.ethernet.carrier",
                    "electrical", "S31 YT8531 carrier up" if rc == 0 else
                    (output.splitlines()[-1] if output else "carrier timeout"),
                ))
            if rc == 0:
                rc, output = s31_shell_command(
                    s31,
                    "ip addr flush dev eth0; ip addr add 192.168.77.2/24 dev eth0; "
                    "ping -c 5 -W 2 192.168.77.1",
                    20.0,
                )
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", "peer.ethernet.icmp", "data",
                    output.splitlines()[-1] if output else "no ping output",
                ))
                for length, count, name in ((128, 64, "bulk"),
                                            (1472, 4, "mtu-1500")):
                    rc, output = s31_shell_command(
                        s31,
                        f"s31-hil-io udp-echo 192.168.77.1 3333 {length} {count}",
                        20.0,
                    )
                    results.append(host_result(
                        "PASS" if rc == 0 and "PASS udp-echo" in output else "FAIL",
                        f"peer.ethernet.{name}", "data",
                        output.splitlines()[-1] if output else "no output",
                    ))

                results.append(p4_command(p4, "ethernet-stop",
                                          "peer.ethernet-stop"))
                rc, output = s31_shell_command(
                    s31,
                    "i=0; while [ \"$(cat /sys/class/net/eth0/carrier 2>/dev/null)\" != 0 ] "
                    "&& [ $i -lt 12 ]; do sleep 1; i=$((i+1)); done; "
                    "test \"$(cat /sys/class/net/eth0/carrier)\" = 0",
                    18.0,
                )
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", "peer.ethernet.link-down",
                    "recovery", "carrier dropped after P4 stop" if rc == 0 else
                    "carrier did not drop",
                ))
                results.append(p4_command(p4, "ethernet-start",
                                          "peer.ethernet-start", 15.0))
                rc, output = s31_shell_command(
                    s31,
                    "i=0; while [ \"$(cat /sys/class/net/eth0/carrier 2>/dev/null)\" != 1 ] "
                    "&& [ $i -lt 12 ]; do sleep 1; i=$((i+1)); done; "
                    "test \"$(cat /sys/class/net/eth0/carrier)\" = 1 && "
                    "ping -c 3 -W 2 192.168.77.1 >/dev/null",
                    20.0,
                )
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", "peer.ethernet.link-recovery",
                    "recovery", "carrier and ICMP recovered" if rc == 0 else
                    (output.splitlines()[-1] if output else "recovery timeout"),
                ))
        finally:
            try:
                p4_command(p4, "ethernet-stop", "peer.ethernet-stop")
            except Exception:
                pass
            if overlay_active:
                try:
                    rc, output = s31_shell_command(
                        s31, "s31-overlay remove gmac --volatile", 10.0
                    )
                    results.append(host_result(
                        "PASS" if rc == 0 else "FAIL", "peer.ethernet.cleanup",
                        "safety", output.splitlines()[-1] if output else "removed",
                    ))
                except Exception:
                    pass
        ok = not failed(results)
        results.append(host_result("PASS" if ok else "FAIL", "summary", "summary",
                                   "direct Ethernet peer case"))
        return results


def run_pwm_pcnt_peer(s31_path: str, p4_path: str, timeout: float) -> list[Result]:
    results: list[Result] = []
    with open_peer_ports(s31_path, p4_path) as (s31, p4):
        try:
            if not wait_for_shell(s31, min(timeout, 20.0)):
                raise RuntimeError("S31 shell prompt was not detected")
            hello = p4_command(p4, "hello", "rpc.hello")
            results.append(hello)
            match = re.search(r"token=([0-9a-fA-F]+)", hello.detail)
            if match is None:
                raise RuntimeError("P4 arm token missing")
            token = match.group(1)
            s31_shell_command(
                s31, "s31-overlay remove pwm-counter --volatile >/dev/null 2>&1 || true"
            )
            rc, output = s31_shell_command(
                s31,
                "s31-overlay apply pwm-counter ledc0.out=42 pcnt0.in=43 "
                "mcpwm0.out=46 --volatile",
            )
            results.append(host_result(
                "PASS" if rc == 0 else "FAIL", "peer.pwm-pcnt.overlay", "probe",
                output.splitlines()[-1] if output else "applied",
            ))
            if rc == 0:
                results.append(p4_command(p4, "pulse-monitor-start",
                                          "peer.pulse-monitor-start"))
                pwm_setup = (
                    "p=''; for c in /sys/class/pwm/pwmchip*; do "
                    "readlink $c/device | grep -q 20392000 && p=$c; done; "
                    "test -n \"$p\"; echo 0 >$p/export 2>/dev/null || true; "
                    "echo 1000000 >$p/pwm0/period; echo 250000 >$p/pwm0/duty_cycle; "
                    "echo 1 >$p/pwm0/enable; echo $p"
                )
                rc, output = s31_shell_command(s31, pwm_setup, 10.0)
                time.sleep(1.0)
                measured = p4_command(p4, "pulse-monitor-report",
                                      "peer.pulse-monitor-report")
                results.append(measured)
                frequency = re.search(r"frequency_hz=([0-9]+)", measured.detail)
                duty = re.search(r"duty_permille=([0-9]+)", measured.detail)
                pwm_ok = (rc == 0 and frequency is not None and duty is not None and
                          900 <= int(frequency.group(1)) <= 1100 and
                          200 <= int(duty.group(1)) <= 300)
                results.append(host_result(
                    "PASS" if pwm_ok else "FAIL", "peer.pwm.measurement",
                    "electrical", measured.detail if measured.detail else output,
                ))
                s31_shell_command(
                    s31,
                    "for c in /sys/class/pwm/pwmchip*; do readlink $c/device | "
                    "grep -q 20392000 && echo 0 >$c/pwm0/enable 2>/dev/null; done",
                )

                rc, output = s31_shell_command(
                    s31,
                    "c=''; for d in /sys/bus/counter/devices/counter*; do "
                    "grep -q 20389000 $d/name && c=$d; done; test -n \"$c\"; "
                    "echo 0 >$c/count0/count; echo $c",
                )
                results.append(p4_command(p4, f"arm {token}", "safety.arm"))
                generated = p4_command(p4, "pulse-generate 1000 200",
                                       "peer.pulse-generate", 10.0)
                results.append(generated)
                rc2, count_output = s31_shell_command(
                    s31,
                    "for d in /sys/bus/counter/devices/counter*; do "
                    "grep -q 20389000 $d/name && cat $d/count0/count; done; true",
                )
                counts = re.findall(r"^[0-9]+$", count_output, re.MULTILINE)
                count = int(counts[-1]) if counts else -1
                results.append(host_result(
                    "PASS" if rc == 0 and rc2 == 0 and 198 <= count <= 202 else "FAIL",
                    "peer.pcnt.count", "electrical+counter",
                    f"generated=200 counted={count}",
                ))
            rc, output = s31_shell_command(
                s31, "s31-overlay remove pwm-counter --volatile"
            )
            results.append(host_result(
                "PASS" if rc == 0 else "FAIL", "peer.pwm-pcnt.cleanup", "safety",
                output.splitlines()[-1] if output else "removed",
            ))
        finally:
            try:
                p4_command(p4, "peer-stop", "peer.stop", 3.0)
            except Exception:
                pass
            try:
                s31_shell_command(
                    s31, "s31-overlay remove pwm-counter --volatile >/dev/null 2>&1 || true",
                    3.0,
                )
            except Exception:
                pass
        results.append(host_result(
            "FAIL" if failed(results) else "PASS", "summary", "summary",
            "S31 PWM measurement and PCNT external-pulse case",
        ))
        return results


def run_p4(port_path: str, timeout: float) -> list[Result]:
    port = open_serial(port_path)
    try:
        # Verify that the bidirectional UART RPC path itself works before the
        # longer self-test.  Opening some USB-UART adapters resets the board,
        # so allow the boot task to install its UART driver first.
        time.sleep(0.5)
        port.write(b"\r\nhil hello\r\n")
        results = collect(port, min(timeout, 5.0), stop_on_summary=False)
        if not any(result.test == "rpc.hello" for result in results):
            port.write(b"\r\nhil hello\r\n")
            results.extend(collect(port, min(timeout, 5.0), stop_on_summary=False))
        if not any(result.test == "rpc.hello" for result in results):
            results.append(
                Result(
                    board="esp32-p4-wifi6-dev-kit",
                    status="FAIL",
                    test="rpc.hello",
                    level="firmware",
                    detail="no response from UART RPC console",
                    raw={
                        "board": "esp32-p4-wifi6-dev-kit",
                        "status": "FAIL",
                        "test": "rpc.hello",
                        "level": "firmware",
                        "detail": "no response from UART RPC console",
                    },
                )
            )
            return results
        port.write(b"\r\nhil selftest\r\n")
        results.extend(collect(port, timeout, expected_board="esp32-p4-wifi6-dev-kit"))
        return results
    finally:
        port.close()


def run_wifi_suspend_checks(s31, p4, cycles: int) -> list[Result]:
    results = []
    for cycle in range(1, cycles + 1):
        command = (
            "cat /proc/sys/kernel/random/boot_id >/tmp/hil-pm-boot; "
            "dd if=/dev/urandom of=/tmp/hil-pm-ram bs=1024 count=128 2>/dev/null && "
            "sha256sum /tmp/hil-pm-ram >/tmp/hil-pm-sum && "
            "echo -1 >/sys/module/esp32s31_lp/parameters/mem_wake_gpio && "
            "echo 2000 >/sys/module/esp32s31_lp/parameters/mem_wake_ms && "
            "echo mem >/sys/power/state && "
            "cmp /tmp/hil-pm-boot /proc/sys/kernel/random/boot_id && "
            "sha256sum -c /tmp/hil-pm-sum && "
            "cat /sys/devices/system/cpu/online && "
            "cat /sys/bus/platform/devices/*/radio_health"
        )
        started = time.monotonic()
        rc, output = s31_shell_command(s31, command, 45.0)
        healthy = (rc == 0 and "/tmp/hil-pm-ram: OK" in output and
                   "0-1" in output and "abi=1 state=2 wifi_init=0" in output)
        results.append(host_result(
            "PASS" if healthy else "FAIL", f"s31.wifi-suspend-{cycle}", "recovery",
            f"elapsed={time.monotonic() - started:.3f}s " + output[-2200:]))
        if not healthy:
            break
        rc, output = s31_shell_command(
            s31, "wpa_cli -p /run/wpa_supplicant -i wlan0 reassociate >/dev/null; "
            "n=0; while [ $n -lt 20 ]; do "
            "wpa_cli -p /run/wpa_supplicant -i wlan0 status | "
            "grep -q '^wpa_state=COMPLETED$' && break; "
            "sleep 1; n=$((n+1)); done; [ $n -lt 20 ] && "
            "ping -c 2 -W 2 192.168.4.1 && "
            "s31-hil-io udp-echo 192.168.4.1 3334 1472 256", 45.0)
        report = p4_command(p4, "wifi-ap-report", "c6.wifi-ap-report", 8.0)
        expected = 256 * (cycle + 1)
        healthy = (rc == 0 and report.status == "PASS" and all(
            item in report.detail for item in (f"packets={expected}",
            f"bytes={expected * 1472}", "pattern_errors=0", "echo_errors=0")))
        results.append(host_result(
            "PASS" if healthy else "FAIL", f"s31.wifi-resume-data-{cycle}", "data",
            "userspace reassociate; " + report.detail + "; " + output[-1000:]))
        if not healthy:
            break
    return results


def run_c6_wifi(s31_path: str, p4_path: str, timeout: float,
                open_ap: bool = False, suspend_cycles: int = 0) -> list[Result]:
    # The peer's byte pattern repeats every 256 packets. Each invocation of
    # s31-hil-io starts at packet zero, so end each PM phase on that boundary.
    packet_count = 256 if suspend_cycles else 64
    ssid = "S31-HIL-P4"
    password = "" if open_ap else "s31hiltest"
    password_token = "-" if open_ap else password
    ap_address = "192.168.4.1"
    profile = "/tmp/s31-hil-wpa.conf"
    wpa_log = "/tmp/s31-hil-wpa.log"
    pidfile = "/run/esp32-config/s31-hil-wpa.pid"
    dhcp_pidfile = "/run/esp32-config/s31-hil-udhcpc.pid"
    with open_peer_ports(s31_path, p4_path) as (s31, p4):
        results: list[Result] = []
        ap_started = False
        runtime_started = False
        radio_started = False
        radio_switch_attempted = False
        bt_was_running = False
        report_collected = False

        def diagnostic(detail: str) -> None:
            try:
                _rc, output = s31_shell_command(
                    s31,
                    "echo 0 >/sys/module/esp32s31_radio/parameters/radio_timing "
                    "2>/dev/null || true; "
                    "echo t >/proc/sysrq-trigger 2>/dev/null || true; sleep 1; "
                    "echo RADIO_LOG; dmesg | tail -n 350; "
                    "echo WPA_STATUS; wpa_cli -p /run/wpa_supplicant "
                    "-i wlan0 status 2>&1; echo WPA_LOG; "
                    f"tail -n 80 {wpa_log} 2>/dev/null || true; "
                    "echo DHCP_LOG; tail -n 40 /tmp/s31-hil-udhcpc.log "
                    "2>/dev/null || true; echo NET_STATE; "
                    "ip -details -statistics link show dev wlan0 2>&1; "
                    "for f in carrier operstate flags; do printf '%s=' \"$f\"; "
                    "cat /sys/class/net/wlan0/$f 2>/dev/null || true; done; "
                    "cat /proc/net/dev | grep wlan0 || true; echo PROCESSES; "
                    "ps w | grep -E 'udhcpc|wpa_supplicant' | grep -v grep || true",
                    12.0,
                )
                detail += "\n" + output
            except Exception as exc:
                detail += "\n" + str(exc)
            trace = radio_trace_tail(s31)
            if trace:
                detail += "\nSERIAL_TRACE\n" + trace
            raw_trace = raw_serial_trace_tail(s31)
            if raw_trace:
                detail += "\nRAW_SERIAL_TRACE\n" + raw_trace
            results.append(host_result(
                "FAIL", "s31.wifi-runtime-diagnostic", "diagnostic",
                detail[-32000:],
            ))

        try:
            if not wait_for_shell(s31, min(timeout, 30.0)):
                raise RuntimeError("S31 shell prompt was not detected")
            time.sleep(1.0)
            try:
                results.append(p4_command(p4, "hello", "rpc.hello", 5.0))
            except RuntimeError:
                results.append(p4_command(p4, "hello", "rpc.hello", 10.0))

            rc, output = s31_shell_command(
                s31,
                "ok=1; { test ! -f /etc/esp32-conf/wifi.conf || "
                "grep -q '^enabled=0$' /etc/esp32-conf/wifi.conf; } || "
                "{ echo policy-not-disabled; ok=0; }; "
                "test ! -f /etc/esp32-conf/wpa_supplicant.conf || "
                "{ echo persistent-profile-present; ok=0; }; "
                "if pidof wpa_supplicant >/dev/null 2>&1; then "
                "echo supplicant-already-running; ok=0; fi; "
                "if /etc/init.d/S40btstack status >/dev/null 2>&1; then "
                "echo BTSTACK_WAS_RUNNING=1; else "
                "echo BTSTACK_WAS_RUNNING=0; fi; [ \"$ok\" = 1 ]",
                10.0,
            )
            bt_was_running = "BTSTACK_WAS_RUNNING=1" in output
            results.append(host_result(
                "PASS" if rc == 0 else "FAIL", "s31.wifi-runtime-preflight",
                "safety", "persistent Wi-Fi is disabled; wlan0 is idle"
                if rc == 0 else "runtime preflight failed: " + output[-240:],
            ))
            stage_ok = rc == 0

            if stage_ok:
                rc, output = s31_shell_command(
                    s31,
                    "/etc/init.d/S40btstack stop; rc=$?; "
                    "count=0; ref=1; while [ \"$count\" -lt 12 ]; do "
                    "ref=$(cat /sys/module/esp32s31_radio/refcnt "
                    "2>/dev/null || echo 0); [ \"$ref\" = 0 ] && break; "
                    "sleep 1; count=$((count + 1)); done; "
                    "if [ \"$rc\" -ne 0 ] || [ \"$ref\" != 0 ]; then "
                    "echo bt-stop-rc=$rc radio-refcnt=$ref; ps w | "
                    "grep s31-btstack | grep -v grep || true; false; else true; fi",
                    22.0,
                )
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL",
                    "s31.wifi-runtime-quiesce", "safety",
                    "pre-existing BTstack user stopped before radio mode switch"
                    if rc == 0 else "failed to quiesce BTstack: " + output[-240:],
                ))
                stage_ok = rc == 0

            if stage_ok:
                radio_switch_attempted = True
                try:
                    rc, output = s31_shell_command(
                        s31,
                        "(sleep 5; echo t >/proc/sysrq-trigger "
                        "2>/dev/null || true) & watchdog=$!; "
                        "/etc/init.d/S00s31-radio stop; radio_rc=$?; "
                        "kill $watchdog 2>/dev/null || true; "
                        "wait $watchdog 2>/dev/null || true; "
                        "[ \"$radio_rc\" -eq 0 ]",
                        25.0,
                    )
                except Exception as exc:
                    rc = 1
                    output = str(exc) + "\nRAW_SERIAL_TRACE\n" + \
                        raw_serial_trace_tail(s31)
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL",
                    "s31.wifi-runtime-radio-stop", "recovery",
                    "BT radio module stopped before the volatile Wi-Fi start"
                    if rc == 0 else "BT radio stop failed: " + output[-12000:],
                ))
                stage_ok = rc == 0

            if stage_ok:
                try:
                    rc, output = s31_shell_command(
                        s31,
                        "count=0; radio_rc=0; S31_RADIO_VOLATILE_MODE=wifi "
                        "/etc/init.d/S00s31-radio start || radio_rc=$?; "
                    "if [ \"$radio_rc\" -eq 0 ]; then "
                    "while [ \"$count\" -lt 20 ] && "
                    "! ip link show dev wlan0 >/dev/null 2>&1; do "
                    "sleep 1; count=$((count + 1)); done; fi; "
                    "[ \"$radio_rc\" -ne 0 ] || echo 1 >"
                    "/sys/module/esp32s31_radio/parameters/radio_timing; "
                    "[ \"$radio_rc\" -eq 0 ] && "
                    "ip link show dev wlan0 >/dev/null 2>&1 && "
                    "grep -qx wifi "
                    "/sys/module/esp32s31_radio/parameters/mode",
                        35.0,
                    )
                except Exception as exc:
                    rc = 1
                    output = str(exc) + "\nRAW_SERIAL_TRACE\n" + \
                        raw_serial_trace_tail(s31)
                radio_started = rc == 0
                results.append(host_result(
                    "PASS" if radio_started else "FAIL",
                    "s31.wifi-runtime-radio", "probe",
                    "volatile radio-wifi overlay/module exposed wlan0"
                    if radio_started else "volatile radio startup failed: " +
                    output[-240:],
                ))
                stage_ok = radio_started

            if stage_ok:
                ap_result = p4_command(
                    p4, f"wifi-ap-start {ssid} {password_token} 6",
                    "c6.wifi-ap-start", max(timeout, 30.0),
                )
                results.append(ap_result)
                ap_started = ap_result.status == "PASS"
                stage_ok = ap_started

            if stage_ok:
                profile_command = (
                    f"printf 'network={{\\n  ssid=\"{ssid}\"\\n  "
                    f"key_mgmt=NONE\\n}}\\n' >{profile}"
                    if open_ap else
                    f"wpa_passphrase '{ssid}' '{password}' | "
                    f"sed '/^[[:space:]]*#psk=/d' >{profile}"
                )
                rc, output = s31_shell_command(
                    s31,
                    f"rm -f {profile} {wpa_log} {pidfile}; "
                    f"{profile_command} && chmod 600 {profile} && "
                    "ip link set dev wlan0 up && mkdir -p "
                    "/run/wpa_supplicant /run/esp32-config && "
                    "wpa_supplicant -B -D nl80211 -i wlan0 "
                    "-C /run/wpa_supplicant "
                    f"-c {profile} -P {pidfile} -f {wpa_log} -dd",
                    15.0,
                )
                runtime_started = rc == 0
                results.append(host_result(
                    "PASS" if runtime_started else "FAIL",
                    "s31.wifi-runtime-start", "safety",
                    "temporary /tmp profile and verbose supplicant started"
                    if runtime_started else "runtime start failed: " + output[-240:],
                ))
                stage_ok = runtime_started

            if stage_ok:
                deadline = time.monotonic() + 45.0
                rc = 1
                output = "association status was not sampled"
                while time.monotonic() < deadline:
                    try:
                        rc, output = s31_shell_command(
                            s31,
                            "carrier=$(cat /sys/class/net/wlan0/carrier "
                            "2>/dev/null || echo 0); "
                            f"connected=0; grep -q 'CTRL-EVENT-CONNECTED' {wpa_log} "
                            "2>/dev/null && connected=1; "
                            "printf 'carrier=%s connected=%s\\n' \"$carrier\" "
                            "\"$connected\"; "
                            "[ \"$carrier\" = 1 ] && [ \"$connected\" = 1 ]",
                            5.0,
                        )
                    except RuntimeError as exc:
                        output = str(exc)
                        rc = 1
                        if not wait_for_shell(s31, 5.0):
                            continue
                    if rc == 0:
                        break
                    time.sleep(1.0)
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", "s31.wifi-associate",
                    "electrical", "runtime profile reached COMPLETED association"
                    if rc == 0 else "runtime association failed: " +
                    output[-240:],
                ))
                stage_ok = rc == 0

            if stage_ok:
                rc, output = s31_shell_command(
                    s31,
                    f"rm -f {dhcp_pidfile}; "
                    "udhcpc -n -q -i wlan0 -t 5 -T 2 "
                    ">/tmp/s31-hil-udhcpc.log 2>&1 & "
                    f"echo $! >{dhcp_pidfile}",
                    5.0,
                )
                if rc == 0:
                    deadline = time.monotonic() + 15.0
                    output = "DHCP client started but no lease was observed"
                    while time.monotonic() < deadline:
                        rc, output = s31_shell_command(
                            s31,
                            "ip -4 -o addr show dev wlan0 2>/dev/null | "
                            "grep -q '192.168.4.'",
                            3.0,
                        )
                        if rc == 0:
                            break
                        time.sleep(1.0)
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", "s31.wifi-dhcp", "data",
                    "S31 acquired a 192.168.4.x lease from the P4 SoftAP"
                    if rc == 0 else "DHCP failed: " + output[-240:],
                ))
                stage_ok = rc == 0

            if not stage_ok and radio_switch_attempted:
                diagnostic(output)

            if stage_ok:
                rc, output = s31_shell_command(
                    s31, f"ping -c 3 -W 2 {ap_address}", 15.0,
                )
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", "s31.wifi-ip", "data",
                    "S31 DHCP address and ICMP path to P4 pass" if rc == 0 else
                    "S31 ICMP gate failed: " + output[-240:],
                ))
                stage_ok = rc == 0

            if stage_ok:
                rc, output = s31_shell_command(
                    s31, f"s31-hil-io udp-echo {ap_address} 3334 1472 {packet_count}",
                    max(timeout, 40.0),
                )
                results.append(host_result(
                    "PASS" if rc == 0 else "FAIL", "s31.wifi-downlink", "data",
                    f"{packet_count} exact 1472-byte UDP echoes received from P4" if rc == 0
                    else "UDP exact-echo failed: " + output[-240:],
                ))
                report = p4_command(
                    p4, "wifi-ap-report", "c6.wifi-ap-report", 8.0,
                )
                report_collected = True
                if (report.status == "PASS" and
                        not all(token in report.detail for token in
                                (f"packets={packet_count}", f"bytes={packet_count * 1472}",
                                 "pattern_errors=0", "echo_errors=0"))):
                    report = host_result(
                        "FAIL", "c6.wifi-ap-report", "data",
                        "unexpected P4 counters: " + report.detail,
                    )
                results.append(report)
                if suspend_cycles and rc == 0 and report.status == "PASS":
                    results.extend(run_wifi_suspend_checks(s31, p4, suspend_cycles))
        except Exception as exc:
            results.append(host_result(
                "FAIL", "c6.wifi-runner", "recovery", str(exc),
            ))
            diagnostic(str(exc))
        finally:
            if ap_started and not report_collected:
                try:
                    results.append(p4_command(
                        p4, "wifi-ap-report", "c6.wifi-ap-report", 8.0,
                    ))
                except Exception as exc:
                    results.append(host_result(
                        "FAIL", "c6.wifi-ap-report", "diagnostic", str(exc),
                    ))
            try:
                if not wait_for_shell(s31, min(timeout, 15.0)):
                    raise RuntimeError("S31 shell unavailable for runtime cleanup")
                cleanup_steps: list[tuple[str, str, float]] = [(
                    "clients",
                    "for pfile in " + dhcp_pidfile + " " + pidfile + "; do "
                    "[ -r \"$pfile\" ] || continue; pid=$(cat \"$pfile\"); "
                    "kill \"$pid\" 2>/dev/null || true; count=0; "
                    "while kill -0 \"$pid\" 2>/dev/null && "
                    "[ \"$count\" -lt 3 ]; do sleep 1; "
                    "count=$((count + 1)); done; kill -9 \"$pid\" "
                    "2>/dev/null || true; done; "
                    "ip addr flush dev wlan0 2>/dev/null || true; "
                    "ip link set dev wlan0 down 2>/dev/null || true; "
                    f"rm -f {profile} {wpa_log} {pidfile} {dhcp_pidfile} "
                    "/tmp/s31-hil-udhcpc.log",
                    15.0,
                )]
                if radio_switch_attempted:
                    cleanup_steps.extend((
                        ("radio-refcount",
                        "count=0; ref=1; "
                        "while [ \"$count\" -lt 12 ]; do "
                        "ref=$(cat /sys/module/esp32s31_radio/refcnt "
                        "2>/dev/null || echo 0); [ \"$ref\" = 0 ] && break; "
                        "sleep 1; count=$((count + 1)); done; "
                        "echo radio-refcnt=$ref; [ \"$ref\" = 0 ]",
                        18.0),
                        ("radio-stop",
                        "echo 0 >/sys/module/esp32s31_radio/parameters/"
                        "radio_timing 2>/dev/null || true; "
                        "(sleep 5; echo t >/proc/sysrq-trigger "
                        "2>/dev/null || true) & "
                        "/etc/init.d/S00s31-radio stop",
                        20.0),
                        ("radio-restore",
                        "s31-overlay remove radio-wifi --volatile "
                        ">/dev/null 2>&1 || true; "
                        "/etc/init.d/S00s31-radio start && "
                        "grep -qx disabled "
                        "/proc/device-tree/soc/radio/wifi/status "
                        "2>/dev/null",
                        35.0),
                    ))
                if bt_was_running:
                    cleanup_steps.append((
                        "bt-restore",
                        "/etc/init.d/S40btstack start && "
                        "/etc/init.d/S40btstack status >/dev/null 2>&1",
                        25.0,
                    ))
                cleanup_steps.append((
                    "persistent-policy",
                    "{ test ! -f /etc/esp32-conf/wifi.conf || "
                    "grep -q '^enabled=0$' /etc/esp32-conf/wifi.conf; } && "
                    "test ! -f /etc/esp32-conf/wpa_supplicant.conf",
                    5.0,
                ))
                cleanup_ok = True
                cleanup_detail = []
                for step_name, step_command, step_timeout in cleanup_steps:
                    try:
                        rc, output = s31_shell_command(
                            s31, step_command, step_timeout,
                        )
                    except Exception as exc:
                        cleanup_ok = False
                        trace = raw_serial_trace_tail(s31)
                        cleanup_detail.append(
                            f"{step_name}=timeout({exc})" +
                            (("\nSERIAL_TRACE\n" + trace) if trace else "")
                        )
                        break
                    cleanup_detail.append(
                        f"{step_name}={'pass' if rc == 0 else 'fail'}" +
                        ((":" + output[-120:]) if rc != 0 and output else "")
                    )
                    if rc != 0:
                        cleanup_ok = False
                        break
                results.append(host_result(
                    "PASS" if cleanup_ok else "FAIL", "s31.wifi-cleanup",
                    "recovery", "temporary runtime state removed; persistent "
                    "disabled policy unchanged; " + ", ".join(cleanup_detail)
                    if cleanup_ok else "runtime cleanup failed: " +
                    ", ".join(cleanup_detail),
                ))
            except Exception as exc:
                results.append(host_result(
                    "FAIL", "s31.wifi-cleanup", "recovery", str(exc),
                ))
            if ap_started:
                try:
                    results.append(p4_command(
                        p4, "wifi-ap-stop", "c6.wifi-ap-stop", 8.0,
                    ))
                except Exception as exc:
                    results.append(host_result(
                        "FAIL", "c6.wifi-ap-stop", "recovery", str(exc),
                    ))

        results.append(host_result(
            "FAIL" if failed(results) else "PASS", "summary", "summary",
            f"P4/C6 {'open' if open_ap else 'WPA2'} SoftAP with ephemeral S31 "
            "profile and exact bidirectional UDP",
        ))
        return results


def run_c6_wifi_recover(s31_path: str, timeout: float) -> list[Result]:
    prefix = "/etc/esp32-conf/.s31-hil-wifi"
    wifi_conf_backup = prefix + ".conf"
    wifi_profile_backup = prefix + ".profile"
    wifi_conf_absent = prefix + ".conf-absent"
    wifi_profile_absent = prefix + ".profile-absent"
    backup_active = prefix + ".active"
    port = open_serial(s31_path)
    results: list[Result] = []
    try:
        if not wait_for_shell(port, min(timeout, 30.0)):
            raise RuntimeError("S31 shell prompt was not detected")
        restore = (
            f"if [ ! -f {backup_active} ]; then echo no-active-backup; "
            "else "
            f"if [ -f {wifi_conf_backup} ]; then cp {wifi_conf_backup} "
            "/etc/esp32-conf/wifi.conf; "
            f"elif [ -f {wifi_conf_absent} ]; then rm -f "
            "/etc/esp32-conf/wifi.conf; else exit 41; fi; "
            f"if [ -f {wifi_profile_backup} ]; then cp "
            f"{wifi_profile_backup} /etc/esp32-conf/wpa_supplicant.conf; "
            f"elif [ -f {wifi_profile_absent} ]; then rm -f "
            "/etc/esp32-conf/wpa_supplicant.conf; else exit 42; fi; "
            f"rm -f {prefix}.*; sync; fi"
        )
        try:
            rc, output = s31_shell_command(port, restore, 25.0)
        except RuntimeError:
            if not wait_for_shell(port, min(timeout, 30.0)):
                raise RuntimeError("S31 shell did not recover after restore")
            rc, output = 0, "restore marker lost; checking final state"
        verify = (
            f"test ! -f {backup_active} && "
            f"! ls {prefix}.* >/dev/null 2>&1; rc=$?; "
            "echo WIFI_CONF; cat /etc/esp32-conf/wifi.conf 2>/dev/null || "
            "echo absent; echo WIFI_PROFILE; "
            "if [ -f /etc/esp32-conf/wpa_supplicant.conf ]; then "
            "echo present; else echo absent; fi; [ \"$rc\" -eq 0 ]"
        )
        verify_rc, verify_output = s31_shell_command(port, verify, 15.0)
        rc = rc or verify_rc
        results.append(host_result(
            "PASS" if rc == 0 else "FAIL", "s31.wifi-recover",
            "recovery", (output + "\n" + verify_output)[-1000:],
        ))
    except Exception as exc:
        results.append(host_result(
            "FAIL", "s31.wifi-recover", "recovery", str(exc),
        ))
    finally:
        port.close()
    results.append(host_result(
        "FAIL" if failed(results) else "PASS", "summary", "summary",
        "restore interrupted C6 Wi-Fi HIL policy without radio stimulus",
    ))
    return results


def run_c6_ble(s31_path: str, p4_path: str, timeout: float) -> list[Result]:
    with open_peer_ports(s31_path, p4_path) as (s31, p4):
        results: list[Result] = []
        bt_runtime_conf = "/tmp/hil-bluetooth.conf"
        try:
            if not wait_for_shell(s31, min(timeout, 20.0)):
                raise RuntimeError("S31 shell prompt was not detected")
            time.sleep(1.5)
            try:
                results.append(p4_command(p4, "hello", "rpc.hello", 5.0))
            except RuntimeError:
                results.append(p4_command(p4, "hello", "rpc.hello", 10.0))
            rc, output = s31_shell_command(
                s31,
                "{ test ! -f /etc/esp32-conf/wifi.conf || "
                "grep -q '^enabled=0$' /etc/esp32-conf/wifi.conf; } && "
                "{ test ! -f /etc/esp32-conf/bluetooth.conf || "
                "grep -q '^enabled=0$' /etc/esp32-conf/bluetooth.conf; } && "
                "/etc/init.d/S40btstack stop >/dev/null 2>&1; "
                "/etc/init.d/S00s31-radio stop >/dev/null 2>&1 || true; "
                "printf 'enabled=1\\nindex=0\\nle=1\\n' "
                f">{bt_runtime_conf}; "
                "S31_RADIO_VOLATILE_MODE=bt "
                "/etc/init.d/S00s31-radio start >/tmp/hil-bt-enable.log 2>&1 && "
                f"S31_BTSTACK_CONFIG={bt_runtime_conf} "
                "/etc/init.d/S40btstack start >>/tmp/hil-bt-enable.log 2>&1; "
                "_rc=$?; sleep 2; "
                "grep -q 'advertising as S31 Radio' /run/s31-btstack-a2dp.log || _rc=1; "
                "cat /tmp/hil-bt-enable.log; [ \"$_rc\" -eq 0 ]",
                max(timeout, 50.0),
            )
            results.append(host_result(
                "PASS" if rc == 0 else "FAIL", "s31.ble-peripheral",
                "probe", "S31 BTstack advertises service ff10" if rc == 0 else
                "S31 BTstack BLE peripheral failed to start: " + output[-160:],
            ))
            if rc == 0:
                results.append(p4_command(
                    p4, "ble-test", "c6.ble-test", max(timeout, 50.0),
                ))
        finally:
            try:
                cleanup_rc, cleanup_output = s31_shell_command(
                    s31,
                    "/etc/init.d/S40btstack stop >/dev/null 2>&1 || true; "
                    "/etc/init.d/S00s31-radio stop >/dev/null 2>&1 || true; "
                    "s31-overlay remove radio-bluetooth --volatile "
                    ">/dev/null 2>&1 || true; "
                    f"rm -f {bt_runtime_conf} /tmp/hil-bt-enable.log; "
                    "test ! -e /dev/s31-hci && "
                    "{ test ! -f /etc/esp32-conf/wifi.conf || "
                    "grep -q '^enabled=0$' /etc/esp32-conf/wifi.conf; } && "
                    "{ test ! -f /etc/esp32-conf/bluetooth.conf || "
                    "grep -q '^enabled=0$' /etc/esp32-conf/bluetooth.conf; }",
                    max(timeout, 50.0),
                )
                results.append(host_result(
                    "PASS" if cleanup_rc == 0 else "FAIL",
                    "s31.ble-cleanup", "recovery",
                    "volatile Bluetooth runtime stopped; persistent disabled "
                    "policy unchanged" if cleanup_rc == 0 else
                    "policy restore failed: " + cleanup_output[-240:],
                ))
            except Exception as exc:
                results.append(host_result(
                    "FAIL", "s31.ble-cleanup", "recovery",
                    "failed to restore the pre-test Wi-Fi/Bluetooth policy: " +
                    str(exc),
                ))
        results.append(host_result(
            "FAIL" if failed(results) else "PASS", "summary", "summary",
            "C6 hosted BLE scan, S31 ff10/ff11 GATT read, and reconnect",
        ))
        return results


def wait_for_shell(port, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    pending = bytearray()
    marker = b"__S31_HIL_SHELL_READY__"
    login_sent = False
    abort_sent = False
    last_nudge = time.monotonic()
    last_probe = 0.0
    boot_activity = False
    while time.monotonic() < deadline:
        now = time.monotonic()
        chunk = port.read(0.5)
        pending.extend(chunk)
        tail = bytes(pending[-1024:])
        if any(token in tail for token in (b"ESP-ROM:", b"U-Boot", b"Booting Linux",
                                            b"Stopping network:", b"Starting kernel")):
            boot_activity = True
        if marker in (line.strip(b"\r") for line in tail.split(b"\n")):
            return True
        if b"login:" in tail and not login_sent:
            port.write(b"root\r\n")
            login_sent = True
            pending.clear()
            continue
        if b"Password:" in tail:
            port.write(b"\r\n")
            pending.clear()
            continue

        fragment = tail.rsplit(b"\n", 1)[-1].strip()
        if fragment == b">" and not abort_sent:
            # Abort only an observed ash continuation prompt. An unsolicited
            # SIGINT can cancel rcK while a software reboot is still stopping
            # services, or interrupt rcS during a new boot.
            port.write(b"\x03\r\n")
            abort_sent = True
            pending.clear()
            continue
        if fragment.endswith(b"#") and now - last_probe >= 1.0:
            port.write(b"printf '__S31_HIL_SHELL_READY__\\n'\r\n")
            last_probe = now
            pending.clear()
        elif not boot_activity and now - last_nudge >= 2.0:
            # Nudge only an otherwise quiet console, after observing it first.
            # Sending a newline during U-Boot can stop autoboot.
            port.write(b"\r\n")
            last_nudge = now
    return False


def run_s31(port_path: str, case: str, timeout: float,
            allow_usb_write: bool, peer_connected: bool,
            local_ip: str, peer_ip: str) -> list[Result]:
    port = open_serial(port_path)
    try:
        if not wait_for_shell(port, min(timeout, 20.0)):
            raise RuntimeError("S31 shell prompt was not detected")
        arguments = ["s31-hil-agent", "--case", case]
        if allow_usb_write:
            arguments.append("--allow-usb-write")
        if peer_connected:
            arguments.append("--peer-connected")
        arguments.extend(["--local-ip", local_ip, "--peer-ip", peer_ip])
        command = " ".join(arguments)
        port.write((command + "\r\n").encode("ascii"))
        return collect(port, timeout, expected_board="esp32-s31", expected_case=case)
    finally:
        port.close()


def save_results(path: str, results: Iterable[Result]) -> None:
    results = list(results)
    record = {
        "counts": {status: sum(r.status == status and r.test != "summary" for r in results) for status in ("PASS", "FAIL", "SKIP")},
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "results": [result.raw or dict(board=result.board, status=result.status, test=result.test, level=result.level, detail=result.detail) for result in results],
    }
    destination = Path(path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


def failed(results: Iterable[Result]) -> bool:
    return any(result.status not in {"PASS", "SKIP"} for result in results)


def main() -> int:
    if len(sys.argv) >= 3 and sys.argv[1] == "--serial-worker":
        worker_parser = argparse.ArgumentParser(add_help=False)
        worker_parser.add_argument("--serial-worker", required=True)
        worker_parser.add_argument("--baud", type=int, default=115200)
        worker_args = worker_parser.parse_args()
        return serial_worker(worker_args.serial_worker, worker_args.baud)

    parser = argparse.ArgumentParser()
    parser.add_argument("--board", choices=("s31", "p4", "both"), default="both")
    parser.add_argument("--s31-port")
    parser.add_argument("--p4-port")
    parser.add_argument(
        "--case", choices=("firmware", "gpio", "uart", "spi", "spi-stress", "spi-target", "i2c", "i2s", "i2s-stress", "pwm-pcnt", "power-wake", "c6-wifi", "c6-wifi-recover", "c6-ble", "peer", "sdmmc", "ethernet", "usb-drive", "mtd", "lp-core", "smp-irq-dma", "all"),
        default="firmware",
    )
    parser.add_argument("--peer-connected", action="store_true")
    parser.add_argument("--allow-usb-write", action="store_true")
    parser.add_argument("--local-ip", default="192.168.77.2/24")
    parser.add_argument("--peer-ip", default="192.168.77.1")
    parser.add_argument("--wifi-ap-open", action="store_true",
                        help="use an open P4 SoftAP for Wi-Fi fault isolation")
    parser.add_argument("--wifi-suspend-cycles", type=int, default=0,
                        help="radio-loaded mem cycles with reconnect and exact UDP verification")
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--repeat", type=int, default=1,
                        help="repeat the selected peer case in one HIL run")
    parser.add_argument("--spi-stress-speed", type=int, default=5000000,
                        help="SPI stress clock in Hz (default: 5000000)")
    parser.add_argument("--spi-stress-length", type=int, default=4096,
                        help="SPI stress payload bytes (default: 4096)")
    parser.add_argument("--spi-stress-modes", default="0,1,2,3",
                        help="comma-separated SPI modes (default: 0,1,2,3)")
    parser.add_argument("--i2c-speed", type=int, default=100000,
                        choices=(100000, 400000, 1000000),
                        help="I2C bus clock in Hz (default: 100000)")
    parser.add_argument("--i2s-rate", type=int, default=8000,
                        choices=(8000, 16000, 32000, 48000),
                        help="I2S peer sample rate in Hz (default: 8000)")
    parser.add_argument("--lp-wake-connected", action="store_true",
                        help="confirm the dedicated P4-to-S31 LP wake wire")
    parser.add_argument("--lp-wake-s31-gpio", type=int, default=0,
                        help="S31 always-on LP GPIO number (default: 0)")
    parser.add_argument("--lp-wake-p4-gpio", type=int, default=2,
                        help="P4 safe-pool GPIO driving the wake (default: 2)")
    parser.add_argument("--lp-wake-active-low", action="store_true",
                        help="drive a falling rather than rising wake level")
    parser.add_argument("--output")
    args = parser.parse_args()
    if not 0 <= args.wifi_suspend_cycles <= 20:
        parser.error("--wifi-suspend-cycles must be between 0 and 20")
    if args.repeat < 1 or args.repeat > 100:
        parser.error("--repeat must be between 1 and 100")
    if args.spi_stress_speed < 1 or args.spi_stress_speed > 40000000:
        parser.error("--spi-stress-speed must be between 1 and 40000000")
    if args.spi_stress_length < 1 or args.spi_stress_length > 4096:
        parser.error("--spi-stress-length must be between 1 and 4096")
    try:
        spi_stress_modes = tuple(int(mode) for mode in
                                 args.spi_stress_modes.split(","))
    except ValueError:
        parser.error("--spi-stress-modes must be a comma-separated mode list")
    if (not spi_stress_modes or len(set(spi_stress_modes)) != len(spi_stress_modes)
            or any(mode < 0 or mode > 3 for mode in spi_stress_modes)):
        parser.error("--spi-stress-modes must contain unique modes from 0 to 3")
    if args.lp_wake_s31_gpio < 0 or args.lp_wake_s31_gpio > 7:
        parser.error("--lp-wake-s31-gpio must be between 0 and 7")
    if args.lp_wake_p4_gpio not in (2, 3, 4, 5, 20, 21, 22, 23, 46, 47, 48):
        parser.error("--lp-wake-p4-gpio is not in the P4 safe GPIO pool")

    all_results: list[Result] = []
    if args.case == "all":
        s31_port = (args.s31_port or autodetect_port("s31")
                    if args.board in ("s31", "both") else None)
        p4_port = (args.p4_port or autodetect_port("p4")
                   if args.board in ("p4", "both") else None)
        if args.board in ("s31", "both") and s31_port is None:
            print("all requires the S31 port", file=sys.stderr)
            return 2
        if args.board in ("p4", "both") and p4_port is None:
            print("all requires the P4 port", file=sys.stderr)
            return 2

        def add_stage(name: str, operation) -> None:
            try:
                stage_results = operation()
                # A single top-level summary below represents the aggregate.
                # The firmware case's peer SKIPs are placeholders; real peer
                # cases are host-orchestrated immediately afterwards.
                all_results.extend(
                    result for result in stage_results
                    if result.test != "summary" and not (
                        name in ("s31-firmware", "p4-firmware") and
                        result.status == "SKIP" and
                        result.test.startswith("peer.")
                    )
                )
            except Exception as exc:
                all_results.append(host_result(
                    "FAIL", f"all.{name}", "orchestration", str(exc),
                ))

        if p4_port is not None:
            print(f"P4 port: {p4_port}")
            add_stage("p4-firmware", lambda: run_p4(p4_port, args.timeout))
        if s31_port is not None:
            print(f"S31 port: {s31_port}")
            add_stage("s31-firmware", lambda: run_s31(
                s31_port, "firmware", args.timeout, False, False,
                args.local_ip, args.peer_ip,
            ))

        if s31_port is not None and p4_port is not None:
            peer_stages = (
                ("gpio", lambda: run_gpio_peer(s31_port, p4_port,
                                                args.timeout)),
                ("uart", lambda: run_uart_peer(s31_port, p4_port,
                                                args.timeout)),
                ("spi", lambda: run_spi_peer(s31_port, p4_port,
                                              args.timeout)),
                ("spi-stress", lambda: run_spi_stress_peer(
                    s31_port, p4_port, args.timeout,
                    args.spi_stress_speed, args.spi_stress_length,
                    spi_stress_modes,
                )),
                ("spi-target", lambda: run_spi_target_peer(
                    s31_port, p4_port, args.timeout,
                )),
                ("i2c", lambda: run_i2c_peer(
                    s31_port, p4_port, args.timeout, args.i2c_speed,
                )),
                ("i2s", lambda: run_i2s_peer(
                    s31_port, p4_port, args.timeout,
                    stress=False, rate=args.i2s_rate,
                )),
                ("i2s-stress", lambda: run_i2s_peer(
                    s31_port, p4_port, args.timeout,
                    stress=True, rate=args.i2s_rate,
                )),
                ("pwm-pcnt", lambda: run_pwm_pcnt_peer(
                    s31_port, p4_port, args.timeout,
                )),
                ("ethernet", lambda: run_ethernet_peer(
                    s31_port, p4_port, args.timeout,
                )),
                ("c6-wifi", lambda: run_c6_wifi(
                    s31_port, p4_port, args.timeout, args.wifi_ap_open,
                    args.wifi_suspend_cycles,
                )),
                ("c6-ble", lambda: run_c6_ble(
                    s31_port, p4_port, args.timeout,
                )),
            )
            for stage_name, operation in peer_stages:
                add_stage(stage_name, operation)
            if args.lp_wake_connected:
                add_stage("power-wake", lambda: run_power_wake_peer(
                    s31_port, p4_port, args.timeout,
                    args.lp_wake_s31_gpio, args.lp_wake_p4_gpio,
                    not args.lp_wake_active_low,
                ))
            else:
                all_results.append(host_result(
                    "SKIP", "peer.power-wake", "electrical",
                    "dedicated LP wake wire not confirmed; pass "
                    "--lp-wake-connected after wiring",
                ))

        if s31_port is not None:
            for local_case in ("sdmmc", "usb-drive", "mtd", "lp-core",
                               "smp-irq-dma"):
                add_stage(local_case, lambda local_case=local_case: run_s31(
                    s31_port, local_case,
                    max(args.timeout, 120.0)
                    if local_case == "smp-irq-dma" else args.timeout,
                    args.allow_usb_write, False,
                    args.local_ip, args.peer_ip,
                ))
            add_stage("c6-wifi-recover", lambda: run_c6_wifi_recover(
                s31_port, args.timeout,
            ))

        all_results.append(host_result(
            "FAIL" if failed(all_results) else "PASS",
            "summary", "summary",
            "complete host-orchestrated S31/P4 HIL suite",
        ))
        if args.output:
            save_results(args.output, all_results)
        return 1 if failed(all_results) else 0
    if args.case == "power-wake":
        if not args.lp_wake_connected:
            print("power-wake requires --lp-wake-connected after wiring the "
                  "selected P4 GPIO to the selected S31 LP GPIO", file=sys.stderr)
            return 2
        s31_port = args.s31_port or autodetect_port("s31")
        p4_port = args.p4_port or autodetect_port("p4")
        if s31_port is None or p4_port is None:
            print("power-wake requires both S31 and P4 ports", file=sys.stderr)
            return 2
        for _ in range(args.repeat):
            all_results.extend(run_power_wake_peer(
                s31_port, p4_port, args.timeout,
                args.lp_wake_s31_gpio, args.lp_wake_p4_gpio,
                not args.lp_wake_active_low,
            ))
        if args.output:
            save_results(args.output, all_results)
        return 1 if failed(all_results) else 0
    if args.case == "c6-wifi-recover":
        s31_port = args.s31_port or autodetect_port("s31")
        if s31_port is None:
            print("C6 Wi-Fi recovery requires the S31 port", file=sys.stderr)
            return 2
        all_results.extend(run_c6_wifi_recover(s31_port, args.timeout))
        if args.output:
            save_results(args.output, all_results)
        return 1 if failed(all_results) else 0
    if args.case == "c6-wifi":
        s31_port = args.s31_port or autodetect_port("s31")
        p4_port = args.p4_port or autodetect_port("p4")
        if s31_port is None or p4_port is None:
            print("C6 Wi-Fi case requires both S31 and P4 ports", file=sys.stderr)
            return 2
        all_results.extend(run_c6_wifi(
            s31_port, p4_port, args.timeout, args.wifi_ap_open,
            args.wifi_suspend_cycles,
        ))
        if args.output:
            save_results(args.output, all_results)
        return 1 if failed(all_results) else 0
    if args.case == "c6-ble":
        s31_port = args.s31_port or autodetect_port("s31")
        p4_port = args.p4_port or autodetect_port("p4")
        if s31_port is None or p4_port is None:
            print("C6 BLE case requires both S31 and P4 ports", file=sys.stderr)
            return 2
        all_results.extend(run_c6_ble(s31_port, p4_port, args.timeout))
        if args.output:
            save_results(args.output, all_results)
        return 1 if failed(all_results) else 0
    if args.case == "ethernet" and args.board == "both":
        s31_port = args.s31_port or autodetect_port("s31")
        p4_port = args.p4_port or autodetect_port("p4")
        if s31_port is None or p4_port is None:
            print("ethernet peer case requires both S31 and P4 ports",
                  file=sys.stderr)
            return 2
        all_results.extend(run_ethernet_peer(s31_port, p4_port, args.timeout))
        if args.output:
            save_results(args.output, all_results)
        return 1 if failed(all_results) else 0
    if args.case in ("gpio", "uart", "spi", "spi-stress", "spi-target", "i2c", "i2s", "i2s-stress",
                     "pwm-pcnt"):
        s31_port = args.s31_port or autodetect_port("s31")
        p4_port = args.p4_port or autodetect_port("p4")
        if s31_port is None or p4_port is None:
            print(f"{args.case} peer case requires both S31 and P4 ports", file=sys.stderr)
            return 2
        runner = {"gpio": run_gpio_peer, "uart": run_uart_peer,
                  "spi": run_spi_peer, "i2c": run_i2c_peer,
                  "spi-stress": run_spi_stress_peer,
                  "spi-target": run_spi_target_peer,
                  "i2s": run_i2s_peer,
                  "i2s-stress": run_i2s_peer,
                  "pwm-pcnt": run_pwm_pcnt_peer}[args.case]
        for _ in range(args.repeat):
            if args.case == "spi-stress":
                all_results.extend(runner(
                    s31_port, p4_port, args.timeout,
                    args.spi_stress_speed, args.spi_stress_length,
                    spi_stress_modes,
                ))
            elif args.case == "i2c":
                all_results.extend(runner(
                    s31_port, p4_port, args.timeout, args.i2c_speed,
                ))
            elif args.case in ("i2s", "i2s-stress"):
                all_results.extend(run_i2s_peer(
                    s31_port, p4_port, args.timeout,
                    stress=args.case == "i2s-stress", rate=args.i2s_rate,
                ))
            else:
                all_results.extend(runner(s31_port, p4_port, args.timeout))
        if args.output:
            save_results(args.output, all_results)
        return 1 if failed(all_results) else 0
    if args.board in ("p4", "both"):
        p4_port = args.p4_port or autodetect_port("p4")
        if p4_port is None:
            print("P4 CH343 port not found", file=sys.stderr)
            return 2
        print(f"P4 port: {p4_port}")
        all_results.extend(run_p4(p4_port, args.timeout))
    if args.board in ("s31", "both"):
        s31_port = args.s31_port or autodetect_port("s31")
        if s31_port is None:
            print("S31 CP2102N port not found", file=sys.stderr)
            return 2
        print(f"S31 port: {s31_port}")
        all_results.extend(
            run_s31(
                s31_port,
                args.case,
                args.timeout,
                args.allow_usb_write,
                args.peer_connected,
                args.local_ip,
                args.peer_ip,
            )
        )
    if args.output:
        save_results(args.output, all_results)
    if not all_results:
        print("No HIL results received", file=sys.stderr)
        return 3
    if not any(result.test == "summary" for result in all_results):
        print("HIL run ended without a summary result", file=sys.stderr)
        return 4
    return 1 if failed(all_results) else 0


if __name__ == "__main__":
    raise SystemExit(main())
