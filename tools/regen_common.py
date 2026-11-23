#!/usr/bin/env python3
"""Shared steps of the code generator (tools/r2_regen.py): run the PPU lifter
and its post passes on an ELF, apply the port's hooks, and post-process lifted
SPU modules.
"""
import glob, os, re, shutil, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))

# Machine-readable progress for Setup (setup.ps1 reads these lines; PORT_REGEN_PROGRESS=0 turns them off):
#   @@PROGRESS stage=<name> done=<n> total=<n>
_plock = threading.Lock()
_plast = {}

def progress(stage, done, total, force=False):
    if os.environ.get('PORT_REGEN_PROGRESS', '1') == '0':
        return
    now = time.time()
    with _plock:
        # at most ~4 lines a second per stage, but always the first and the last one
        if not force and 0 < done < total and now - _plast.get(stage, 0) < 0.25:
            return
        _plast[stage] = now
        print('@@PROGRESS stage=%s done=%d total=%d' % (stage, done, total), flush=True)

def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.stderr.write(r.stdout[-2000:] + r.stderr[-4000:])
        raise SystemExit('FAILED: ' + ' '.join(cmd[:4]))
    return r.stdout

_LIFT_RE = re.compile(r'\.\.\. (lifted|emitting) (\d+)/(\d+) functions')

def run_progress(cmd, **kw):
    """run() for the PPU lifter: its "... lifted N/M functions" / "... emitting N/M functions" lines become
    @@PROGRESS lines (stages ppu_lift / ppu_emit) while it runs; the rest of its output is kept for errors."""
    env = dict(kw.pop('env', None) or os.environ, PYTHONUNBUFFERED='1')
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace', env=env, **kw)
    tail = []
    for line in p.stdout:
        m = _LIFT_RE.search(line)
        if m:
            progress('ppu_lift' if m.group(1) == 'lifted' else 'ppu_emit', int(m.group(2)), int(m.group(3)))
        tail.append(line)
        if len(tail) > 400: del tail[:200]
    p.wait()
    if p.returncode != 0:
        sys.stderr.write(''.join(tail)[-6000:])
        raise SystemExit('FAILED: ' + ' '.join(cmd[:4]))

def same_content(a, b):
    """True when files a and b exist and have the same bytes."""
    try:
        if os.path.getsize(a) != os.path.getsize(b): return False
        with open(a, 'rb') as fa, open(b, 'rb') as fb:
            while True:
                x = fa.read(1 << 20); y = fb.read(1 << 20)
                if x != y: return False
                if not x: return True
    except OSError:
        return False

def write_if_changed(path, data):
    """Writes text/bytes to path only when the content differs, so an unchanged file keeps its date and the
    build does not compile it again. Returns True when the file was written."""
    b = data.encode('utf-8') if isinstance(data, str) else data
    try:
        with open(path, 'rb') as f:
            if f.read() == b: return False
    except OSError:
        pass
    with open(path, 'wb') as f: f.write(b)
    return True

def sync_tree(src, dst, keep=(), stage=None):
    """Makes dst a copy of src, writing only the files whose content changed (they get a new date; the others keep
    theirs, so Ninja compiles only what really changed) and deleting files src does not have (names in keep are
    left alone). Returns (changed, unchanged, removed)."""
    files = []
    for root, _, names in os.walk(src):
        for n in names: files.append(os.path.relpath(os.path.join(root, n), src))
    files.sort()
    changed = kept = removed = 0
    if stage: progress(stage, 0, len(files), force=True)
    for i, rel in enumerate(files):
        s, d = os.path.join(src, rel), os.path.join(dst, rel)
        if same_content(s, d):
            kept += 1
        else:
            os.makedirs(os.path.dirname(d), exist_ok=True)
            shutil.copyfile(s, d)
            changed += 1
        if stage: progress(stage, i + 1, len(files))
    want = {r.lower() for r in files}
    keep = {k.lower() for k in keep}
    if os.path.isdir(dst):
        for root, dirs, names in os.walk(dst, topdown=False):
            for n in names:
                rel = os.path.relpath(os.path.join(root, n), dst)
                if rel.lower() not in want and n.lower() not in keep:
                    os.remove(os.path.join(root, n)); removed += 1
            if root != dst and not os.listdir(root):
                os.rmdir(root)
    return changed, kept, removed

