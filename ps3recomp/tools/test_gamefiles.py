#!/usr/bin/env python3
"""port_gamefiles.h: the launcher's Game files folder (project/tests/gamefiles_test.cpp). Windows only."""
import os
import shutil
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def main():
    if os.name != 'nt':
        print('SKIP: port_gamefiles.h is Windows-only')
        return
    src = REPO / 'project' / 'tests' / 'gamefiles_test.cpp'
    cxx = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++')
    if not cxx:
        print('SKIP: no C++ compiler (set CXX)')
        return
    work = REPO / 'build_test_setup' / 'gamefiles'   # git-ignored; the test makes and removes its fake game here
    work.mkdir(parents=True, exist_ok=True)
    exe = work / 'gamefiles_test.exe'
    subprocess.run([cxx, '-std=c++17', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-I' + str(REPO / 'project'), str(src),
                    '-o', str(exe)], check=True, timeout=120)
    subprocess.run([str(exe)], check=True, timeout=20, cwd=work)
    shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
