#!/usr/bin/env python3
"""Generate the ORIGINAL input samples for the Pa5X KST test dataset (D5).

docs/research/KST_TEST_DATASET_SPEC.md: the owner imports these WAVs on the
Pa5X, so every sample in the resulting KST has a known origin (this script)
and known properties (length, rate, root, loop). Each file is a plain sine
tone, 16-bit mono PCM, with a distinct frequency and length so it can be told
apart in PA Manager's Sample list and in any later byte-level comparison.

  python3 tools/make_kst_test_samples.py <out-dir>    # writes 5 WAVs + MANIFEST.txt
  python3 tools/make_kst_test_samples.py --self-test

Standard library only. Output is deterministic (same bytes on every run).
"""
import hashlib, math, os, struct, sys

RATE = 48000
# name, MIDI root note, seconds. Lengths differ so file sizes differ.
SAMPLES = [
    ("T_SINE_C3", 48, 0.50),
    ("T_SINE_C4", 60, 0.75),
    ("T_SINE_C5", 72, 1.00),
    ("T_SINE_G4", 67, 1.25),
    ("T_CLICK",   60, 0.05),   # short decaying burst, for the Drum Kit keys
]

def freq(note):
    return 440.0 * 2 ** ((note - 69) / 12)

def pcm(note, seconds, click=False):
    n = int(RATE * seconds)
    f = freq(note)
    out = bytearray()
    for i in range(n):
        env = math.exp(-i / (RATE * 0.01)) if click else min(1.0, i / 480, (n - i) / 480)  # 10 ms fade in/out
        out += struct.pack("<h", int(round(12000 * env * math.sin(2 * math.pi * f * i / RATE))))
    return bytes(out)

def wav(data):
    fmt = struct.pack("<HHIIHH", 1, 1, RATE, RATE * 2, 2, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body

def build():
    return {name: wav(pcm(note, sec, name == "T_CLICK")) for name, note, sec in SAMPLES}

def self_test():
    files = build()
    assert len(files) == 5
    for (name, note, sec), (_, b) in zip(SAMPLES, files.items()):
        assert b[:4] == b"RIFF" and b[8:12] == b"WAVE", name
        assert struct.unpack("<I", b[4:8])[0] == len(b) - 8, name
        ch, rate, bits = struct.unpack("<H", b[22:24])[0], struct.unpack("<I", b[24:28])[0], struct.unpack("<H", b[34:36])[0]
        assert (ch, rate, bits) == (1, RATE, 16), name
        assert struct.unpack("<I", b[40:44])[0] == int(RATE * sec) * 2, name
    sizes = [len(b) for b in files.values()]
    assert len(set(sizes)) == len(sizes), "sizes must differ"
    assert build() == files, "must be deterministic"
    print("self-test OK")

def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    if len(argv) != 2:
        sys.exit(__doc__)
    out = argv[1]
    os.makedirs(out, exist_ok=True)
    lines = []
    for (name, note, sec), (_, b) in zip(SAMPLES, build().items()):
        path = os.path.join(out, name + ".wav")
        with open(path, "wb") as fh:
            fh.write(b)
        lines.append(f"{name}.wav  root={note}  {sec}s  {RATE}Hz/16/mono  {len(b)} bytes  sha256={hashlib.sha256(b).hexdigest()}")
    with open(os.path.join(out, "MANIFEST.txt"), "w") as fh:
        fh.write("\n".join(lines) + "\n")
    print("\n".join(lines))

if __name__ == "__main__":
    main(sys.argv)
