#!/bin/sh
# aiarranger-resources CLI contract: exit 0 ok / 3 not ok / 1 usage or input error; JSON parses;
# the same package twice is refused; --kst stays outside the graph. Usage: test_resources_cli.sh BUILD_DIR PYTHON
B="$1"; PY="$2"; T=$(mktemp -d); fails=0
expect() { want=$1; shift; "$@" >/dev/null 2>&1; got=$?; if [ "$got" -eq "$want" ]; then echo "  PASS: exit $want: $*"; else echo "  FAIL: exit $got (want $want): $*"; fails=$((fails+1)); fi; }
ok() { if "$@"; then echo "  PASS: $*"; else echo "  FAIL: $*"; fails=$((fails+1)); fi; }
"$B/aiarranger-sound" demo --out "$T/organ" >/dev/null && "$B/aiarranger-sound" drums --out "$T/drums" >/dev/null || fails=1
ORG=$(awk -F '\t' '$1 == "package" { print $2 }' "$T/organ/RESOURCES.tsv")
expect 0 "$B/aiarranger-resources" budget "$T/organ" "$T/drums"
expect 0 "$B/aiarranger-resources" check "$T/organ" "$T/drums"
expect 1 "$B/aiarranger-resources" budget "$T/organ" "$T/organ"            # same package twice
expect 1 "$B/aiarranger-resources" budget "$T/missing"
expect 1 "$B/aiarranger-resources" where-used "not-an-id" "$T/organ"
expect 3 "$B/aiarranger-resources" delete-check "prov:$ORG/Sample/0" "$T/organ"     # in use
expect 0 "$B/aiarranger-resources" delete-check "prov:$ORG/Sound/0" "$T/organ"      # provisional, unreferenced
expect 3 "$B/aiarranger-resources" remap "prov:$ORG/Sample/0" "prov:$ORG/Multisample/0" "$T/organ"
expect 0 "$B/aiarranger-resources" remap "prov:$ORG/Sample/0" "prov:$ORG/Sample/1" "$T/organ"
expect 0 "$B/aiarranger-resources" where-used "prov:$ORG/Sample/0" "$T/organ"
ok sh -c "'$B/aiarranger-resources' budget --json '$T/organ' '$T/drums' | '$PY' -I -c 'import json,sys; d=json.load(sys.stdin); assert d[\"uniqueSamples\"]==47 and d[\"compressed\"][\"reversible\"] is False'"
mkdir -p "$T/kst/PCM"; printf abcd > "$T/kst/PCM/S1.PCM"
ok sh -c "'$B/aiarranger-resources' budget --json --kst '$T/kst' '$T/organ' | '$PY' -I -c 'import json,sys; d=json.load(sys.stdin); assert d[\"undecodedKst\"]==dict(inGraph=False,pcmFiles=1,pcmDiskBytes=4,pcmFolder=True) and d[\"uniqueSamples\"]==38'"
expect 1 "$B/aiarranger-resources" budget --kst "$T/nokst" "$T/organ"
printf 'aiarranger-evidence\t1\nsource\tR\towner ok\tpa-manager-report\nnode\tpa5x:USR/Style/1\tBallad\nref\tpa5x:USR/Style/1\tStyleTrack\tpa5x:USR/Sound/5\tconfirmed\tVar1\nend\t4\n' > "$T/ev.txt"
expect 3 "$B/aiarranger-resources" delete-check --evidence "$T/ev.txt" "pa5x:USR/Sound/5" "$T/organ"    # confirmed by evidence: in use
expect 1 "$B/aiarranger-resources" check --evidence "$T/ev.txt" --evidence "$T/ev.txt" "$T/organ"    # one source twice
printf 'aiarranger-evidence\t1\nsource\tR2\towner ok\tpa-manager-report\nnode\tpa5x:USR/Pad/1\tP\nref\tpa5x:USR/Pad/1\tPadTrack\tpa5x:USR/Sound/5\tinferred\tname\nend\t4\n' > "$T/ev2.txt"
ok sh -c "'$B/aiarranger-resources' where-used --evidence '$T/ev2.txt' pa5x:USR/Sound/5 '$T/organ' | grep -q 'INFERRED, not proven  \[evidence: R2\]'"
ok sh -c "'$B/aiarranger-resources' where-used --evidence '$T/ev2.txt' pa5x:USR/Sound/5 '$T/organ' | grep -q 'POSSIBLY: only through an inferred reference'"
ok sh -c "'$B/aiarranger-resources' delete-check --evidence '$T/ev.txt' pa5x:USR/Sound/6 '$T/organ' | grep -q 'not scanned: SongBookSound'"
printf 'aiarranger-evidence\t1\nsource\tR\towner ok\tpa-manager-report\nscanned\tProgramChange\tall\nend\t3\n' > "$T/bad.txt"
expect 1 "$B/aiarranger-resources" check --evidence "$T/bad.txt" "$T/organ"     # Q19: refused
printf 'aiarranger-memory-info\t1\nsamples\t10\n' > "$T/before.txt"; printf 'aiarranger-memory-info\t1\nsamples\t48\n' > "$T/after.txt"
ok sh -c "'$B/aiarranger-resources' memory-compare '$T/before.txt' '$T/after.txt' '$T/organ' | grep -q 'predicted +38  MATCH'"
expect 1 "$B/aiarranger-resources" memory-compare "$T/before.txt" "$T/organ"   # too few arguments
expect 1 "$B/aiarranger-resources" check --json "$T/organ"                     # --json is budget-only
printf 'garbage\n' > "$T/organ/RESOURCES.tsv"
expect 1 "$B/aiarranger-resources" budget "$T/organ"

