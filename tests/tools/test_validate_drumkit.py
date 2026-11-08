#!/usr/bin/env python3
"""Negative tests for tools/validate_drumkit_package.py: each mutation of a valid Drum Kit package
must FAIL, the untouched package must PASS. Usage: test_validate_drumkit.py PACKAGE_DIR"""
import os, re, shutil, subprocess, struct, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
VALIDATOR = os.path.join(HERE, "..", "..", "tools", "validate_drumkit_package.py")

def run(d):
    return subprocess.run([sys.executable, "-I", VALIDATOR, d], capture_output=True, text=True)

def sub(path, old, new, count=1):
    s = open(path, "rb").read().decode("utf-8")
    assert old in s, (path, old)
    open(path, "wb").write(s.replace(old, new, count).encode("utf-8"))

def files(d, ext):
    return sorted(os.path.join(dp, f) for dp, _, fs in os.walk(d) for f in fs if f.endswith(ext))

def kick(d): return [f for f in files(d, ".txmatrix") if os.path.basename(f).startswith("B_")][0]
def prog(d): return files(d, ".txprog")[0]

def truncate_data(d):   # declared frames stay, the audio bytes shrink; move smpl before data so the file still parses
    f = [w for w in files(d, ".wav") if "Kick Soft" in w][0]
    b = open(f, "rb").read()
    di, si = b.index(b"data"), b.index(b"smpl")
    data, smpl = b[di:si], b[si:]
    open(f, "wb").write(b[:di] + smpl + data[:8 + 100])

def control_char(d):
    sub(prog(d), 'tx:name="Kick"', 'tx:name="Ki\x01ck"')

MUTATIONS = {   # name -> (mutator, why it must fail)
    "key above G9 (Codex #2)": lambda d: sub(prog(d), 'tx:low-key="C2" tx:low-vel="0" tx:high-key="C2"', 'tx:low-key="G#9" tx:low-vel="0" tx:high-key="G#9"'),
    "NaN cell level (Codex #3)": lambda d: sub(kick(d), 'tx:attenuation="0 dB"', 'tx:attenuation="NaN dB"'),
    "level without unit": lambda d: sub(kick(d), 'tx:attenuation="0 dB"', 'tx:attenuation="0"'),
    "level above +6 dB": lambda d: sub(kick(d), 'tx:attenuation="0 dB"', 'tx:attenuation="6.5 dB"'),
    "truncated WAV payload (Codex #4)": truncate_data,
    "duplicate matrix id (Codex #5)": lambda d: sub(prog(d), '<tx:wavematrix tx:id="1"', '<tx:wavematrix tx:id="0"'),
    "duplicate wave id (Codex #5)": lambda d: sub(kick(d), 'tx:id="1"', 'tx:id="0"'),
    "unknown element (Codex #6)": lambda d: sub(kick(d), "</tx:wavematrix>", '<tx:unsupported tx:anything="1"/>\n</tx:wavematrix>'),
    "unknown attribute (Codex #6)": lambda d: sub(prog(d), 'tx:choke-group="1"', 'tx:choke-group="1" tx:invented="1"'),
    "control character in a name (XML-invalid)": control_char,
    "choke 9": lambda d: sub(prog(d), 'tx:choke-group="1"', 'tx:choke-group="9"'),
    "two-key region": lambda d: sub(prog(d), 'tx:low-key="C2" tx:low-vel="0" tx:high-key="C2"', 'tx:low-key="C2" tx:low-vel="0" tx:high-key="C#2"'),
    "descending velocity rows": lambda d: sub(kick(d), '<tx:row tx:value="90">', '<tx:row tx:value="0">'),
    "path escaping the package": lambda d: sub(kick(d), 'tx:path="AIA Demo Kit Samples/', 'tx:path="../AIA Demo Kit Samples/'),
    "missing WAV": lambda d: os.remove([w for w in files(d, ".wav") if "Tom Lo" in w][0]),
    "smpl root differs": lambda d: sub(kick(d), 'tx:root="C4"', 'tx:root="D4"', 2),
    "Y axis not velocity": lambda d: sub(kick(d), 'tx:source="Vel"', 'tx:source="--"'),
}

def main():
    src = sys.argv[1]
    fails = 0
    r = run(src)
    print(("  PASS" if r.returncode == 0 else "  FAIL") + ": untouched package passes")
    fails += r.returncode != 0
    for name, mut in MUTATIONS.items():
        with tempfile.TemporaryDirectory() as t:
            d = os.path.join(t, "pkg"); shutil.copytree(src, d)
            mut(d)
            r = run(d)
            good = r.returncode == 1 and "Traceback" not in r.stderr
            print(("  PASS" if good else "  FAIL") + ": rejects " + name)
            if not good: print(r.stdout[-600:], r.stderr[-600:])
            fails += not good
    print("%d failed" % fails)
    sys.exit(1 if fails else 0)

if __name__ == "__main__":
    main()
