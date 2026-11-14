#!/usr/bin/env bash
# P2.6 iPadOS host: build the core with CMake, compile the Swift host with
# swiftc (no Xcode project), and run its tests.
#
#   apps/ipados/build.sh macos   host tests natively on macOS (Command Line Tools are enough)
#   apps/ipados/build.sh sim     host tests + app launch/background inside a booted
#                                iPad simulator (needs Xcode: CI macos-latest)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$ROOT/apps/ipados"
MODE="${1:-macos}"
ARCH="$(uname -m)"
FIXTURE="$ROOT/fixtures/synthetic/SYNTH_FULL_SFF2.uasf"

case "$MODE" in
  macos)
    B="$ROOT/build-ipados-macos"; SDK=macosx; TARGET="$ARCH-apple-macos14.0"
    cmake -S "$ROOT" -B "$B" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 >/dev/null ;;
  sim)
    B="$ROOT/build-ipados-sim"; SDK=iphonesimulator; TARGET="$ARCH-apple-ios17.0-simulator"
    cmake -S "$ROOT" -B "$B" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 >/dev/null ;;
  *) echo "usage: $0 macos|sim" >&2; exit 2 ;;
esac

cmake --build "$B" --target arranger-bridge arranger-engine -j4
SWIFTC=(xcrun --sdk "$SDK" swiftc -target "$TARGET" -swift-version 5 -O -warnings-as-errors
        -I "$HERE/CAIArranger" -L "$B")
"${SWIFTC[@]}" "$HERE"/Host/*.swift "$HERE/Tests/main.swift" -o "$B/ipados-host-tests"

if [ "$MODE" = macos ]; then
  # The app only type-checks here: its target is iPadOS.
  "${SWIFTC[@]}" -parse-as-library -typecheck "$HERE"/Host/*.swift "$HERE"/App/*.swift
  exec "$B/ipados-host-tests" "$FIXTURE"
fi

# ── Simulator ──
APP="$B/AIArranger.app"; BUNDLE_ID=com.aiarranger.ipad.host
rm -rf "$APP"; mkdir -p "$APP"
"${SWIFTC[@]}" -parse-as-library "$HERE"/Host/*.swift "$HERE"/App/*.swift -o "$APP/AIArranger"
cp "$HERE/App/Info.plist" "$FIXTURE" "$APP/"
codesign --force --sign - "$APP"

UDID="$(xcrun simctl list devices available -j | python3 -c '
import json, sys
devs = [d for rt, ds in json.load(sys.stdin)["devices"].items() if "iOS" in rt for d in ds if "iPad" in d["name"]]
print(devs[-1]["udid"] if devs else "")')"
[ -n "$UDID" ] || { echo "no iPad simulator available" >&2; exit 1; }
xcrun simctl boot "$UDID" 2>/dev/null || true
xcrun simctl bootstatus "$UDID" -b >/dev/null

echo "== host tests in the iPad simulator ($UDID) =="
SIMCTL_CHILD_AIARR_MIDI_HW_TX=0 xcrun simctl spawn "$UDID" "$B/ipados-host-tests" "$FIXTURE"

echo "== app: install, launch self-test, background =="
LOG="$B/app.log"; rm -f "$LOG"
xcrun simctl install "$UDID" "$APP"
SIMCTL_CHILD_AIARR_SELFTEST=1 SIMCTL_CHILD_AIARR_MIDI_HW_TX=0 \
  xcrun simctl launch --terminate-running-process --stderr="$LOG" "$UDID" "$BUNDLE_ID"
wait_for() { for _ in $(seq 60); do grep -q "$1" "$LOG" 2>/dev/null && return 0; sleep 1; done; return 1; }
ok=1
wait_for "SELFTEST " || ok=0
xcrun simctl launch "$UDID" com.apple.Preferences >/dev/null   # sends our app to the background
wait_for "LIFECYCLE background" || ok=0
xcrun simctl terminate "$UDID" "$BUNDLE_ID" || true
cat "$LOG"
grep -q "SELFTEST PASS" "$LOG" || ok=0
grep -q "LIFECYCLE background playing=0 stuck=0" "$LOG" || ok=0
[ "$ok" = 1 ] && echo "app simulator smoke: PASS" || { echo "app simulator smoke: FAIL" >&2; exit 1; }
