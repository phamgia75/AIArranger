#!/usr/bin/env python3
"""Write two ORIGINAL synthetic KST-style folders for hands-on tests of the KST Library tab and the Library Manager.

The KST tools (Inventory, Compare, Merge Plan, Library Manager) work on files and bytes only; they never decode
content. These folders follow the OBSERVED Pa5X KST layout (one real KST listing, docs/research/
PA_MANAGER_WINDOWS_FUNCTIONAL_AUDIT.md §3c; manual v1.4 p.721 extensions, p.731-732 type -> bank -> file):
upper-case type folders (KEYBOARDSET, not KEYBOARD_SET), bank sub-folders inside STYLE/KEYBOARDSET/PAD, grid types
(SOUND) holding files directly, one file per Style named after it, nothing loose at the top level. The bytes are
placeholders made here: NOT Korg formats, never to be loaded on an instrument (`aiarranger-resources validate`
reports them as R-10 ERROR).

  python3 tools/make_synthetic_kst_library.py <out-dir>   # writes <out-dir>/SYNTH_A.KST, SYNTH_B.KST
  python3 tools/make_synthetic_kst_library.py --self-test

Designed differences between A and B (what Compare and Merge Plan should report):
  same      STYLE/BANK01/Style One.STG, PAD/BANK01/Pad One.PDC, KEYBOARDSET/BANK01/Kbs One.KSC
  changed   STYLE/BANK01/Style Two.STG (same size, last byte differs), SOUND/USER01.PCG (size differs)
  only A    PAD/BANK01/Pad Two.PDC        only B    STYLE/BANK02/Style Three.STG
  duplicate inside A: STYLE/BANK01/Style One.STG == STYLE/BANK02/Style Four.STG
Standard library only; output is deterministic. Refuses an existing output folder.
"""
import hashlib, os, sys, tempfile

def blob(tag, size):
    seed = ("AIARRANGER-SYNTHETIC-NOT-A-KORG-FILE " + tag + "\n").encode()
    return (seed * (size // len(seed) + 1))[:size]

A = {
    "STYLE/BANK01/Style One.STG": blob("style one", 4096),
    "STYLE/BANK01/Style Two.STG": blob("style two", 3000),
    "STYLE/BANK02/Style Four.STG": blob("style one", 4096),
    "PAD/BANK01/Pad One.PDC": blob("pad one", 1500),
    "PAD/BANK01/Pad Two.PDC": blob("pad two", 1700),
    "SOUND/USER01.PCG": blob("sound", 9000),
    "KEYBOARDSET/BANK01/Kbs One.KSC": blob("keyboard set", 1200),
}
B = dict(A)
del B["PAD/BANK01/Pad Two.PDC"], B["STYLE/BANK02/Style Four.STG"]
B["STYLE/BANK01/Style Two.STG"] = A["STYLE/BANK01/Style Two.STG"][:-1] + b"#"
B["SOUND/USER01.PCG"] = blob("sound", 9500)
B["STYLE/BANK02/Style Three.STG"] = blob("style three", 2048)

# The observed layout (same as library/library_validate.h's Pa5X table, rules R-03/R-04/R-05/R-06).
TYPE_EXT = {"STYLE": ".STG", "KEYBOARDSET": ".KSC", "PAD": ".PDC", "SOUND": ".PCG"}
BANKED = {"STYLE", "KEYBOARDSET", "PAD"}

def write(out):
    if os.path.exists(out):
        sys.exit("refusing: %s already exists" % out)
    for name, files in (("SYNTH_A.KST", A), ("SYNTH_B.KST", B)):
        for rel, data in sorted(files.items()):
            p = os.path.join(out, name, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "wb") as f:
                f.write(data)
    with open(os.path.join(out, "MANIFEST.txt"), "w") as f:   # beside the KST folders, not inside (R-06)
        for name, files in (("SYNTH_A.KST", A), ("SYNTH_B.KST", B)):
            for rel, data in sorted(files.items()):
                f.write("%s/%s\t%d\t%s\n" % (name, rel, len(data), hashlib.sha256(data).hexdigest()))

def self_test():
    a1, a4 = "STYLE/BANK01/Style One.STG", "STYLE/BANK02/Style Four.STG"
    assert A[a1] == A[a4]
    two = "STYLE/BANK01/Style Two.STG"
    assert len(A[two]) == len(B[two]) and A[two] != B[two]
    assert len(A["SOUND/USER01.PCG"]) != len(B["SOUND/USER01.PCG"])
    assert set(A) - set(B) == {"PAD/BANK01/Pad Two.PDC", a4} and set(B) - set(A) == {"STYLE/BANK02/Style Three.STG"}
    for rel in list(A) + list(B):   # follows the observed layout (SET_KST_GAP §5 #1-#3)
        parts = rel.split("/")
        assert parts[0] in TYPE_EXT and rel.endswith(TYPE_EXT[parts[0]]), rel
        assert len(parts) == (3 if parts[0] in BANKED else 2), rel
    with tempfile.TemporaryDirectory() as d:
        write(os.path.join(d, "x"))
        m1 = open(os.path.join(d, "x", "MANIFEST.txt")).read()
        write(os.path.join(d, "y"))
        assert m1 == open(os.path.join(d, "y", "MANIFEST.txt")).read(), "not deterministic"
        assert sorted(os.listdir(os.path.join(d, "x", "SYNTH_A.KST"))) == ["KEYBOARDSET", "PAD", "SOUND", "STYLE"]
    print("self-test passed")

if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        self_test()
    elif len(sys.argv) == 2:
        write(sys.argv[1])
    else:
        sys.exit(__doc__)
