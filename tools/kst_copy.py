#!/usr/bin/env python3
"""Transactional, verified copy of a KST folder (docs/SOUND_CONVERSION_ROADMAP.md K1).

    python3 -I tools/kst_copy.py SRC DST        copy SRC to the NEW folder DST
    python3 -I tools/kst_copy.py --verify DST   re-check DST against DST.manifest.sha256
    python3 -I tools/kst_copy.py --self-test

Folder level only: no KST file is opened as a format, parsed or changed. The copy goes to
DST.partial-<pid> first, every copied file is re-hashed against its source, the source is re-hashed
after the copy to prove it did not change, and only then is the folder renamed to DST. The manifest
(SHA-256 per file, `sha256sum` format) is written NEXT TO the copy (DST.manifest.sha256), so the KST
folder itself stays byte-identical to the source. Never overwrites: refuses an existing DST or
manifest, DST inside SRC, and symbolic links. On any failure the partial folder is
removed and the source is untouched. Exit 0 = copied/verified, 1 = refused or failed.
"""
import hashlib, os, shutil, sys, tempfile

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()

def tree(root):
    """{relative posix path: (size, sha256)} for every file; raises on a symlink."""
    out = {}
    for dp, dirs, files in os.walk(root, followlinks=False):
        for name in dirs + files:
            if os.path.islink(os.path.join(dp, name)):
                raise ValueError(f"symbolic link not copied: {os.path.join(dp, name)}")
        for name in files:
            p = os.path.join(dp, name)
            out[os.path.relpath(p, root).replace(os.sep, "/")] = (os.path.getsize(p), sha256(p))
    return out

def manifest_path(dst):
    return os.path.abspath(dst).rstrip(os.sep) + ".manifest.sha256"

def copy(src, dst, _after_copy=None):
    src, dst = os.path.realpath(src), os.path.abspath(dst)
    man = manifest_path(dst)
    if not os.path.isdir(src): return f"refusing: {src} is not a folder"
    if os.path.lexists(dst): return f"refusing: {dst} already exists"
    if os.path.lexists(man): return f"refusing: {man} already exists"
    real_parent = os.path.realpath(os.path.dirname(dst))
    if os.path.commonpath([src, real_parent]) == src: return "refusing: DST is inside SRC"   # (SRC inside DST needs DST to exist: refused above)
    tmp = f"{dst}.partial-{os.getpid()}"
    if os.path.lexists(tmp): return f"refusing: {tmp} already exists"
    try:
        before = tree(src)
    except ValueError as e:
        return f"refusing: {e}"
    try:
        os.makedirs(tmp)
        for dp, dirs, _ in os.walk(src):
            for d in dirs: os.makedirs(os.path.join(tmp, os.path.relpath(os.path.join(dp, d), src)), exist_ok=True)
        for rel, (size, digest) in before.items():
            target = os.path.join(tmp, *rel.split("/"))
            shutil.copy2(os.path.join(src, *rel.split("/")), target)
            if os.path.getsize(target) != size or sha256(target) != digest:
                raise IOError(f"copy of {rel} does not match its source")
        if _after_copy: _after_copy()
        if tree(src) != before: raise IOError("the source changed during the copy; copy discarded")
        if os.path.lexists(dst) or os.path.lexists(man): raise IOError("DST or its manifest appeared during the copy")
        os.rename(tmp, dst)
    except (OSError, IOError, ValueError) as e:
        shutil.rmtree(tmp, ignore_errors=True)   # only ever our own partial folder
        return f"failed: {e}"
    with open(man, "x", encoding="utf-8") as f:   # "x": never overwrite
        for rel in sorted(before): f.write(f"{before[rel][1]}  {rel}\n")
    total = sum(s for s, _ in before.values())
    print(f"copied {len(before)} files, {total} bytes: {src} -> {dst}\nmanifest: {man}\nsource re-hashed after copy: unchanged")
    return None

