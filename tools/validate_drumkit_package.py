#!/usr/bin/env python3
"""Independent structural check of a TX16Wx Drum Kit package (.txperf -> .txprog -> .txmatrix -> WAV).

Shares no code with AIArranger's C++ writer: Python's XML and wave readers only. The rules come from
Korg's guide "Making Pa5X Sounds and Drum Kits with TX16Wx" (Standard Drum Kit: one Program, each
key a single-key region holding a matrix with one column and up to 16 velocity rows, Y source =
velocity, choke 0-8, level at most +6 dB, family prefixes B_ S_ H_ T_ C_ L_ E_ X_) and from Korg's
example "Real-Drums DrumKit". SOFTWARE + INDEPENDENT TOOL evidence only; a Pa5X import is the proof.

    python3 -I tools/validate_drumkit_package.py DIR [--reference]
DIR holds the .txperf (searched recursively). Default (our packages): 16-bit WAVs with a 'smpl'
chunk whose root/loop match the matrix. --reference: also accepts what Korg's own example contains
(24-bit WAVs, WAVs without 'smpl', zero-length "Forward" loops). Exit 0 = all checks pass.
"""
import os, re, struct, sys, wave
import xml.etree.ElementTree as ET

# Every element path and attribute in Korg's "Real-Drums DrumKit" example; anything else is refused
# (the writer must stay inside the example's format).
ALLOWED = {
    "performance": {"/performance": {"created-by", "name", "schemaLocation"},
                    "/performance/slot": {"channel", "color", "name", "output", "program"},
                    "/performance/slot/detune": {"value"}, "/performance/slot/pan": {"value"}, "/performance/slot/send": set(),
                    "/performance/slot/send/level": {"value"}, "/performance/slot/transpose": {"value"},
                    "/performance/slot/velocity-curve": {"shape", "smooth"}, "/performance/slot/volume": {"value"}},
    "program": {"/program": {"created-by", "name", "quality", "schemaLocation"},
                "/program/bounds": {"high-key", "high-vel", "low-key", "low-vel"},
                "/program/group": {"choke-group", "coarse", "color", "fine", "name", "noteprio", "output", "pan", "playback",
                                   "playmode", "polymode", "quality", "scale", "soundshape", "volume"},
                "/program/group/region": {"attenuation", "fine", "loop", "matrix", "mode", "pan", "release", "root", "shift", "tune"},
                "/program/group/region/bounds": {"high-key", "high-vel", "low-key", "low-vel"},
                "/program/soundshape": {"glide-mode", "id", "name", "pan", "pwm", "unison", "unison-cyclic-spread", "unison-depth",
                                        "unison-pan", "unison-spread", "unison-start", "volume"},
                "/program/soundshape/aeg": {"attack", "attack-shape", "decay1", "decay1-shape", "decay2", "decay2-shape", "level1",
                                            "level2", "release", "release-shape", "sustain"},
                "/program/soundshape/modulation": set(), "/program/soundshape/send": {"level"}, "/program/wavematrix": {"id", "path"}},
    "wavematrix": {"/wavematrix": {"created-by", "name", "schemaLocation"}, "/wavematrix/row": {"value"},
                   "/wavematrix/row/cell": {"attenuation", "fine", "mode", "pan", "root", "wave"},
                   "/wavematrix/wave": {"id", "path", "root"}, "/wavematrix/wave/loop": {"end", "mode", "name", "start"},
                   "/wavematrix/xaxis": {"source"}, "/wavematrix/xaxis/curve": {"shape", "smooth"},
                   "/wavematrix/yaxis": {"source"}, "/wavematrix/yaxis/curve": {"shape", "smooth"}},
}

def unknown(root, kind):
    """Element paths / attributes not in Korg's example."""
    out = []
    def walk(e, path):
        p = path + "/" + e.tag.split("}")[-1]
        if p not in ALLOWED[kind]: out.append("element " + p); return
        out.extend("attribute %s@%s" % (p, a.split("}")[-1]) for a in e.attrib if a.split("}")[-1] not in ALLOWED[kind][p])
        for c in e: walk(c, p)
    walk(root, "")
    return out

NOTES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
FAMILIES = set("BSHTCLEX")
NS = "http://www.tx16wx.com/3.0/"

def key(name):  # C-1 = 0 ... G9 = 127; anything else (including G#9 = 128) raises ValueError
    p = name[:-2] if name.endswith("-1") else name.rstrip("0123456789")
    k = NOTES.index(p) + 12 * (int(name[len(p):]) + 1)
    if not 0 <= k <= 127: raise ValueError("key %s outside 0-127" % name)
    return k

def level(s):
    """Cell level such as '-2.5 dB'; ValueError unless a finite number followed by ' dB'."""
    m = re.fullmatch(r"(-?\d+(?:\.\d+)?) dB", s or "")
    if not m: raise ValueError("bad level %r" % s)
    return float(m.group(1))

