#!/usr/bin/env python3
"""Content identity of a compiler installation, including native subtools/sysroot."""
import argparse
import hashlib
import json
from pathlib import Path


def identify(compiler):
    compiler = Path(compiler).absolute()
    root = compiler.parent.parent
    # A fixture/single-file compiler still has an honest, bounded identity.
    paths = [compiler]
    if compiler.parent.name == 'bin':
        paths = []
        for child in sorted(root.iterdir()):
            if child.name in {'share', '.git'}:
                continue
            if child.is_dir() and not child.is_symlink():
                paths.extend(sorted(p for p in child.rglob('*') if p.is_file()))
            elif child.is_file():
                paths.append(child)
    combined = hashlib.sha256()
    count = 0
    for path in sorted(set(paths)):
        with path.open('rb') as stream:
            content = hashlib.file_digest(stream, 'sha256').hexdigest()
        combined.update(str(path.relative_to(root)).encode() + b'\0' + content.encode() + b'\n')
        count += 1
    return {'sha256': combined.hexdigest(), 'file_count': count}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', required=True)
    p.add_argument('--output', type=Path)
    a = p.parse_args()
    text = json.dumps(identify(a.compiler), sort_keys=True) + '\n'
    if a.output:
        a.output.parent.mkdir(parents=True, exist_ok=True)
        if not a.output.exists() or a.output.read_text() != text:
            temp = a.output.with_suffix('.tmp'); temp.write_text(text); temp.replace(a.output)
    else:
        print(text.strip())
