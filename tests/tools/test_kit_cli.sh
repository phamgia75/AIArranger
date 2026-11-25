#!/bin/sh
# aiarranger-kit end to end on outputs the repo makes itself: demo Sound package, demo Drum Kit, KMP folder, and a marker
# SMF Style converted from the synthetic Yamaha fixture with a sound map. Usage: BUILD_DIR SOURCE_DIR
B="$1"; R="$2"; T=$(mktemp -d); fails=0
expect() { want=$1; shift; "$@" >/dev/null 2>&1; got=$?; if [ "$got" -eq "$want" ]; then echo "  PASS: exit $want: $*"; else echo "  FAIL: exit $got (want $want): $*"; fails=$((fails+1)); fi; }
check() { if sh -c "$2" >/dev/null 2>&1; then echo "  PASS: $1"; else echo "  FAIL: $1"; fails=$((fails+1)); fi; }
mkdir "$T/out"
"$B/aiarranger-sound" demo --out "$T/out/AIA Demo Organ" >/dev/null &&
"$B/aiarranger-sound" drums --out "$T/out/AIA Demo Kit" >/dev/null &&
"$B/aiarranger-sound" kmp --source demo --out "$T/out/AIA Organ KMP" >/dev/null &&
printf 'bass 121 0 33\ndrum 120 0 0\nacc1 121 0 0\n' > "$T/map.txt" &&
"$B/aiarranger-convert" --input "$R/fixtures/synthetic/SYNTH_WALTZ_68.sty" --target pa5x --output "$T/out/Waltz.mid" --sound-map "$T/map.txt" >/dev/null ||
    { echo "  FAIL: fixtures"; exit 1; }
(cd "$T/out" && find . -type f -exec shasum -a 256 {} + | sort) > "$T/before"
expect 0 "$B/aiarranger-kit" build --in "$T/out/AIA Demo Organ" --in "$T/out/AIA Demo Kit" --in "$T/out/AIA Organ KMP" --in "$T/out/Waltz.mid" --out "$T/AIA Kit"
check "kit verifies with shasum" "cd '$T/AIA Kit' && shasum -a 256 -c PACKAGE.sha256"
check "label in KIT.md and manifest.json" "grep -q 'STRUCTURALLY VALID — NOT VERIFIED ON A PA5X' '$T/AIA Kit/KIT.md' && grep -q '\"hardwareVerified\": false' '$T/AIA Kit/manifest.json'"
check "the sound-mapped Style is in STYLES" "cmp '$T/out/Waltz.mid' '$T/AIA Kit/STYLES/Waltz.mid'"
expect 0 "$B/aiarranger-kit" verify "$T/AIA Kit"
check "sources unchanged" "(cd '$T/out' && find . -type f -exec shasum -a 256 {} + | sort) | cmp - '$T/before'"
expect 1 "$B/aiarranger-kit" build --in "$T/out/Waltz.mid" --out "$T/AIA Kit"
expect 1 "$B/aiarranger-kit" build --in "$T/map.txt" --out "$T/K2"
check "refused kit wrote nothing" "[ ! -e '$T/K2' ] && [ ! -e '$T/K2.INCOMPLETE' ]"
printf x >> "$T/AIA Kit/STYLES/Waltz.mid"
expect 1 "$B/aiarranger-kit" verify "$T/AIA Kit"
expect 1 "$B/aiarranger-kit" build --out "$T/K3"

# ── M2c selective CLI checks (`select-plan` & `select-build`) ──
printf 'bass 121 0 33\ndrum 120 64 0\nacc1 121 64 3\n' > "$T/map_user.txt"
"$B/aiarranger-convert" --input "$R/fixtures/synthetic/SYNTH_WALTZ_68.sty" --target pa5x --output "$T/out/WaltzUser.mid" --sound-map "$T/map_user.txt" >/dev/null ||
    { echo "  FAIL: selective user style fixture"; fails=$((fails+1)); }