ok = True
def check(cond, msg):
    global ok
    ok &= bool(cond)
    print(("  ok    " if cond else "  FAIL  ") + msg)
    return bool(cond)

def inside(base, rel):
    """Resolve a package-relative path; None unless it is relative, '/'-separated and stays inside base."""
    if not rel or rel.startswith("/") or "\\" in rel or re.match(r"^[A-Za-z]:", rel) or ".." in rel.split("/"):
        return None
    p = os.path.realpath(os.path.join(base, *rel.split("/")))
    return p if p.startswith(os.path.realpath(base) + os.sep) else None

def load(path, kind):
    raw = open(path, "rb").read()
    root = ET.fromstring(raw.decode("utf-8-sig"))
    return raw.startswith(b"\xef\xbb\xbf"), root, (lambda e, a: e.get("{%s%s}%s" % (NS, kind, a))), {"tx": NS + kind}

def smpl(path):
    b = open(path, "rb").read(); o = 12
    while o + 8 <= len(b):
        cid, size = b[o:o + 4], struct.unpack("<I", b[o + 4:o + 8])[0]
        if cid == b"smpl" and size >= 36:
            note, n = struct.unpack("<I", b[o + 20:o + 24])[0], struct.unpack("<I", b[o + 36:o + 40])[0]
            return note, [struct.unpack("<II", b[o + 52 + 24 * i:o + 60 + 24 * i]) for i in range(n)]
        o += 8 + size + (size & 1)
    return None

def matrix(path, reference):
    bom, m, A, ns = load(path, "wavematrix")
    base = os.path.dirname(path); errs = unknown(m, "wavematrix")
    if m.tag != "{%swavematrix}wavematrix" % NS: errs.append("not a wavematrix")
    name = A(m, "name") or ""
    if re.match(r"^[A-Z]_", name) and name[0] not in FAMILIES: errs.append("unknown family prefix " + name[:2])
    y, x = m.find("tx:yaxis", ns), m.find("tx:xaxis", ns)
    if y is None or A(y, "source") != "Vel": errs.append("Y source is not velocity")
    if x is None or A(x, "source") != "--": errs.append("X source is not empty (round-robin columns are out of scope)")
    waves = {}
    for w in m.findall("tx:wave", ns):
        f = inside(base, A(w, "path"))
        if f is None or not os.path.isfile(f): errs.append("wave path missing or outside the package: %s" % A(w, "path")); continue
        with wave.open(f) as r:
            ch, bits, rate, frames = r.getnchannels(), 8 * r.getsampwidth(), r.getframerate(), r.getnframes()
            if len(r.readframes(frames)) != frames * ch * r.getsampwidth():   # declared length vs actual audio bytes
                errs.append(os.path.basename(f) + ": data chunk shorter than its declared length")
        if ch not in (1, 2) or not 11025 <= rate <= 48000 or frames > 1048576 or bits not in ((16, 24) if reference else (16,)):
            errs.append("%s: %d ch %d bit %d Hz %d frames" % (os.path.basename(f), ch, bits, rate, frames))
        loops = [(int(A(l, "start")), int(A(l, "end"))) for l in w.findall("tx:loop", ns) if A(l, "mode") == "Forward"]
        real = [(s, e) for s, e in loops if s != e or not reference]
        if any(not 0 <= s < e <= frames for s, e in real): errs.append(os.path.basename(f) + ": loop outside the sample")
        sm = smpl(f)
        if sm is None and not reference: errs.append(os.path.basename(f) + ": no smpl chunk")
        if sm is not None and not reference:
            if A(w, "root") is None or sm[0] != key(A(w, "root")): errs.append(os.path.basename(f) + ": smpl root != matrix root")
            if sm[1] != [(s, e - 1) for s, e in real]: errs.append(os.path.basename(f) + ": smpl loop != matrix loop")
        if A(w, "id") in waves: errs.append("wave id %s declared twice" % A(w, "id"))
        waves[A(w, "id")] = f
    rows = m.findall("tx:row", ns)
    vals = [int(A(r, "value")) for r in rows]
    if not 1 <= len(rows) <= 16: errs.append("%d velocity rows (1-16)" % len(rows))
    if not vals or vals[0] != 0 or any(b <= a for a, b in zip(vals, vals[1:])) or vals[-1] > 127:
        errs.append("row velocities %s do not start at 0 and ascend" % vals)
    for r in rows:
        cells = r.findall("tx:cell", ns)
        if len(cells) != 1: errs.append("row %s has %d cells (one column)" % (A(r, "value"), len(cells))); continue
        if A(cells[0], "wave") not in waves: errs.append("cell refers to a missing wave")
        att = A(cells[0], "attenuation") or "0 dB"
        if level(att) > 6.0: errs.append("cell level %s above +6 dB" % att)
    return bom, name, len(rows), errs

