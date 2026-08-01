#!/usr/bin/env python3
"""Build and install the ESP32-S31 Linux toolchain from ../crosstool-NG.

This mirrors crosstool-NG's build-s31-linux-toolchain GitHub workflow, but
keeps both the ct-ng build workspace and the installed toolchain in this
checkout.  It deliberately does not use the prebuilt-toolchain release.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys


TARGET = "riscv32-esp-linux-musl"
MUSL_VERSION = "1.2.5"
REQUIRED_MUSL_PATCH = "0002-riscv32-add-xespv-string-operations.patch"


def run(command: list[str], *, cwd: Path) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def set_config_value(config: str, key: str, value: str) -> str:
    """Replace a Kconfig assignment, including an explicitly disabled one."""
    assignment = f'{key}="{value}"'
    lines = config.splitlines()
    for index, line in enumerate(lines):
        if line.startswith(f"{key}=") or line == f"# {key} is not set":
            lines[index] = assignment
            break
    else:
        lines.append(assignment)
    return "\n".join(lines) + "\n"


def remove_partial_downloads(repo_root: Path) -> None:
    # ct-ng itself stores source archives in build/downloads, while its
    # binutils wrapper bootstrap writes Rust downloads below the ct-ng work
    # directory.  Neither is usable once interrupted; let their respective
    # downloaders fetch a complete copy on the next run.
    for downloads in (
        repo_root / "build" / "downloads",
        repo_root / "build" / "crosstool-ng" / ".build",
    ):
        if not downloads.exists():
            continue
        for path in downloads.rglob("*"):
            if path.is_file() and (
                path.name.endswith(".part")
                or path.name.endswith(".sha256.part")
                or path.name.endswith(".partial")
            ):
                print(f"Removing incomplete download: {path.relative_to(repo_root)}")
                path.unlink()


def make_tree_writable(path: Path) -> None:
    """Allow removal of a release toolchain, which is installed read-only."""
    for directory, _, files in os.walk(path):
        directory_path = Path(directory)
        directory_path.chmod(directory_path.stat().st_mode | stat.S_IWUSR | stat.S_IXUSR)
        for name in files:
            file_path = directory_path / name
            file_path.chmod(file_path.stat().st_mode | stat.S_IWUSR)


def patch_set_hash(patch_dir: Path) -> str:
    """Hash the ordered bundled patch set so stale source caches are rejected."""
    digest = hashlib.sha256()
    for patch in sorted(patch_dir.glob("*.patch")):
        digest.update(patch.name.encode())
        digest.update(b"\0")
        digest.update(patch.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def invalidate_musl_cache(work_dir: Path) -> None:
    ctng_src = work_dir / ".build" / "src"
    for path in (
        ctng_src / f"musl-{MUSL_VERSION}",
        ctng_src / f".musl-{MUSL_VERSION}.extracted",
        ctng_src / f".musl-{MUSL_VERSION}.patched",
    ):
        if path.is_dir():
            print(f"Invalidating cached musl source: {path}")
            make_tree_writable(path)
            shutil.rmtree(path)
        elif path.exists():
            print(f"Invalidating cached musl marker: {path}")
            path.unlink()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--ct-ng-dir",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "crosstool-NG",
        help="crosstool-NG checkout (default: ../crosstool-NG)",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=os.cpu_count() or 1,
        help="parallel build jobs (default: available CPUs)",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="replace an existing installed toolchain",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.jobs < 1:
        raise SystemExit("--jobs must be at least 1")

    repo_root = Path(__file__).resolve().parent
    ctng_dir = args.ct_ng_dir.resolve()
    config_source = repo_root / "configs" / f"{TARGET}.config"
    kernel_dir = repo_root / "linux-esp32-s31"
    wrappers_dir = ctng_dir / "esp-toolchain-bin-wrappers" / "gnu-riscv-binutils"
    musl_patch_dir = ctng_dir / "packages" / "musl" / MUSL_VERSION
    required_musl_patch = musl_patch_dir / REQUIRED_MUSL_PATCH
    prefix = repo_root / "toolchain" / TARGET
    work_dir = repo_root / "build" / "crosstool-ng"
    sources_dir = repo_root / "build" / "toolchain-src"

    for path, description in (
        (ctng_dir / "bootstrap", "crosstool-NG checkout"),
        (config_source, "S31 crosstool-NG config"),
        (kernel_dir, "S31 kernel source"),
        (wrappers_dir / "Cargo.toml", "Espressif RISC-V binutils wrappers"),
        (required_musl_patch, "bundled S31 musl patch"),
    ):
        if not path.exists():
            raise SystemExit(f"Missing {description}: {path}")

    remove_partial_downloads(repo_root)
    if prefix.exists():
        if not args.force:
            raise SystemExit(
                f"Toolchain already exists at {prefix}. Use --force to replace it."
            )
        print(f"Removing existing toolchain: {prefix}")
        make_tree_writable(prefix)
        shutil.rmtree(prefix)

    work_dir.mkdir(parents=True, exist_ok=True)
    sources_dir.mkdir(parents=True, exist_ok=True)

    # crosstool-NG caches patched source trees. Tie that cache to the complete
    # ordered bundled patch set, not merely to the musl version number.
    patch_hash_file = work_dir / ".musl-patches.sha256"
    current_patch_hash = patch_set_hash(musl_patch_dir)
    cached_patch_hash = (
        patch_hash_file.read_text().strip() if patch_hash_file.exists() else ""
    )
    if args.force or cached_patch_hash != current_patch_hash:
        invalidate_musl_cache(work_dir)

    # This is the same local ct-ng bootstrap used by the S31 GitHub workflow.
    run(["./bootstrap"], cwd=ctng_dir)
    run(["./configure", "--enable-local"], cwd=ctng_dir)
    run(["make", f"-j{args.jobs}"], cwd=ctng_dir)

    config = config_source.read_text()
    for setting in (
        "CT_PATCH_BUNDLED=y",
        'CT_PATCH_ORDER="bundled"',
        "CT_LIBC_MUSL=y",
    ):
        if setting not in config.splitlines():
            raise SystemExit(f"Missing required toolchain setting: {setting}")
    for key, value in (
        ("CT_PREFIX_DIR", str(prefix)),
        ("CT_LINUX_CUSTOM_LOCATION", str(kernel_dir)),
        ("CT_BINUTILS_ESP32P4_BIN_WRAPPERS_LOCATION", str(wrappers_dir)),
        ("CT_LOCAL_TARBALLS_DIR", str(sources_dir)),
    ):
        config = set_config_value(config, key, value)
    generated_config = work_dir / "s31-linux.config"
    generated_config.write_text(config)

    ctng = ctng_dir / "ct-ng"
    if not ctng.is_file():
        raise SystemExit(f"ct-ng was not built at {ctng}")
    run([str(ctng), "defconfig", f"DEFCONFIG={generated_config}"], cwd=work_dir)
    resolved_config = (work_dir / ".config").read_text().splitlines()
    for setting in (
        "CT_PATCH_BUNDLED=y",
        'CT_PATCH_ORDER="bundled"',
        f'CT_MUSL_VERSION="{MUSL_VERSION}"',
    ):
        if setting not in resolved_config:
            raise SystemExit(f"Resolved toolchain config is missing: {setting}")
    run([str(ctng), f"build.{args.jobs}"], cwd=work_dir)

    gcc = prefix / "bin" / f"{TARGET}-gcc"
    if not gcc.is_file():
        raise SystemExit(f"Toolchain build completed without compiler: {gcc}")
    run([str(gcc), "--version"], cwd=work_dir)
    patch_hash_file.write_text(current_patch_hash + "\n")

    marker = prefix / ".source-build"
    prefix.chmod(prefix.stat().st_mode | stat.S_IWUSR | stat.S_IXUSR)
    marker.write_text(
        "Built locally by build_linux_toolchain.py\n"
        f"config_sha256={hashlib.sha256(generated_config.read_bytes()).hexdigest()}\n"
        f"ct_ng_dir={ctng_dir}\n"
    )
    print(f"Installed {TARGET} toolchain at {prefix}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode) from error
