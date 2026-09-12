#!/usr/bin/env python3
"""Reject an unintended IDF checkout; allow explicit, recorded local experiments."""
import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

def versions():
    values = {}
    for line in (ROOT / "configs/build-versions.mk").read_text().splitlines():
        if line and not line.startswith("#"):
            key, value = line.replace(":=", "=").replace("?=", "=").split("=", 1)
            values[key.strip()] = value.strip()
    return values

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--idf", type=Path, required=True)
    p.add_argument("--allow-unpinned", action="store_true")
    a = p.parse_args()
    actual = subprocess.check_output(["git", "-C", str(a.idf), "rev-parse", "HEAD"], text=True).strip()
    expected = versions()["ESP_IDF_REF"]
    if actual != expected:
        message = f"IDF revision {actual} differs from pinned {expected}"
        if not a.allow_unpinned:
            p.exit(1, message + "; select the pinned checkout or explicitly set S31_ALLOW_UNPINNED=1\n")
        print("WARNING: " + message, file=sys.stderr)
    print("IDF revision: " + actual)

if __name__ == "__main__":
    main()