def kit(d, reference):
    perfs = [os.path.join(dp, f) for dp, _, fs in os.walk(d) for f in fs if f.endswith(".txperf")]
    if not check(len(perfs) == 1, "one .txperf (%d)" % len(perfs)): return
    bom, perf, A, ns = load(perfs[0], "performance")
    base = os.path.dirname(perfs[0])
    check(bom, "UTF-8 BOM like Korg's examples")
    check(not unknown(perf, "performance"), "only elements/attributes present in Korg's example (.txperf) %s" % unknown(perf, "performance"))
    check(perf.tag == "{%sperformance}performance" % NS, "root element tx:performance in the 3.0 namespace")
    slots = perf.findall("tx:slot", ns)
    if not check(len(slots) == 1, "one slot = Standard Drum Kit (%d; Ambience kits have two)" % len(slots)): return
    pp = inside(base, A(slots[0], "program"))
    if not check(pp is not None and os.path.isfile(pp), "slot program %s exists inside the package" % A(slots[0], "program")): return
    _, prog, P, pns = load(pp, "program")
    check(prog.tag == "{%sprogram}program" % NS and P(prog, "name") == A(perf, "name"), "Program name == Performance name")
    check(not unknown(prog, "program"), "only elements/attributes present in Korg's example (.txprog) %s" % unknown(prog, "program"))
    mats, dup = {}, False
    for wm in prog.findall("tx:wavematrix", pns):
        f = inside(base, P(wm, "path"))
        dup |= P(wm, "id") in mats
        mats[P(wm, "id")] = f if f and os.path.isfile(f) else None
    lower = [m.lower() for m in mats.values() if m]
    check(mats and not dup and all(mats.values()) and len(set(lower)) == len(mats),
          "%d matrices: unique ids, every path exists inside the package, no duplicates (case-insensitive)" % len(mats))
    keys, errs, layers, used = {}, [], 0, set()
    for g in prog.findall("tx:group", pns):
        ch = int(P(g, "choke-group") or 0)
        if not 0 <= ch <= 8: errs.append("group %s choke %d (0-8)" % (P(g, "name"), ch))
        for r in g.findall("tx:region", pns):
            b = r.find("tx:bounds", pns)
            lo, hi = key(P(b, "low-key")), key(P(b, "high-key"))
            if lo != hi: errs.append("region %s-%s spans more than one key" % (P(b, "low-key"), P(b, "high-key")))
            if (P(b, "low-vel"), P(b, "high-vel")) != ("0", "127"): errs.append("region on %s does not cover velocity 0-127" % P(b, "low-key"))
            if lo in keys: errs.append("key %s assigned twice" % P(b, "low-key"))
            keys[lo] = (P(g, "name"), ch)
            pan = int((P(r, "pan") or "0%").rstrip("%")); shift = int(P(r, "shift") or 0); tune = int(P(r, "tune") or 0)
            if not (-100 <= pan <= 100 and -12 <= shift <= 12 and -50 <= tune <= 50):
                errs.append("key %s pan %d%% shift %d tune %d outside Pa5X/TX ranges" % (P(b, "low-key"), pan, shift, tune))
            mid = P(r, "matrix")
            if not mats.get(mid): errs.append("key %s refers to a missing matrix" % P(b, "low-key")); continue
            used.add(mid)
            mbom, name, n, merr = matrix(mats[mid], reference)
            layers += n
            errs += ["%s: %s" % (os.path.basename(mats[mid]), e) for e in merr]
            if not mbom and not reference: errs.append(os.path.basename(mats[mid]) + ": no UTF-8 BOM")
    check(used == set(mats), "every matrix is used by a region")
    for e in errs: print("        " + e)
    check(keys and not errs, "%d keys, %d velocity layers: single-key regions, unique keys, 1-16 ascending rows from 0, one column, "
          "Y = velocity, choke 0-8, level <= +6 dB, families valid, WAVs inside the package%s"
          % (len(keys), layers, "" if reference else ", 16-bit with smpl root/loop == matrix"))
    chokes = sorted({c for _, c in keys.values() if c})
    print("  info  exclusive groups: %s" % (", ".join("%d (%s)" % (c, ", ".join(sorted({n for n, cc in keys.values() if cc == c}))) for c in chokes) or "none"))

if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--reference"]
    if len(args) != 1: sys.exit("usage: validate_drumkit_package.py DIR [--reference]")
    if not os.path.isdir(args[0]): sys.exit("FAIL: not a folder: %s" % args[0])
    print("TX16Wx Drum Kit package (Python XML/wave readers):")
    try:
        kit(args[0], "--reference" in sys.argv)
    except (ValueError, ET.ParseError, wave.Error, EOFError, TypeError, AttributeError, struct.error) as e:
        check(False, "malformed package: %s: %s" % (type(e).__name__, e))
    print("PASS" if ok else "FAIL"); sys.exit(0 if ok else 1)
