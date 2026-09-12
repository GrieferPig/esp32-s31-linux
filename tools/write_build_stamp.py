#!/usr/bin/env python3
"""Update a dependency stamp only when its build inputs change."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--value", action="append", default=[])
    p.add_argument("--file", action="append", default=[], type=Path)
    p.add_argument("--compiler")
    p.add_argument("--repo", type=Path)
    a = p.parse_args()
    identity = {"values": a.value, "files": {}}
    for path in a.file + ([Path(a.compiler)] if a.compiler else []):
        with path.open("rb") as stream:
            identity["files"][str(path.resolve())] = hashlib.file_digest(stream, "sha256").hexdigest()
    if a.compiler:
        identity["compiler"] = subprocess.check_output([a.compiler, "--version"], text=True).splitlines()[0]
    if a.repo:
        identity["revision"] = subprocess.check_output(["git", "-C", str(a.repo), "rev-parse", "HEAD"], text=True).strip()
    text = json.dumps(identity, sort_keys=True, indent=2) + "\n"
    if not a.output.exists() or a.output.read_text() != text:
        temporary = a.output.with_name(a.output.name + ".tmp")
        temporary.write_text(text)
        temporary.replace(a.output)


if __name__ == "__main__":
    main()
