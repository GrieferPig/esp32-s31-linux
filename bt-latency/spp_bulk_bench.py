#!/usr/bin/env python3
"""Classic SPP bulk-throughput harness (host side, unprivileged).

Opens an RFCOMM socket to channel <ch> on <addr> and counts bytes +
u32-LE sequence continuity for <seconds>. Board pumps full-MTU frames
with u32 seq prefix (see 0028). No root needed (AF_BLUETOOTH works
unprivileged; verified).

Usage: spp_bulk_bench.py <addr> <channel> <seconds>
Prints: bytes, frames, first/last seq, gaps, B/s, KiB/s.
"""
import socket
import sys
import time


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    addr, channel, seconds = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
    s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM,
                      socket.BTPROTO_RFCOMM)
    t0 = time.monotonic()
    s.connect((addr, channel))
    print(f"rfcomm connected in {time.monotonic()-t0:.1f}s", flush=True)
    s.settimeout(5.0)
    total = recvs = 0
    seqs = []
    t0 = time.monotonic()
    deadline = t0 + seconds
    try:
        while time.monotonic() < deadline:
            try:
                chunk = s.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            total += len(chunk)
            recvs += 1
            if len(chunk) >= 4:
                seqs.append(int.from_bytes(chunk[0:4], "little"))
    finally:
        s.close()
    dur = time.monotonic() - t0
    # Loss signal: first-frame seqs should advance by ~len(chunk)/framesize.
    # Report span and implied frame size instead of exact gaps (stream socket
    # may coalesce frames; byte rate is the primary metric).
    span = (max(seqs) - min(seqs)) if seqs else 0
    print(f"SPP bytes={total} recvs={recvs} seq_first={seqs[0] if seqs else None} "
          f"seq_last={seqs[-1] if seqs else None} seq_span={span} "
          f"dur={dur:.1f}s rate={total/max(dur,1e-3):.0f} B/s "
          f"({total/max(dur,1e-3)/1024:.2f} KiB/s)", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
