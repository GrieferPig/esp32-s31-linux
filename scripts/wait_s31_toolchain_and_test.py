#!/usr/bin/env python3
"""Wait for an S31 toolchain workflow, publish it, and test it on hardware."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time


DEFAULT_REPOSITORY = "GrieferPig/crosstool-NG-s31"
DEFAULT_RELEASE_TAG = "esp32s31-linux-gcc-15.2.0-4"
TARGET = "riscv32-esp-linux-musl"


def run(
    command: list[str],
    *,
    cwd: Path,
    capture: bool = False,
    timeout: int | None = None,
) -> str:
    print("+", " ".join(command), flush=True)
    result = subprocess.run(
        command,
        cwd=cwd,
        check=True,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        timeout=timeout,
    )
    return result.stdout if capture else ""


def gh_json(args: list[str], *, cwd: Path) -> dict:
    return json.loads(run(["gh", *args], cwd=cwd, capture=True))


def wait_for_run(
    repository: str,
    run_id: int,
    expected_sha: str,
    poll_seconds: int,
    repo_root: Path,
) -> dict:
    while True:
        state = gh_json(
            [
                "run",
                "view",
                str(run_id),
                "-R",
                repository,
                "--json",
                "status,conclusion,headSha,url,updatedAt",
            ],
            cwd=repo_root,
        )
        actual_sha = state["headSha"]
        if not actual_sha.startswith(expected_sha):
            raise SystemExit(
                f"Workflow SHA mismatch: expected {expected_sha}, got {actual_sha}"
            )
        print(
            f"workflow {run_id}: status={state['status']} "
            f"conclusion={state['conclusion'] or '-'} {state['url']}",
            flush=True,
        )
        if state["status"] == "completed":
            if state["conclusion"] != "success":
                raise SystemExit(
                    f"Toolchain workflow failed with conclusion {state['conclusion']}"
                )
            return state
        time.sleep(poll_seconds)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download_artifact(
    repository: str,
    run_id: int,
    expected_sha: str,
    destination: Path,
    repo_root: Path,
) -> tuple[Path, Path]:
    artifact_name = f"{TARGET}-{expected_sha}"
    run(
        [
            "gh",
            "run",
            "download",
            str(run_id),
            "-R",
            repository,
            "--name",
            artifact_name,
            "--dir",
            str(destination),
        ],
        cwd=repo_root,
    )
    archive = destination / f"{TARGET}.tar.xz"
    checksum = destination / f"{TARGET}.tar.xz.sha256"
    if not archive.is_file() or not checksum.is_file():
        raise SystemExit(f"Downloaded artifact is incomplete in {destination}")
    expected_hash = checksum.read_text().split()[0].lower()
    actual_hash = sha256(archive)
    if not re.fullmatch(r"[0-9a-f]{64}", expected_hash):
        raise SystemExit(f"Invalid checksum file: {checksum}")
    if actual_hash != expected_hash:
        raise SystemExit(
            f"Toolchain checksum mismatch: expected {expected_hash}, got {actual_hash}"
        )
    print(f"artifact SHA256 verified: {actual_hash}", flush=True)
    return archive, checksum


def publish_release(
    repository: str,
    release_tag: str,
    expected_sha: str,
    archive: Path,
    checksum: Path,
    repo_root: Path,
) -> None:
    view = subprocess.run(
        ["gh", "release", "view", release_tag, "-R", repository],
        cwd=repo_root,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if view.returncode:
        run(
            [
                "gh",
                "release",
                "create",
                release_tag,
                "-R",
                repository,
                "--target",
                expected_sha,
                "--title",
                f"ESP32-S31 Linux toolchain {release_tag}",
                "--notes",
                (
                    "S31 Linux toolchain with XEspV musl string operations and "
                    "control-flow-safe Xesploop hardware-loop generation.\n\n"
                    f"Built from crosstool-NG commit `{expected_sha}`."
                ),
            ],
            cwd=repo_root,
        )
    run(
        [
            "gh",
            "release",
            "upload",
            release_tag,
            "-R",
            repository,
            "--clobber",
            str(archive),
            str(checksum),
        ],
        cwd=repo_root,
    )
    print(f"published release {repository}@{release_tag}", flush=True)


def install_toolchain(archive: Path, release_tag: str, repo_root: Path) -> Path:
    toolchains = repo_root / "toolchain"
    destination = toolchains / TARGET
    toolchains.mkdir(exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{TARGET}.", dir=toolchains))
    try:
        with tarfile.open(archive, "r:xz") as source:
            source.extractall(staging, filter="data")
        compiler = staging / "bin" / f"{TARGET}-gcc"
        if not compiler.is_file():
            raise SystemExit(f"Compiler missing from artifact: {compiler}")
        (staging / ".release").write_text(release_tag + "\n")
        backup: Path | None = None
        if destination.exists():
            stamp = time.strftime("%Y%m%d%H%M%S", time.gmtime())
            backup = toolchains / f"{TARGET}.previous.{stamp}"
            destination.rename(backup)
            print(f"previous toolchain retained at {backup}", flush=True)
        staging.rename(destination)
        print(f"installed toolchain at {destination}", flush=True)
        return destination
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def verify_compiler(toolchain: Path, work_dir: Path, repo_root: Path) -> None:
    compiler = toolchain / "bin" / f"{TARGET}-gcc"
    source = work_dir / "xesploop-control-flow-test.c"
    assembly = work_dir / "xesploop-control-flow-test.s"
    source.write_text(
        """
