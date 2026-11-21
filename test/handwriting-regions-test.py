#!/usr/bin/env python3
"""Host test of the writing regions (glassine_handwriting_v1 version 2) in sway's
glassine-handwriting.c: the stand-in compositor (handwriting-standin.c) with its stand-in
pen, handwriting-regions-client.c as the app and handwriting-regions-service.py as the
service. Run by handwriting-regions-test.sh, which builds them:

    handwriting-regions-test.py STANDIN CLIENT SERVICE WORKDIR
"""
import os
import queue
import signal
import subprocess
import sys
import threading
import time

STANDIN, CLIENT, SERVICE, WORK = sys.argv[1:5]
FAILS = []


class Proc:
    def __init__(self, argv, env):
        self.p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env, text=True, bufsize=1)
        self.q = queue.Queue()
        self.lines = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            self.q.put(line.rstrip("\n"))

    def send(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()

    def wait(self, pred, timeout=3.0):
        """the first new line for which pred holds, else None"""
        end = time.time() + timeout
        while time.time() < end:
            try:
                line = self.q.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            self.lines.append(line)
            if pred(line):
                return line
        return None

    def drain(self, secs=0.4):
        """every line that comes within secs"""
        got = []
        end = time.time() + secs
        while time.time() < end:
            try:
                got.append(self.q.get(timeout=max(0.01, end - time.time())))
            except queue.Empty:
                break
        self.lines += got
        return got

    def stop(self):
        try:
            self.p.stdin.close()
        except OSError:
            pass
        try:
            self.p.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.p.kill()


def check(name, ok):
    print("  %s %s" % ("ok  " if ok else "FAIL", name))
    if not ok:
        FAILS.append(name)


def service_log(path):
    try:
        with open(path) as f:
            return f.read().splitlines()
    except FileNotFoundError:
        return []


class World:
    def __init__(self, tag, no_regions=False):
        self.log = os.path.join(WORK, "service-%s.log" % tag)
        open(self.log, "w").close()
        rt = os.path.join(WORK, "rt-" + tag)
        os.makedirs(rt, mode=0o700, exist_ok=True)
        env = dict(os.environ, XDG_RUNTIME_DIR=rt, GLASSINE_HANDWRITING_SERVICE=SERVICE, STANDIN_LOG=self.log)
        if no_regions:
            env["STANDIN_NO_REGIONS"] = "1"
        self.standin = Proc([STANDIN], env)
        sock = self.standin.wait(lambda l: l.startswith("socket "))
        env["WAYLAND_DISPLAY"] = sock.split()[1]
        self.env = env
        # the service's hello arrives before the client binds
        time.sleep(0.5)
        self.t = 1000

    def client(self, *args):
        return Proc([CLIENT, *args], self.env)

    def pen(self, x0, y0, x1=None, y1=None, pressure=0.5, between=None):
        """a stroke from (x0, y0) to (x1, y1) in surface coordinates, 5 points"""
        x1 = x0 if x1 is None else x1
        y1 = y0 + 4 if y1 is None else y1
        self.t += 10
        self.standin.send("down 0 %g %g %d %g 0 0" % (x0, y0, self.t, pressure))
        self.standin.wait(lambda l: l == "ok")
        for i in range(1, 5):
            self.t += 10
            self.standin.send("move %g %g %d 0.5 0 0" % (x0 + (x1 - x0) * i / 4, y0 + (y1 - y0) * i / 4, self.t))
            self.standin.wait(lambda l: l == "ok")
            if between and i == 2:
                between()
        self.t += 10
        self.standin.send("up %d" % self.t)
        self.standin.wait(lambda l: l == "ok")

    def stop(self):
        self.standin.stop()


def main():
    w = World("main")

    print("writing regions: capabilities")
    v1 = w.client("-1")
    check("a version 1 manager gets no regions bit (caps 7)", v1.wait(lambda l: l.startswith("caps")) == "caps 7")
    v1.stop()
    c = w.client()
    check("a version 2 manager gets the regions bit (caps 23)", c.wait(lambda l: l.startswith("caps")) == "caps 23")
    c.wait(lambda l: l == "ready")
    for cmd in ["surface", "region a 20 30 200 100", "region b 20 130 200 100", "region top 100 80 60 100 1",
                "neaten a 1 0.5", "sync"]:
        c.send(cmd)
    c.wait(lambda l: l == "synced")

    print("writing regions: routing")
    w.pen(25, 35, 25, 160)
    line = c.wait(lambda l: l.startswith("stroke "))
    check("a stroke goes to the region it starts in, whole (a 0)", line and line.startswith("stroke a 0 5 "))
    check("its points are in the region's coordinates (5, 5)", line and line.endswith(" 5.00 5.00"))
    v = c.wait(lambda l: l.startswith("value a"))
    check("the region's value from its own reading (w0)", v and v.endswith("text=w0"))
    check("with neatening on, its neat stroke", any(l.startswith("neat a 0 1 5 6.00") for l in c.lines))
    check("the word names its stroke", any(l.startswith("word a 1 0 2 ") and l.endswith("strokes=0") for l in c.lines))
    check("the saved state comes with the value", v and "state={\"confirmed\": []} " in v)
    check("candidates at a pause", c.wait(lambda l: l.startswith("cands_done a 1 2")) is not None)

    w.pen(110, 90)
    line = c.wait(lambda l: l.startswith("stroke "))
    check("where regions overlap, the higher layer gets it (top)", line and line.startswith("stroke top 0 "))
    c.send("layer top -1")
    c.send("sync")
    c.wait(lambda l: l == "synced")
    w.pen(110, 90)
    line = c.wait(lambda l: l.startswith("stroke "))
    check("a lower layer loses (a over top)", line and line.startswith("stroke a 1 "))
    c.send("layer top 0")
    c.send("sync")
    c.wait(lambda l: l == "synced")
    w.pen(110, 90)
    line = c.wait(lambda l: l.startswith("stroke "))
    check("of equal layers, the one made last (top over a)", line and line.startswith("stroke top 1 "))
    c.send("layer top 1")

    w.pen(600, 500)
    check("a stroke outside every region is not delivered", not any(l.startswith("stroke") for l in c.drain()))

    c.send("clip b 0 180 400 50")
    c.send("sync")
    c.wait(lambda l: l == "synced")
    w.pen(30, 140)
    check("outside the clip a region takes no stroke", not any(l.startswith("stroke") for l in c.drain()))
    w.pen(30, 190)
    line = c.wait(lambda l: l.startswith("stroke "))
    check("inside the clip it does (b 0)", line and line.startswith("stroke b 0 "))

    print("writing regions: a removed region, text mode, privacy")
    time.sleep(0.3)                     # the service has logged what came before
    before = len(service_log(w.log))

    def gone():
        c.send("destroy b")
        c.send("sync")
        c.wait(lambda l: l == "synced")
    w.pen(30, 200, 30, 210, between=gone)
    check("a stroke whose region is removed mid-stroke is dropped",
          not any(l.startswith("stroke") for l in c.drain()))
    log = service_log(w.log)[before:]
    check("the service forgets the removed region", any('"op":"region_destroy"' in l for l in log))
    check("and gets no stroke for it", not any('"op":"region_stroke"' in l and '"r":2' in l for l in log))

    c.send("region pw 300 30 100 50")
    c.send("ctx pw 8 0")
    c.send("region draw 300 100 100 50")
    c.send("reading draw 0")
    c.send("sync")
    c.wait(lambda l: l == "synced")
    w.pen(310, 40)
    line = c.wait(lambda l: l.startswith("stroke "))
    check("a private region still gets its raw strokes", line and line.startswith("stroke pw 0 "))
    w.pen(310, 110)
    line = c.wait(lambda l: l.startswith("stroke "))
    check("a region with reading off gets raw strokes (a drawing area)", line and line.startswith("stroke draw 0 "))
    check("and no value", not any(l.startswith("value pw") or l.startswith("value draw") for l in c.drain(0.6)))
    log = service_log(w.log)
    check("the private and the drawing region never reach the service",
          not any('"name":"pw"' in l or '"name":"draw"' in l for l in log))
    c.send("report pw 1 1 0")
    check("a private region's report is not kept",
          c.wait(lambda l: l.startswith("report_done pw")) == "report_done pw 1 1")

    c.send("mode a 1")
    c.send("sync")
    c.wait(lambda l: l == "synced")
    w.pen(30, 40)
    check("a region in text mode takes no stroke", not any(l.startswith("stroke") for l in c.drain()))
    check("the mode reaches the service", any('"op":"region_mode"' in l and '"mode":"text"' in l
                                              for l in service_log(w.log)))

    print("writing regions: edit, strike, scroll, report")
    c.send("edit a w0 hello")
    c.send("mode a 0")
    v = c.wait(lambda l: l.startswith("value a"))
    check("an edit gives the new value", v and v.endswith("text=w0 hello"))
    check("an unchanged word keeps its ink", any(l.startswith("word a") and l.endswith("strokes=0") for l in c.lines[-8:]))
    sw = [l for l in c.lines[-8:] if l.startswith("setword a")]
    check("an added word is set in the font, with its outline", sw and " 3 5 " in sw[0] and "shape=12 op0=0" in sw[0])
    check("the replaced ink leaves (gone 1)", any(l.startswith("gone a") and l.endswith("strokes=1") for l in c.lines[-8:]))

    time.sleep(0.3)                     # the service has logged what came before
    before = len(service_log(w.log))
    c.send("rect a 20 -470 200 100")
    c.send("sync")
    c.wait(lambda l: l == "synced")
    time.sleep(0.3)
    check("a scroll sends the service nothing", service_log(w.log)[before:] == [])
    check("and gives no value", not any(l.startswith("value") for l in c.drain(0.3)))
    w.pen(25, -465)
    line = c.wait(lambda l: l.startswith("stroke "))
    check("a stroke after the scroll lands in the moved region, same coordinates",
          line and line.startswith("stroke a 2 5 5.00 5.00"))
    v = c.wait(lambda l: l.startswith("value a"))
    w.pen(25, -460, pressure=0.95)
    g = c.wait(lambda l: l.startswith("gesture a"))
    check("a strike-through is a gesture of the region, naming its stroke", g and g.endswith(" 0 strokes=3"))
    v = c.wait(lambda l: l.startswith("value a"))
    check("its words leave the value", v and v.endswith("text=w0 hello"))
    check("and its ink and its own stroke are gone",
          any(l.startswith("gone a") and l.endswith("strokes=2,3") for l in c.lines[-6:]))
    serial = int(v.split()[2])
    c.send("report a %d 1 0" % serial)
    check("a report per region (stored)", c.wait(lambda l: l.startswith("report_done a")) == "report_done a %d 0" % serial)
    rep = [l for l in service_log(w.log) if '"op":"region_report"' in l]
    check("the report reaches the service with its serial and action",
          rep and '"serial":%d' % serial in rep[-1] and '"action":"accepted"' in rep[-1])

    c.send("long a 5000")
    v = c.wait(lambda l: l.startswith("value a"))
    check("a value of 5000 bytes arrives whole, in parts", v and " len=5000 " in v)
    check("no part over 2048 bytes", not any(l.startswith("oversized") for l in c.lines))

    print("writing regions: stored ink")
    c.send("region r2 20 300 200 100")
    c.send("restore r2 5,6 x_y c3RhdGU= ")
    v = c.wait(lambda l: l.startswith("value r2"))
    check("strokes, value and saved state handed back give the value", v and v.endswith("text=x_y"))
    rs = [l for l in service_log(w.log) if '"op":"region_restore"' in l]
    check("the service gets the strokes with their numbers and the state as given",
          rs and '"k":5' in rs[-1] and '"k":6' in rs[-1] and '"value":"x_y"' in rs[-1] and
          '"state":"YzNSaGRHVT0="' in rs[-1])
    w.pen(30, 310)
    line = c.wait(lambda l: l.startswith("stroke "))
    check("a live stroke after them is numbered above them (7)", line and line.startswith("stroke r2 7 "))
    c.send("region r3 20 420 200 50")
    c.send("restore r3 2 - -")
    v = c.wait(lambda l: l.startswith("value r3"))
    check("strokes alone are read", v and v.endswith("text=w2"))

    print("writing regions: a service restart")
    out = subprocess.run(["pgrep", "-f", "--", SERVICE + " --pen-fd"], capture_output=True, text=True).stdout.split()
    pid = int(out[0]) if out else None
    check("the service runs", pid is not None)
    if pid:
        before = len(service_log(w.log))
        os.kill(pid, signal.SIGTERM)
        end = time.time() + 8
        rs = []
        while time.time() < end and not rs:
            time.sleep(0.2)
            rs = [l for l in service_log(w.log)[before:] if '"op":"region_restore"' in l and '"name"' not in l]
        check("the compositor hands the region's ink to the new service", any('"k":0' in l for l in rs))
        check("with the last value and state", any('"value":' in l and '"state":' in l for l in rs))
    c.stop()
    time.sleep(0.3)
    log = service_log(w.log)
    check("the surface's end when the app goes", '"op":"destroy","s":1' in "".join(log[-3:]) or
          any('"op":"destroy","s":1' in l for l in log))
    w.stop()

    print("writing regions: a service without regions")
    w = World("old", no_regions=True)
    c = w.client()
    check("no regions bit (caps 7)", c.wait(lambda l: l.startswith("caps")) == "caps 7")
    c.wait(lambda l: l == "ready")
    for cmd in ["surface", "region a 0 0 100 100", "sync"]:
        c.send(cmd)
    c.wait(lambda l: l == "synced")
    w.pen(10, 10)
    line = c.wait(lambda l: l.startswith("stroke "))
    check("regions still route raw strokes", line and line.startswith("stroke a 0 "))
    time.sleep(0.3)
    check("and send the service no region op", not any('"op":"region' in l for l in service_log(w.log)))
    c.stop()
    w.stop()

    print("writing regions: all cases passed" if not FAILS else "writing regions: FAILED (%d)" % len(FAILS))
    return 1 if FAILS else 0


sys.exit(main())
