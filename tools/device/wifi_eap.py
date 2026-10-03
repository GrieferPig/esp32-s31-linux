#!/usr/bin/env python3
"""Provision the S31 firmware EAP client using iw vendor commands.

Configuration is local JSON; certificate/password file paths are relative to it.
Only binary stdin carries credentials to iw (or to iw over authenticated SSH).
"""
import argparse
import json
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys

WRITE, COMMIT, CLEAR = 6, 7, 8
FIELDS = ("identity", "username", "password", "ca", "domain", "cert", "key")


def packet(op, field=0, offset=0, total=0, data=b""):
    if len(data) > 512 or offset + len(data) > total:
        raise ValueError("invalid EAP chunk")
    return struct.pack("<5I", op, field, offset, total, len(data)) + data


def load_profile(path):
    path = Path(path)
    config = json.loads(path.read_text())
    allowed = {"identity", "username", "domain", "password_file", "ca_file",
               "cert_file", "key_file"}
    if set(config) - allowed:
        raise ValueError("unknown profile field")
    fields = {}
    for i, name in enumerate(FIELDS):
        if name in ("identity", "username", "domain"):
            value = config.get(name, "").encode("utf-8")
        else:
            filename = config.get(name + "_file")
            value = (path.parent / filename).read_bytes() if filename else b""
        if not value:
            continue
        if len(value) > 4095 or b"\0" in value:
            raise ValueError("profile fields must be NUL-free and at most 4095 bytes")
        fields[i] = value
    if not all(i in fields for i in (0, 3, 4)):
        raise ValueError("identity, ca_file and domain are required")
    if len(fields[4]) > 253:
        raise ValueError("domain is too long")
    if (5 in fields) != (6 in fields):
        raise ValueError("cert_file and key_file must be supplied together")
    if 5 not in fields and not all(i in fields for i in (1, 2)):
        raise ValueError("PEAP requires username and password_file")
    return fields


def provision(fields, send):
    send(packet(CLEAR))
    try:
        for field, data in sorted(fields.items()):
            for offset in range(0, len(data), 512):
                send(packet(WRITE, field, offset, len(data), data[offset:offset + 512]))
        send(packet(COMMIT))
    except BaseException:
        try:
            send(packet(CLEAR))
        except Exception:
            pass
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", nargs="?")
    parser.add_argument("--interface", default="wlan0")
    parser.add_argument("--ssh", metavar="USER@HOST")
    parser.add_argument("--clear", action="store_true")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_.:-]{1,15}", args.interface):
        parser.error("invalid interface name")
    if args.ssh and (args.ssh.startswith("-") or not re.fullmatch(r"[A-Za-z0-9_.@:\[\]-]+", args.ssh)):
        parser.error("invalid SSH destination")
    if not args.clear and not args.profile:
        parser.error("a profile or --clear is required")
    command = ["iw", "dev", args.interface, "vendor", "send", "0x18fe34", "0x1", "-"]
    if args.ssh:
        command = ["ssh", "-T", args.ssh, shlex.join(command)]

    def send(data):
        subprocess.run(command, input=data, check=True, stdout=subprocess.DEVNULL)

    try:
        if args.clear:
            send(packet(CLEAR))
        else:
            provision(load_profile(args.profile), send)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        # Never format the profile, packet, or subprocess stdin in diagnostics.
        print("EAP provisioning failed: " + type(error).__name__, file=sys.stderr)
        return 1
    print("EAP profile cleared" if args.clear else "EAP profile installed; ready to associate")
    return 0


if __name__ == "__main__":
    sys.exit(main())
