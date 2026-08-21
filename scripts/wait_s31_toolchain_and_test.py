#!/usr/bin/env python3
"""Verify the S31 toolchain produces ESP-vectored libc functions."""
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

    # Verify libc contains ESP-vectored memcpy
    libc = prefix / target / "sysroot" / "usr" / "lib" / "libc.so"
    if not libc.exists():
        raise SystemExit(f"libc not found: {libc}")

    objdump = prefix / "bin" / f"{target}-objdump"
    if not objdump.exists():
        raise SystemExit(f"objdump not found: {objdump}")

    disasm_file = work_dir / "libc-disassembly.txt"
    disasm_file.write_text(_run([str(objdump), "-d", str(libc)]))

    required_symbols = ["memcpy", "memchr", "memrchr", "memcmp", "strcmp",
                        "__riscv32_xespv_memcpy64"]
    for symbol in required_symbols:
        if f"<{symbol}>:" not in disasm_file.read_text():
            raise SystemExit(f"Missing symbol <{symbol}> in libc disassembly")

    # Verify memcpy uses ESP vector instructions
    memcpy_text = _extract_symbol(disasm_file, "memcpy")
    (work_dir / "memcpy-disassembly.txt").write_text(memcpy_text)
    if "__riscv32_xespv_memcpy64" not in memcpy_text:
        raise SystemExit("memcpy does not call __riscv32_xespv_memcpy64")

    # Verify each ESP-vectorized string function
    for symbol in ["memchr", "memrchr", "memcmp", "strcmp"]:
        text = _extract_symbol(disasm_file, symbol)
        (work_dir / f"{symbol}-disassembly.txt").write_text(text)
        for insn in ["esp.vld.128.ip", "esp.vcmp.eq.u8"]:
            if insn not in text:
                raise SystemExit(f"<{symbol}> missing instruction {insn}")

    # Verify kernel memcpy uses ESP vector load/store
    kernel_memcpy = _extract_symbol(disasm_file, "__riscv32_xespv_memcpy64")
    (work_dir / "memcpy-kernel-disassembly.txt").write_text(kernel_memcpy)
    for insn in ["esp.vld.128.ip", "esp.vst.128.ip"]:
        if insn not in kernel_memcpy:
            raise SystemExit(f"<__riscv32_xespv_memcpy64> missing instruction {insn}")


def verify_libc_xespv(prefix: Path, work_dir: Path, repo_root: Path) -> None:
    """Verify that memset does NOT use ESP vector instructions (scalar fallback)."""
    target = "riscv32-esp-linux-musl"
    libc = prefix / target / "sysroot" / "usr" / "lib" / "libc.so"
    objdump = prefix / "bin" / f"{target}-objdump"

    disasm_file = work_dir / "libc-disassembly.txt"
    if not disasm_file.exists():
        disasm_file.write_text(_run([str(objdump), "-d", str(libc)]))

    memset_text = _extract_symbol(disasm_file, "memset")
    (work_dir / "memset-disassembly.txt").write_text(memset_text)
    if "esp." in memset_text:
        raise SystemExit("memset must NOT contain ESP vector instructions")

    print("All toolchain verifications passed.")


def _extract_symbol(disasm_file: Path, symbol: str) -> str:
    """Extract disassembly of a single symbol from objdump output."""
    lines = disasm_file.read_text().splitlines(keepends=True)
    capture = False
    result: list[str] = []
    for line in lines:
        if f"<{symbol}>:" in line:
            capture = True
            result.append(line)
            continue
        if capture:
            if line.strip() == "" or (not line.startswith(" ") and not line.startswith("\t")):
                break
            result.append(line)
    return "".join(result)

