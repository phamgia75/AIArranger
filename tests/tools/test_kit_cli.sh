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
rm -rf "$T"
[ "$fails" -eq 0 ]