def ppu(port, tk, elf, outdir, name, chunks, post_passes, hooks, work, reglocals=0):
    print('[%s] lifting PPU code...' % name, flush=True)
    progress('ppu_analyze', 0, 1, force=True)
    od = os.path.join(work, name + '_an')
    run([sys.executable, os.path.join(tk, 'ppu_loader.py'), elf, '-o', od])
    progress('ppu_analyze', 1, 1, force=True)
    stem = os.path.splitext(os.path.basename(elf))[0]
    tmp = os.path.join(work, name)
    if os.path.isdir(tmp): shutil.rmtree(tmp)
    run_progress([sys.executable, os.path.join(tk, 'ppu_lifter.py'), elf, '--functions', os.path.join(od, stem + '.functions.json'),
                  '--hle-stubs', os.path.join(od, stem + '.imports.json'), '--chunks', str(chunks), '--output', tmp])
    chunksf = sorted(glob.glob(os.path.join(tmp, 'ppu_recomp_0*.cpp')))
    if post_passes:
        print('[%s] register cache + fast memory passes...' % name, flush=True)
        # weighted by size: the chunks differ a lot (a few MB to 200 MB)
        sizes = [os.path.getsize(f) for f in chunksf]
        tot = sum(sizes) or 1
        done = 0
        progress('ppu_post', 0, tot, force=True)
        for f, sz in zip(chunksf, sizes):
            run([sys.executable, os.path.join(HERE, 'regcache.py'), f, f])
            run([sys.executable, os.path.join(HERE, 'ppu_fastmem.py'), f, f])
            done += sz
            progress('ppu_post', done, tot, force=True)
        # the function table is its own file in this lifter version: give it the
        # same vm_base declaration guard ppu_fastmem.py gives the chunks
        tf = os.path.join(tmp, 'ppu_recomp_table.cpp')
        if os.path.exists(tf):
            s = open(tf, encoding='utf-8', errors='surrogateescape').read()
            s = s.replace('\nextern "C" uint8_t* vm_base;\n', '\n#ifndef PPU_FIXED_BOUNDS   /* v163: ppu_recomp.h declares it const */\nextern "C" uint8_t* vm_base;\n#endif\n', 1)
            open(tf, 'w', encoding='utf-8', errors='surrogateescape').write(s)
    progress('ppu_hooks', 0, 1, force=True)
    run([sys.executable, os.path.join(HERE, 'ppu_hooks.py'), 'apply', tmp, hooks])
    shutil.copy(os.path.join(HERE, 'hooks', 'ppu_hle_nids.cpp'), os.path.join(tmp, 'ppu_hle_nids.cpp'))
    progress('ppu_hooks', 1, 1, force=True)
    if reglocals:
        # experimental (scratch/cpu_reglocals.md): lazy write-back + liveness-aware
        # spill/reload around calls; L>=2 uses whole-program preserve/read summaries
        print('[%s] reglocals L%d pass...' % (name, reglocals), flush=True)
        lz = os.path.join(HERE, 'regcache_lazy.py')
        opts = ['--lazy-wb', '--reglocals=%d' % reglocals]
        if reglocals >= 2:
            summ = os.path.join(work, name + '_reglocals_summary.json')
            run([sys.executable, lz, '--summarize', summ] + chunksf)
            opts.append('--summary=' + summ)
        for f in chunksf:
            run([sys.executable, lz, f, f] + opts)
    dst = os.path.join(port, outdir)
    # only the files whose content changed are written (Setup's code-generator stamp is kept)
    c, k, r = sync_tree(tmp, dst, keep=('.generator_inputs',), stage='ppu_copy')
    print('[%s] %s: %d files changed, %d unchanged, %d removed' % (name, outdir, c, k, r), flush=True)

def spu_post(f, hooks):
    run([sys.executable, os.path.join(HERE, 'spu_hooks.py'), 'apply', 'pre', f, hooks])
    run([sys.executable, os.path.join(HERE, 'spu_regcache.py'), f, f])
    run([sys.executable, os.path.join(HERE, 'spu_directhop.py'), f])
    run([sys.executable, os.path.join(HERE, 'spu_hooks.py'), 'apply', 'post', f, hooks])
    run([sys.executable, os.path.join(HERE, 'spu_w0.py'), f])

def fix_includes(d):
    for f in glob.glob(os.path.join(d, 'spu_recomp.*')):
        s = open(f, encoding='utf-8', errors='surrogateescape').read().replace('"../../runtime/spu/', '"')
        open(f, 'w', encoding='utf-8', errors='surrogateescape').write(s)