# M2d: kst-native selective-plan CLI contract (exit 0 clean / 3 blocking / 1 usage or missing select or D18 overlap)
mkdir -p "$T/demo-kst"
"$B/aiarranger-resources" kst-native demo "$T/demo-kst" >/dev/null || fails=$((fails+1))
DEMO_KST=$(find "$T/demo-kst" -maxdepth 1 -type d -name '*style*.KST' -o -name '*Style*.KST' | head -n 1)
if [ -z "$DEMO_KST" ]; then DEMO_KST=$(find "$T/demo-kst" -maxdepth 1 -type d -name '*.KST' | head -n 1); fi
DEMO_REL=$(cd "$DEMO_KST" && find STYLE KEYBOARDSET PAD -type f \( -name '*.stg' -o -name '*.ksc' -o -name '*.pdc' \) 2>/dev/null | head -n 1)
expect 1 "$B/aiarranger-resources" kst-native selective-plan
expect 1 "$B/aiarranger-resources" kst-native selective-plan --source "$DEMO_KST"
expect 1 "$B/aiarranger-resources" kst-native selective-plan --source "$DEMO_KST" --target-bank STYLE/X --select "$DEMO_REL"
expect 1 "$B/aiarranger-resources" kst-native selective-plan --source "$DEMO_KST" --select "$DEMO_REL" --policy bogus
expect 1 "$B/aiarranger-resources" kst-native selective-plan --source "$DEMO_KST" --select "STYLE/NoSuch/Missing.stg"
expect 0 "$B/aiarranger-resources" kst-native selective-plan --source "$DEMO_KST" --select "$DEMO_REL"
ok sh -c "'$B/aiarranger-resources' kst-native selective-plan --source '$DEMO_KST' --select '$DEMO_REL' --json | '$PY' -I -c 'import json,sys; raw=sys.stdin.read(); assert \"$T\" not in raw; d=json.loads(raw); assert d[\"schema\"]==\"aiarranger-selective-export-report/1\" and d[\"hardwareVerified\"] is False and d[\"coverage\"][\"deleteProofAvailable\"] is False and d[\"memoryProjection\"][\"fitVerdict\"]==\"unverified-proxy\" and d[\"summary\"][\"executable\"] is True'"
expect 0 "$B/aiarranger-resources" kst-native selective-plan --source "$DEMO_KST" --select "$DEMO_REL" --export-plan "$T/advisory-out"
ok test -f "$T/advisory-out/SELECTIVE_EXPORT_REPORT.md"
ok test -f "$T/advisory-out/selective-plan.json"
ok test -f "$T/advisory-out/DIAGNOSTICS.json"
ok test -f "$T/advisory-out/PACKAGE.sha256"
ok sh -c "test -z \"\$(find '$T/advisory-out' -type f \( -iname '*.pcg' -o -iname '*.kmp' -o -iname '*.pcm' -o -iname '*.stg' -o -iname '*.ksc' -o -iname '*.pdc' \))\""
expect 1 "$B/aiarranger-resources" kst-native selective-plan --source "$DEMO_KST" --select "$DEMO_REL" --export-plan "$DEMO_KST/overlap-d18"

rm -rf "$T"
[ "$fails" -eq 0 ]
