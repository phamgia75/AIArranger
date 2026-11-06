#!/usr/bin/env python3
"""Generate ORIGINAL synthetic SFF2-style test files (issue #27).

Public test fixtures with no third-party musical content: every note below was
written for this generator (generic patterns over a C major source chord). They
mirror only the documented container structure of a Yamaha SFF2 style
(Wierzba/Bedesem "Style Files" v2.1): an SMF (format 0, 1920 PPQN) with
"SFF2"/"SInt"/section markers, followed by a CASM chunk of CSEG/Sdec/Ctb2
blocks (47-byte Ctb2, middle zone at bytes 28-33).

Fixtures (fixtures/synthetic/, see README.md):
  SYNTH_POP_SFF2.sty    4/4, 120 BPM, 7 sections, 5 tracks
  SYNTH_FULL_SFF2.sty   4/4, 92 BPM, all 15 sections, + guitar track (NTR 2)
  SYNTH_WALTZ_68.sty    6/8, 132 BPM (quarter), 6 sections
  SYNTH_BREAK_TAIL.sty  1-bar Fill In BA followed by a 9-bar unmarked tail (#26)

Deterministic: same output bytes on every run. Stdlib only.
Usage: tools/make_synthetic_sty.py fixtures/synthetic
License of the output: CC0 (see fixtures/synthetic/README.md).
"""
import os
import struct
import sys

PPQN = 1920

# Channels (0-based) and CASM track configs:
# name, ch, NTR, NTT(raw, bit7 = bass), High Key, Note Low, Note High, RTR
TRACKS = [
    ("Rhythm1", 8, 1, 0x00, 6, 0, 127, 1),
    ("Rhythm2", 9, 1, 0x00, 6, 0, 127, 1),
    ("Bass", 10, 0, 0x81, 3, 28, 127, 4),
    ("Chord1", 11, 0, 0x02, 7, 0, 127, 3),
    ("Pad", 13, 0, 0x02, 7, 0, 127, 1),
]
GUITAR = ("Chord2", 12, 2, 0x01, 11, 0, 127, 3)   # NTR 2 = Guitar, NTT 1 = Stroke
SETUP = {  # ch: (bank MSB, bank LSB, program) - generic GM-style choices
    8: (127, 0, 0), 9: (127, 0, 0), 10: (0, 0, 33), 11: (0, 0, 0), 12: (0, 0, 25), 13: (0, 0, 89),
}


def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))


