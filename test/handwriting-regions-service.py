#!/usr/bin/env python3
"""A stand-in handwriting service for handwriting-regions-test.py: the compositor's channel
on fd 3 (--pen-fd 3). It says hello (with "regions" unless STANDIN_NO_REGIONS is set),
writes every line it gets to $STANDIN_LOG, and answers the region ops with canned
results: each stroke k is read as the word "wK"; a stroke starting with pressure above
0.9 is a strike-through of the stroke before it; an edit keeps the words "wK" it finds as
ink and sets the others in the font; a restore reads its value, else its strokes. With
the display set (region_set) the ink words come with a shape too."""
import base64
import json
import os
import sys

if sys.argv[1:] != ["--pen-fd", "3"]:
    sys.exit(64)
chan = os.fdopen(3, "r+b", buffering=0)
log = open(os.environ["STANDIN_LOG"], "a")
regions = {}   # (s, r) -> {"strokes": [k], "serial": n, "words": [(text, [k])], "display": "ink"|"set"}


def send(m):
    chan.write(json.dumps(m).encode() + b"\n")


def value(s, r, g, text=None, gone=()):
    g["serial"] += 1
    words = []
    out = []
    pos = 0
    for w, ks in g["words"]:
        if out:
            pos += 1
        if ks:
            words.append({"start": pos, "length": len(w.encode()), "strokes": ks, "dx": 0, "dy": 0})
            if g.get("display") == "set":
                x = 10.0 * len(out)
                words[-1].update(box=[x, 0, x + 8, 12], path=["M", x, 0, "L", x + 8, 0, "L", x, 12, "Z"])
        else:
            x = 10.0 * len(out)
            words.append({"start": pos, "length": len(w.encode()), "box": [x, 0, x + 8, 12],
                          "path": ["M", x, 0, "L", x + 8, 0, "Q", x + 8, 12, x, 12, "Z"]})
        out.append(w)
        pos += len(w.encode())
    t = " ".join(out) if text is None else text
    state = base64.b64encode(json.dumps({"confirmed": []}).encode()).decode()
    m = {"op": "region_value", "s": s, "r": r, "serial": g["serial"], "text": t, "words": words, "state": state}
    if gone:
        m["gone"] = list(gone)
    send(m)
    send({"op": "region_candidates", "s": s, "r": r, "serial": g["serial"],
          "items": [{"text": t, "from": "ink", "fit": True}, {"text": t.upper(), "from": "beam", "fit": False}]})


hello = {"op": "hello", "version": 2, "neaten": True, "text": True, "letters": False, "languages": ["en"]}
if not os.environ.get("STANDIN_NO_REGIONS"):
    hello["regions"] = True
send(hello)
buf = b""
while True:
    d = chan.read(65536)
    if not d:
        break
    buf += d
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        log.write(line.decode() + "\n")
        log.flush()
        m = json.loads(line)
        op, s, r = m.get("op"), m.get("s"), m.get("r")
        key = (s, r)
        if op == "region":
            # a region named "shown…" starts with the display set (the version 2 client's case)
            regions[key] = {"strokes": [], "serial": 0, "words": [],
                            "display": "set" if m.get("name", "").startswith("shown") else "ink"}
        elif op == "region_set" and key in regions:
            g = regions[key]
            d = m.get("display", "ink")
            if d != g.get("display", "ink"):
                g["display"] = d
                if g["words"]:
                    value(s, r, g)
        elif op == "region_destroy":
            regions.pop(key, None)
        elif op == "region_stroke" and key in regions:
            g = regions[key]
            k = m["k"]
            send({"op": "region_neat", "s": s, "r": r, "k": k, "how": "line",
                  "points": [[p[0] + 1, p[1]] for p in m["points"]]})
            if m["points"][0][3] > 0.9 and g["words"]:
                w, ks = g["words"].pop()
                send({"op": "region_gesture", "s": s, "r": r, "serial": g["serial"] + 1, "kind": "strike",
                      "strokes": [k]})
                value(s, r, g, gone=ks + [k])
            else:
                g["words"].append(("w%d" % k, [k]))
                value(s, r, g)
        elif op == "region_edit" and key in regions:
            g = regions[key]
            ink = {w: ks for w, ks in g["words"] if ks}
            new = [(w, ink.get(w, [])) for w in m["text"].split()]
            kept = {k for _, ks in new for k in ks}
            gone = [k for _, ks in g["words"] for k in ks if k not in kept]
            g["words"] = new
            value(s, r, g, text=m["text"], gone=gone)
        elif op == "region_restore" and key in regions:
            g = regions[key]
            ks = [e["k"] for e in m["strokes"]]
            v = m.get("value")
            if v is not None:
                g["words"] = [(w, [ks[i]] if i < len(ks) else []) for i, w in enumerate(v.split())]
            else:
                g["words"] = [("w%d" % k, [k]) for k in ks]
            value(s, r, g)
        elif op == "region_report":
            send({"op": "region_report", "s": s, "r": r, "serial": m["serial"], "result": "stored"})
