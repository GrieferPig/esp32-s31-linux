#!/usr/bin/env python3
"""BLE notify bulk-throughput harness (host side, unprivileged).

Connects to a BlueZ device, subscribes to one NOTIFY characteristic via
AcquireNotify, and counts bytes + u32-LE sequence continuity for a fixed
window. No root needed (all through bluetoothd D-Bus + the acquired fd).

Usage: ble_notify_bench.py <device-address> <service-uuid16> <char-uuid16> <seconds>
  uuid16 as hex, e.g.: ble_notify_bench.py 30:ED:A0:F3:D4:AE ff10 ff12 20
Expects the device to be already connected (use bluetoothctl connect).
Prints: MTU, bytes, packets, first/last seq, gaps, duplicates, B/s, KiB/s.
"""
import sys
import time
import dbus
import dbus.mainloop.glib


def norm_uuid(u16):
    # BlueZ exposes 16-bit UUIDs in full 128-bit form with leading zeros.
    u16 = u16.lower().lstrip("0x")
    return "0000%04x-0000-1000-8000-00805f9b34fb" % int(u16, 16)


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    addr, svc16, char16, seconds = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
    import gi
    gi.require_version("GLib", "2.0")
    from gi.repository import GLib
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    om = dbus.Interface(bus.get_object("org.bluez", "/"),
                        "org.freedesktop.DBus.ObjectManager")
    dev_path = "/org/bluez/hci0/dev_" + addr.replace(":", "_")
    want_svc, want_char = norm_uuid(svc16), norm_uuid(char16)
    char_path = None
    for path, ifaces in om.GetManagedObjects().items():
        # NOTE: BlueZ 5.83 does not always include the Device property on
        # characteristics; match the object path (implies the device).
        if dev_path not in str(path):
            continue
        ch = ifaces.get("org.bluez.GattCharacteristic1")
        if not ch:
            continue
        if str(ch.get("UUID", "")).lower() == want_char:
            char_path = path
            break
    if not char_path:
        print(f"characteristic {want_char} not found on {addr}")
        return 1
    char = dbus.Interface(bus.get_object("org.bluez", char_path),
                          "org.bluez.GattCharacteristic1")
    char.StartNotify()
    fd_obj, mtu = char.AcquireNotify(dbus.Dictionary({}, signature="sv"))
    fd = fd_obj.take()
    print(f"notify acquired: mtu={mtu} char={char_path}", flush=True)
    import os
    f = os.fdopen(fd, "rb", buffering=0)
    total = 0
    pkts = 0
    first_seq = None
    last_seq = None
    gaps = 0
    dups = 0
    t0 = time.monotonic()
    deadline = t0 + seconds
    while time.monotonic() < deadline:
        try:
            chunk = f.read(65536)
        except BlockingIOError:
            time.sleep(0.002)
            continue
        except OSError:
            break
        if not chunk:
            break
        # One read() carries one notification value: u32 LE seq + pattern.
        if len(chunk) >= 4:
            seq = int.from_bytes(chunk[0:4], "little")
            if first_seq is None:
                first_seq = last_seq = seq
            elif seq == last_seq:
                dups += 1
            else:
                if seq != ((last_seq + 1) & 0xFFFFFFFF):
                    gaps += (seq - last_seq - 1) & 0xFFFFFFFF
                last_seq = seq
            pkts += 1
            total += len(chunk)
    dur = time.monotonic() - t0
    print(f"bytes={total} packets~{pkts} first_seq={first_seq} last_seq={last_seq} "
          f"gaps={gaps} dups={dups} dur={dur:.1f}s rate={total/max(dur,1e-3):.0f} B/s "
          f"({total/max(dur,1e-3)/1024:.2f} KiB/s)", flush=True)
    try:
        char.StopNotify()
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
