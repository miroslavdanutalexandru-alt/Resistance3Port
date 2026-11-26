"""Live reader for R3_JOBWATCH (ps3recomp/runtime/spu/spu_jobwatch.c).

Attaches to the shared memory block Local\\R3JobWatch_<pid> of a running
Resistance3.exe started with R3_JOBWATCH=1 and prints the counters and the job
403 event stream as it happens (anomalies marked !!).

    python tools\\r3jobwatch.py            # finds Resistance3.exe
    python tools\\r3jobwatch.py <pid>
    python tools\\r3jobwatch.py <pid> --all    # every event, not just anomalies
"""
import mmap
import struct
import subprocess
import sys
import time

MAGIC = 0x574A3352
HDR = 1024
EV = struct.Struct("<IHHII6IQ")   # seq kind spu tid ea v[6] t_us
CNT = ["get", "item", "put", "llar", "llc_ok", "llc_fail", "writes", "polls",
       "BAD-STATE", "CONCURRENT", "MIDPASS", "BADCALL", "lost", "stress"]
KIND = {1: "GET", 2: "ITEM", 3: "PUT", 4: "LLAR", 5: "PUTLLC", 6: "WRITE", 7: "POLL", 8: "STRESS",
        16: "!!BAD-STATE", 17: "!!CONCURRENT", 18: "!!MIDPASS", 19: "!!BADCALL"}


def find_pid():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq Resistance3.exe", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.strip('"').split('","')
        if len(parts) > 1 and parts[1].isdigit():
            return int(parts[1])
    return None


def who(spu, tid):
    if spu == 0xFFFF:
        return f"PPU t{tid}"
    if spu == 0xFFFE:
        return f"host {tid}"
    return f"spu{spu} h{tid}"


def fmt(kind, spu, tid, ea, v, t):
    k = KIND.get(kind, str(kind))
    w = who(spu, tid)
    head = f"{t / 1e6:12.6f} {k:<13} {w:<11}"
    if kind == 1:
        return f"{head} obj={ea:08X} state={v[0]:04X} item={v[1]:08X}"
    if kind == 3:
        return f"{head} obj={ea:08X} writes={v[0]:04X} mem-before={v[1]:04X} got={v[2]:04X} pass={v[4]}us"
    if kind in (4, 5):
        return f"{head} line={ea:08X} " + " ".join(f"{x:08X}" for x in v[:4])
    if kind == 6:
        return f"{head} ea={ea:08X} word={v[0]:08X} lr={v[1]:08X} obj={v[4]:08X} state={v[3]:04X}"
    if kind == 7:
        return f"{head} obj={ea:08X} {v[0]:04X} -> {v[1]:04X}"
    return f"{head} ea={ea:08X} " + " ".join(f"{x:08X}" for x in v)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    show_all = "--all" in sys.argv
    pid = int(args[0]) if args else find_pid()
    if not pid:
        sys.exit("Resistance3.exe is not running")
    name = f"Local\\R3JobWatch_{pid}"
    try:
        hdr = mmap.mmap(-1, HDR, tagname=name, access=mmap.ACCESS_READ)
    except OSError as e:
        sys.exit(f"cannot open {name}: {e} (was the game started with R3_JOBWATCH=1?)")
    magic, ver, hpid, ring_n = struct.unpack_from("<4I", hdr, 0)
    if magic != MAGIC:
        sys.exit(f"{name}: no jobwatch header (magic {magic:08X})")
    ev_size = struct.unpack_from("<I", hdr, 20)[0]
    hdr.close()
    m = mmap.mmap(-1, HDR + ring_n * ev_size, tagname=name, access=mmap.ACCESS_READ)
    print(f"attached to {name} (ring {ring_n})")
    head0 = struct.unpack_from("<l", m, 16)[0]
    # Replay what the ring still holds (anomalies only unless --all).
    nxt = max(0, head0 - (ring_n - 64))
    last_cnt = 0.0
    while True:
        head = struct.unpack_from("<l", m, 16)[0]
        if head - nxt > ring_n - 64:
            print(f"--- {head - nxt - (ring_n - 64)} events lost ---")
            nxt = head - (ring_n - 64)
        while nxt < head:
            off = HDR + (nxt % ring_n) * ev_size
            seq, kind, spu, tid, ea, *rest = EV.unpack_from(m, off)
            v, t = rest[:6], rest[6]
            if seq == nxt + 1 and (show_all or kind >= 16):
                print(fmt(kind, spu, tid, ea, v, t), flush=True)
            nxt += 1
        if time.time() - last_cnt >= 5:
            last_cnt = time.time()
            cnt = struct.unpack_from("<24l", m, 40)
            nobj = struct.unpack_from("<l", m, 40 + 24 * 4)[0]
            print(f"[{time.strftime('%H:%M:%S')}] objs={nobj} " +
                  " ".join(f"{n}={c}" for n, c in zip(CNT, cnt)), flush=True)
        time.sleep(0.1)


if __name__ == "__main__":
    main()
