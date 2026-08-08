#!/usr/bin/env python3
"""Exercise the Linux UHCI UART at 2 Mbaud through the rootfs crctest agent."""

import argparse
import json
import re
import statistics
import time

import serial


BAUD = 2_000_000
DATA_BYTES = 2 * 1024 * 1024
SEED = 0x123456789ABCDEF0
MASK64 = (1 << 64) - 1


def crc16_ccitt(data):
    crc = 0
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def test_data(length):
    state = SEED
    output = bytearray()
    while len(output) < length:
        state ^= (state << 13) & MASK64
        state ^= state >> 7
        state ^= (state << 17) & MASK64
        state &= MASK64
        output.extend(state.to_bytes(8, "little"))
    return bytes(output[:length])


def reset_board(port):
    port.setDTR(False)
    port.setRTS(True)
    time.sleep(0.2)
    port.setDTR(False)
    port.setRTS(False)
    time.sleep(0.2)
    port.setDTR(True)
    port.setRTS(False)


def read_until(port, pattern, timeout):
    data = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        chunk = port.read(port.in_waiting or 1)
        if chunk:
            data.extend(chunk)
            match = pattern.search(data)
            if match:
                return bytes(data), match
    raise TimeoutError(f"timeout waiting for {pattern.pattern!r}; tail={bytes(data[-160:])!r}")


def wait_ready(port, timeout=45):
    return read_until(port, re.compile(rb"CRCTEST READY\r?\n"), timeout)


def run_tx(port):
    port.reset_input_buffer()
    port.write(b"tx\n")
    port.flush()
    data, match = read_until(
        port,
        re.compile(
            rb"TXR DONE bytes=(\d+) time=([0-9.]+)s "
            rb"speed=([0-9.]+) KiB/s cpu=([0-9.]+)%"
        ),
        25,
    )
    result = {
        "direction": "tx",
        "device_bytes": int(match.group(1)),
        "device_time_seconds": float(match.group(2)),
        "device_speed_KiBps": float(match.group(3)),
        "device_cpu_percent": float(match.group(4)),
        "host_bytes_before_footer": match.start(),
    }
    wait_ready(port)
    return result


def run_rx(port, payload, expected_crc):
    port.reset_input_buffer()
    port.write(f"rx {len(payload)}\n".encode())
    port.flush()
    read_until(port, re.compile(rb"RXR READY \d+\r?\n"), 10)
    started = time.monotonic()
    for offset in range(0, len(payload), 16 * 1024):
        port.write(payload[offset:offset + 16 * 1024])
    port.flush()
    host_time = time.monotonic() - started
    data, match = read_until(
        port,
        re.compile(
            rb"RXR DONE bytes=(\d+) time=([0-9.]+)s "
            rb"speed=([0-9.]+) KiB/s cpu=([0-9.]+)%"
        ),
        25,
    )
    got = re.search(rb"RXR GOT (\d+) crc=([0-9a-fA-F]{4})", data)
    if not got:
        raise RuntimeError(f"RXR GOT missing; tail={data[-240:]!r}")
    actual_crc = int(got.group(2), 16)
    result = {
        "direction": "rx",
        "device_bytes": int(match.group(1)),
        "reported_bytes": int(got.group(1)),
        "actual_crc": f"{actual_crc:04x}",
        "expected_crc": f"{expected_crc:04x}",
        "crc_ok": actual_crc == expected_crc,
        "device_time_seconds": float(match.group(2)),
        "device_speed_KiBps": float(match.group(3)),
        "device_cpu_percent": float(match.group(4)),
        "host_write_speed_KiBps": len(payload) / 1024 / host_time,
    }
    wait_ready(port)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="/dev/ttyUSB0")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--output", default="linux-uart-bench-results.json")
    args = parser.parse_args()

    payload = test_data(DATA_BYTES)
    expected_crc = crc16_ccitt(payload)
    results = []
    with serial.Serial(args.port, BAUD, timeout=0.2, write_timeout=20) as port:
        reset_board(port)
        wait_ready(port, 60)
        for run in range(1, args.runs + 1):
            tx = run_tx(port)
            print(f"run {run} TX: {tx}", flush=True)
            rx = run_rx(port, payload, expected_crc)
            print(f"run {run} RX: {rx}", flush=True)
            results.extend((tx, rx))

    summary = {}
    for direction in ("tx", "rx"):
        selected = [item for item in results if item["direction"] == direction]
        summary[direction] = {
            "runs": len(selected),
            "all_full_length": all(item["device_bytes"] == DATA_BYTES for item in selected),
            "mean_device_speed_KiBps": statistics.mean(
                item["device_speed_KiBps"] for item in selected
            ),
            "mean_device_cpu_percent": statistics.mean(
                item["device_cpu_percent"] for item in selected
            ),
        }
        if direction == "rx":
            summary[direction]["all_crc_ok"] = all(item["crc_ok"] for item in selected)
    document = {
        "port": args.port,
        "baud": BAUD,
        "notes": {
            "tx": "Device-side blocking write completion; CP210 host RX drops bytes under a continuous 2 Mbaud stream.",
            "rx": "End-to-end byte count and CRC validation.",
        },
        "results": results,
        "summary": summary,
    }
    with open(args.output, "w", encoding="utf-8") as output:
        json.dump(document, output, indent=2)
        output.write("\n")
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    main()
