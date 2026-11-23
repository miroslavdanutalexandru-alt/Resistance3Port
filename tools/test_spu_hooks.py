#!/usr/bin/env python3
"""Checks that the player regen (tools/r3_regen.py) puts the port's SPU hooks back.

project/spu/*/ is not shipped: setup.ps1 regenerates it with r3_regen.py, which applies
tools/hooks/spu/<module>.json (tools/spu_hooks.py) after its passes. For every hooked module:
  * with <fresh-dir> (the output of r3_regen.py --only spu --spu-dir <fresh-dir>): take the
    regenerated file as is (end-to-end check of the player regen);
  * else simulate a fresh lift by removing the hook's added lines from
    project/spu/<module>/spu_recomp.c and apply the json exactly as r3_regen.py does;
  * require the result to equal project/spu/<module>/spu_recomp.c byte for byte, and to
    contain the native-kernel gate lines (r3nj_gate / r3na_gate).
Also checks that r3_regen.py still applies the hooks.

  python tools/test_spu_hooks.py [fresh-dir]
"""
import glob, json, os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)
SPU = os.path.join(PORT, 'project', 'spu')
HK = os.path.join(HERE, 'hooks', 'spu')
WORK = os.path.join(PORT, 'scratch', 'test_spu_hooks')
GATE = re.compile(r'\b(r3nj_gate|r3na_gate)\(ctx, \d+')


def rd(p):
    return open(p, encoding='utf-8', errors='surrogateescape').read()


def strip(txt, H):
    """undo the post hunks: drop each hunk's added lines (all hooks here only add)."""
    added = set()
    for hs in list(H['post'].values()) + [H['preamble']]:
        for h in hs:
            if h['rm']:
                return None
            added.update(h['add'])
    return '\n'.join(l for l in txt.split('\n') if l not in added)


def main():
    fresh = sys.argv[1] if len(sys.argv) > 1 else None
    src = rd(os.path.join(HERE, 'r3_spu.py'))   # the SPU half of tools/r3_regen.py
    fails = 0
    if "hook('post', n, f)" not in src or "'spu_hooks.py'" not in src:
        print('FAIL r3_spu.py (r3_regen.py --only spu) does not apply tools/hooks/spu/*.json'); fails += 1
    os.makedirs(WORK, exist_ok=True)
    gates = 0; n_mod = 0
    for hp in sorted(glob.glob(os.path.join(HK, '*.json'))):
        mod = os.path.splitext(os.path.basename(hp))[0]
        cur_p = os.path.join(SPU, mod, 'spu_recomp.c')
        if not os.path.exists(cur_p):
            print('skip %s (module not in this game)' % mod); continue
        n_mod += 1
        H = json.load(open(hp))
        cur = rd(cur_p)
        out = os.path.join(WORK, mod + '.c')
        if fresh:
            shutil.copyfile(os.path.join(fresh, mod, 'spu_recomp.c'), out)
        else:
            s = strip(cur, H)
            if s is None:
                print('FAIL %s: hook removes lines, needs a real fresh lift (pass fresh-dir)' % mod); fails += 1; continue
            if GATE.search(s):
                print('FAIL %s: gate line not covered by the json' % mod); fails += 1
            open(out, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(s)
        for stage in (() if fresh else ('pre', 'post')):
            if H[stage] or (stage == 'post' and H['preamble']):
                r = subprocess.run([sys.executable, os.path.join(HERE, 'spu_hooks.py'), 'apply', stage, out, hp],
                                   capture_output=True, text=True, cwd=HERE)
                if r.returncode:
                    print('FAIL %s %s: %s' % (mod, stage, (r.stdout + r.stderr).strip()[-500:])); fails += 1
        got = rd(out)
        g = len(GATE.findall(got)); gates += g
        if got != cur:
            print('FAIL %s: regenerated file differs from project/spu' % mod); fails += 1
        elif not g:
            print('FAIL %s: no gate lines after applying hooks' % mod); fails += 1
        else:
            print('ok   %s: %d gate line(s), identical to project/spu' % (mod, g))
    if not n_mod:
        print('FAIL no hooked module found'); fails += 1
    print('%s: %d module(s), %d gate line(s), %d failure(s)' % ('PASS' if not fails else 'FAIL', n_mod, gates, fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
