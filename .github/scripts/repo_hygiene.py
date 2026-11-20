#!/usr/bin/env python3
"""Repository hygiene check over `git ls-files`.

1. Fails on any tracked path that matches a game-file deny pattern.
2. Fails on any tracked file over MAX_BYTES unless its path is in
   .github/large-file-allowlist.txt.

Reads only paths and file sizes; it never opens file contents and runs no
repository code. Usage: python3 .github/scripts/repo_hygiene.py [repo_root]
"""
import os
import re
import subprocess
import sys

MAX_BYTES = 5 * 1024 * 1024  # 5 MB

# These patterns mirror the $strict list in scripts/package_beta.ps1 (same regular
# expressions, same labels). PowerShell -match is case-insensitive, so re.IGNORECASE
# is used here. If that list changes, change this one too.
STRICT = {
    'game data / ISO-derived': r'(^|/)(PS3_GAME|USRDIR|TROPDIR|gamefiles|gamedata|savedata)(/|$)|(^|/)EBOOT\.(BIN|elf)$|\.(elf|self|sprx|sdat|edat|iso|pkg|psarc|pup|rap)$|TROPHY\.TRP$',
    'generated game code': r'(^|/)recompiled|^project/spu/[^/]+/|(^|/)lle/',
    'build output': r'(^|/)build[^/]*/|(^|/)CMakeFiles/|\.(obj|exp|ilk|pdb|ninja)$|\.pdb\.syms$|(^|/)obj/',
    'caches / logs / dumps': r'\.foz$|(^|/)shadercache/|(^|/)logs/|\.log$|\.dmp$|(^|/)(crash|dumps?)/|(^|/)(rsxcap|fpval|cache)/',
    'user settings / saves': r'(^|/)settings\.(ini|bat)$|portable\.txt$|trophies\.json$|mouselook_r2\.cfg$',
    'downloaded SDKs': r'^tools/(dlss|xess|streamline|ffmpeg_bin)/|(^|/)(nvngx_dlssg?|sl\.\w+|NvLowLatencyVk|libxess|avcodec-\d+|avutil-\d+|swresample-\d+|r2_dlss|rfom_dlss)\.dll$|nvsdk_ngx',
    'RPCS3 key_vault': r'key_vault',
}
COMPILED = {k: re.compile(v, re.IGNORECASE) for k, v in STRICT.items()}


def main():
    root = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.getcwd()
    out = subprocess.run(['git', '-C', root, 'ls-files', '-z'], check=True, capture_output=True).stdout
    paths = [p.decode('utf-8', 'surrogateescape') for p in out.split(b'\0') if p]

    allow_file = os.path.join(root, '.github', 'large-file-allowlist.txt')
    allow = set()
    if os.path.exists(allow_file):
        for line in open(allow_file, encoding='utf-8'):
            line = line.split('#', 1)[0].strip()
            if line:
                allow.add(line)

    errors = []
    big_seen = set()
    for p in paths:
        for label, rx in COMPILED.items():
            if rx.search(p):
                errors.append('game-file pattern [%s]: %s' % (label, p))
                break
        full = os.path.join(root, p)
        if os.path.islink(full) or not os.path.isfile(full):
            continue  # symlinks, submodules, files deleted in the working tree
        size = os.path.getsize(full)
        if size > MAX_BYTES:
            big_seen.add(p)
            if p not in allow:
                errors.append('over %d MB and not in .github/large-file-allowlist.txt: %s (%d bytes)'
                              % (MAX_BYTES // (1024 * 1024), p, size))

    for p in sorted(allow - big_seen):
        print('note: allow-list entry is not over the cap (or not tracked): ' + p)
    if errors:
        for e in errors:
            print('ERROR: ' + e)
        print('%d problem(s) in %d tracked files' % (len(errors), len(paths)))
        return 1
    print('ok: %d tracked files, no game-file patterns, none over the cap without an allow-list entry' % len(paths))
    return 0


if __name__ == '__main__':
    sys.exit(main())
