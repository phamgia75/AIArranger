#!/usr/bin/env bash
# Build the engine + importer + all test programs and run them via CTest.
#
# Each tests/engine/*.cpp file is a standalone program with its own main(),
# so they are built as individual executables (see CMakeLists.txt) rather
# than linked into one binary. CTest runs them all and aggregates pass/fail.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"

cmake -S "$ROOT" -B "$BUILD" >/dev/null
cmake --build "$BUILD" -j4
ctest --test-dir "$BUILD" --output-on-failure --timeout 300
