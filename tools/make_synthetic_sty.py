#!/usr/bin/env python3
"""Generate ORIGINAL synthetic SFF1/SFF2-style test files (issue #27 + Track B Q1).

Public test fixtures with no third-party musical content: every note below was
written for this generator (generic patterns). They mirror only the documented
container structure of Yamaha SFF1/SFF2 styles (Wierzba/Bedesem "Style Files"
v2.1): an SMF (format 0, 1920 or 480 PPQN) with "SFF1"/"SFF2"/"SInt"/section
markers, followed by a CASM chunk of CSEG/Sdec/Ctb2 (or SFF1 Ctab/Cntt) blocks,
and optional OTSc/FNRP/MDB metadata chunks.

12 CC0 Synthetic Torture Fixtures (fixtures/synthetic/, see README.md):
   1. SYNTH_POP_SFF2.sty              4/4, 120 BPM, 7 sections, 5 tracks (existing, byte-identical)
   2. SYNTH_FULL_SFF2.sty             4/4, 92 BPM, all 15 sections, + guitar NTR 2 (existing, byte-identical)
   3. SYNTH_WALTZ_68.sty              6/8, 132 BPM, 6 sections (existing, byte-identical)
   4. SYNTH_BREAK_TAIL.sty            1-bar Fill In BA + 9-bar unmarked tail #26 (existing, byte-identical)
   5. SYNTH_SFF1_CTAB_CNTT.sty        SFF1 / 480 PPQN / 4/4, Ctab + Cntt (0 Ctb2 blocks)
   6. SYNTH_SRC_CHORD_NON_C.sty       SFF2 / 1920 / 4/4, non-C/non-Major Ctb2[18..19] source chords + limits
   7. SYNTH_CHORD_MUTE_SPLIT.sty      SFF2 / 1920 / 4/4, mutually exclusive Ctb2[11..17] chord-mute pairs
   8. SYNTH_MEGAVOICE_GUITAR_BASS.sty SFF2 / 1920 / 4/4, MegaVoice 63/120 & 63/60, 0..11, 96..124, 4 vel zones
   9. SYNTH_REVO_XG_DRUMS.sty         SFF2 / 1920 / 4/4, Revo!/XG drums 13..34 & 82..105 + swapped Rhythm1/2
  10. SYNTH_MIX_SYSEX_CC_HYGIENE.sty  SFF2 / 1920 / 4/4, SInt mix CCs, XG SysEx, NRPN/CC94, open CC64/PB
  11. SYNTH_MIXED_METER_RUBATO.sty    SFF2 / 1920 / 6/8+4/4+2/4, mid-style FF 58, Ending FF 51 ritard, flams
  12. SYNTH_MULTI_ZONE_SART_OTS.sty   SFF2 / 1920 / 4/4, multi-zone CASM, S.Art key-switches, mid-style voice, OTSc/FNRP/MDB

Deterministic: same output bytes on every run. Stdlib only.
Usage: tools/make_synthetic_sty.py fixtures/synthetic
License of the output: CC0 (see fixtures/synthetic/README.md).
"""
import os
import struct
import sys

PPQN = 1920

# Channels (0-based) and CASM track configs (legacy 8-tuple for existing 4 fixtures):
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


def chunk(cid, data):
    return cid + struct.pack(">I", len(data)) + data


# ── Original 4 fixtures (MUST remain 100% byte-identical) ─────────────

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


# ── Extended Q1 Torture Fixture Helpers (Fixtures 5..12) ──────────────

def ctb2_full(
    name,
    src_ch,
    dest_ch=None,
    editable=1,
    note_mute=0x0FFF,
    chord_mute=0x3FFFFFFFF,
    src_root=0,
    src_type=0,
    zone_low=0,
    zone_high=127,
    mid=(0, 2, 7, 0, 127, 3),
    low=None,
    high=None,
):
    """Full 47-byte SFF2 Ctb2 entry with all documented fields (Tables 12-16)."""
    if dest_ch is None:
        dest_ch = src_ch
    if low is None:
        low = (mid[0], mid[1], 11, 0, 127, mid[5])
    if high is None:
        high = (mid[0], mid[1], 11, 0, 127, mid[5])
    e = bytearray(47)
    e[0] = src_ch & 0x0F
    e[1:9] = name.encode("ascii").ljust(8)[:8]
    e[9] = dest_ch & 0x0F
    e[10] = editable & 0xFF
    # [11..12] Note Mute (12-bit big-endian)
    e[11] = (note_mute >> 8) & 0x0F
    e[12] = note_mute & 0xFF
    # [13..17] Chord Mute (34-bit big-endian across 5 bytes)
    e[13] = (chord_mute >> 32) & 0x03
    e[14] = (chord_mute >> 24) & 0xFF
    e[15] = (chord_mute >> 16) & 0xFF
    e[16] = (chord_mute >> 8) & 0xFF
    e[17] = chord_mute & 0xFF
    e[18] = src_root & 0xFF
    e[19] = src_type & 0xFF
    e[20] = zone_low & 0x7F
    e[21] = zone_high & 0x7F
    e[22:28] = bytes(low)
    e[28:34] = bytes(mid)
    e[34:40] = bytes(high)
    return chunk(b"Ctb2", bytes(e))


def ctab_entry(
    name,
    src_ch,
    dest_ch=None,
    editable=1,
    note_mute=0x0FFF,
    chord_mute=0x3FFFFFFFF,
    src_root=0,
    src_type=0,
    ntr=0,
    ntt=2,
    hk=7,
    lo=0,
    hi=127,
    rtr=3,
):
    """Documented 33-byte SFF1 Ctab entry (Wierzba/Bedesem v2.1 Tables 5-10)."""
    if dest_ch is None:
        dest_ch = src_ch
    e = bytearray(33)
    e[0] = src_ch & 0x0F
    e[1:9] = name.encode("ascii").ljust(8)[:8]
    e[9] = dest_ch & 0x0F
    e[10] = editable & 0xFF
    e[11] = (note_mute >> 8) & 0x0F
    e[12] = note_mute & 0xFF
    e[13] = (chord_mute >> 32) & 0x03
    e[14] = (chord_mute >> 24) & 0xFF
    e[15] = (chord_mute >> 16) & 0xFF
    e[16] = (chord_mute >> 8) & 0xFF
    e[17] = chord_mute & 0xFF
    e[18] = src_root & 0xFF
    e[19] = src_type & 0xFF
    e[20] = ntr & 0xFF
    e[21] = ntt & 0xFF
    e[22] = hk & 0xFF
    e[23] = lo & 0x7F
    e[24] = hi & 0x7F
    e[25] = rtr & 0xFF
    return chunk(b"Ctab", bytes(e))


