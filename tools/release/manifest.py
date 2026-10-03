#!/usr/bin/env python3
"""Record source/configuration/compiler/artifact identity without host paths or logs."""
import argparse
import hashlib
import json
import os
import subprocess
import tempfile
import shutil
import lzma
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.checks.versions import versions
from tools.checks.layout import flash_layout

ROOT = Path(__file__).resolve().parents[2]
SCHEMA_VERSION = 3
SOURCE_CONFIGS = ("configs/build-versions.mk", "configs/esp32s31-layout.cfg",
                  "configs/kernel/common.config", "configs/kernel/board.config")

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
    source_roots = {"buildroot-external", "configs", "firmware", "rootfs", "shared", "tools", "mk", ".github", "arch", "drivers", "include", "kernel", "lib", "mm", "net", "sound", "platform", "package", "board", "support", "Documentation"}
    source_names = {"Makefile", "Kconfig", "Config.in", "sdkconfig.defaults", "sdkconfig.radio.defaults", "CMakeLists.txt"}
    source_suffixes = {".c", ".h", ".S", ".py", ".sh", ".mk", ".md", ".yml", ".yaml", ".json", ".txt", ".cfg", ".rsp", ".lds", ".dtso", ".dts", ".dtsi"}
    files = {}
    omitted = 0
    for raw in untracked:
        if not raw:
            continue
        name = raw.decode()
        entry = Path(name)
        if entry.parts[0] in {"build", "out", "cache", "logs", "toolchain"} or any(part in {"build-radio", "build-hil-gcc14", "build-hil-gcc15", "build-idf6", "__pycache__", ".cache"} for part in entry.parts[:-1]) or not (entry.parts[0] in source_roots or entry.name in source_names or entry.suffix in source_suffixes):
            omitted += 1
            continue
        candidate = path / entry
        if candidate.is_symlink():
            files[name] = hashlib.sha256(candidate.readlink().as_posix().encode()).hexdigest()
        elif candidate.is_file():
            files[name] = digest(candidate)
    return {"commit": git("rev-parse", "HEAD").decode().strip(), "dirty": bool(patch or any(untracked)), "tracked_diff_sha256": hashlib.sha256(patch).hexdigest(), "untracked_source_files": files, "other_untracked_entries": omitted}

ARTIFACTS = ("spl_app.bin", "u-boot.itb", "esp32s31_generic.dtb", "xipImage",
             "rootfs.sqfs", "radio.bin", "radio.json", "s31_full_flash.bin")
NATIVE_IMAGES = {
    "xipImage": "linux/arch/riscv/boot/xipImage",
    "esp32s31_generic.dtb": "linux/arch/riscv/boot/dts/espressif/esp32s31_generic.dtb",
}
BINDINGS = {
    "kernel_vmlinux_sha256": "linux/vmlinux",
    "kernel_module_sha256": "linux/drivers/platform/esp32s31-radio.ko",
    "radio_payload_sha256": "radio/linux_radio.localized.o",
}


