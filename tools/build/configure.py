#!/usr/bin/env python3
"""Small native configuration boundaries, not a build scheduler."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from toolchain_identity import identify as toolchain_identity


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run(argv, **kw):
    print('+', ' '.join(map(str, argv)), flush=True)
    subprocess.run(list(map(str, argv)), check=True, **kw)


def save(path, value):
    text = json.dumps(value, sort_keys=True, indent=2) + '\n'
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != text:
        temp = path.with_suffix(path.suffix + '.tmp')
        temp.write_text(text)
        temp.replace(path)


def identity(files, values):
    return {'files': {str(Path(p).resolve()): digest(p) for p in files}, 'values': values}


def same(path, obj):
    return path.exists() and json.loads(path.read_text()) == obj


def parse_config(path):
    result = {}
    for line in path.read_text().splitlines():
        if line.startswith('CONFIG_') or line.startswith('BR2_'):
            key, value = line.split('=', 1)
            result[key] = value
        elif line.startswith('# CONFIG_') and line.endswith(' is not set'):
            result[line[2:-11]] = 'n'
    return result


def contracts(a):
    config = parse_config(a.output / '.config')
    required = {'CONFIG_XIP_KERNEL': 'y', 'CONFIG_ESP32S31_RADIO_XIP': 'y',
                'CONFIG_MODULES': 'y', 'CONFIG_MTD_PARTITIONED_MASTER': 'y',
                'CONFIG_TRIM_UNUSED_KSYMS': 'y'}
    # Every build retains the full board peripheral contract.
    required.update({f'CONFIG_{x}': 'y' for x in ['MMC', 'MMC_BLOCK', 'MMC_DW', 'MMC_DW_PLTFM', 'I2C', 'SPI', 'CAN']})
    bad = [f'{key}: expected {value}, resolved {config.get(key, "n")}' for key, value in required.items() if config.get(key, 'n') != value]
    if bad:
        raise SystemExit('Resolved feature contract failed:\n' + '\n'.join(bad))


def native(a):
    a.output.mkdir(parents=True, exist_ok=True)
    config_dir = 'arch/riscv/configs' if a.kind == 'linux' else 'configs'
    files = [a.source / config_dir / a.defconfig, *a.fragment, Path(a.compiler)]
    obj = identity(files, [a.kind, a.cmdline, a.cross])
    obj['toolchain'] = toolchain_identity(a.compiler)
    stamp = a.output / '.s31-config.json'
    make = ['make', '-C', a.source, f'O={a.output}', 'ARCH=riscv', f'CROSS_COMPILE={a.cross}']
    if not same(stamp, obj) or not (a.output / '.config').exists():
        if stamp.exists():
            previous = json.loads(stamp.read_text())
            compiler_key = str(Path(a.compiler).resolve())
            if previous.get('toolchain') != obj['toolchain'] or previous.get('values', [])[-1:] != obj['values'][-1:]:
                run(make + ['clean'])
        run(make + [a.defconfig])
        if a.kind == 'linux' or a.fragment:
            merged = parse_config(a.output / '.config')
            for fragment in a.fragment:
                merged.update(parse_config(fragment))
            if a.cmdline:
                merged['CONFIG_CMDLINE'] = json.dumps(a.cmdline)
            (a.output / '.config').write_text(''.join(f'# {k} is not set\n' if v == 'n' else f'{k}={v}\n' for k, v in merged.items()))
            run(make + ['olddefconfig'])
            if a.kind == 'linux':
                contracts(a)
        save(stamp, obj)
    if a.kind == 'linux':
        contracts(a)
    save(a.output.parent / 'reports' / f'{a.kind}-inputs.json', obj)


def idf(a):
    defaults = [a.source / 'sdkconfig.defaults', a.source / 'sdkconfig.radio.defaults']
    idf = Path(os.environ['IDF_PATH'])
    files = defaults + [a.source / 'CMakeLists.txt', a.link_list, idf / 'tools/cmake/version.cmake']
    compiler = shutil.which('riscv32-esp-elf-gcc')
    if not compiler:
        raise SystemExit('ESP-IDF compiler unavailable after export.sh')
    files.append(Path(compiler))
    obj = identity(files, [str(idf), subprocess.check_output(['git', '-C', idf, 'rev-parse', 'HEAD'], text=True).strip()])
    obj['toolchain'] = toolchain_identity(compiler)
    a.output.mkdir(parents=True, exist_ok=True)
    stamp = a.output / '.s31-config.json'
    if not same(stamp, obj) or not (a.output / 'build.ninja').exists():
        if stamp.exists() and (a.output / 'build.ninja').exists():
            old = json.loads(stamp.read_text())
            key = str(Path(compiler).resolve())
            if old.get('toolchain') != obj['toolchain']:
                run(['ninja', '-C', a.output, 'clean'])
        # Reset only when defaults/toolchain really change; no stale choices survive.
        (a.output / 'sdkconfig').unlink(missing_ok=True)
        run(['idf.py', '-C', a.source, '-B', a.output, '-D', f'SDKCONFIG={a.output}/sdkconfig', '-D', 'SDKCONFIG_DEFAULTS=' + ';'.join(map(str, defaults)), 'reconfigure'])
        save(stamp, obj)
    available = subprocess.check_output(['ninja', '-C', a.output, '-t', 'targets', 'all'], text=True)
    targets = {line.split(':', 1)[0] for line in available.splitlines()}
    requested = {line.strip() for line in a.link_list.read_text().splitlines() if line.startswith('esp-idf/') and line.endswith('.a')}
    closure = sorted(targets & requested)
    if not closure:
        raise SystemExit('No radio IDF archive targets resolved')
    run(['ninja', '-C', a.output, '-j', a.jobs, *closure])


def buildroot(a):
    files = [a.source_config, Path(a.compiler), *a.fragment]
    obj = identity(files, [a.toolchain, a.overlay, a.static_overlay, str(a.external)])
    obj['toolchain'] = toolchain_identity(a.compiler)
    stamp = a.output / '.s31-config.json'
    resolved = a.output / '.s31-resolved-config.sha256'
    if resolved.exists() and (a.output / '.config').exists() and resolved.read_text().strip() != digest(a.output / '.config'):
        raise SystemExit('Buildroot resolved configuration was edited; run make buildroot-reconfigure to discard stale target contents.')
    if not same(stamp, obj) and (a.output / '.config').exists():
        raise SystemExit('Buildroot package selection/toolchain identity changed. Run make buildroot-reconfigure; cached downloads are retained. Refusing stale target contents.')
    a.output.mkdir(parents=True, exist_ok=True)
    if not same(stamp, obj) or not (a.output / '.config').exists():
        lines = a.source_config.read_text().splitlines()
        lines = [line for line in lines if not line.startswith(('BR2_TOOLCHAIN_EXTERNAL_PATH=', 'BR2_ROOTFS_OVERLAY='))]
        lines += [f'BR2_TOOLCHAIN_EXTERNAL_PATH="{a.toolchain}"', f'BR2_ROOTFS_OVERLAY="{a.static_overlay} {a.overlay}"']
        for fragment in a.fragment:
            lines += fragment.read_text().splitlines()
        cfg = a.output.parent / 'generated' / 'buildroot_defconfig'
        cfg.parent.mkdir(parents=True, exist_ok=True)
        cfg.write_text('\n'.join(lines) + '\n')
        run(['make', '-C', a.source, f'O={a.output}', f'BR2_EXTERNAL={a.external}', f'BR2_DEFCONFIG={cfg}', 'defconfig'])
        save(stamp, obj)
    resolved.write_text(digest(a.output / '.config') + '\n')
    save(a.output.parent / 'reports' / 'buildroot-inputs.json', obj)


def package_inputs(a):
    # Buildroot local packages do not resync source automatically. A content stamp
    # selects only changed packages; failed builds leave the old stamp untouched.
    files = sorted(p for root in a.input for p in ([root] if root.is_file() else root.rglob('*')) if p.is_file() and '__pycache__' not in p.parts)
    obj = identity(files, a.value)
    if not same(a.stamp, obj):
        if (a.output / '.config').exists() and any(p.is_dir() for p in (a.output / 'build').glob(a.package + '-*')):
            make = ['make', '-C', a.source, f'O={a.output}', f'BR2_EXTERNAL={a.external}', 'BR2_WGET=false', 'BR2_CURL=false', 'BR2_GIT=false', 'BR2_SVN=false', 'BR2_HG=false', f'BR2_DL_DIR={a.downloads}', f'BR2_JLEVEL={a.jobs}']
            if a.package == 'btstack-s31':
                run(make + [a.package + '-dirclean'])
                run(make + [a.package])
            else:
                run(make + [a.package + '-rebuild'])
        save(a.stamp, obj)


def doctor(a):
    missing = []
    for name in ['make', 'python3', 'git', 'ninja', 'cmake', 'dtc', 'flock', 'bison', 'flex', 'swig', 'rsync', 'xz', 'mksquashfs', 'unsquashfs']:
        if not shutil.which(name): missing.append(name)
    if not Path(a.compiler).is_file(): missing.append(a.compiler)
    if not (a.idf / 'export.sh').is_file(): missing.append(str(a.idf / 'export.sh'))
    if missing: raise SystemExit('Missing preparation prerequisites:\n  ' + '\n  '.join(missing))
    if not shutil.which('mkfs.jffs2'):
        print('Optional persist-image tool missing: mkfs.jffs2 (normal firmware updates preserve persist).')
    print('Host prerequisites present. No downloads, builds or device actions performed.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    n = sub.add_parser('native'); n.add_argument('--kind', choices=['linux', 'uboot'], required=True)
    n.add_argument('--defconfig', required=True); n.add_argument('--cross', required=True); n.add_argument('--cmdline', default='')
    i = sub.add_parser('idf'); i.add_argument('--jobs', default='1'); i.add_argument('--link-list', type=Path, required=True)
    b = sub.add_parser('buildroot'); b.add_argument('--source-config', type=Path, required=True); b.add_argument('--external', type=Path, required=True)
    for key in ['toolchain', 'overlay', 'static-overlay']: b.add_argument('--'+key, required=True)
    k = sub.add_parser('package-inputs'); k.add_argument('--downloads', type=Path, required=True); k.add_argument('--jobs', default='1'); k.add_argument('--external', type=Path, required=True); k.add_argument('--package', required=True); k.add_argument('--stamp', type=Path, required=True); k.add_argument('--input', action='append', type=Path, default=[]); k.add_argument('--value', action='append', default=[])
    for p in [n, i, b, k]:
        p.add_argument('--source', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    for p in [n, b]:
        p.add_argument('--compiler', required=True); p.add_argument('--fragment', type=Path, action='append', default=[])
    d = sub.add_parser('doctor'); d.add_argument('--root', type=Path, required=True); d.add_argument('--compiler', required=True); d.add_argument('--idf', type=Path, required=True)
    a = parser.parse_args()
    {'native': native, 'idf': idf, 'buildroot': buildroot, 'package-inputs': package_inputs, 'doctor': doctor}[a.command](a)

if __name__ == '__main__':
    main()
