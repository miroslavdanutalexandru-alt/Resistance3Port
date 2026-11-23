#!/usr/bin/env python3
"""Resistance 3 port -- regenerate all game-derived code from the player's own
decrypted EBOOT.elf (Resistance 3 is one program: EBOOT.BIN == game.self).

    python tools/r3_regen.py --eboot <EBOOT.elf> [--port <port dir>] [--only ppu|spu]

Writes, inside the port folder:
    recompiled/          PPU code
    project/spu/<mod>/   lifted SPU images: the Wws job manager (policy module)
                         and the embedded SPU ELF programs. Job modules not
                         lifted yet run in the SPU interpreter.
Nothing here is taken from the game except through the lifters run on the
player's own file; tools/hooks/r3.json holds the port's own edits.
The SPU half lives in tools/r3_spu.py (Setup stamps the two halves separately).
Progress goes to stdout as "@@PROGRESS stage=<name> done=<n> total=<n>" lines.
"""
import argparse, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from regen_common import ppu
from r3_spu import spu


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--eboot', required=True)
    ap.add_argument('--port', default=os.path.dirname(HERE)); ap.add_argument('--work', default=None)
    ap.add_argument('--only', choices=['ppu', 'spu'], default=None)
    ap.add_argument('--chunks', type=int, default=28)
    ap.add_argument('--spu-dir', default=None, help='write the SPU modules here instead of project/spu (tests)')
    ap.add_argument('--reglocals', type=int, choices=[0, 1, 2, 3], default=0,
                    help='experimental guest-registers-in-locals pass (scratch/cpu_reglocals.md); 0 = off')
    a = ap.parse_args()
    port = os.path.abspath(a.port); tk = os.path.join(port, 'ps3recomp', 'tools')
    work = os.path.abspath(a.work or os.path.join(port, 'build_regen'))
    os.makedirs(work, exist_ok=True)
    if a.only in (None, 'ppu'):
        ppu(port, tk, a.eboot, 'recompiled', 'r3', a.chunks, True, os.path.join(HERE, 'hooks', 'r3.json'), work, reglocals=a.reglocals)
    if a.only in (None, 'spu'):
        spu(port, tk, os.path.abspath(a.eboot), work, a.spu_dir and os.path.abspath(a.spu_dir))
    print('done: game code regenerated.')


if __name__ == '__main__':
    main()
