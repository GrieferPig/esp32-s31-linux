#!/usr/bin/env python3
"""Record source/configuration/compiler/artifact identity without host paths or logs."""
import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path
from check_build_versions import versions

ROOT = Path(__file__).resolve().parents[1]

def digest(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()

def revision(path):
    command = ["git", "-C", str(path)]
    # Desktop-managed Windows checkouts can reference WSL metadata by UNC.
    # Git for Linux needs that same metadata expressed in its native namespace.
    gitfile = path / ".git"
    if os.name != "nt" and gitfile.is_file():
        target = gitfile.read_text().strip().removeprefix("gitdir: ")
        parts = target.lstrip("/").split("/", 2)
        if (len(parts) == 3 and parts[0] in {"wsl.localhost", "wsl$"}
                and parts[1] == os.environ.get("WSL_DISTRO_NAME")):
            command = ["git", "--git-dir=/" + parts[2], "--work-tree=" + str(path)]
    def git(*args):
        return subprocess.check_output([*command, *args])
    patch = git("diff", "HEAD", "--binary")
    untracked = git("ls-files", "--others", "--exclude-standard", "-z").split(b"\0")
    # Do not walk/hash unrelated output trees as if they were build inputs.
    source_roots = {"buildroot-external", "configs", "firmware", "rootfs", "shared", "tools", ".github", "arch", "drivers", "include", "kernel", "lib", "mm", "net", "sound", "platform", "package", "board", "support", "Documentation"}
    source_names = {"Makefile", "Kconfig", "Config.in", "sdkconfig.defaults", "sdkconfig.radio.defaults", "CMakeLists.txt"}
    source_suffixes = {".c", ".h", ".S", ".py", ".sh", ".mk", ".md", ".yml", ".yaml", ".json", ".txt", ".cfg", ".rsp", ".lds", ".dtso", ".dts", ".dtsi"}
    files = {}
    omitted = 0
    for raw in untracked:
        if not raw:
            continue
        name = raw.decode()
        entry = Path(name)
        if any(part in {"build", "build-radio", "build-hil-gcc14", "build-hil-gcc15", "build-idf6", "logs", "toolchain", "__pycache__", ".cache"} for part in entry.parts[:-1]) or not (entry.parts[0] in source_roots or entry.name in source_names or entry.suffix in source_suffixes):
            omitted += 1
            continue
        candidate = path / entry
        if candidate.is_symlink():
            files[name] = hashlib.sha256(candidate.readlink().as_posix().encode()).hexdigest()
        elif candidate.is_file():
            files[name] = digest(candidate)
    return {"commit": git("rev-parse", "HEAD").decode().strip(), "dirty": bool(patch or any(untracked)), "tracked_diff_sha256": hashlib.sha256(patch).hexdigest(), "untracked_source_files": files, "other_untracked_entries": omitted}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--compiler", type=Path, required=True)
    p.add_argument("--idf", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--kernel-config", type=Path, default=ROOT / "build/linux-6.18/.config")
    a = p.parse_args()
    repos = [".", "linux-esp32-s31", "opensbi-esp32-s31", "u-boot-esp32-s31", "buildroot", "docs"]
    configs = ["configs/build-versions.mk", "configs/esp32s31-layout.cfg", str(a.kernel_config.relative_to(ROOT) if a.kernel_config.is_absolute() and a.kernel_config.is_relative_to(ROOT) else a.kernel_config), "build/buildroot/.config", "firmware/radio/idf_deps/build-radio/sdkconfig", "firmware/radio/.s31-build-config"]
    artifacts = ["spl_app.bin", "u-boot.itb", "esp32s31_generic.dtb", "xipImage", "rootfs.sqfs", "radio.bin", "radio.json", "esp32s31-radio-fw-v1.o", "s31_full_flash.bin"]
    data = {"schema_version": 1, "expected_dependencies": versions(), "sources": {name: revision(ROOT / name) for name in repos}, "idf": revision(a.idf), "compiler": {"version": subprocess.check_output([str(a.compiler), "--version"], text=True).splitlines()[0], "sha256": digest(a.compiler), "installed_release": (a.compiler.parent.parent / ".release").read_text().strip() if (a.compiler.parent.parent / ".release").exists() else None}, "configs": {name: digest(ROOT / name) for name in configs if (ROOT / name).is_file()}, "artifacts": {name: digest(ROOT / "build" / name) for name in artifacts if (ROOT / "build" / name).is_file()}}
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
    print("Build manifest written")

if __name__ == "__main__":
    main()