def notes_44(name, bar, rel_bar, nbars):
    """(tick, ch, note, vel, dur) for one 4/4 bar of a section. Original."""
    BAR = PPQN * 4
    q, e, s = PPQN, PPQN // 2, PPQN // 4
    ev = []
    is_fill = name.startswith("Fill In") and name != "Fill In BA"
    is_break = name == "Fill In BA"
    is_intro = name.startswith("Intro")
    is_end = name.startswith("Ending")
    last_end_bar = is_end and rel_bar == nbars - 1

    # Rhythm2: kick 36, snare 38, closed hat 42.
    if is_break:
        ev.append((0, 9, 36, 110, e))
    elif last_end_bar:
        ev.append((0, 9, 36, 110, q)); ev.append((0, 9, 49, 100, q * 2))
    else:
        for i in range(8):
            ev.append((i * e, 9, 42, 70 if i % 2 else 90, e // 2))
        if not is_intro:
            ev.append((0, 9, 36, 110, e)); ev.append((2 * q, 9, 36, 105, e))
            ev.append((q, 9, 38, 100, e))
            if is_fill:
                for i in range(8):
                    ev.append((2 * q + i * s, 9, 38, 70 + 4 * i, s // 2))
            else:
                ev.append((3 * q, 9, 38, 100, e))
    # Rhythm1: shaker 70 on off-beats (Mains only).
    if name.startswith("Main"):
        for i in range(4):
            ev.append((i * q + e, 8, 70, 60, e // 2))
    # Bass: root/fifth, B adds the third; break = one root.
    if is_break or last_end_bar:
        ev.append((0, 10, 36, 100, q))
    elif is_fill:
        for i in range(4):
            ev.append((i * q, 10, 36 if i % 2 == 0 else 43, 96, q - 120))
    else:
        ev.append((0, 10, 36, 100, q + e)); ev.append((2 * q, 10, 43, 92, q))
        if name == "Main B":
            ev.append((3 * q + e, 10, 40, 88, e - 120))  # ends before the bar line
    # Chord1: off-beat triad comp; Main B adds the major 7th.
    if not (is_break or is_intro or last_end_bar):
        chord = [60, 64, 67] + ([71] if name == "Main B" else [])
        for beat in (1, 3):
            for n in chord:
                ev.append((beat * q, 11, n, 84, e))
    # Pad: sustained C maj7 voicing.
    if not is_break:
        for n in (48, 52, 55, 59):
            ev.append((0, 13, n, 64, BAR - 240 if not last_end_bar else q * 2))
    return [(bar * BAR + t, ch, n, v, d) for (t, ch, n, v, d) in ev]


def notes_44_guitar(name, bar, rel_bar, nbars):
    """notes_44 plus a strum on the guitar track in Mains C and D."""
    ev = notes_44(name, bar, rel_bar, nbars)
    if name in ("Main C", "Main D"):
        base = bar * PPQN * 4
        for beat in (0, 2):
            for k, n in enumerate((52, 55, 60, 64)):   # E3 G3 C4 E4, 30-tick strum
                ev.append((base + beat * PPQN + 30 * k, 12, n, 80, PPQN - 240))
    return ev


def notes_68(name, bar, rel_bar, nbars):
    """One 6/8 bar (6 eighths = 5760 ticks): a two-pulse pattern. Original."""
    e = PPQN // 2               # eighth note
    dq = 3 * e                  # dotted quarter (the 6/8 beat)
    BAR = 6 * e
    ev = []
    is_fill = name.startswith("Fill In")
    last_end_bar = name.startswith("Ending") and rel_bar == nbars - 1
    if last_end_bar:
        ev += [(0, 9, 36, 110, dq), (0, 10, 36, 100, dq), (0, 13, 48, 64, dq), (0, 13, 55, 64, dq)]
    else:
        for i in range(6):
            ev.append((i * e, 9, 42, 85 if i % 3 == 0 else 60, e // 2))
        ev.append((0, 9, 36, 110, e))
        if is_fill:
            for i in range(6):
                ev.append((dq + i * (e // 2), 9, 38, 70 + 5 * i, e // 4))
        else:
            ev.append((dq, 9, 38, 95, e))
        ev.append((0, 10, 36, 100, dq - 120)); ev.append((dq, 10, 43, 90, dq - 120))
        if not name.startswith("Intro"):
            for i in (1, 2, 4, 5):
                for n in (60, 64, 67):
                    ev.append((i * e, 11, n, 76, e - 60))
        for n in (48, 52, 55):
            ev.append((0, 13, n, 60, BAR - 240))
    return [(bar * BAR + t, ch, n, v, d) for (t, ch, n, v, d) in ev]


def tail_events(first_bar, nbars):
    """Unmarked material after the last marker (#26 shape): Rhythm1/Chord1/Pad only."""
    BAR, q = PPQN * 4, PPQN
    ev = []
    for b in range(first_bar, first_bar + nbars):
        for i in range(4):
            ev.append((b * BAR + i * q + q // 2, 8, 70, 55, q // 4))
        for n in (60, 64, 67):
            ev.append((b * BAR + q, 11, n, 70, q // 2))
        for n in (48, 52, 55):
            ev.append((b * BAR, 13, n, 60, BAR - 240))
    return ev


SPECS = {
    "SYNTH_POP_SFF2.sty": dict(
        mpqn=500000, ts=(4, 2, 24), bar=PPQN * 4, notes=notes_44, tracks=TRACKS,
        sections=[("Main A", 1, 2), ("Main B", 3, 2), ("Fill In AA", 5, 1), ("Fill In BB", 6, 1),
                  ("Intro A", 7, 2), ("Ending A", 9, 2), ("Fill In BA", 11, 1)],
        end_bar=12),
    "SYNTH_FULL_SFF2.sty": dict(
        mpqn=652174, ts=(4, 2, 24), bar=PPQN * 4, notes=notes_44_guitar, tracks=TRACKS + [GUITAR],
        sections=[("Main A", 1, 2), ("Main B", 3, 2), ("Main C", 5, 2), ("Main D", 7, 2),
                  ("Fill In AA", 9, 1), ("Fill In BB", 10, 1), ("Fill In CC", 11, 1), ("Fill In DD", 12, 1),
                  ("Intro A", 13, 2), ("Intro B", 15, 2), ("Intro C", 17, 1),
                  ("Ending A", 18, 2), ("Ending B", 20, 2), ("Ending C", 22, 1), ("Fill In BA", 23, 1)],
        end_bar=24),
    "SYNTH_WALTZ_68.sty": dict(
        mpqn=454545, ts=(6, 3, 36), bar=PPQN * 3, notes=notes_68, tracks=TRACKS,
        sections=[("Main A", 1, 4), ("Main B", 5, 4), ("Fill In AA", 9, 1), ("Fill In BB", 10, 1),
                  ("Intro A", 11, 2), ("Ending A", 13, 2)],
        end_bar=15),
    "SYNTH_BREAK_TAIL.sty": dict(
        mpqn=500000, ts=(4, 2, 24), bar=PPQN * 4, notes=notes_44, tracks=TRACKS,
        sections=[("Main A", 1, 2), ("Fill In BA", 3, 1)],
        tail=(4, 9), end_bar=13),
}


def build_mtrk(spec):
    events = []  # (tick, order, bytes)
    def meta(tick, typ, data, order=0):
        events.append((tick, order, bytes([0xFF, typ]) + vlq(len(data)) + data))
    num, dd, clocks = spec["ts"]
    meta(0, 0x58, bytes([num, dd, clocks, 8]))
    meta(0, 0x51, spec["mpqn"].to_bytes(3, "big"))
    meta(0, 0x06, b"SFF2")
    meta(0, 0x06, b"SInt")
    used = {t[1] for t in spec["tracks"]}
    for ch, (msb, lsb, pc) in sorted(SETUP.items()):
        if ch not in used:
            continue
        events.append((0, 1, bytes([0xB0 | ch, 0, msb])))
        events.append((0, 1, bytes([0xB0 | ch, 32, lsb])))
        events.append((0, 2, bytes([0xC0 | ch, pc])))
        events.append((0, 2, bytes([0xB0 | ch, 7, 100])))
    notes = []
    for name, first, nbars in spec["sections"]:
        meta(first * spec["bar"], 0x06, name.encode())
        for rb in range(nbars):
            notes += spec["notes"](name, first + rb, rb, nbars)
    if "tail" in spec:
        notes += tail_events(*spec["tail"])
    for (t, ch, n, v, d) in notes:
        events.append((t, 4, bytes([0x90 | ch, n, v])))
        events.append((t + d, 3, bytes([0x80 | ch, n, 0])))  # offs before ons at a tick
    events.sort(key=lambda x: (x[0], x[1], x[2]))
    out, last = bytearray(), 0
    for tick, _, data in events:
        out += vlq(tick - last) + data
        last = tick
    out += vlq(spec["end_bar"] * spec["bar"] - last) + bytes([0xFF, 0x2F, 0x00])
    return b"MTrk" + struct.pack(">I", len(out)) + bytes(out)


def chunk(cid, data):
    return cid + struct.pack(">I", len(data)) + data


def ctb2(name, ch, ntr, ntt, hk, lo, hi, rtr):
    e = bytearray(47)
    e[0] = ch
    e[1:9] = name.encode().ljust(8)[:8]
    e[9] = ch
    e[18], e[19] = 0, 2           # source chord: root C, type value as observed in Genos files
    e[20], e[21] = 0, 127         # middle zone only
    e[22:28] = e[34:40] = bytes([ntr, ntt, 11, 0, 127, rtr])  # unused zones: defaults, so a wrong-zone read is visible
    e[28:34] = bytes([ntr, ntt, hk, lo, hi, rtr])               # middle zone
    return chunk(b"Ctb2", bytes(e))


def build_casm(spec):
    body = bytearray()
    for i, (name, _, _) in enumerate(spec["sections"]):
        sec = chunk(b"Sdec", name.encode()) + b"".join(ctb2(*t) for t in spec["tracks"])
        body += sec if i == 0 else chunk(b"CSEG", sec)   # section 1 sits in the outer CSEG
    return chunk(b"CASM", chunk(b"CSEG", bytes(body)))


def build(spec):
    mthd = b"MThd" + struct.pack(">IHHH", 6, 0, 1, PPQN)
    return mthd + build_mtrk(spec) + build_casm(spec)


def main():
    if len(sys.argv) != 2 or not os.path.isdir(sys.argv[1]):
        sys.exit(__doc__)
    for name, spec in SPECS.items():
        data = build(spec)
        path = os.path.join(sys.argv[1], name)
        with open(path, "wb") as f:
            f.write(data)
        print(f"{path}: {len(data)} bytes")


if __name__ == "__main__":
    main()
