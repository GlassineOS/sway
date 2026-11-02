#!/usr/bin/env python3
"""The dead band's host test: judges glassine-epd-sink's BAND lines (band-test.sh).
The window's values: src/wayland/glassine-band-client.c.
  band-check.py DIR N   (DIR/NAME.log per run, as band-test.sh names them)"""
import re
import sys

D, N = sys.argv[1], int(sys.argv[2])
fails = 0


def fail(run, msg):
    global fails
    fails += 1
    print(f"FAIL {run}: {msg}")


def table(gamma):
    if gamma == 1:
        return list(range(256))
    return [round(255 * (k / 255) ** gamma) for k in range(256)]


def truth(fr):
    return {
        "E": 128 + fr * 17 % 128,
        "A": 40 + fr,
        "B": 126 + 3 * (fr & 1),
        "C": 20 + fr * 9 % 207,
        "D": 202 if fr >= N else 200,
    }


def frames(run, gamma):
    """the frames of a run with the window up, as dicts with f = the frame's number"""
    t = table(gamma)
    code = {t[truth(fr)["E"]]: fr for fr in range(N + 1)}
    if len(code) != N + 1:
        sys.exit("frame codes not unique through this table")
    out = []
    for line in open(f"{D}/{run}.log"):
        m = re.match(r"BAND seq=(\d+) tag=(\w+) true=(\d+) rects=(\d+) px=(\d+) t=([\d.]+) dt=([\d.]+) s=([\d,]+) fnv=(\w+)", line)
        if not m:
            continue
        s = [int(v) for v in m[8].split(",")]
        v = dict(zip("DCBAE", s))
        if v["A"] < t[40] or v["E"] not in code:     # (before the window: the desktop)
            continue
        out.append({"seq": int(m[1]), "true": int(m[3]), "rects": int(m[4]), "px": int(m[5]), "t": float(m[6]),
                    "dt": float(m[7]), "v": v, "f": code[v["E"]], "fnv": m[9]})
    if not out:
        fail(run, "no frames with the window")
    return out


def by_frame(fs):
    return {x["f"]: x["fnv"] for x in fs if not x["true"]}


def banded(run, T, gamma=1):
    """error < T everywhere, drift through, flicker gone, then the true frame"""
    t = table(gamma)
    fs = frames(run, gamma)
    motion = [x for x in fs if not x["true"] and x["f"] < N]
    worst = 0
    for x in fs:
        for k in "ABCD":
            e = abs(x["v"][k] - t[truth(x["f"])[k]])
            worst = max(worst, e)
            if e >= T:
                fail(run, f"frame {x['f']} band {k}: {x['v'][k]}, true {t[truth(x['f'])[k]]}: error {e} >= T {T}")
    a = [x["v"]["A"] for x in motion]
    steps = [q - p for p, q in zip(a, a[1:]) if q != p]
    # (frames the sink never saw can make a step larger: only "held, then >= T" matters)
    if any(st < T for st in steps):
        fail(run, f"drift moved by less than T: {sorted(set(steps))}")
    if not steps or a[-1] - a[0] < t[40 + motion[-1]["f"]] - t[40 + motion[0]["f"]] - T:
        fail(run, f"drift did not pass: {a[0]}..{a[-1]}")
    b = [x["v"]["B"] for x in motion[1:]]
    if T > 3 and len(set(b)) > 2:
        fail(run, f"flicker through the band: {sorted(set(b))}")
    bflip = sum(p != q for p, q in zip(b, b[1:]))
    # a frame's D after the pause: kept; its true frame right behind
    last = [x for x in fs if not x["true"] and x["f"] == N]
    trues = [x for x in fs if x["true"]]
    if T > 2:
        if not last or last[-1]["v"]["D"] != t[200]:
            fail(run, f"frame {N}: D {last[-1]['v']['D'] if last else '?'}: not kept at {t[200]}")
    if not trues:
        fail(run, "no true frame")
        return fs
    z = trues[-1]
    for k in "ABCD":
        if z["v"][k] != t[truth(z["f"])[k]]:
            fail(run, f"the true frame: band {k} {z['v'][k]}, true {t[truth(z['f'])[k]]}")
    if z["f"] != N:
        fail(run, f"the last true frame shows frame {z['f']}, not {N}")
    print(f"{run}: {len(fs)} frames, A moved {len(steps)}x in {len(a)} frames (steps {min(steps)}..{max(steps)}), B flips {bflip}, "
          f"worst error {worst} (T {T}), {len(trues)} true frames, the last {z['t'] - last[-1]['t']:.0f} ms after its frame, "
          f"{z['rects']} rects {z['px']} px")
    return fs


