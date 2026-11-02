#!/usr/bin/env python3
"""The dead band at the level that reaches the panel (band-test.sh):
the panel image (the shadow the engine is handed) after each
update of a dithered motion run, T 4 / 8 against T 0.
  band-dots.py DUMPDIR PATTERN LOGDIR
Bands as in glassine-band-client (panel columns: D C B A E, the window's rows reversed).
flips = pixels that changed between consecutive A2 updates, % of the band per update."""
import re
import sys
import numpy as np

W, H = 1872, 1404
DIR, PAT, LOG = sys.argv[1], sys.argv[2], sys.argv[3]
BANDS = {"D": 0, "C": 1, "B": 2, "A": 3, "E": 4}


def col(b):
    i = BANDS[b]
    return int((i + .1) * W / 5), int((i + .9) * W / 5)


def load(T):
    d = np.memmap(f"{DIR}/dump-{PAT}-{T}.raw", dtype=np.uint8, mode="r").reshape(-1, H, W)
    modes = [int(m[1]) for m in re.finditer(r"DUMP \d+ mode (\d+)", open(f"{LOG}/dots-{PAT}-{T}/epdd.log").read())]
    return d, modes[:len(d)]


runs = {T: load(T) for T in (0, 4, 8)}
ref, refm = runs[0]
for T, (d, m) in runs.items():
    a2 = [i for i in range(1, len(d)) if m[i] != 2 and m[i - 1] != 2]
    line = []
    for b in BANDS:
        x0, x1 = col(b)
        fl = [np.count_nonzero(d[i, :, x0:x1] != d[i - 1, :, x0:x1]) / ((x1 - x0) * H) * 100 for i in a2]
        line.append(f"{b} {np.mean(fl) if fl else 0:.2f}")
    vals = []
    if a2:
        x0, x1 = col("A")
        vals = np.unique(d[a2[len(a2) // 2], :, x0:x1])
    print(f"{PAT} T {T}: {len(d)} updates, {len(a2)} A2 pairs, modes {sorted(set(m))}; flips % an update: {', '.join(line)}; "
          f"band A values mid-run: {len(vals)} ({[int(v) for v in vals[:4]]})")
    if T:
        n = min(len(d), len(ref))
        dp = [i for i in range(n) if m[i] != 2 and refm[i] != 2]
        line = []
        for b in BANDS:
            x0, x1 = col(b)
            sa = [np.count_nonzero(d[i, :, x0:x1] != ref[i, :, x0:x1]) / ((x1 - x0) * H) * 100 for i in dp]
            line.append(f"{b} {np.mean(sa) if sa else 0:.2f}")
        e, r = d[-1], ref[-1]
        print(f"{PAT} T {T} vs T 0: dots differ % (same update index, {len(dp)} A2 updates): {', '.join(line)}; "
              f"final image: {np.count_nonzero(e != r)} bytes differ, {np.count_nonzero((e ^ r) >> 4)} in level, last modes {m[-1]} / {refm[-1]}")
