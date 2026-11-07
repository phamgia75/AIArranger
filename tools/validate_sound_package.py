#!/usr/bin/env python3
"""Independent check of an `aiarranger-sound demo --out DIR` folder (roadmap S3/S5).

Uses code that is not AIArranger's: Python's XML and wave readers for the .txprog package, and
FluidSynth (third-party SF2 implementation) to render the .sf2 and measure pitch and velocity layer.
SOFTWARE + INDEPENDENT TOOL evidence only; a Pa5X import is the hardware proof (roadmap S6).

    python3 -I tools/validate_sound_package.py DIR [--pitch-only]   # needs `fluidsynth` on PATH
--pitch-only: for instruments other than the demo organ (skips the organ's Soft/Bright layer check).
Exit 0 = all checks pass.
"""
import math, os, struct, subprocess, sys, tempfile, wave
import xml.etree.ElementTree as ET

NOTES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
def key(name):  # TX16Wx names: C-1 = 0 ... G9 = 127
    p = name[:-2] if name.endswith("-1") else name.rstrip("0123456789")
    return NOTES.index(p) + 12 * (int(name[len(p):]) + 1)

ok = True
def check(cond, msg):
    global ok
    ok &= bool(cond)
    print(("  ok    " if cond else "  FAIL  ") + msg)

def smpl(path):
    """(unity note, [(start, end inclusive), ...]) from a WAV 'smpl' chunk, or None."""
    b = open(path, "rb").read(); o = 12
    while o + 8 <= len(b):
        cid, size = b[o:o + 4], struct.unpack("<I", b[o + 4:o + 8])[0]
        if cid == b"smpl" and size >= 36:
            note, n = struct.unpack("<I", b[o + 20:o + 24])[0], struct.unpack("<I", b[o + 36:o + 40])[0]
            return note, [struct.unpack("<II", b[o + 52 + 24 * i:o + 60 + 24 * i]) for i in range(n)]
        o += 8 + size + (size & 1)
    return None

def txprog(root):
    tx = [os.path.join(dp, f) for dp, _, fs in os.walk(os.path.join(root, "tx")) for f in fs if f.endswith(".txprog")]
    check(len(tx) == 1, f"one .txprog in tx/ ({len(tx)})")
    if not tx: return
    path = tx[0]; base = os.path.dirname(path)
    raw = open(path, "rb").read()
    check(raw.startswith(b"\xef\xbb\xbf"), "UTF-8 BOM like Korg's examples")
    ns = {"tx": "http://www.tx16wx.com/3.0/program"}
    prog = ET.fromstring(raw.decode("utf-8-sig"))
    A = lambda e, a: e.get("{%s}%s" % (ns["tx"], a))
    check(prog.tag == "{%s}program" % ns["tx"], "root element tx:program in the 3.0 program namespace")
    waves = {}
    for w in prog.findall("tx:wave", ns):
        f = os.path.join(base, *A(w, "path").split("/"))
        exists = os.path.isfile(f)
        frames = rate = 0
        if exists:
            with wave.open(f) as r:
                frames, rate = r.getnframes(), r.getframerate()
                good = r.getnchannels() == 1 and r.getsampwidth() == 2 and 11025 <= rate <= 48000 and frames <= 1048576
        loops = w.findall("tx:loop", ns)
        lp_ok = all(0 <= int(A(l, "start")) < int(A(l, "end")) <= frames for l in loops)
        if exists:   # the WAV's own 'smpl' chunk must say the same as the .txprog (either may be what the Pa5X reads)
            sm = smpl(f)
            want_loop = [(int(A(l, "start")), int(A(l, "end")) - 1) for l in loops if A(l, "mode") == "Forward"]
            lp_ok &= sm is not None and sm[0] == key(A(w, "root")) and sm[1] == want_loop
        waves[A(w, "id")] = (exists and good and lp_ok, key(A(w, "root")))
    check(waves and all(v[0] for v in waves.values()),
          f"{len(waves)} waves: file exists next to the .txprog, mono 16-bit 11025-48000 Hz, <= 1048576 frames, loop inside, "
          "WAV smpl root/loop == .txprog root/loop")
    groups = prog.findall("tx:group", ns)
    tiles = True; vel = []
    for g in groups:
        regs = sorted(((key(A(b, "low-key")), key(A(b, "high-key")), int(A(b, "low-vel")), int(A(b, "high-vel")), A(r, "wave"))
                       for r in g.findall("tx:region", ns) for b in [r.find("tx:bounds", ns)]))
        nxt = 0
        for lo, hi, vlo, vhi, wid in regs:
            tiles &= lo == nxt and wid in waves and lo <= waves[wid][1] + 2 and waves[wid][1] - 1 <= hi + 1
            nxt = hi + 1
        tiles &= nxt == 128
        vel.append({(r[2], r[3]) for r in regs})
    check(len(groups) in (1, 2) and tiles, f"{len(groups)} group(s); each tiles keys 0-127 with no gap/overlap, root inside its zone")
    flat = sorted(v for s in vel for v in s)
    check(all(len(s) == 1 for s in vel) and flat[0][0] == 0 and flat[-1][1] == 127 and
          all(flat[i][1] + 1 == flat[i + 1][0] for i in range(len(flat) - 1)),
          f"velocity zones per group {flat} tile 0-127")

