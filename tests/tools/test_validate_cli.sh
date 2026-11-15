#!/bin/sh
# aiarranger-resources validate (E8): the synthetic KST generator follows the observed layout, so the only findings are
# its 11 placeholder files (R-10 ERROR, exit 3); a missing folder and an unreadable file are NOT validated (exit 1).
# Every check counts on its own. Usage: test_validate_cli.sh BUILD_DIR PYTHON REPO_DIR [EXPECTED_R10]
B="$1"; PY="$2"; REPO="$3"; WANT_R10="${4:-11}"; T=$(mktemp -d); fails=0
check() { if eval "$2"; then echo "  PASS: $1"; else echo "  FAIL: $1"; fails=$((fails+1)); fi; }
"$PY" -I "$REPO/tools/make_synthetic_kst_library.py" "$T/s" >/dev/null; rc=$?
check "generator ran" "[ $rc -eq 0 ]"
"$B/aiarranger-resources" validate "$T/s/SYNTH_A.KST" > "$T/out" 2>&1; rc=$?
check "placeholders only: exit 3" "[ $rc -eq 3 ]"
check "exactly $WANT_R10 R-10 ERROR lines" "[ \$(grep -c '^  ERROR  R-10' \"$T/out\") -eq $WANT_R10 ]"
check "no other ERROR" "[ \$(grep -c '^  ERROR' \"$T/out\") -eq $WANT_R10 ]"
check "no WARN" "! grep -q '^  WARN' \"$T/out\""
"$B/aiarranger-resources" validate "$T/none" >/dev/null 2>&1; rc=$?
check "missing folder: exit 1" "[ $rc -eq 1 ]"
chmod 000 "$T/s/SYNTH_A.KST/SOUND/USER01.PCG"
"$B/aiarranger-resources" validate "$T/s/SYNTH_A.KST" > "$T/out2" 2>&1; rc=$?
chmod 644 "$T/s/SYNTH_A.KST/SOUND/USER01.PCG"
check "unreadable file: exit 1 (not validated)" "[ $rc -eq 1 ] && grep -q 'NOT validated' \"$T/out2\""
rm -rf "$T"
echo "$fails failure(s)"
[ "$fails" -eq 0 ]
