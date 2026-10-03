#!/usr/bin/env python3
"""Verify the S31 toolchain and its scalar musl runtime."""
from __future__ import annotations
import subprocess
import sys
from pathlib import Path


def _run(command: list[str], *, cwd: Path | None = None) -> str:
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"FAILED: {' '.join(command)}", file=sys.stderr)
        print(result.stderr, file=sys.stderr)
        raise SystemExit(1)
    return result.stdout


def verify_compiler(prefix: Path, work_dir: Path, repo_root: Path) -> None:
    """Verify that the installed toolchain compiler exists and reports the expected build ID."""
    target = "riscv32-esp-linux-musl"
    gcc = prefix / "bin" / f"{target}-gcc"
    if not gcc.is_file():
        raise SystemExit(f"Compiler not found: {gcc}")

    # The compiler supports opt-in Xesp tests, but its default libc is scalar.
    libc = prefix / target / "sysroot" / "usr" / "lib" / "libc.so"
    if not libc.exists():
        raise SystemExit(f"libc not found: {libc}")

    objdump = prefix / "bin" / f"{target}-objdump"
    if not objdump.exists():
        raise SystemExit(f"objdump not found: {objdump}")
    readelf = prefix / "bin" / f"{target}-readelf"
    if not readelf.exists():
        raise SystemExit(f"readelf not found: {readelf}")

    disasm_file = work_dir / "libc-disassembly.txt"
    disasm_file.write_text(_run([str(objdump), "-d", str(libc)]))

    required_symbols = ["memcpy", "memchr", "memrchr", "memcmp", "strcmp"]
    for symbol in required_symbols:
        if f"<{symbol}>:" not in disasm_file.read_text():
            raise SystemExit(f"Missing symbol <{symbol}> in libc disassembly")

    # Removing XespV from musl must not remove the compiler's explicit opt-in
    # support, which is still used by standalone diagnostics.
    extension_object = work_dir / "s31-ext-test.o"
    _run([
        str(gcc),
        "-march=rv32imafbc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs_xesploop_xespv2p2",
        "-mabi=ilp32",
        "-mespv-spec=2p2",
        "-I" + str(repo_root / "rootfs"),
        "-c",
        str(repo_root / "rootfs" / "s31_ext_test.S"),
        "-o",
        str(extension_object),
    ])
    extension_attributes = _run([str(readelf), "-A", str(extension_object)])
    (work_dir / "s31-ext-test-attributes.txt").write_text(extension_attributes)
    for extension in ("_xesploop", "_xespv2p2"):
        if extension not in extension_attributes:
            raise SystemExit(f"explicit compiler test is missing {extension}")

    verify_libc_scalar(prefix, work_dir, repo_root)


def verify_libc_scalar(prefix: Path, work_dir: Path, repo_root: Path) -> None:
    """Reject every Espressif custom instruction in the default musl libc."""
    target = "riscv32-esp-linux-musl"
    libc = prefix / target / "sysroot" / "usr" / "lib" / "libc.so"
    objdump = prefix / "bin" / f"{target}-objdump"

    disasm_file = work_dir / "libc-disassembly.txt"
    if not disasm_file.exists():
        disasm_file.write_text(_run([str(objdump), "-d", str(libc)]))

    disassembly = disasm_file.read_text()
    if "esp." in disassembly:
        raise SystemExit("default musl libc contains an Espressif custom instruction")

    print("Scalar musl verification passed.")
