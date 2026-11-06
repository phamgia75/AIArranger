#!/usr/bin/env python3
"""Generate an ORIGINAL synthetic SFF2-style test file (issue #27).

Public test fixture with no third-party musical content: every note below was
written for this generator (generic 4/4 pop patterns over a C major source
chord). It mirrors only the documented container structure of a Yamaha SFF2
style (Wierzba/Bedesem "Style Files" v2.1): an SMF (format 0, 1920 PPQN) with
"SFF2"/"SInt"/section markers, followed by a CASM chunk of CSEG/Sdec/Ctb2
blocks (47-byte Ctb2, middle zone at bytes 28-33).

Deterministic: same output bytes on every run. Stdlib only.
Usage: tools/make_synthetic_sty.py fixtures/synthetic/SYNTH_POP_SFF2.sty
License of the output: CC0 (see fixtures/synthetic/README.md).
"""
import struct
import sys

PPQN = 1920
BAR = PPQN * 4

# Section layout, 0-based bars (bar 0 = SInt setup, as in Genos files).
SECTIONS = [  # (marker, first bar, bars)
    ("Main A", 1, 2), ("Main B", 3, 2),
    ("Fill In AA", 5, 1), ("Fill In BB", 6, 1),
    ("Intro A", 7, 2), ("Ending A", 9, 2),
    ("Fill In BA", 11, 1),
]
END_BAR = 12

# Channels (0-based) and CASM track configs:
# name, ch, NTR, NTT(raw, bit7 = bass), High Key, Note Low, Note High, RTR
TRACKS = [
    ("Rhythm1", 8, 1, 0x00, 6, 0, 127, 1),
    ("Rhythm2", 9, 1, 0x00, 6, 0, 127, 1),
    ("Bass", 10, 0, 0x81, 3, 28, 127, 4),
    ("Chord1", 11, 0, 0x02, 7, 0, 127, 3),
    ("Pad", 13, 0, 0x02, 7, 0, 127, 1),
]
SETUP = {  # ch: (bank MSB, bank LSB, program) - generic GM-style choices
    8: (127, 0, 0), 9: (127, 0, 0), 10: (0, 0, 33), 11: (0, 0, 0), 13: (0, 0, 89),
}


def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))


def notes_for(name, bar, rel_bar, nbars):
    """(tick_in_bar, ch, note, vel, dur) for one bar of a section. Original."""
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


def build_mtrk():
    events = []  # (tick, order, bytes)
    def meta(tick, typ, data, order=0):
        events.append((tick, order, bytes([0xFF, typ]) + vlq(len(data)) + data))
    meta(0, 0x58, bytes([4, 2, 24, 8]))               # 4/4
    meta(0, 0x51, (500000).to_bytes(3, "big"))        # 120 BPM
    meta(0, 0x06, b"SFF2")
    meta(0, 0x06, b"SInt")
    for ch, (msb, lsb, pc) in sorted(SETUP.items()):
        events.append((0, 1, bytes([0xB0 | ch, 0, msb])))
        events.append((0, 1, bytes([0xB0 | ch, 32, lsb])))
        events.append((0, 2, bytes([0xC0 | ch, pc])))
        events.append((0, 2, bytes([0xB0 | ch, 7, 100])))
    for name, first, nbars in SECTIONS:
        meta(first * BAR, 0x06, name.encode())
        for rb in range(nbars):
            for (t, ch, n, v, d) in notes_for(name, first + rb, rb, nbars):
                events.append((t, 4, bytes([0x90 | ch, n, v])))
                events.append((t + d, 3, bytes([0x80 | ch, n, 0])))  # offs before ons at a tick
    events.sort(key=lambda x: (x[0], x[1], x[2]))
    out, last = bytearray(), 0
    for tick, _, data in events:
        out += vlq(tick - last) + data
        last = tick
    out += vlq(END_BAR * BAR - last) + bytes([0xFF, 0x2F, 0x00])
    return b"MTrk" + struct.pack(">I", len(out)) + bytes(out)


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


def chunk(cid, data):
    return cid + struct.pack(">I", len(data)) + data


def build_casm():
    body = bytearray()
    for i, (name, _, _) in enumerate(SECTIONS):
        sec = chunk(b"Sdec", name.encode()) + b"".join(ctb2(*t) for t in TRACKS)
        body += sec if i == 0 else chunk(b"CSEG", sec)   # section 1 sits in the outer CSEG
    return chunk(b"CASM", chunk(b"CSEG", bytes(body)))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    mthd = b"MThd" + struct.pack(">IHHH", 6, 0, 1, PPQN)
    data = mthd + build_mtrk() + build_casm()
    with open(sys.argv[1], "wb") as f:
        f.write(data)
    print(f"{sys.argv[1]}: {len(data)} bytes")


if __name__ == "__main__":
    main()