printf 'bass 121 0 33\ndrum 120 0 0\nacc1 121 74 40\n' > "$T/map_missing.txt"
"$B/aiarranger-convert" --input "$R/fixtures/synthetic/SYNTH_WALTZ_68.sty" --target pa5x --output "$T/out/WaltzMissing.mid" --sound-map "$T/map_missing.txt" >/dev/null ||
    { echo "  FAIL: selective missing style fixture"; fails=$((fails+1)); }

# select-plan executable (exit 0) + JSON schema check
"$B/aiarranger-kit" select-plan --workspace "$T/out" --style WaltzUser.mid \
    --bind "USER01/3=AIA Demo Organ" --bind "USERDK01/0=AIA Demo Kit" --json > "$T/plan.json"
rc=$?
if [ "$rc" -eq 0 ]; then echo "  PASS: exit 0: select-plan --json"; else echo "  FAIL: exit $rc (want 0): select-plan --json"; fails=$((fails+1)); fi
check "select-plan JSON schema and hardwareVerified false" \
    "grep -q '\"schema\": \"aiarranger-selective-export-report/1\"' '$T/plan.json' && grep -q '\"hardwareVerified\": false' '$T/plan.json' && grep -q '\"executable\": true' '$T/plan.json'"
FP=$(sed -n 's/.*"planFingerprint": "\([0-9a-f]\{64\}\)".*/\1/p' "$T/plan.json" | head -n 1)
check "select-plan emitted 64-hex fingerprint" "[ '${#FP}' -eq 64 ]"

# select-plan blocked by missing User Sound (exit 3)
expect 3 "$B/aiarranger-kit" select-plan --workspace "$T/out" --style WaltzMissing.mid --json

# select-build blocked refuses and writes nothing (exit 3)
expect 3 "$B/aiarranger-kit" select-build --workspace "$T/out" --style WaltzMissing.mid --out "$T/BlockedSelKit"
check "blocked select-build wrote nothing" "[ ! -e '$T/BlockedSelKit' ] && [ ! -e '$T/BlockedSelKit.INCOMPLETE' ]"

# select-build with pinned --expect-fingerprint succeeds (exit 0) and verifies via verifyKit
expect 0 "$B/aiarranger-kit" select-build --workspace "$T/out" --style WaltzUser.mid \
    --bind "USER01/3=AIA Demo Organ" --bind "USERDK01/0=AIA Demo Kit" \
    --expect-fingerprint "$FP" --out "$T/SelKit"
expect 0 "$B/aiarranger-kit" verify "$T/SelKit"
check "selective kit verifies with shasum and has SELECTIVE_MANIFEST.json + DIAGNOSTICS.json" \
    "cd '$T/SelKit' && shasum -a 256 -c PACKAGE.sha256 && test -f SELECTIVE_MANIFEST.json && test -f DIAGNOSTICS.json && test ! -e KMP"

# select-build with mismatched --expect-fingerprint fails (exit 1) and writes nothing
BAD_FP="0000000000000000000000000000000000000000000000000000000000000000"
expect 1 "$B/aiarranger-kit" select-build --workspace "$T/out" --style WaltzUser.mid \
    --bind "USER01/3=AIA Demo Organ" --bind "USERDK01/0=AIA Demo Kit" \
    --expect-fingerprint "$BAD_FP" --out "$T/BadFpKit"
check "bad fingerprint select-build wrote nothing" "[ ! -e '$T/BadFpKit' ] && [ ! -e '$T/BadFpKit.INCOMPLETE' ]"

# select-build D18 overlap inside workspace refused (exit 1)
expect 1 "$B/aiarranger-kit" select-build --workspace "$T/out" --style WaltzUser.mid \
    --bind "USER01/3=AIA Demo Organ" --bind "USERDK01/0=AIA Demo Kit" \
    --out "$T/out/OverlapKit"
check "overlap select-build wrote nothing" "[ ! -e '$T/out/OverlapKit' ]"

rm -rf "$T"
[ "$fails" -eq 0 ]
