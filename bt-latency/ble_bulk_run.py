#!/usr/bin/env python3
"""AcquireNotify-only BLE host-fd run: pair -> connect -> wait for 0xff12 -> stream bench.

Collapses all host-side gaps so a flapping LE link (supervision deaths
observed) cannot expire between steps. Prints pair/connect timings plus
the streaming numbers. Unprivileged (bluetoothd D-Bus only).
Usage: ble_bulk_run.py <addr> <svc16> <char16> <seconds>
"""
import subprocess
import time
import select
import fcntl
import os
import sys
import json
from pathlib import Path
import dbus
import dbus.mainloop.glib


def norm_uuid(u16):
    return "0000%04x-0000-1000-8000-00805f9b34fb" % int(u16.lower().lstrip("0x"), 16)


def run_ctl(cmds, timeout, match_any):
    """Run one bluetoothctl session, return (matched_label, seconds)."""
    p = subprocess.Popen(["bluetoothctl", "--timeout", str(timeout + 20),
                          "--agent", "NoInputNoOutput"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, bufsize=1)
    fl = fcntl.fcntl(p.stdout, fcntl.F_GETFL)
    fcntl.fcntl(p.stdout, fcntl.F_SETFL, fl | os.O_NONBLOCK)
    for c in cmds:
        p.stdin.write(c + "\n")
    p.stdin.flush()
    t0 = time.monotonic()
    buf = ""
    res = None
    while time.monotonic() - t0 < timeout:
        r, _, _ = select.select([p.stdout], [], [], 1.0)
        if r:
            try:
                chunk = p.stdout.read(65536)
            except Exception:
                chunk = ""
            if chunk:
                buf += chunk
                low = buf.lower()
                for label, keys in match_any:
                    if any(k in low for k in keys):
                        res = label
                        break
                if res:
                    break
    dt = time.monotonic() - t0
    p.kill()
    p.wait()
    return res, dt


def main():
    addr, svc16, char16, seconds = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
    res, dt = run_ctl(["agent on", "default-agent", f"pair {addr}"],
                      60, [("BONDED", ["paired: yes", "pairing successful", "org.bluez.error.alreadyexists"]),
                           ("FAILED", ["failed to pair"]),
                           ("NOTAVAIL", ["not available"])])
    print(f"pair: {res} in {dt:.1f}s", flush=True)
    if res != "BONDED":
        return 1
    res, dt = run_ctl([f"connect {addr}"], 60,
                      [("UP", ["connection successful"]),
                       ("FAILED", ["failed to connect"]),
                       ("NOTAVAIL", ["not available"])])
    print(f"connect: {res} in {dt:.1f}s", flush=True)
    if res != "UP":
        return 1
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    want = norm_uuid(char16)
    dev_path = "/org/bluez/hci0/dev_" + addr.replace(":", "_")
    char_path = None
    t0 = time.monotonic()
    while time.monotonic() - t0 < 90 and not char_path:
        om = dbus.Interface(bus.get_object("org.bluez", "/"),
                            "org.freedesktop.DBus.ObjectManager")
        for path, ifaces in om.GetManagedObjects().items():
            # NOTE: BlueZ 5.83 omits Device on characteristics; match path.
            if dev_path not in str(path):
                continue
            ch = ifaces.get("org.bluez.GattCharacteristic1")
            if not ch:
                continue
            if str(ch.get("UUID", "")).lower() == want:
                char_path = path
                break
        if not char_path:
            time.sleep(3)
    print(f"char {char16} objects: {'FOUND ' + str(char_path).split('/')[-1] if char_path else 'MISSING'} "
          f"after {time.monotonic() - t0:.0f}s", flush=True)
    if not char_path:
        return 1
    # AcquireNotify owns the notification session. Do not call StartNotify first.
    ch = dbus.Interface(bus.get_object("org.bluez", char_path),
                        "org.bluez.GattCharacteristic1")
    interval = os.environ.get("S31_BLE_INTERVAL")
    value_len = int(os.environ.get("S31_BLE_VALUE_LEN", "504"))
    if interval:
        iv = int(interval)
        ch.WriteValue(dbus.Array([0xa1, iv & 255, iv >> 8, value_len & 255, value_len >> 8], signature="y"),
                      dbus.Dictionary({"type": "request"}, signature="sv"))
        print(f"native control interval={iv} value_bytes={value_len}", flush=True)
        time.sleep(2)
    fd_obj = mtu = None
    for attempt in range(4):
        try:
            fd_obj, mtu = ch.AcquireNotify(dbus.Dictionary({}, signature="sv"))
            break
        except Exception as e:
            print(f"subscribe attempt {attempt+1}: {str(e)[:60]}", flush=True)
            time.sleep(10)
    if fd_obj is None:
        return 1
    print(f"notify acquired: mtu={mtu}", flush=True)
    fd = fd_obj.take()
    os.set_blocking(fd, False)
    f = os.fdopen(fd, "rb", buffering=0)
    total = pkts = gaps = dups = bad = 0
    raw = bytearray()
    arrivals = []
    first_seq = last_seq = None
    t0 = time.monotonic()
    deadline = t0 + seconds
    while time.monotonic() < deadline:
        if not select.select([fd], [], [], max(0, deadline-time.monotonic()))[0]:
            break
        try:
            chunk = f.read(65536)
        except BlockingIOError:
            time.sleep(0.002)
            continue
        except OSError:
            break
        if not chunk:
            break
        if len(chunk) >= 4:
            seq = int.from_bytes(chunk[0:4], "little")
            if first_seq is None:
                first_seq = last_seq = seq
                if os.environ.get("S31_STREAM_STARTED_FILE"):
                    Path(os.environ["S31_STREAM_STARTED_FILE"]).write_text(json.dumps({"first_host_packet":time.monotonic()}))
            elif seq == last_seq:
                dups += 1
            else:
                if seq != ((last_seq + 1) & 0xFFFFFFFF):
                    gaps += (seq - last_seq - 1) & 0xFFFFFFFF
                last_seq = seq
            if any(chunk[k] != ((k ^ seq) & 255) for k in range(4, len(chunk))):
                bad += 1
            raw.extend(chunk)
            arrivals.append((time.monotonic() - t0, len(chunk), seq))
            pkts += 1
            total += len(chunk)
    dur = time.monotonic() - t0
    print(f"STREAM bytes={total} pkts={pkts} first={first_seq} last={last_seq} "
          f"gaps={gaps} dups={dups} bad_patterns={bad} dur={dur:.1f}s "
          f"rate={total/max(dur, 1e-3):.0f} B/s ({total/max(dur, 1e-3)/1024:.2f} KiB/s)", flush=True)
    f.close()
    result = dict(bytes=total, packets=pkts, first_seq=first_seq, last_seq=last_seq,
                  gaps=gaps, duplicates=dups, bad_patterns=bad, duration=dur,
                  KiBs=total / max(dur, 1e-3) / 1024, arrivals=arrivals)
    if os.environ.get("S31_BENCH_RAW"):
        dest = Path(os.environ["S31_BENCH_RAW"])
        dest.write_bytes(raw)
        dest.with_suffix(".json").write_text(json.dumps(result, indent=2))
    return 0 if dur >= seconds - 0.1 and pkts and not (gaps or dups or bad) else 1


if __name__ == "__main__":
    sys.exit(main())
