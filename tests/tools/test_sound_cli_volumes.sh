#!/bin/sh
# Every publishing writer behind aiarranger-sound refuses a mounted volume before creating anything: a path under
# /Volumes, and (where /Volumes exists, i.e. macOS) a symlink into it. Usage: test_sound_cli_volumes.sh BUILD_DIR
B="$1"; T=$(mktemp -d); V="AIA-VOLTEST-$$"; fails=0
check() {   # $1 = label, $2 = --out, rest = the subcommand
    label=$1; out=$2; shift 2
    err=$("$B/aiarranger-sound" "$@" --out "$out" 2>&1 >/dev/null); rc=$?
    if [ "$rc" -eq 1 ] && printf '%s' "$err" | grep -q "mounted volume" && [ ! -e "/Volumes/$V" ]; then echo "  PASS: $label: $*"
    else echo "  FAIL: $label (exit $rc): $* -> $err"; fails=$((fails+1)); fi
}
[ -d /Volumes ] && ln -s /Volumes "$T/vol"
for cmd in "demo" "drums" "kmp --source demo"; do   # writePackage, writeDrumPackage, writeKorgFolder
    # shellcheck disable=SC2086
    check "/Volumes path" "/Volumes/$V/pkg" $cmd
    # shellcheck disable=SC2086
    [ -L "$T/vol" ] && check "symlink into /Volumes" "$T/vol/$V/pkg" $cmd
done
[ -L "$T/vol" ] || echo "  (no /Volumes here: the symlink case runs on macOS only)"
rm -rf "$T"
echo "Results: $fails failed"
[ "$fails" -eq 0 ]