def cntt_entry(src_ch, ntt_byte):
    """Documented SFF1 Cntt companion entry (2 bytes: src_ch, ntt with bit7=bass)."""
    return chunk(b"Cntt", bytes([src_ch & 0x0F, ntt_byte & 0xFF]))


def pack_smf_style(ppqn, raw_events, end_tick, casm_chunk, extra_chunks=()):
    """Assemble format-0 SMF + CASM + optional top-level chunks deterministically."""
    raw_events.sort(key=lambda x: (x[0], x[1], x[2]))
    out, last = bytearray(), 0
    for tick, _, data in raw_events:
        assert tick >= last, f"tick {tick} < last {last}"
        out += vlq(tick - last) + data
        last = tick
    assert end_tick >= last, f"end_tick {end_tick} < last {last}"
    out += vlq(end_tick - last) + bytes([0xFF, 0x2F, 0x00])
    mthd = b"MThd" + struct.pack(">IHHH", 6, 0, 1, ppqn)
    mtrk = b"MTrk" + struct.pack(">I", len(out)) + bytes(out)
    return mthd + mtrk + casm_chunk + b"".join(extra_chunks)


def add_meta(events, tick, typ, data, order=0):
    events.append((tick, order, bytes([0xFF, typ]) + vlq(len(data)) + data))


def add_sysex(events, tick, payload, order=1):
    """payload excludes leading 0xF0; includes trailing 0xF7."""
    events.append((tick, order, bytes([0xF0]) + vlq(len(payload)) + payload))


def add_cc(events, tick, ch, cc, val, order=2):
    events.append((tick, order, bytes([0xB0 | (ch & 0x0F), cc & 0x7F, val & 0x7F])))


def add_pc(events, tick, ch, pc, order=2):
    events.append((tick, order, bytes([0xC0 | (ch & 0x0F), pc & 0x7F])))


def add_pb(events, tick, ch, val14, order=2):
    lsb = val14 & 0x7F
    msb = (val14 >> 7) & 0x7F
    events.append((tick, order, bytes([0xE0 | (ch & 0x0F), lsb, msb])))


def add_note(events, tick, ch, pitch, vel, dur):
    events.append((tick, 4, bytes([0x90 | (ch & 0x0F), pitch & 0x7F, vel & 0x7F])))
    events.append((tick + dur, 3, bytes([0x80 | (ch & 0x0F), pitch & 0x7F, 0])))


def add_sint_mix(events, channel_setup):
    """Write Bank/PC plus full SInt mix CCs (CC7/10/11/91/93) at tick 0."""
    for ch, cfg in sorted(channel_setup.items()):
        msb, lsb, pc, vol, pan, expr, rev, cho = cfg
        add_cc(events, 0, ch, 0, msb, order=1)
        add_cc(events, 0, ch, 32, lsb, order=1)
        add_pc(events, 0, ch, pc, order=2)
        add_cc(events, 0, ch, 7, vol, order=2)
        add_cc(events, 0, ch, 10, pan, order=2)
        add_cc(events, 0, ch, 11, expr, order=2)
        add_cc(events, 0, ch, 91, rev, order=2)
        add_cc(events, 0, ch, 93, cho, order=2)


def wrap_casm_sections(sec_payloads):
    """Wrap list of (sec_name, raw_subchunks_bytes) into Yamaha CASM/CSEG."""
    body = bytearray()
    for i, (name, subchunks) in enumerate(sec_payloads):
        sec = chunk(b"Sdec", name.encode("ascii")) + subchunks
        body += sec if i == 0 else chunk(b"CSEG", sec)
    return chunk(b"CASM", chunk(b"CSEG", bytes(body)))