def verify(dst):
    dst = os.path.abspath(dst); man = manifest_path(dst)
    if not os.path.isfile(man): return f"no manifest at {man}"
    want = {}
    with open(man, encoding="utf-8") as f:
        for line in f:
            digest, rel = line.rstrip("\n").split("  ", 1)
            want[rel] = digest
    try:
        have = {rel: d for rel, (_, d) in tree(dst).items()}
    except ValueError as e:
        return str(e)
    bad = [f"changed: {r}" for r in sorted(want) if r in have and have[r] != want[r]]
    bad += [f"missing: {r}" for r in sorted(set(want) - set(have))]
    bad += [f"extra:   {r}" for r in sorted(set(have) - set(want))]
    if bad: return "\n".join(bad)
    print(f"verified {len(want)} files against {man}")
    return None

def self_test():
    fails = 0
    def check(cond, msg):
        nonlocal fails
        print(("  PASS: " if cond else "  FAIL: ") + msg); fails += not cond
    with tempfile.TemporaryDirectory() as t:
        src = os.path.join(t, "MY.SET")
        os.makedirs(os.path.join(src, "STYLE")); os.makedirs(os.path.join(src, "PCM", "empty"))
        files = {"STYLE/BANK01.STY": os.urandom(5000), "PCM/RAM01.PCM": bytes(range(256)) * 300, "SOUND.PCG": b""}
        for rel, data in files.items():
            with open(os.path.join(src, *rel.split("/")), "wb") as f: f.write(data)
        before = tree(src)
        dst = os.path.join(t, "copy", "MY.SET")
        os.makedirs(os.path.dirname(dst))
        check(copy(src, dst) is None, "copy succeeds")
        check(tree(dst) == before and os.path.isdir(os.path.join(dst, "PCM", "empty")), "copy is byte-identical, empty folders kept")
        check(tree(src) == before, "source unchanged")
        check(not os.path.exists(os.path.join(dst, "MY.SET.manifest.sha256")) and os.path.isfile(dst + ".manifest.sha256"),
              "manifest written next to the copy, not inside it")
        check(verify(dst) is None, "verify passes on the fresh copy")
        check((copy(src, dst) or "").startswith("refusing"), "refuses an existing DST")
        check((copy(src, os.path.join(src, "inner")) or "").startswith("refusing"), "refuses DST inside SRC")
        check((copy(os.path.join(src, "STYLE"), src) or "").startswith("refusing"), "refuses copying into the folder that contains SRC (exists)")
        with open(os.path.join(dst, "STYLE", "BANK01.STY"), "r+b") as f: f.seek(10); f.write(b"\x00\xff")
        os.remove(os.path.join(dst, "SOUND.PCG"))
        with open(os.path.join(dst, "NEW.TXT"), "w") as f: f.write("x")
        r = verify(dst) or ""
        check("changed: STYLE/BANK01.STY" in r and "missing: SOUND.PCG" in r and "extra:   NEW.TXT" in r, "verify reports changed, missing and extra files")
        d2 = os.path.join(t, "copy2")
        def mutate():
            with open(os.path.join(src, "SOUND.PCG"), "wb") as f: f.write(b"changed")
        r = copy(src, d2, _after_copy=mutate) or ""
        check(r.startswith("failed") and not os.path.exists(d2) and not any(n.startswith("copy2.partial") for n in os.listdir(t)),
              "a source change during the copy discards it (no DST, no partial folder)")
        with open(os.path.join(src, "SOUND.PCG"), "wb") as f: f.write(b"")
        os.symlink(os.path.join(src, "SOUND.PCG"), os.path.join(src, "LINK"))
        r = copy(src, os.path.join(t, "copy3")) or ""
        check(r.startswith("refusing") and "symbolic link" in r and not os.path.exists(os.path.join(t, "copy3")), "refuses a source with symbolic links")
    print(f"\nResults: {'0 failed' if not fails else f'{fails} failed'}")
    return 1 if fails else 0

if __name__ == "__main__":
    a = sys.argv[1:]
    if a == ["--self-test"]: sys.exit(self_test())
    if len(a) == 2 and a[0] == "--verify": err = verify(a[1])
    elif len(a) == 2 and not a[0].startswith("-"): err = copy(a[0], a[1])
    else: sys.exit(__doc__)
    if err: print(err, file=sys.stderr)
    sys.exit(1 if err else 0)
