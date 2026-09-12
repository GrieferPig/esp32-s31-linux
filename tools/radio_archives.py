#!/usr/bin/env python3
"""Resolve the captured radio archive order for both Make prerequisites and ld."""
import argparse
import re
from pathlib import Path


def archives(capture, idf, build):
    ninja = build / "build.ninja"
    declared = set(re.findall(r"^build (esp-idf/[^ :]+\.a):", ninja.read_text(), re.M)) if ninja.is_file() else set()
    for line in capture.read_text().splitlines():
        line = line.strip().replace("@IDF_PATH@", str(idf))
        if line == "esp-idf/freertos/libfreertos.a" or not line.endswith(".a"):
            continue
        path = build / line if line.startswith("esp-idf/") else Path(line)
        # Keep declared archives in the graph even if missing, so make fails
        # instead of silently linking a smaller closure.
        if line in declared or not line.startswith("esp-idf/") or path.is_file():
            yield str(path)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("capture", type=Path)
    p.add_argument("idf", type=Path)
    p.add_argument("build", type=Path)
    a = p.parse_args()
    print(" ".join(archives(a.capture, a.idf, a.build)))
