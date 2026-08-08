#!/usr/bin/env python3
import argparse
import json
import statistics
import time

import serial

BAUD = 2_000_000
TX_BYTES = 2 * 1024 * 1024
RX_BYTES = 1 * 1024 * 1024
SEED = 0x123456789ABCDEF0
MASK64 = (1 << 64) - 1


def crc16_ccitt(crc, data):
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


def read_line(port, deadline):
    data = bytearray()
    while time.monotonic() < deadline:
        ch = port.read(1)
        if not ch:
            continue
        if ch == b"\n":
            return data.decode("ascii", errors="replace").strip()
        if ch != b"\r":
            data.extend(ch)
    raise TimeoutError(f"UART line timeout; partial={data!r}")


def wait_for(port, prefix, seconds=20):
    deadline = time.monotonic() + seconds
    seen = []
    while time.monotonic() < deadline:
        line = read_line(port, deadline)
        seen.append(line)
        if line.startswith(prefix):
            return line
    raise TimeoutError(f"did not see {prefix!r}; lines={seen}")


def read_exact(port, length, seconds=30):
    deadline = time.monotonic() + seconds
    output = bytearray(length)
    view = memoryview(output)
    offset = 0
    while offset < length and time.monotonic() < deadline:
        count = port.readinto(view[offset:])
        if count:
            offset += count
    if offset != length:
        raise TimeoutError(f"binary timeout: got {offset}/{length} bytes")
    return output


def parse_fields(line):
    fields = {}
    for item in line.split():
        if "=" in item:
            key, value = item.split("=", 1)
            fields[key] = value.rstrip("%")
    return fields


def run_tx(port, expected_payload):
    port.write(b"tx\n")
    port.flush()
    header = wait_for(port, "TXR CRC=")
    expected_crc = int(parse_fields(header)["CRC"], 16)
    started = time.monotonic()
    payload_and_footer = bytearray()
    marker = b"TXR DONE "
    deadline = time.monotonic() + 30
    footer_end = -1
    marker_at = -1
    while time.monotonic() < deadline:
        chunk = port.read(port.in_waiting or 1)
        if chunk:
            payload_and_footer.extend(chunk)
            marker_at = payload_and_footer.find(marker)
            if marker_at >= 0:
                footer_end = payload_and_footer.find(b"\n", marker_at)
                if footer_end >= 0:
                    break
    if footer_end < 0:
        raise TimeoutError(f"TX footer timeout after {len(payload_and_footer)} bytes")
    payload = payload_and_footer[:marker_at]
    elapsed = time.monotonic() - started
    actual_crc = crc16_ccitt(0, payload)
    footer = payload_and_footer[marker_at:footer_end].decode("ascii", errors="replace")
    fields = parse_fields(footer)
    return {
        "direction": "tx",
        "bytes": len(payload),
        "crc": f"{actual_crc:04x}",
        "expected_crc": f"{expected_crc:04x}",
        "crc_ok": actual_crc == expected_crc,
        "content_ok": payload == expected_payload,
        "missing_bytes": TX_BYTES - len(payload),
        "host_speed_Bps": len(payload) / elapsed,
        "device_speed_Bps": float(fields["speed"]),
        "device_cpu_percent": float(fields["cpu"]),
        "device_time_us": int(fields["time_us"]),
    }


def run_rx(port, payload):
    port.write(f"rx {len(payload)}\n".encode())
    port.flush()
    wait_for(port, "RXR READY")
    started = time.monotonic()
    for offset in range(0, len(payload), 16 * 1024):
        port.write(payload[offset:offset + 16 * 1024])
    port.flush()
    elapsed = time.monotonic() - started
    footer = wait_for(port, "RXR DONE", 15)
    fields = parse_fields(footer)
    expected_crc = crc16_ccitt(0, payload)
    actual_crc = int(fields["crc"], 16)
    return {
        "direction": "rx",
        "bytes": int(fields["bytes"]),
        "crc": f"{actual_crc:04x}",
        "expected_crc": f"{expected_crc:04x}",
        "crc_ok": actual_crc == expected_crc,
        "host_write_speed_Bps": len(payload) / elapsed,
        "device_speed_Bps": float(fields["speed"]),
        "device_cpu_percent": float(fields["cpu"]),
        "device_time_us": int(fields["time_us"]),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="/dev/ttyUSB0")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--output", default="uart-bench-results.json")
    args = parser.parse_args()

    tx_payload = test_data(TX_BYTES)
    rx_payload = tx_payload[:RX_BYTES]
    results = []
    with serial.Serial(args.port, BAUD, timeout=0.2, write_timeout=20) as port:
        port.reset_input_buffer()
        # The firmware may already be waiting after emitting its first READY.
        port.write(b"sync\n")
        port.flush()
        ready = wait_for(port, "IDF UART BENCH READY", 20)
        print(ready, flush=True)
        for run in range(1, args.runs + 1):
            tx = run_tx(port, tx_payload)
            print(f"run {run} TX: {tx}", flush=True)
            wait_for(port, "IDF UART BENCH READY")
            rx = run_rx(port, rx_payload)
            print(f"run {run} RX: {rx}", flush=True)
            results.extend((tx, rx))
            wait_for(port, "IDF UART BENCH READY")

    summary = {}
    for direction in ("tx", "rx"):
        selected = [result for result in results if result["direction"] == direction]
        summary[direction] = {
            "runs": len(selected),
            "all_crc_ok": all(result["crc_ok"] for result in selected),
            "mean_device_speed_Bps": statistics.mean(result["device_speed_Bps"] for result in selected),
            "mean_device_cpu_percent": statistics.mean(result["device_cpu_percent"] for result in selected),
        }
    document = {"port": args.port, "baud": BAUD, "results": results, "summary": summary}
    with open(args.output, "w", encoding="utf-8") as output:
        json.dump(document, output, indent=2)
        output.write("\n")
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    main()
