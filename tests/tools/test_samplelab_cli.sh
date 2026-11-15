#!/bin/sh
# aiarranger-samplelab end to end on WAVs the repo makes itself (the demo organ package's wav/). Usage: BUILD_DIR
B="$1"; T=$(mktemp -d); fails=0
expect() { want=$1; shift; "$@" >/dev/null 2>&1; got=$?; if [ "$got" -eq "$want" ]; then echo "  PASS: exit $want: $*"; else echo "  FAIL: exit $got (want $want): $*"; fails=$((fails+1)); fi; }
check() { if sh -c "$2" >/dev/null 2>&1; then echo "  PASS: $1"; else echo "  FAIL: $1"; fails=$((fails+1)); fi; }
"$B/aiarranger-sound" demo --out "$T/demo" >/dev/null || { echo "  FAIL: demo package"; exit 1; }
W=$(ls "$T/demo/wav" | head -1)
expect 0 "$B/aiarranger-samplelab" analyze --in "$T/demo/wav/$W"
expect 0 "$B/aiarranger-samplelab" batch --in "$T/demo/wav"
check "batch dry run writes nothing" "[ \"\$(ls '$T')\" = demo ]"
expect 0 "$B/aiarranger-samplelab" batch --in "$T/demo/wav" --trim -60 --normalize rms --target -18 --out "$T/batch"
check "batch folder verifies" "cd '$T/batch' && shasum -a 256 -c PACKAGE.sha256"
check "batch WAVs make a Sound package" "'$B/aiarranger-sound' wav --in '$T/batch/wav' --name 'Lab Organ' --layers AIA_Organ_Soft,AIA_Organ_Bright --splits 90 --out '$T/pkg'"
expect 1 "$B/aiarranger-samplelab" batch --in "$T/demo/wav" --out "$T/batch"
expect 0 "$B/aiarranger-samplelab" slice --in "$T/demo/wav/$W" --name Organ --bpm 120 --out "$T/slice"
check "slice folder verifies and has its MIDI" "cd '$T/slice' && shasum -a 256 -c PACKAGE.sha256 && [ -s Organ.mid ]"
expect 1 "$B/aiarranger-samplelab" slice --in "$T/demo/wav/$W" --name Organ --bars 1 --bpm 120 --out "$T/slice2"
expect 1 "$B/aiarranger-samplelab" slice --in "$T/demo/wav/$W" --name Organ --bars 1 --out "$T/slice2"
expect 0 "$B/aiarranger-samplelab" loop --in "$T/demo/wav/$W" --start 4000 --end 18000 --snap 200 --xfade 1000 --out "$T/loop"
check "loop folder verifies" "cd '$T/loop' && shasum -a 256 -c PACKAGE.sha256"
expect 1 "$B/aiarranger-samplelab" loop --in "$T/demo/wav/$W" --start 10 --end 20 --out "$T/loop2"
expect 1 "$B/aiarranger-samplelab" analyze --in "$T/missing.wav"
rm -rf "$T"
[ "$fails" -eq 0 ]