def plain(run, gamma=1):
    t = table(gamma)
    fs = frames(run, gamma)
    if any(x["true"] for x in fs):
        fail(run, "true frames with the band off")
    for x in fs:
        for k in "ABCD":
            if x["v"][k] != t[truth(x["f"])[k]]:
                fail(run, f"frame {x['f']} band {k}: {x['v'][k]} != {t[truth(x['f'])[k]]}")
    print(f"{run}: {len(fs)} frames, all exact")
    return fs


def same_bytes(what, a, b):
    ma, mb = by_frame(a), by_frame(b)
    common = set(ma) & set(mb)
    diff = [f for f in common if ma[f] != mb[f]]
    if len(common) < N // 2 or diff:
        fail(what, f"{len(common)} frames in common, {len(diff)} differ: {sorted(diff)[:8]}")
    else:
        print(f"{what}: byte-identical, {len(common)} frames in common")


for g in (1, 1.8):
    tg = "" if g == 1 else "-g18"
    base, t0 = plain(f"base{tg}", g), plain(f"t0{tg}", g)
    same_bytes(f"T 0 == the tree before the band (gamma {g})", base, t0)
    for T in (4, 8):
        fs = banded(f"t{T}{tg}", T, g)
        z = [x for x in fs if x["true"]]
        ref = by_frame(t0).get(N)
        if z and z[-1]["fnv"] != ref:
            fail(f"t{T}{tg}", f"the true frame's bytes {z[-1]['fnv']} != T 0's frame {N} {ref}")
        elif z:
            print(f"t{T}{tg}: the true frame == T 0's frame {N}, byte for byte")
        changed = [f for f, h in by_frame(fs).items() if f < N and f in by_frame(t0) and h != by_frame(t0)[f]]
        print(f"t{T}{tg}: {len(changed)} of {len(by_frame(fs))} motion frames differ from T 0's")

# T changes while the window moves (the sink: after its frame 50, T 8 -> 0): exact from
# then on, except what no frame has redrawn (D is still and never banded here); the true
# frame waits for the quiet, the pause here
fs = frames("change", 1)
tr = [x for x in fs if x["true"]]
inexact = [x["seq"] for x in fs if any(x["v"][k] != truth(x["f"])[k] for k in "ABCD")]
if not tr or not inexact or max(inexact) > 54:
    fail("change", f"T 8 -> 0: {len(tr)} true frames, inexact seqs up to {max(inexact) if inexact else None}")
else:
    print(f"change: T 8 -> 0 at sink frame 50: last inexact frame seq {max(inexact)}, {len(tr)} true frames, "
          f"{sum(x['seq'] > max(inexact) for x in fs)} exact frames after it")

# epdd gone with banded greys in our last target: the next one's first picture is true
fs = frames("reconnect2", 1)
if fs:
    x = fs[0]
    if x["rects"] != 0 or any(x["v"][k] != truth(x["f"])[k] for k in "ABCD"):
        fail("reconnect", f"the first frame after the reconnect: {x}")
    else:
        print(f"reconnect: the first frame is whole and true (frame {x['f']})")

print("band-check:", "FAILED" if fails else "ok")
sys.exit(1 if fails else 0)
