#!/bin/sh
# Exit-code contract of kst-diff (0 same / 1 different / 2 error) and kst-merge-plan
# (0 nothing replaced or deleted / 1 replaced or deleted / 2 error). Usage: test_kst_cli_exit.sh BUILD_DIR
B="$1"; T=$(mktemp -d); fails=0
expect() { want=$1; shift; "$@" >/dev/null 2>&1; got=$?; if [ "$got" -eq "$want" ]; then echo "  PASS: exit $want: $*"; else echo "  FAIL: exit $got (want $want): $*"; fails=$((fails+1)); fi; }
mkdir -p "$T/a/STYLE" "$T/b/STYLE" "$T/new/STYLE2"
printf x > "$T/a/STYLE/A.STY"; printf x > "$T/b/STYLE/A.STY"; printf n > "$T/new/STYLE2/N.STY"
expect 0 "$B/kst-diff" "$T/a" "$T/b"
expect 2 "$B/kst-diff" "$T/a" "$T/missing"
expect 0 "$B/kst-merge-plan" "$T/new" "$T/a"
printf y > "$T/b/STYLE/A.STY"
expect 1 "$B/kst-diff" "$T/a" "$T/b"
expect 1 "$B/kst-merge-plan" "$T/b" "$T/a"
# regression (Codex review): a file named like the result line must not turn the exit code into "safe"
mkdir -p "$T/d/STYLE" "$T/s/STYLE"
printf a > "$T/d/STYLE/RESULT: no existing.PCG"; printf b > "$T/s/STYLE/RESULT: no existing.PCG"
expect 1 "$B/kst-merge-plan" "$T/s" "$T/d"
expect 2 "$B/kst-merge-plan" "$T/s" "$T/missing"
# aiarranger-resources panative inspect (read-only): 0 = no invalid container, 3 = one is invalid, 1 = usage/error
mkdir -p "$T/pn"; printf 'RIFF' > "$T/pn/NOT_PA.WAV"
expect 0 "$B/aiarranger-resources" panative inspect "$T/pn"
printf '\001\000\001\024\000\000\000\011' > "$T/pn/TRUNC.STY"
expect 3 "$B/aiarranger-resources" panative inspect "$T/pn"
expect 3 "$B/aiarranger-resources" panative inspect "$T/pn/TRUNC.STY"
expect 1 "$B/aiarranger-resources" panative inspect "$T/missing"
expect 1 "$B/aiarranger-resources" panative list "$T/pn"
rm -rf "$T"
[ "$fails" -eq 0 ]