__attribute__((noinline)) int early_exit(const unsigned char *p)
{
  for (int i = 0; i < 32; i++)
    if (!p[i])
      return i;
  return -1;
}
__attribute__((noinline)) unsigned simple_sum(const unsigned char *p)
{
  unsigned sum = 0;
  for (int i = 0; i < 32; i++)
    sum += p[i];
  return sum;
}
__attribute__((noinline)) void *branch_end(void **items, int count, void **out)
{
  void *last = 0;
  for (int i = 0; i < count; i++) {
    void *item = *items++;
    if (item)
      last = item;
  }
  if (out)
    *out = 0;
  return last;
}
"""
    )
    run([str(compiler), "-O2", "-S", str(source), "-o", str(assembly)], cwd=repo_root)
    text = assembly.read_text()
    early = text.split("early_exit:", 1)[1].split(".size\tearly_exit", 1)[0]
    simple = text.split("simple_sum:", 1)[1].split(".size\tsimple_sum", 1)[0]
    branch = text.split("branch_end:", 1)[1].split(".size\tbranch_end", 1)[0]
    if "esp.lp.setup" in early:
        raise SystemExit("Unsafe HWLP generated for an early-return loop")
    if "esp.lp.setup" in branch:
        raise SystemExit("Unsafe HWLP generated for a loop that can skip its end")

    setup = re.search(r"esp[.]lp[.]setup[^,]*,[^,]*,\s*([^\s]+)", simple)
    if not setup:
        raise SystemExit("Safe straight-line loop did not use HWLP")
    lines = simple.splitlines()
    try:
        label_index = next(
            index for index, line in enumerate(lines) if line.strip() == setup[1] + ":"
        )
        end_opcode = next(
            line.strip().split()[0]
            for line in lines[label_index + 1 :]
            if line.strip() and not line.lstrip().startswith(".")
        )
    except (StopIteration, IndexError) as error:
        raise SystemExit("Could not resolve straight-line HWLP end") from error
    if re.match(r"^(?:b|j)", end_opcode) or end_opcode in {
        "ret",
        "call",
        "tail",
    }:
        raise SystemExit(f"Unsafe straight-line HWLP end opcode: {end_opcode}")
    if end_opcode == "nop":
        raise SystemExit("Straight-line HWLP still contains synthetic nop padding")
    print("compiler Xesploop control-flow checks passed", flush=True)


def verify_libc_xespv(toolchain: Path, work_dir: Path, repo_root: Path) -> None:
    objdump = toolchain / "bin" / f"{TARGET}-objdump"
    libc = toolchain / TARGET / "sysroot" / "usr" / "lib" / "libc.so"
    disassembly_path = work_dir / "libc-disassembly.txt"
    disassembly = run(
        [str(objdump), "-d", str(libc)],
        cwd=repo_root,
        capture=True,
        timeout=120,
    )
    disassembly_path.write_text(disassembly)

    def function_body(symbol: str) -> str:
        match = re.search(
            rf"^[0-9a-f]+ <{re.escape(symbol)}>:\n(.*?)(?=\n[0-9a-f]+ <|\Z)",
            disassembly,
            re.M | re.S,
        )
        if not match:
            raise SystemExit(f"Missing libc symbol: {symbol}")
        return match.group(1)

    memcpy = function_body("memcpy")
    if "__riscv32_xespv_memcpy64" not in memcpy:
        raise SystemExit("libc memcpy does not call its XEspV kernel")
    memcpy_kernel = function_body("__riscv32_xespv_memcpy64")
    for opcode in ("esp.vld.128.ip", "esp.vst.128.ip"):
        if opcode not in memcpy_kernel:
            raise SystemExit(f"libc memcpy XEspV kernel is missing {opcode}")
    for symbol in ("memchr", "memrchr", "memcmp", "strcmp"):
        body = function_body(symbol)
        for opcode in ("esp.vld.128.ip", "esp.vcmp.eq.u8"):
            if opcode not in body:
                raise SystemExit(f"libc {symbol} is missing {opcode}")
    if "esp." in function_body("memset"):
        raise SystemExit("libc memset unexpectedly contains an XEsp instruction")
    print("musl XEspV per-function checks passed", flush=True)


def scan_rootfs_hwloops(toolchain: Path, repo_root: Path) -> None:
    target = repo_root / "build" / "buildroot" / "target"
    objdump = toolchain / "bin" / f"{TARGET}-objdump"
    setup_re = re.compile(r"\besp[.]lp[.]setup\b.*?#\s*([0-9a-fA-F]+)")
    instruction_re = re.compile(r"^\s*([0-9a-fA-F]+):\s+([.a-zA-Z0-9_]+)")
    unsafe_re = re.compile(r"^(?:b|j)|^(?:ret|call|tail)$")
    elf_count = setup_count = 0
    unsafe: list[str] = []
    for path in target.rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        try:
            if path.read_bytes()[:4] != b"\x7fELF":
                continue
            disassembly = run(
                [str(objdump), "-d", "--no-show-raw-insn", str(path)],
                cwd=repo_root,
                capture=True,
                timeout=120,
            )
        except (OSError, subprocess.CalledProcessError):
            continue
        elf_count += 1
        instructions: dict[int, str] = {}
        for line in disassembly.splitlines():
            match = instruction_re.match(line)
            if match:
                instructions[int(match.group(1), 16)] = match.group(2)
        for match in setup_re.finditer(disassembly):
            setup_count += 1
            address = int(match.group(1), 16)
            opcode = instructions.get(address, "<missing>")
            if opcode in {"<missing>", "nop"} or unsafe_re.match(opcode):
                unsafe.append(f"{path.relative_to(target)}:{address:x}:{opcode}")
    print(
        f"rootfs HWLP scan: ELFs={elf_count} setups={setup_count} unsafe={len(unsafe)}",
        flush=True,
    )
    if unsafe:
        raise SystemExit("Unsafe HWLP ends:\n" + "\n".join(unsafe[:30]))


def serial_command(port, command: str, marker: str, timeout: int) -> str:
    started = time.monotonic()
    port.write((command + f"; rc=$?; echo {marker}:$rc\n").encode())
    deadline = time.monotonic() + timeout
    output = bytearray()
    pattern = re.compile(rb"(?:\r?\n)" + marker.encode() + rb":([0-9]+)")
    while time.monotonic() < deadline:
        chunk = port.read(4096)
        if chunk:
            chunk = chunk.replace(b"\x00", b"")
            output.extend(chunk)
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
            match = pattern.search(output)
            if match:
                if int(match.group(1)):
                    raise SystemExit(f"Board command failed: {command}")
                elapsed = time.monotonic() - started
                print(f"{marker} host elapsed: {elapsed:.3f} s", flush=True)
                return output.decode(errors="replace")
    raise SystemExit(f"Timed out waiting for board command: {command}")


def test_board(serial_device: str, baud: int) -> None:
    import serial

    # Configure modem-control lines before opening the port.  pyserial's
    # default open-then-configure sequence can pulse DTR/RTS and reset S31,
    # losing the beginning of the boot log.
    port = serial.Serial()
    port.port = serial_device
    port.baudrate = baud
    port.timeout = 0.25
    port.dtr = False
    port.rts = False
    port.open()
    try:
        deadline = time.monotonic() + 120
        output = bytearray()
        port.write(b"\n")
        while time.monotonic() < deadline:
            chunk = port.read(4096)
            if chunk:
                chunk = chunk.replace(b"\x00", b"")
                output.extend(chunk)
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
                if b"login:" in output:
                    port.write(b"root\n")
                    time.sleep(1)
                    break
                if re.search(rb"(?:^|\n)[^\n]*#\s*$", output):
                    break
        else:
            raise SystemExit("Board did not reach a login or root shell prompt")

        serial_command(port, "pidof dbus-daemon", "__S31_DBUS_BOOT", 20)
        serial_command(
            port,
            "i=0; while [ $i -lt 20 ]; do /etc/init.d/S30dbus-daemon restart "
            ">/dev/null || exit 1; pidof dbus-daemon >/dev/null || exit 1; "
            "i=$((i+1)); done",
            "__S31_DBUS_STRESS",
            120,
        )
        serial_command(port, "s31-libc-test", "__S31_LIBC", 120)
        core_started = time.monotonic()
        coremark = serial_command(
            port,
            "before=$(cut -d' ' -f1 /proc/uptime); coremark; core_rc=$?; "
            "after=$(cut -d' ' -f1 /proc/uptime); "
            "echo __S31_COREMARK_WALL:$before:$after; test $core_rc -eq 0",
            "__S31_COREMARK",
            300,
        )
        core_host_seconds = time.monotonic() - core_started
        if "CoreMark 1.0" not in coremark or "Correct operation validated" not in coremark:
            raise SystemExit("CoreMark output did not report a validated run")
        reported = re.search(r"Total time \(secs\):\s*([0-9.]+)", coremark)
        board_wall = re.search(
            r"__S31_COREMARK_WALL:([0-9.]+):([0-9.]+)", coremark
        )
        if reported and board_wall:
            benchmark_seconds = float(reported.group(1))
            board_process_seconds = float(board_wall.group(2)) - float(
                board_wall.group(1)
            )
            serial_overhead = core_host_seconds - board_process_seconds
            print(
                "CoreMark timing: "
                f"benchmark={benchmark_seconds:.3f}s "
                f"board-process={board_process_seconds:.3f}s "
                f"host-command={core_host_seconds:.3f}s "
                f"non-benchmark={board_process_seconds - benchmark_seconds:.3f}s "
                f"serial={serial_overhead:+.3f}s",
                flush=True,
            )
            tolerance = max(3.0, board_process_seconds * 0.10)
            if abs(serial_overhead) > tolerance:
                raise SystemExit(
                    "CoreMark board process time disagrees with host monotonic time"
                )
        dmesg = serial_command(
            port,
            "dmesg | tail -n 200",
            "__S31_DMESG",
            30,
        )
        if re.search(r"dbus-daemon.*(?:segfault|unhandled signal)", dmesg, re.I):
            raise SystemExit("dbus-daemon fault found in dmesg")
    finally:
        port.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-id", type=int, required=True)
    parser.add_argument("--sha", required=True, help="Expected workflow commit SHA")
    parser.add_argument("--repository", default=DEFAULT_REPOSITORY)
    parser.add_argument("--release-tag", default=DEFAULT_RELEASE_TAG)
    parser.add_argument("--poll-seconds", type=int, default=30)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--serial-device", default="/dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--no-release", action="store_true")
    parser.add_argument("--no-flash", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.poll_seconds < 5 or args.jobs < 1:
        raise SystemExit("poll-seconds must be >= 5 and jobs must be >= 1")
    repo_root = Path(__file__).resolve().parent.parent
    expected_sha = args.sha.lower()
    wait_for_run(
        args.repository,
        args.run_id,
        expected_sha,
        args.poll_seconds,
        repo_root,
    )
    with tempfile.TemporaryDirectory(prefix="s31-toolchain-artifact.") as temp:
        work_dir = Path(temp)
        archive, checksum = download_artifact(
            args.repository,
            args.run_id,
            expected_sha,
            work_dir,
            repo_root,
        )
        if not args.no_release:
            publish_release(
                args.repository,
                args.release_tag,
                expected_sha,
                archive,
                checksum,
                repo_root,
            )
        toolchain = install_toolchain(archive, args.release_tag, repo_root)
        verify_compiler(toolchain, work_dir, repo_root)
        verify_libc_xespv(toolchain, work_dir, repo_root)

    run(["make", "clean"], cwd=repo_root)
    run(["make", f"JOBS={args.jobs}", "linux"], cwd=repo_root)
    run(["make", f"JOBS={args.jobs}", "rootfs"], cwd=repo_root)
    run(["make", f"JOBS={args.jobs}", "coremark"], cwd=repo_root)
    scan_rootfs_hwloops(toolchain, repo_root)
    if not args.no_flash:
        if not Path(args.serial_device).exists():
            raise SystemExit(f"Serial device does not exist: {args.serial_device}")
        run(["make", "flash-linux"], cwd=repo_root)
        run(["make", "flash-rootfs"], cwd=repo_root)
        test_board(args.serial_device, args.baud)
    print("S31 toolchain, Linux, Buildroot, dbus, libc, and CoreMark tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