def atomic_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, prefix="." + path.name,
                                     delete=False) as stream:
        temporary = Path(stream.name)
        try:
            json.dump(data, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        except BaseException:
            temporary.unlink(missing_ok=True)
            raise
    try:
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def checked_digest(path):
    if not path.is_file() or not path.stat().st_size:
        raise ValueError(f"required artifact missing or empty: {path}")
    return digest(path)


def verify_radio_binding(output_root, images_dir=None, *, paths=None, imports=None):
    """Reject stale radio images before recording a new matched artifact set."""
    images = images_dir or output_root / "images"
    report = json.loads((images / "radio.json").read_text())
    if report.get("sha256") != checked_digest(images / "radio.bin"):
        raise ValueError("radio.bin does not match its build-time radio.json")
    binding = report.get("binding", {})
    if not binding:
        raise ValueError("radio.json has no kernel/module binding; rebuild the matched image set")
    resolved = {key: output_root / name for key, name in BINDINGS.items()}
    resolved.update(paths or {})
    resolved["radio_imports_sha256"] = imports or output_root / "radio/linux-radio-linked-imports.txt"
    for key, path in resolved.items():
        if binding.get(key) != checked_digest(path):
            raise ValueError(f"radio binding mismatch: {key}; rebuild the matched kernel/module/radio set")
    return binding


def verify_native_images(output_root, artifacts):
    for name, native in NATIVE_IMAGES.items():
        if artifacts.get(name) != checked_digest(output_root / native):
            raise ValueError(f"published {name} differs from current native kernel output")


def verify_radio_package(staging, output_root):
    """Validate copied package bytes before an archive can replace the old one."""
    source_manifest = output_root / "images/build-manifest.json"
    data = verify_manifest(source_manifest, output_root)
    if checked_digest(staging / "build-manifest.json") != checked_digest(source_manifest):
        raise ValueError("staged package manifest changed during copying")
    for name in ("radio.bin", "radio.json"):
        if checked_digest(staging / "firmware" / name) != data["artifacts"][name]:
            raise ValueError(f"staged package firmware mismatch: {name}")
    try:
        module = lzma.decompress((staging / "module/esp32s31-radio.ko.xz").read_bytes())
    except lzma.LZMAError as error:
        raise ValueError("staged package module is not valid XZ") from error
    if hashlib.sha256(module).hexdigest() != data["radio_binding"]["kernel_module_sha256"]:
        raise ValueError("staged package module differs from paired kernel module")
    copies = {"config/sdkconfig.defaults": ROOT / "firmware/radio/idf_deps/sdkconfig.defaults",
              "config/sdkconfig.radio.defaults": ROOT / "firmware/radio/idf_deps/sdkconfig.radio.defaults",
              "RADIO_BUNDLE_LICENSES.md": ROOT / "firmware/radio/RADIO_BUNDLE_LICENSES.md"}
    for overlay in ("radio-wifi", "radio-bluetooth", "radio-combo"):
        name = "esp32s31-overlay-" + overlay + ".dtbo"
        copies["overlays/" + name] = output_root / "linux/arch/riscv/boot/dts/espressif" / name
    for name, source in copies.items():
        if checked_digest(staging / name) != checked_digest(source):
            raise ValueError(f"staged package copy mismatch: {name}")


def verify_flash_contents(images):
    slots, capacities = flash_layout()
    files = {"SPL": "spl_app.bin", "UBOOT_ITB": "u-boot.itb", "DTB": "esp32s31_generic.dtb",
             "RADIO": "radio.bin", "KERNEL": "xipImage", "ROOTFS": "rootfs.sqfs"}
    with (images / "s31_full_flash.bin").open("rb") as full:
        for slot, name in files.items():
            data = (images / name).read_bytes()
            if not 0 < len(data) <= capacities[slot]:
                raise ValueError(f"{slot} image exceeds capacity or is empty")
            full.seek(slots["SLOT_" + slot])
            if full.read(len(data)) != data:
                raise ValueError(f"combined flash image does not contain matched {name}")


def packed_module_digest(image):
    tool = shutil.which("unsquashfs")
    if tool is None:
        raise ValueError("unsquashfs is required to verify the module actually packed in rootfs")
    result = subprocess.run([tool, "-cat", str(image), "usr/lib/s31-radio/esp32s31-radio.ko.xz"],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode or not result.stdout:
        raise ValueError("cannot extract the paired radio module from rootfs: " +
                         result.stderr.decode(errors="replace").strip())
    return hashlib.sha256(result.stdout).hexdigest()


def rootfs_provenance(output_root, artifacts, binding):
    path = output_root / "reports/rootfs-provenance.json"
    if not path.exists():
        raise ValueError("rootfs provenance missing; build or explicitly repack the matched rootfs")
    record = json.loads(path.read_text())
    if record.get("mode") not in {"incremental-baseline", "native-buildroot"}:
        raise ValueError("rootfs provenance has an unsupported build mode")
    if record.get("rootfs_sha256") != artifacts["rootfs.sqfs"]:
        raise ValueError("rootfs provenance does not match current rootfs image")
    if record.get("module_sha256") != binding["kernel_module_sha256"]:
        raise ValueError("rootfs provenance does not match current paired module")
    if record.get("packaged_module_sha256") != packed_module_digest(output_root / "images/rootfs.sqfs"):
        raise ValueError("rootfs packed module does not match provenance")
    return {"sha256": digest(path), "details": record}


def verify_manifest(path, output_root, *, check_inputs=True):
    data = json.loads(path.read_text())
    if data.get("schema_version") != SCHEMA_VERSION or "profile" in data:
        raise ValueError("unsupported or legacy manifest; rebuild the full image set")
    source_configs = data.get("source_configs", {})
    if not set(SOURCE_CONFIGS).issubset(source_configs):
        raise ValueError("manifest is missing canonical source configuration bindings")
    for name in source_configs:
        entry = Path(name)
        if entry.is_absolute() or ".." in entry.parts:
            raise ValueError("unsafe configuration path in manifest")
        if entry.parts[:2] == ("configs", "profiles"):
            raise ValueError("unsupported or legacy manifest; rebuild the full image set")
    images = path.parent
    for name in ARTIFACTS:
        if data.get("artifacts", {}).get(name) != checked_digest(images / name):
            raise ValueError(f"manifest artifact mismatch: {name}")
    verify_flash_contents(images)
    report = json.loads((images / "radio.json").read_text())
    if report.get("sha256") != data["artifacts"]["radio.bin"] or report.get("binding") != data.get("radio_binding"):
        raise ValueError("manifest and radio build-time binding differ")
    provenance = data.get("build_provenance", {}).get("rootfs", {}).get("details", {})
    if (provenance.get("mode") not in {"incremental-baseline", "native-buildroot"} or
            provenance.get("rootfs_sha256") != data["artifacts"]["rootfs.sqfs"] or
            provenance.get("module_sha256") != data["radio_binding"]["kernel_module_sha256"]):
        raise ValueError("manifest rootfs provenance is missing or does not bind the paired module")
    if provenance.get("packaged_module_sha256") != packed_module_digest(images / "rootfs.sqfs"):
        raise ValueError("manifest rootfs packed module does not match provenance")
    if check_inputs:
        verify_native_images(output_root, data["artifacts"])
        # Paths are bounded relative names, never arbitrary manifest-supplied host paths.
        for key, name in BINDINGS.items():
            if data["radio_binding"].get(key) != checked_digest(output_root / name):
                raise ValueError(f"manifest paired input mismatch: {key}")
        imports = output_root / "radio/linux-radio-linked-imports.txt"
        if data["radio_binding"].get("radio_imports_sha256") != checked_digest(imports):
            raise ValueError("manifest radio import contract changed")
        for name, expected in data.get("generated_configs", {}).items():
            entry = Path(name)
            if entry.is_absolute() or ".." in entry.parts:
                raise ValueError("unsafe generated configuration path in manifest")
            if checked_digest(output_root / entry) != expected:
                raise ValueError(f"manifest generated configuration changed: {name}")
        provenance = rootfs_provenance(output_root, data["artifacts"], data["radio_binding"])
        if provenance != data.get("build_provenance", {}).get("rootfs"):
            raise ValueError("manifest rootfs provenance changed")
        for name, expected in data.get("source_configs", {}).items():
            entry = Path(name)
            if entry.is_absolute() or ".." in entry.parts:
                raise ValueError("unsafe configuration path in manifest")
            if checked_digest(ROOT / entry) != expected:
                raise ValueError(f"manifest configuration changed: {name}")
    return data


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--compiler", type=Path)
    p.add_argument("--idf", type=Path)
    p.add_argument("--output-root", type=Path)
    p.add_argument("--output", type=Path)
    p.add_argument("--config", action="append", default=[], type=Path,
                   help="additional source configuration to hash (repository-relative)")
    p.add_argument("--verify-radio-package", type=Path, help="verify a staged radio package before archiving")
    p.add_argument("--verify", type=Path, help="verify an existing manifest without rebuilding")
    p.add_argument("--artifacts-only", action="store_true", help="verify a distributed image set without build intermediates")
    a = p.parse_args()
    root = (a.output_root or ROOT / "out").resolve()
    try:
        if a.verify_radio_package:
            verify_radio_package(a.verify_radio_package, root)
            print("Staged radio package verified")
            return
        if a.verify:
            verify_manifest(a.verify, root, check_inputs=not a.artifacts_only)
            print("Matched artifact manifest verified")
            return
        if a.compiler is None or a.idf is None:
            p.error("creating a manifest requires --compiler and --idf")
        output = a.output or root / "images/build-manifest.json"
        repos = [".", "linux-esp32-s31", "opensbi-esp32-s31", "u-boot-esp32-s31", "buildroot", "docs"]
        configs = [*SOURCE_CONFIGS, *(str(x) for x in a.config)]
        resolved_path = root / "reports/resolved-config.json"
        resolved = json.loads(resolved_path.read_text())
        if "profile" in resolved:
            raise ValueError("legacy resolved configuration; rebuild the full image set")
        for fragment in resolved.get("kernel_fragments", []):
            entry = Path(fragment).resolve()
            try:
                configs.append(str(entry.relative_to(ROOT)))
            except ValueError:
                p.error("kernel fragment must be repository-relative for release provenance")
        source_configs = {}
        for name in configs:
            entry = Path(name)
            if entry.is_absolute() or ".." in entry.parts:
                p.error("--config must be repository-relative")
            if entry.parts[:2] == ("configs", "profiles"):
                p.error("legacy source configuration is unsupported")
            source_configs[name] = checked_digest(ROOT / entry)
        generated = ("linux/.config", "buildroot/.config", "idf-radio/sdkconfig",
                     "generated/config.json", "generated/build-config.json", "generated/config.mk",
                     "generated/radio-kernel-symbols.txt",
                     "reports/resolved-config.json", "reports/linux-inputs.json", "reports/buildroot-inputs.json",
                     "reports/uboot-inputs.json", "reports/linux.config", "reports/buildroot.config")
        binding = verify_radio_binding(root, output.parent)
        data = {
            "schema_version": SCHEMA_VERSION,
            "expected_dependencies": versions(),
            "sources": {name: revision(ROOT / name) for name in repos},
            "idf": revision(a.idf),
            "compiler": {"version": subprocess.check_output([str(a.compiler), "--version"], text=True).splitlines()[0],
                         "sha256": digest(a.compiler),
                         "installed_release": (a.compiler.parent.parent / ".release").read_text().strip()
                         if (a.compiler.parent.parent / ".release").exists() else None},
            "source_configs": source_configs,
            "generated_configs": {name: digest(root / name) for name in generated if (root / name).is_file()},
            "radio_binding": binding,
            "artifacts": {name: checked_digest(output.parent / name) for name in ARTIFACTS},
        }
        verify_native_images(root, data["artifacts"])
        verify_flash_contents(output.parent)
        provenance = rootfs_provenance(root, data["artifacts"], binding)
        if provenance:
            data["build_provenance"] = {"rootfs": provenance}
        atomic_json(output, data)
        print("Build manifest written: " + str(output))
    except (ValueError, OSError, KeyError, json.JSONDecodeError) as error:
        p.exit(1, "manifest: " + str(error) + "\n")


if __name__ == "__main__":
    main()