def render(sf2, layers=True):
    tests = [(24, 60), (25, 60), (36, 60), (48, 110), (57, 60), (60, 60), (60, 110), (61, 110), (69, 60),
             (72, 110), (84, 60), (96, 110), (100, 60), (60, 89), (60, 90)]
    HOLD, GAP, RATE, tpq = 3.0, 0.5, 48000, 480
    def vlq(n):
        b = [n & 0x7f]; n >>= 7
        while n: b.insert(0, (n & 0x7f) | 0x80); n >>= 7
        return bytes(b)
    ev = b""; last = 0; t = 0
    for n, v in tests:
        on = int(t * 2 * tpq); off = int((t + HOLD) * 2 * tpq)
        ev += vlq(on - last) + bytes([0x90, n, v]) + vlq(off - on) + bytes([0x80, n, 0]); last = off; t += HOLD + GAP
    ev = vlq(0) + b"\xff\x51\x03" + (500000).to_bytes(3, "big") + ev + vlq(tpq) + b"\xff\x2f\x00"
    with tempfile.TemporaryDirectory() as d:
        mid, out = os.path.join(d, "t.mid"), os.path.join(d, "r.wav")
        open(mid, "wb").write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, tpq) + b"MTrk" + struct.pack(">I", len(ev)) + ev)
        subprocess.run(["fluidsynth", "-ni", "-q", "-R", "0", "-C", "0", "-g", "0.5", "-r", str(RATE), "-F", out, sf2, mid],
                       check=True, stderr=subprocess.DEVNULL)
        with wave.open(out) as w:
            ch = w.getnchannels(); raw = w.readframes(w.getnframes())
    x = struct.unpack("<%dh" % (len(raw) // 2), raw)[0::ch]
    def dtft(seg, f, win):
        re = im = 0.0; c, s = math.cos(2 * math.pi * f / RATE), math.sin(2 * math.pi * f / RATE); cr, ci = 1.0, 0.0
        for i, v in enumerate(seg):
            a = v * win[i]; re += a * cr; im -= a * ci; cr, ci = cr * c - ci * s, ci * c + cr * s
        return math.hypot(re, im)
    worst = 0; t = 0; allgood = True
    for n, v in tests:
        a = int((t + 0.5) * RATE); N = int(1.5 * RATE); seg = x[a:a + N]; t += HOLD + GAP  # 0.5-2.0 s: several loop passes
        win = [0.5 - 0.5 * math.cos(2 * math.pi * i / (N - 1)) for i in range(N)]
        f0 = 440 * 2 ** ((n - 69) / 12); g = (math.sqrt(5) - 1) / 2
        # 1-cent grid first: real instruments have close double peaks a plain golden search can lock onto
        c0 = max(range(-30, 31), key=lambda c: dtft(seg, f0 * 2 ** (c / 1200), win))
        lo, hi = f0 * 2 ** ((c0 - 1) / 1200), f0 * 2 ** ((c0 + 1) / 1200)
        for _ in range(16):
            m1 = hi - g * (hi - lo); m2 = lo + g * (hi - lo)
            if dtft(seg, m1, win) > dtft(seg, m2, win): hi = m2
            else: lo = m1
        f = (lo + hi) / 2; cents = 1200 * math.log2(f / f0); worst = max(worst, abs(cents))
        a1 = dtft(seg, f, win); a5 = dtft(seg, 5 * f, win) if 5 * f < RATE / 2 else 0
        layer = "Bright" if a1 and a5 / a1 > 0.1 else "Soft"
        allgood &= abs(cents) < (1.0 if layers else 5.0) and a1 > 0 and (not layers or layer == ("Bright" if v >= 90 else "Soft"))
        print(f"        note {n:3d} vel {v:3d}: {f:9.3f} Hz ({cents:+.3f} cents) layer {layer}")
    check(allgood, f"FluidSynth render: {len(tests)} notes within {'1 cent' if layers else '5 cents'} (worst {worst:.3f})"
          + (", velocity 89/90 switch layers" if layers else ""))

if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--pitch-only"]
    if len(args) != 1: sys.exit("usage: validate_sound_package.py DIR [--pitch-only]")
    d = args[0]
    print("TXPROG package (Python XML/wave readers):"); txprog(d)
    sf2 = [f for f in os.listdir(d) if f.endswith(".sf2")]
    check(len(sf2) == 1, "one .sf2")
    if sf2: print("SF2 (FluidSynth):"); render(os.path.join(d, sf2[0]), "--pitch-only" not in sys.argv)
    print("PASS" if ok else "FAIL"); sys.exit(0 if ok else 1)
