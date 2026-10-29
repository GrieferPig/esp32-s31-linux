#!/usr/bin/env python3
"""Run a native build with one Python interpreter, including env shebangs."""
import argparse
import os
from pathlib import Path
import shutil
import shlex


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--python', required=True)
    parser.add_argument('--shim', type=Path, required=True)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('a native command is required')
    interpreter = shutil.which(args.python)
    if not interpreter:
        parser.error('host Python is not executable: ' + args.python)
    interpreter = os.path.abspath(interpreter)
    shim = args.shim.resolve()
    shim.mkdir(parents=True, exist_ok=True)
    # An extra symlink to a venv interpreter changes the executable location
    # used for pyvenv.cfg discovery. Execute its real invocation path instead.
    wrapper = '#!/bin/sh\nexec ' + shlex.quote(interpreter) + ' "$@"\n'
    for name in ('python', 'python3'):
        link = shim / name
        if not link.is_symlink() and link.is_file() and link.read_text() == wrapper:
            continue
        temporary = shim / f'.{name}-{os.getpid()}'
        try:
            temporary.write_text(wrapper)
            temporary.chmod(0o755)
            temporary.replace(link)
        finally:
            temporary.unlink(missing_ok=True)
    env = dict(os.environ, PATH=str(shim) + os.pathsep + os.environ.get('PATH', ''))
    os.execvpe(command[0], command, env)


if __name__ == '__main__':
    main()