def add_std_drums_44(events, base, ppqn, name, is_last_end=False):
    q, e, s = ppqn, ppqn // 2, ppqn // 4
    if name == "Fill In BA":
        add_note(events, base, 9, 36, 110, e)
        add_note(events, base, 9, 49, 98, q)
        return
    if is_last_end:
        add_note(events, base, 9, 36, 110, q)
        add_note(events, base, 9, 49, 100, q * 2)
        return
    for i in range(8):
        add_note(events, base + i * e, 9, 42, 86 if i % 2 == 0 else 66, e // 2)
    if not name.startswith("Intro"):
        add_note(events, base, 9, 36, 108, e)
        add_note(events, base + 2 * q, 9, 36, 104, e)
        add_note(events, base + q, 9, 38, 98, e)
        if name.startswith("Fill In"):
            for i in range(4):
                add_note(events, base + 2 * q + i * (q // 2), 9, 38, 78 + 6 * i, s)
        else:
            add_note(events, base + 3 * q, 9, 38, 98, e)


# Fixture 5: SYNTH_SFF1_CTAB_CNTT.sty (SFF1 / 480 PPQN / 4/4, Ctab + Cntt only)
def build_sff1_ctab_cntt():
    ppqn = 480
    bar = ppqn * 4
    q, e = ppqn, ppqn // 2
    ev = []
    add_meta(ev, 0, 0x58, bytes([4, 2, 24, 8]))
    add_meta(ev, 0, 0x51, (535714).to_bytes(3, "big"))  # 112 BPM
    add_meta(ev, 0, 0x06, b"SFF1")
    add_meta(ev, 0, 0x06, b"SInt")
    setup = {
        # ch: (msb, lsb, pc, vol, pan, expr, rev, cho)
        4:  (0, 0, 4, 90, 76, 110, 34, 18),    # routed via Ctab ch 4 -> dest 14 (Acc4)
        8:  (127, 0, 0, 86, 64, 112, 22, 0),
        9:  (127, 0, 0, 106, 64, 118, 24, 0),
        10: (0, 0, 33, 102, 64, 114, 16, 0),
        11: (0, 0, 0, 94, 52, 112, 32, 12),
        13: (0, 0, 89, 82, 72, 108, 48, 24),
    }
    add_sint_mix(ev, setup)

    sections = [("Intro A", 1, 2), ("Main A", 3, 2), ("Main B", 5, 2),
                ("Fill In AA", 7, 1), ("Ending A", 8, 2), ("Fill In BA", 10, 1)]
    for name, first, nbars in sections:
        add_meta(ev, first * bar, 0x06, name.encode("ascii"))
        for rb in range(nbars):
            b = (first + rb) * bar
            last_end = name.startswith("Ending") and rb == nbars - 1
            add_std_drums_44(ev, b, ppqn, name, last_end)
            if name.startswith("Main"):
                for i in range(4):
                    add_note(ev, b + i * q + e, 8, 70, 62, e // 2)
            # Bass
            if name == "Fill In BA" or last_end:
                add_note(ev, b, 10, 36, 100, q)
            else:
                add_note(ev, b, 10, 36, 100, q + e)
                add_note(ev, b + 2 * q, 10, 43, 92, q)
            # Chord1
            if name != "Fill In BA" and not last_end:
                for beat in (1, 3):
                    for p in (60, 64, 67):
                        add_note(ev, b + beat * q, 11, p, 84, e)
            # Routed SFF1 E.Piano on ch 4 -> dest 14
            if name.startswith("Main"):
                for p in (60, 64, 67):
                    add_note(ev, b + 2 * q, 4, p, 78, q)
            # Pad
            if name != "Fill In BA":
                for p in (48, 52, 55):
                    add_note(ev, b, 13, p, 66, bar - 60 if not last_end else q * 2)

    # SFF1 CASM: ONLY Ctab + Cntt (0 Ctb2 blocks!)
    sec_casm = []
    for name, _, _ in sections:
        sub = b"".join([
            ctab_entry("Rhythm1", 8, dest_ch=8, ntr=1, ntt=0, hk=6, lo=0, hi=127, rtr=1),
            ctab_entry("Rhythm2", 9, dest_ch=9, ntr=1, ntt=0, hk=6, lo=0, hi=127, rtr=1),
            ctab_entry("Bass", 10, dest_ch=10, src_root=0, src_type=0, ntr=0, ntt=1, hk=4, lo=28, hi=55, rtr=4),
            cntt_entry(10, 0x81),  # SFF1 Cntt adds bit7 bass flag!
            ctab_entry("Piano", 11, dest_ch=11, src_root=0, src_type=0, ntr=0, ntt=2, hk=7, lo=48, hi=84, rtr=3),
            ctab_entry("Pad", 13, dest_ch=13, src_root=0, src_type=0, ntr=0, ntt=2, hk=7, lo=48, hi=84, rtr=1),
            ctab_entry("E.Piano", 4, dest_ch=14, src_root=0, src_type=0, ntr=1, ntt=2, hk=7, lo=48, hi=84, rtr=2),
            cntt_entry(4, 0x02),
        ])
        sec_casm.append((name, sub))
    return pack_smf_style(ppqn, ev, 11 * bar, wrap_casm_sections(sec_casm))


# Fixture 6: SYNTH_SRC_CHORD_NON_C.sty (non-C / non-Major source chords + limits)
def build_src_chord_non_c():
    ppqn = PPQN
    bar = ppqn * 4
    q, e = ppqn, ppqn // 2
    ev = []
    add_meta(ev, 0, 0x58, bytes([4, 2, 24, 8]))
    add_meta(ev, 0, 0x51, (508475).to_bytes(3, "big"))  # 118 BPM
    add_meta(ev, 0, 0x06, b"SFF2")
    add_meta(ev, 0, 0x06, b"SInt")
    setup = {
        9:  (127, 0, 0, 108, 64, 116, 20, 0),
        10: (0, 0, 33, 102, 64, 114, 16, 0),
        11: (0, 0, 4, 92, 54, 112, 34, 14),
        13: (0, 0, 89, 80, 74, 106, 46, 20),
    }
    add_sint_mix(ev, setup)
    sections = [("Intro A", 1, 2), ("Main A", 3, 2), ("Main B", 5, 2),
                ("Fill In AA", 7, 1), ("Ending A", 8, 2), ("Fill In BA", 10, 1)]
    for name, first, nbars in sections:
        add_meta(ev, first * bar, 0x06, name.encode("ascii"))
        for rb in range(nbars):
            b = (first + rb) * bar
            last_end = name.startswith("Ending") and rb == nbars - 1
            add_std_drums_44(ev, b, ppqn, name, last_end)
            if name == "Main B":
                # Recorded in D minor (root=2, type=8): D F A
                add_note(ev, b, 10, 38, 100, q + e)      # D2
                add_note(ev, b + 2 * q, 10, 45, 92, q)   # A2
                for beat in (1, 3):
                    for p in (62, 65, 69):               # D4 F4 A4
                        add_note(ev, b + beat * q, 11, p, 84, e)
                for p in (50, 53, 57):                   # D3 F3 A3
                    add_note(ev, b, 13, p, 64, bar - 240)
            elif name == "Fill In BA" or last_end:
                add_note(ev, b, 10, 36, 100, q)
                for p in (48, 51, 55):
                    add_note(ev, b, 13, p, 64, q * 2)
            else:
                # Recorded in C min7 (Bass/Chord1) and G7 (Pad)
                add_note(ev, b, 10, 36, 100, q + e)      # C2
                add_note(ev, b + 2 * q, 10, 43, 92, q)   # G2
                for beat in (1, 3):
                    for p in (60, 63, 67, 70):           # C4 Eb4 G4 Bb4 (Cm7)
                        add_note(ev, b + beat * q, 11, p, 84, e)
                for p in (55, 59, 62, 65):               # G3 B3 D4 F4 (G7)
                    add_note(ev, b, 13, p, 64, bar - 240)

    sec_casm = []
    for name, _, _ in sections:
        if name == "Main B":
            sub = b"".join([
                ctb2_full("Rhythm2", 9, mid=(1, 0, 6, 0, 127, 1)),
                ctb2_full("Bass", 10, src_root=2, src_type=8, mid=(0, 0x81, 2, 28, 52, 4)),
                ctb2_full("Chord1", 11, src_root=2, src_type=8, mid=(0, 2, 5, 48, 84, 3)),
                ctb2_full("Pad", 13, src_root=2, src_type=8, mid=(1, 2, 5, 48, 84, 1)),
            ])
        else:
            sub = b"".join([
                ctb2_full("Rhythm2", 9, mid=(1, 0, 6, 0, 127, 1)),
                ctb2_full("Bass", 10, src_root=0, src_type=11, mid=(0, 0x81, 2, 28, 52, 4)),
                ctb2_full("Chord1", 11, src_root=0, src_type=11, mid=(0, 2, 5, 48, 84, 3)),
                ctb2_full("Pad", 13, src_root=7, src_type=19, mid=(0, 2, 6, 48, 84, 1)),
            ])
        sec_casm.append((name, sub))
    return pack_smf_style(ppqn, ev, 11 * bar, wrap_casm_sections(sec_casm))


# Fixture 7: SYNTH_CHORD_MUTE_SPLIT.sty (mutually exclusive chord-mute channel pairs)
def build_chord_mute_split():
    ppqn = PPQN
    bar = ppqn * 4
    q, e = ppqn, ppqn // 2
    ev = []
    add_meta(ev, 0, 0x58, bytes([4, 2, 24, 8]))
    add_meta(ev, 0, 0x51, (545455).to_bytes(3, "big"))  # 110 BPM
    add_meta(ev, 0, 0x06, b"SFF2")
    add_meta(ev, 0, 0x06, b"SInt")
    setup = {
        3:  (0, 117, 4, 92, 56, 112, 32, 16),  # E.P rt -> Acc4 (Major only)
        6:  (0, 117, 57, 90, 74, 110, 28, 8),  # Tbn Bs -> Acc5 (Major only)
        9:  (127, 0, 0, 108, 64, 118, 22, 0),
        10: (0, 0, 33, 102, 64, 114, 16, 0),
        14: (0, 117, 4, 92, 56, 112, 32, 16),  # E.Piano -> Acc4 (Minor/7th only)
        15: (0, 117, 57, 90, 74, 110, 28, 8),  # Tbn -> Acc5 (Minor/7th only)
    }
    add_sint_mix(ev, setup)
    sections = [("Intro A", 1, 2), ("Main A", 3, 2), ("Main B", 5, 2),
                ("Fill In AA", 7, 1), ("Ending A", 8, 2), ("Fill In BA", 10, 1)]
    for name, first, nbars in sections:
        add_meta(ev, first * bar, 0x06, name.encode("ascii"))
        for rb in range(nbars):
            b = (first + rb) * bar
            last_end = name.startswith("Ending") and rb == nbars - 1
            add_std_drums_44(ev, b, ppqn, name, last_end)
            add_note(ev, b, 10, 36, 100, q + e if name != "Fill In BA" else q)
            if name != "Fill In BA" and not last_end:
                add_note(ev, b + 2 * q, 10, 43, 92, q)
                # Pair 1 -> Acc4: Major triad on ch 3 vs Minor triad on ch 14
                for beat in (1, 3):
                    for p in (60, 64, 67):
                        add_note(ev, b + beat * q, 3, p, 84, e)
                    for p in (60, 63, 67):
                        add_note(ev, b + beat * q, 14, p, 84, e)
                # Pair 2 -> Acc5: Major brass on ch 6 vs Minor7 brass on ch 15
                for p in (55, 60, 64):
                    add_note(ev, b, 6, p, 82, q * 2)
                for p in (55, 60, 63, 70):
                    add_note(ev, b, 15, p, 82, q * 2)
            elif last_end:
                for p in (60, 64, 67):
                    add_note(ev, b, 3, p, 84, q * 2)
                for p in (60, 63, 67):
                    add_note(ev, b, 14, p, 84, q * 2)

    # Mutually exclusive 34-bit chord mute bitmasks
    kMajMask = 0x000000007F
    kMin7Mask = 0x00000FF800
    sec_casm = []
    for name, _, _ in sections:
        sub = b"".join([
            ctb2_full("Rhythm2", 9, dest_ch=9, mid=(1, 0, 6, 0, 127, 1)),
            ctb2_full("Bass", 10, dest_ch=10, mid=(0, 0x81, 3, 28, 55, 4)),
            ctb2_full("E.P  rt", 3, dest_ch=14, note_mute=0x0FFF, chord_mute=kMajMask, src_root=0, src_type=0, mid=(0, 2, 7, 48, 84, 3)),
            ctb2_full("E.Piano", 14, dest_ch=14, note_mute=0x0FFF, chord_mute=kMin7Mask, src_root=0, src_type=8, mid=(0, 2, 7, 48, 84, 3)),
            ctb2_full("Tbn  Bs", 6, dest_ch=15, note_mute=0x0FFF, chord_mute=kMajMask, src_root=0, src_type=0, mid=(0, 2, 7, 48, 84, 3)),
            ctb2_full("Tbn", 15, dest_ch=15, note_mute=0x0FFF, chord_mute=kMin7Mask, src_root=0, src_type=11, mid=(0, 2, 7, 48, 84, 3)),
        ])
        sec_casm.append((name, sub))
    return pack_smf_style(ppqn, ev, 11 * bar, wrap_casm_sections(sec_casm))


# Fixture 8: SYNTH_MEGAVOICE_GUITAR_BASS.sty (MegaVoice 63/120 & 63/60, 0..11, 96..124, 4 vel zones)
def build_megavoice_guitar_bass():
    ppqn = PPQN
    bar = ppqn * 4
    q, e = ppqn, ppqn // 2
    ev = []
    add_meta(ev, 0, 0x58, bytes([4, 2, 24, 8]))
    add_meta(ev, 0, 0x51, (576923).to_bytes(3, "big"))  # 104 BPM
    add_meta(ev, 0, 0x06, b"SFF2")
    add_meta(ev, 0, 0x06, b"SInt")
    setup = {
        9:  (127, 0, 0, 108, 64, 116, 20, 0),
        10: (63, 60, 23, 104, 64, 118, 12, 0),    # MegaVoice Finger Bass
        11: (0, 0, 0, 88, 50, 110, 30, 10),
        12: (63, 120, 21, 98, 78, 114, 28, 14),   # MegaVoice Steel Guitar
        13: (0, 0, 89, 78, 64, 104, 44, 22),
    }
    add_sint_mix(ev, setup)
    sections = [("Intro A", 1, 2), ("Main A", 3, 2), ("Main B", 5, 2),
                ("Fill In AA", 7, 1), ("Ending A", 8, 2), ("Fill In BA", 10, 1)]
    for name, first, nbars in sections:
        add_meta(ev, first * bar, 0x06, name.encode("ascii"))
        for rb in range(nbars):
            b = (first + rb) * bar
            last_end = name.startswith("Ending") and rb == nbars - 1
            add_std_drums_44(ev, b, ppqn, name, last_end)
            # MegaVoice Bass: normal zone (vel 44-56), mute/dead (vel 78-84), harmonic (95),
            # chord-range 0..11 (pitch 4) and >=C7 FX keys (96, 102, 108 at vel 116-124)
            add_note(ev, b, 10, 36, 48, q + e)
            if name != "Fill In BA" and not last_end:
                add_note(ev, b + q, 10, 4, 80, e // 2)           # 0..11 chord-event range
                add_note(ev, b + q + e, 10, 36, 82, e // 2)      # dead note zone (65-90)
                add_note(ev, b + 2 * q, 10, 43, 52, q)           # normal zone (1-64)
                add_note(ev, b + 3 * q, 10, 48, 95, e // 2)      # harmonic zone (91-100)
                add_note(ev, b + 3 * q + e, 10, 102, 120, e // 4) # >=C7 fret noise (101-127)
                # Chord1
                for beat in (1, 3):
                    for p in (60, 64, 67):
                        add_note(ev, b + beat * q, 11, p, 80, e)
                # MegaVoice Steel Guitar: 4 velocity zones + 0..11 + >=C7 slide/body FX
                add_note(ev, b, 12, 5, 76, e // 4)               # 0..11 chord-event range
                for k, p in enumerate((52, 55, 60, 64)):         # normal strum (vel 40..52)
                    add_note(ev, b + 30 * k, 12, p, 42 + 3 * k, q)
                for k, p in enumerate((52, 55, 60, 64)):         # muted strum (vel 78..84)
                    add_note(ev, b + q + 25 * k, 12, p, 78 + 2 * k, e // 2)
                add_note(ev, b + 2 * q, 12, 67, 94, e)           # harmonic zone (91-100)
                for k, fx in enumerate((98, 104, 112, 120)):     # >=C7 FX/body-knock (101-127)
                    add_note(ev, b + 2 * q + e + k * 240, 12, fx, 114 + 3 * k, 180)
                # Pad
                for p in (48, 52, 55):
                    add_note(ev, b, 13, p, 64, bar - 240)

    sec_casm = []
    for name, _, _ in sections:
        sub = b"".join([
            ctb2_full("Rhythm2", 9, mid=(1, 0, 6, 0, 127, 1)),
            ctb2_full("MegaBass", 10, mid=(0, 0x81, 3, 28, 64, 4)),
            ctb2_full("Chord1", 11, mid=(0, 2, 7, 48, 84, 3)),
            ctb2_full("MegaGtr", 12, mid=(2, 1, 11, 40, 88, 3)),
            ctb2_full("Pad", 13, mid=(0, 2, 7, 48, 84, 1)),
        ])
        sec_casm.append((name, sub))
    return pack_smf_style(ppqn, ev, 11 * bar, wrap_casm_sections(sec_casm))


# Fixture 9: SYNTH_REVO_XG_DRUMS.sty (Revo!/XG extended keys 13..34, 82..105 + swapped Rhythm1/2)
def build_revo_xg_drums():
    ppqn = PPQN
    bar = ppqn * 4
    q, e = ppqn, ppqn // 2
    ev = []
    add_meta(ev, 0, 0x58, bytes([4, 2, 24, 8]))
    add_meta(ev, 0, 0x51, (483871).to_bytes(3, "big"))  # 124 BPM
    add_meta(ev, 0, 0x06, b"SFF2")
    add_meta(ev, 0, 0x06, b"SInt")
    setup = {
        8:  (62, 100, 1, 108, 64, 118, 22, 0),   # Revo! Standard Kit on Rhythm1 (SWAPPED primary!)
        9:  (127, 0, 40, 92, 64, 112, 26, 0),    # XG Brush/Perc on Rhythm2 (secondary!)
        10: (0, 0, 33, 102, 64, 114, 16, 0),
        11: (0, 0, 0, 90, 56, 110, 30, 10),
        13: (0, 0, 89, 80, 72, 104, 44, 20),
    }
    add_sint_mix(ev, setup)
    sections = [("Intro A", 1, 2), ("Main A", 3, 2), ("Main B", 5, 2),
                ("Fill In AA", 7, 1), ("Ending A", 8, 2), ("Fill In BA", 10, 1)]
    ext_low = (13, 14, 16, 18, 21, 25, 29, 34)
    ext_high = (82, 83, 84, 88, 91, 95, 100, 105)
    for name, first, nbars in sections:
        add_meta(ev, first * bar, 0x06, name.encode("ascii"))
        if name == "Main B":
            # Mid-style drum kit change on ch 9 -> XG Latin/Ethnic kit 127/0/87
            add_cc(ev, first * bar, 9, 0, 127, order=1)
            add_cc(ev, first * bar, 9, 32, 0, order=1)
            add_pc(ev, first * bar, 9, 87, order=2)
        for rb in range(nbars):
            b = (first + rb) * bar
            last_end = name.startswith("Ending") and rb == nbars - 1
            # SWAPPED: ch 8 (Rhythm1) holds main Kick/Snare/Hat + Revo! extended keys
            add_note(ev, b, 8, 36, 112, e)
            if name != "Fill In BA" and not last_end:
                add_note(ev, b + 2 * q, 8, 36, 106, e)
                add_note(ev, b + q, 8, 38, 102, e)
                add_note(ev, b + 3 * q, 8, 38, 100, e)
                for i in range(8):
                    add_note(ev, b + i * e, 8, 42, 88 if i % 2 == 0 else 64, e // 2)
                # Dense Revo!/XG extended drum keys on ch 8
                for i, p in enumerate(ext_low):
                    add_note(ev, b + i * e + 60, 8, p, 72 + 4 * i, e // 4)
                # Secondary percussion + extended keys on ch 9
                for i in range(4):
                    add_note(ev, b + i * q + e, 9, 70 if i % 2 == 0 else 54, 68, e // 2)
                for i, p in enumerate(ext_high):
                    add_note(ev, b + i * e + 120, 9, p, 66 + 4 * i, e // 4)
            # Bass / Chord1 / Pad
            add_note(ev, b, 10, 36, 100, q + e if name != "Fill In BA" else q)
            if name != "Fill In BA" and not last_end:
                add_note(ev, b + 2 * q, 10, 43, 92, q)
                for beat in (1, 3):
                    for p in (60, 64, 67):
                        add_note(ev, b + beat * q, 11, p, 84, e)
                for p in (48, 52, 55):
                    add_note(ev, b, 13, p, 64, bar - 240)

    sec_casm = []
    for name, _, _ in sections:
        sub = b"".join([
            ctb2_full("Rhythm1", 8, mid=(1, 0, 6, 0, 127, 1)),
            ctb2_full("Rhythm2", 9, mid=(1, 0, 6, 0, 127, 1)),
            ctb2_full("Bass", 10, mid=(0, 0x81, 3, 28, 55, 4)),
            ctb2_full("Chord1", 11, mid=(0, 2, 7, 48, 84, 3)),
            ctb2_full("Pad", 13, mid=(0, 2, 7, 48, 84, 1)),
        ])
        sec_casm.append((name, sub))
    return pack_smf_style(ppqn, ev, 11 * bar, wrap_casm_sections(sec_casm))


# Fixture 10: SYNTH_MIX_SYSEX_CC_HYGIENE.sty (SInt mix CCs, XG SysEx, NRPN/CC94, open CC64 & PB)
def build_mix_sysex_cc_hygiene():
    ppqn = PPQN
    bar = ppqn * 4
    q, e = ppqn, ppqn // 2
    ev = []
    add_meta(ev, 0, 0x58, bytes([4, 2, 24, 8]))
    add_meta(ev, 0, 0x51, (517241).to_bytes(3, "big"))  # 116 BPM
    add_meta(ev, 0, 0x06, b"SFF2")
    add_meta(ev, 0, 0x06, b"SInt")
    # 6 Yamaha XG SysEx messages in SInt (F0 43 10 4C ...)
    add_sysex(ev, 0, bytes([0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7]))        # XG System On
    add_sysex(ev, 0, bytes([0x43, 0x10, 0x4C, 0x02, 0x01, 0x00, 0x01, 0x00, 0xF7]))  # Reverb Type Hall1
    add_sysex(ev, 0, bytes([0x43, 0x10, 0x4C, 0x02, 0x01, 0x20, 0x41, 0x00, 0xF7]))  # Chorus Type
    add_sysex(ev, 0, bytes([0x43, 0x10, 0x4C, 0x08, 0x0A, 0x0B, 0x64, 0xF7]))        # Part 11 Volume
    add_sysex(ev, 0, bytes([0x43, 0x10, 0x4C, 0x08, 0x0B, 0x0E, 0x48, 0xF7]))        # Part 12 Pan
    add_sysex(ev, 0, bytes([0x43, 0x10, 0x4C, 0x02, 0x40, 0x00, 0x02, 0x48, 0xF7]))  # Master EQ Low Gain

    setup = {
        8:  (127, 0, 0, 86, 64, 110, 24, 0),
        9:  (127, 0, 0, 108, 64, 118, 22, 0),
        10: (0, 0, 33, 102, 64, 114, 14, 0),
        11: (0, 0, 0, 94, 52, 112, 36, 18),
        13: (0, 0, 89, 82, 74, 106, 48, 24),
    }
    add_sint_mix(ev, setup)

    sections = [("Intro A", 1, 2), ("Main A", 3, 2), ("Main B", 5, 2),
                ("Fill In AA", 7, 1), ("Ending A", 8, 2), ("Fill In BA", 10, 1)]
    for name, first, nbars in sections:
        sec_start = first * bar
        add_meta(ev, sec_start, 0x06, name.encode("ascii"))
        if name in ("Main A", "Main B"):
            # Yamaha proprietary NRPN + raw CC94 Variation Send
            add_cc(ev, sec_start, 11, 99, 24)
            add_cc(ev, sec_start, 11, 98, 36)
            add_cc(ev, sec_start, 11, 6, 64)
            add_cc(ev, sec_start, 11, 94, 40)
            # Open CC64 sustain left ON at end of section!
            add_cc(ev, sec_start + q, 11, 64, 127)
            # Non-centered Pitch Bend left uncentered at end of section!
            add_pb(ev, sec_start + 2 * q, 10, 10000)
        for rb in range(nbars):
            b = (first + rb) * bar
            last_end = name.startswith("Ending") and rb == nbars - 1
            add_std_drums_44(ev, b, ppqn, name, last_end)
            if name.startswith("Main"):
                for i in range(4):
                    add_note(ev, b + i * q + e, 8, 70, 62, e // 2)
            add_note(ev, b, 10, 36, 100, q + e if name != "Fill In BA" else q)
            if name != "Fill In BA" and not last_end:
                add_note(ev, b + 2 * q, 10, 43, 92, q)
                for beat in (1, 3):
                    for p in (60, 64, 67):
                        add_note(ev, b + beat * q, 11, p, 84, e)
                for p in (48, 52, 55):
                    add_note(ev, b, 13, p, 64, bar - 240)

    sec_casm = []
    for name, _, _ in sections:
        sub = b"".join([
            ctb2_full("Rhythm1", 8, mid=(1, 0, 6, 0, 127, 1)),
            ctb2_full("Rhythm2", 9, mid=(1, 0, 6, 0, 127, 1)),
            ctb2_full("Bass", 10, mid=(0, 0x81, 3, 28, 55, 4)),
            ctb2_full("Chord1", 11, mid=(0, 2, 7, 48, 84, 3)),
            ctb2_full("Pad", 13, mid=(0, 2, 7, 48, 84, 1)),
        ])
        sec_casm.append((name, sub))
    return pack_smf_style(ppqn, ev, 11 * bar, wrap_casm_sections(sec_casm))


# Fixture 11: SYNTH_MIXED_METER_RUBATO.sty (6/8 Intro + 4/4 Main + 2/4 Fill + Ending ritard + flams)
def build_mixed_meter_rubato():
    ppqn = PPQN
    q, e = ppqn, ppqn // 2
    bar44 = ppqn * 4   # 7680
    bar24 = ppqn * 2   # 3840
    bar68 = ppqn * 3   # 5760
    ev = []
    add_meta(ev, 0, 0x58, bytes([6, 3, 36, 8]))           # Initial 6/8 for Intro A
    add_meta(ev, 0, 0x51, (500000).to_bytes(3, "big"))    # 120 BPM
    add_meta(ev, 0, 0x06, b"SFF2")
    add_meta(ev, 0, 0x06, b"SInt")
    setup = {
        9:  (127, 0, 0, 108, 64, 118, 22, 0),
        10: (0, 0, 33, 102, 64, 114, 16, 0),
        11: (0, 0, 0, 92, 54, 112, 32, 12),
        13: (0, 0, 89, 80, 72, 106, 44, 20),
    }
    add_sint_mix(ev, setup)

    # Explicit tick layout across mixed meters
    # Intro A: 2 bars of 6/8 starting at bar68
    t = bar68
    add_meta(ev, t, 0x06, b"Intro A")
    for rb in range(2):
        b = t + rb * bar68
        for i in range(6):
            add_note(ev, b + i * e, 9, 42, 84 if i % 3 == 0 else 64, e // 2)
        add_note(ev, b, 9, 36, 106, e)
        add_note(ev, b + 3 * e, 9, 38, 94, e)
        add_note(ev, b, 10, 36, 100, 3 * e - 120)
        add_note(ev, b + 3 * e, 10, 43, 92, 3 * e - 120)
        for p in (48, 52, 55):
            add_note(ev, b, 13, p, 64, bar68 - 240)
    t += 2 * bar68

    # Main A: 2 bars of 4/4 (FF 58 -> 4/4) + 15-tick snare flams + 1-tick anticipation
    add_meta(ev, t, 0x58, bytes([4, 2, 24, 8]))
    add_meta(ev, t, 0x06, b"Main A")
    for rb in range(2):
        b = t + rb * bar44
        add_std_drums_44(ev, b, ppqn, "Main A")
        # Rapid 15-tick snare flam (stresses 1920 -> 384 PPQN rescale!)
        add_note(ev, b + q - 15, 9, 38, 62, 10)
        add_note(ev, b, 10, 36, 100, q + e)
        add_note(ev, b + 2 * q, 10, 43, 92, q)
        for beat in (1, 3):
            for p in (60, 64, 67):
                add_note(ev, b + beat * q, 11, p, 84, e)
        for p in (48, 52, 55):
            add_note(ev, b, 13, p, 64, bar44 - 240)
    # 1-tick downbeat anticipation right at end of Main A (tick len - 1)
    add_note(ev, t + 2 * bar44 - 1, 10, 36, 86, 1)
    t += 2 * bar44

    # Main B: 2 bars of 4/4
    add_meta(ev, t, 0x06, b"Main B")
    for rb in range(2):
        b = t + rb * bar44
        add_std_drums_44(ev, b, ppqn, "Main B")
        add_note(ev, b, 10, 36, 100, q + e)
        add_note(ev, b + 2 * q, 10, 43, 92, q)
        for beat in (1, 3):
            for p in (60, 64, 67, 71):
                add_note(ev, b + beat * q, 11, p, 84, e)
        for p in (48, 52, 55, 59):
            add_note(ev, b, 13, p, 64, bar44 - 240)
    t += 2 * bar44

    # Fill In AA: 1 bar of 2/4 (FF 58 -> 2/4)
    add_meta(ev, t, 0x58, bytes([2, 2, 24, 8]))
    add_meta(ev, t, 0x06, b"Fill In AA")
    for i in range(4):
        add_note(ev, t + i * e, 9, 38, 82 + 6 * i, e // 2)
    add_note(ev, t, 9, 36, 110, e)
    add_note(ev, t, 10, 36, 100, q)
    add_note(ev, t + q, 10, 43, 94, q - 120)
    for p in (60, 64, 67):
        add_note(ev, t + q, 11, p, 84, e)
    t += bar24

    # Fill In BA: 1 bar of 4/4 (FF 58 -> 4/4)
    add_meta(ev, t, 0x58, bytes([4, 2, 24, 8]))
    add_meta(ev, t, 0x06, b"Fill In BA")
    add_std_drums_44(ev, t, ppqn, "Fill In BA")
    add_note(ev, t, 10, 36, 100, q)
    t += bar44

    # Ending A: 2 bars of 4/4 with mid-Ending FF 51 ritardando tempo ramp
    add_meta(ev, t, 0x06, b"Ending A")
    add_meta(ev, t + bar44, 0x51, (600000).to_bytes(3, "big"))          # 100 BPM
    add_meta(ev, t + bar44 + 2 * q, 0x51, (750000).to_bytes(3, "big"))  # 80 BPM
    for rb in range(2):
        b = t + rb * bar44
        last_end = rb == 1
        add_std_drums_44(ev, b, ppqn, "Ending A", last_end)
        add_note(ev, b, 10, 36, 100, q)
        for p in (48, 52, 55):
            add_note(ev, b, 13, p, 64, q * 2 if last_end else bar44 - 240)
    t += 2 * bar44

    sec_names = ["Intro A", "Main A", "Main B", "Fill In AA", "Fill In BA", "Ending A"]
    sec_casm = []
    for name in sec_names:
        sub = b"".join([
            ctb2_full("Rhythm2", 9, mid=(1, 0, 6, 0, 127, 1)),
            ctb2_full("Bass", 10, mid=(0, 0x81, 3, 28, 55, 4)),
            ctb2_full("Chord1", 11, mid=(0, 2, 7, 48, 84, 3)),
            ctb2_full("Pad", 13, mid=(0, 2, 7, 48, 84, 1)),
        ])
        sec_casm.append((name, sub))
    return pack_smf_style(ppqn, ev, t, wrap_casm_sections(sec_casm))


# Fixture 12: SYNTH_MULTI_ZONE_SART_OTS.sty (multi-zone CASM, S.Art key-switches, mid-style voice, OTSc/FNRP/MDB)
def build_multi_zone_sart_ots():
    ppqn = PPQN
    bar = ppqn * 4
    q, e = ppqn, ppqn // 2
    ev = []
    add_meta(ev, 0, 0x58, bytes([4, 2, 24, 8]))
    add_meta(ev, 0, 0x51, (612245).to_bytes(3, "big"))  # 98 BPM
    add_meta(ev, 0, 0x06, b"SFF2")
    add_meta(ev, 0, 0x06, b"SInt")
    setup = {
        9:  (127, 0, 0, 108, 64, 118, 22, 0),
        10: (0, 0, 33, 102, 64, 114, 16, 0),
        11: (0, 0, 0, 92, 52, 112, 32, 10),      # Intro A: Acoustic Piano 0/0/0
        13: (0, 0, 89, 80, 74, 106, 46, 20),
        14: (8, 64, 66, 94, 68, 114, 36, 12),    # S.Art Sax (bank 8/64/66)
    }
    add_sint_mix(ev, setup)
    sections = [("Intro A", 1, 2), ("Main A", 3, 2), ("Main B", 5, 2),
                ("Fill In AA", 7, 1), ("Ending A", 8, 2), ("Fill In BA", 10, 1)]
    for name, first, nbars in sections:
        sec_start = first * bar
        add_meta(ev, sec_start, 0x06, name.encode("ascii"))
        if name == "Main A":
            # Mid-style voice change on ch 11: Acoustic Piano -> Electric Piano 104/7/4
            add_cc(ev, sec_start, 11, 0, 104, order=1)
            add_cc(ev, sec_start, 11, 32, 7, order=1)
            add_pc(ev, sec_start, 11, 4, order=2)
        for rb in range(nbars):
            b = (first + rb) * bar
            last_end = name.startswith("Ending") and rb == nbars - 1
            add_std_drums_44(ev, b, ppqn, name, last_end)
            add_note(ev, b, 10, 36, 100, q + e if name != "Fill In BA" else q)
            if name != "Fill In BA" and not last_end:
                add_note(ev, b + 2 * q, 10, 43, 92, q)
                # Multi-zone Chord1 spanning Low (<48), Mid (48..72), High (>72)
                add_note(ev, b, 11, 43, 82, q)
                for p in (60, 64, 67):
                    add_note(ev, b + q, 11, p, 84, e)
                add_note(ev, b + 3 * q, 11, 76, 80, e)
                # S.Art Sax on ch 14 with key-switches 12, 15, 19 (C0..B0) + expressive CC1/CC11
                add_note(ev, b, 14, 12, 64, 120)          # S.Art key-switch C0
                add_cc(ev, b, 14, 1, 24)
                add_note(ev, b + 120, 14, 64, 88, q)
                add_note(ev, b + q + e, 14, 15, 64, 120)  # S.Art key-switch D#0
                add_note(ev, b + 2 * q, 14, 67, 92, q)
                add_note(ev, b + 3 * q, 14, 19, 64, 120)  # S.Art key-switch G0
                add_note(ev, b + 3 * q + 120, 14, 72, 86, e)
                # Pad
                for p in (48, 52, 55):
                    add_note(ev, b, 13, p, 64, bar - 240)

    sec_casm = []
    for name, _, _ in sections:
        sub = b"".join([
            ctb2_full("Rhythm2", 9, mid=(1, 0, 6, 0, 127, 1)),
            ctb2_full("Bass", 10, mid=(0, 0x81, 3, 28, 55, 4)),
            ctb2_full(
                "Chord1", 11, editable=1, zone_low=48, zone_high=72,
                low=(0, 0x81, 5, 28, 52, 1),
                mid=(0, 2, 7, 48, 72, 3),
                high=(1, 1, 11, 72, 96, 2),
            ),
            ctb2_full("Pad", 13, mid=(0, 2, 7, 48, 84, 1)),
            ctb2_full("SArtSax", 14, mid=(0, 1, 7, 48, 84, 3)),
        ])
        sec_casm.append((name, sub))

    extra = (
        chunk(b"OTSc", b"SYNTH_OTS_STUB_01"),
        chunk(b"FNRP", b"SYNTH_FNRP_STUB_1"),
        chunk(b"MDB ", b"SYNTH_MDB_STUB_01"),
    )
    return pack_smf_style(ppqn, ev, 11 * bar, wrap_casm_sections(sec_casm), extra_chunks=extra)


Q1_BUILDERS = {
    "SYNTH_SFF1_CTAB_CNTT.sty": build_sff1_ctab_cntt,
    "SYNTH_SRC_CHORD_NON_C.sty": build_src_chord_non_c,
    "SYNTH_CHORD_MUTE_SPLIT.sty": build_chord_mute_split,
    "SYNTH_MEGAVOICE_GUITAR_BASS.sty": build_megavoice_guitar_bass,
    "SYNTH_REVO_XG_DRUMS.sty": build_revo_xg_drums,
    "SYNTH_MIX_SYSEX_CC_HYGIENE.sty": build_mix_sysex_cc_hygiene,
    "SYNTH_MIXED_METER_RUBATO.sty": build_mixed_meter_rubato,
    "SYNTH_MULTI_ZONE_SART_OTS.sty": build_multi_zone_sart_ots,
}


def main():
    if len(sys.argv) != 2 or not os.path.isdir(sys.argv[1]):
        sys.exit(__doc__)
    for name, spec in SPECS.items():
        data = build(spec)
        path = os.path.join(sys.argv[1], name)
        with open(path, "wb") as f:
            f.write(data)
        print(f"{path}: {len(data)} bytes")
    for name, fn in Q1_BUILDERS.items():
        data = fn()
        path = os.path.join(sys.argv[1], name)
        with open(path, "wb") as f:
            f.write(data)
        print(f"{path}: {len(data)} bytes")


if __name__ == "__main__":
    main()
