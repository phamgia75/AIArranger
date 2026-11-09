#!/usr/bin/env python3
"""Write two ORIGINAL synthetic KST-style folders for hands-on tests of the KST Library tab.

The KST tools (Inventory, Compare, Merge Plan) work on files and bytes only; they never decode
content. These folders mimic a KST folder layout with the Pa5X's documented extensions (manual p.721) with placeholder bytes made here, so a tester can
exercise every tab action without touching an owner's or a third party's KST data. The files are
NOT Korg formats and must never be loaded on an instrument.

  python3 tools/make_synthetic_kst_library.py <out-dir>   # writes <out-dir>/SYNTH_A.KST, SYNTH_B.KST
  python3 tools/make_synthetic_kst_library.py --self-test

Designed differences between A and B (what Compare and Merge Plan should report):
  same      STYLE/BANK01.STG, PAD/PAD01.PDC, KEYBOARD_SET/KBS01.KSC
  changed   STYLE/BANK02.STG (same size, last byte differs), SOUND/USER01.PCG (size differs)
  only A    PAD/PAD02.PDC        only B    STYLE/BANK03.STG
  duplicate inside A: STYLE/BANK01.STG == STYLE/BANK04.STG
Standard library only; output is deterministic. Refuses an existing output folder.
"""
import hashlib, os, sys, tempfile

def blob(tag, size):
    seed = ("AIARRANGER-SYNTHETIC-NOT-A-KORG-FILE " + tag + "\n").encode()
    return (seed * (size // len(seed) + 1))[:size]

A = {
    "STYLE/BANK01.STG": blob("style one", 4096),
    "STYLE/BANK02.STG": blob("style two", 3000),
    "STYLE/BANK04.STG": blob("style one", 4096),
    "PAD/PAD01.PDC": blob("pad one", 1500),
    "PAD/PAD02.PDC": blob("pad two", 1700),
    "SOUND/USER01.PCG": blob("sound", 9000),
    "KEYBOARD_SET/KBS01.KSC": blob("keyboard set", 1200),
    "README.TXT": b"Synthetic test library made by tools/make_synthetic_kst_library.py. Not a Korg file.\n",
}
B = dict(A)
del B["PAD/PAD02.PDC"], B["STYLE/BANK04.STG"]
B["STYLE/BANK02.STG"] = A["STYLE/BANK02.STG"][:-1] + b"#"
B["SOUND/USER01.PCG"] = blob("sound", 9500)
B["STYLE/BANK03.STG"] = blob("style three", 2048)

def write(out):
    if os.path.exists(out):
        sys.exit("refusing: %s already exists" % out)
    for name, files in (("SYNTH_A.KST", A), ("SYNTH_B.KST", B)):
        for rel, data in sorted(files.items()):
            p = os.path.join(out, name, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "wb") as f:
                f.write(data)
    with open(os.path.join(out, "MANIFEST.txt"), "w") as f:
        for name, files in (("SYNTH_A.KST", A), ("SYNTH_B.KST", B)):
            for rel, data in sorted(files.items()):
                f.write("%s/%s\t%d\t%s\n" % (name, rel, len(data), hashlib.sha256(data).hexdigest()))

def self_test():
    assert A["STYLE/BANK01.STG"] == A["STYLE/BANK04.STG"]
    assert len(A["STYLE/BANK02.STG"]) == len(B["STYLE/BANK02.STG"]) and A["STYLE/BANK02.STG"] != B["STYLE/BANK02.STG"]
    assert len(A["SOUND/USER01.PCG"]) != len(B["SOUND/USER01.PCG"])
    assert set(A) - set(B) == {"PAD/PAD02.PDC", "STYLE/BANK04.STG"} and set(B) - set(A) == {"STYLE/BANK03.STG"}
    with tempfile.TemporaryDirectory() as d:
        write(os.path.join(d, "x"))
        m1 = open(os.path.join(d, "x", "MANIFEST.txt")).read()
        write(os.path.join(d, "y"))
        assert m1 == open(os.path.join(d, "y", "MANIFEST.txt")).read(), "not deterministic"
    print("self-test passed")

if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        self_test()
    elif len(sys.argv) == 2:
        write(sys.argv[1])
    else:
        sys.exit(__doc__)
