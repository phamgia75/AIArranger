cmake_minimum_required(VERSION 3.20)
# OBJC/OBJCXX are required by the JUCE macOS app (.m/.mm); the engine itself is
# pure C++. They are enabled on Apple only, so the portable core (and its tests)
# also configures with a plain C/C++ toolchain (e.g. the Linux CI job).
project(ai-arranger-engine VERSION 0.2.0 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJC OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(APPLE)
    find_library(COREAUDIO CoreAudio REQUIRED)
    find_library(AUDIOUNIT AudioUnit REQUIRED)
    find_library(COREMIDI CoreMIDI REQUIRED)
    find_library(COREFOUNDATION CoreFoundation REQUIRED)
    find_library(AUDIOTOOLBOX AudioToolbox REQUIRED)
endif()
find_package(Threads REQUIRED)

# ── Engine library (vendor-agnostic runtime) ──────────────────────────
file(GLOB_RECURSE ENGINE_SRC
    src/engine/realtime/*.cpp
    src/engine/midi/*.cpp
    src/engine/trace/*.cpp
    src/engine/arranger/*.cpp
    src/engine/uasf/*.cpp
    src/engine/music/*.cpp
    src/engine/articulation/*.cpp
    src/engine/performance/*.cpp
    src/engine/control/*.cpp
    src/engine/chord/*.cpp
    src/engine/integration/*.cpp
    src/session/*.cpp
    src/control/*.cpp
    src/ui/*.cpp
    src/midi/*.cpp
    src/performance/*.cpp
)

# Split the Apple-framework I/O adapters out of the portable core so the core
# links NO Apple framework (iOS-first portability — see
# docs/gate-plans/IOS_THIN_VERTICAL_SLICE_PLAN.md §1). The target boundary is what
# matters; physical relocation into src/platform/apple/ is a cosmetic follow-up.
set(PLATFORM_APPLE_SRC
    ${CMAKE_SOURCE_DIR}/src/engine/midi/coremidi_out.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/midi/coremidi_in.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/realtime/coreaudio_driver.cpp
)
list(REMOVE_ITEM ENGINE_SRC ${PLATFORM_APPLE_SRC})

# ── Portable core (NO Apple framework) ────────────────────────────────
add_library(arranger-engine STATIC ${ENGINE_SRC})
target_include_directories(arranger-engine PUBLIC src)
target_link_libraries(arranger-engine PUBLIC Threads::Threads)
# GCC/Linux: std::atomic of a non-lock-free size (e.g. std::atomic<EngineSnapshot>)
# lowers to libatomic's __atomic_load/__atomic_store. Apple's toolchain resolves
# these from the system runtime, so only non-Apple hosts link libatomic.
if(NOT APPLE)
    target_link_libraries(arranger-engine PUBLIC atomic)
endif()

# ── Gate 3 latency trace harness (default OFF — production zero-overhead) ──
# When OFF, every AIARR_TRACE_* macro compiles to nothing, no ring is allocated,
# and no trace symbols are emitted. Turn on for measurement with:
#     cmake -DAIARR_LATENCY_TRACE=ON ...
# PUBLIC so the platform layer, tests, tools, and the app all see the same gate.
option(AIARR_LATENCY_TRACE "Enable Gate 3 RT-safe latency trace harness" OFF)
if(AIARR_LATENCY_TRACE)
    target_compile_definitions(arranger-engine PUBLIC AIARR_LATENCY_TRACE)
    message(STATUS "AIArranger: latency trace harness ENABLED (AIARR_LATENCY_TRACE)")
endif()

# ── Apple platform layer (CoreMIDI / CoreAudio adapters) ──────────────
# Isolates every direct Apple-framework dependency. The core sees only the
# vendor-neutral interfaces (IMidiOutputProvider, IRealtimeClock, ...).
# On non-Apple hosts the layer does not exist: PLATFORM_LIBS is empty and every
# target that genuinely needs CoreMIDI/CoreAudio is skipped (APPLE_ONLY_TESTS).
if(APPLE)
    add_library(platform-apple STATIC ${PLATFORM_APPLE_SRC})
    target_include_directories(platform-apple PUBLIC src)
    target_link_libraries(platform-apple PUBLIC arranger-engine
        ${COREAUDIO} ${AUDIOUNIT} ${COREMIDI} ${COREFOUNDATION} ${AUDIOTOOLBOX})
    set(PLATFORM_LIBS platform-apple)
else()
    set(PLATFORM_LIBS "")
    message(STATUS "AIArranger: non-Apple host — building the portable core only "
                   "(no platform-apple, no CoreMIDI/CoreAudio tests/tools)")
endif()

# Tests that exercise the Apple platform layer directly (CoreMidiOut /
# CoreMidiOutputProvider / mach host time). Built and run on Apple only.
set(APPLE_ONLY_TESTS test_coremidi_out test_latency_stability test_midi_output_provider)

# ── Importer library (vendor SFF1/SFF2 isolation) ─────────────────────
file(GLOB_RECURSE IMPORTER_SFF1_SRC
    src/importers/sff1/*.cpp
)

add_library(importer-sff1 STATIC ${IMPORTER_SFF1_SRC})
target_include_directories(importer-sff1 PUBLIC src)
target_link_libraries(importer-sff1 arranger-engine)

# ── Tests ─────────────────────────────────────────────────────────────
# Each test file is a standalone program with its own main(). We build
# one executable per file and register it with CTest, rather than linking
# them all into a single binary (which would collide on main()).
find_package(Threads REQUIRED)
enable_testing()

file(GLOB TEST_SRC tests/engine/*.cpp)
foreach(test_file ${TEST_SRC})
    get_filename_component(test_name ${test_file} NAME_WE)
    if(NOT APPLE AND test_name IN_LIST APPLE_ONLY_TESTS)
        continue()
    endif()
    add_executable(${test_name} ${test_file})
    target_link_libraries(${test_name} importer-sff1 ${PLATFORM_LIBS} arranger-engine Threads::Threads)
    target_compile_definitions(${test_name} PRIVATE
        CORPUS_DIR="${CMAKE_SOURCE_DIR}/corpus/yamaha/sff1"
        REPO_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# ── Gate 12 realtime performer-layer tests ────────────────────────────
file(GLOB REALTIME_TEST_SRC tests/realtime/*.cpp)
foreach(test_file ${REALTIME_TEST_SRC})
    get_filename_component(test_name ${test_file} NAME_WE)
    if(NOT APPLE AND test_name IN_LIST APPLE_ONLY_TESTS)
        continue()
    endif()
    add_executable(${test_name} ${test_file})
    target_link_libraries(${test_name} ${PLATFORM_LIBS} arranger-engine Threads::Threads)
    target_compile_definitions(${test_name} PRIVATE
        REALTIME_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/realtime")
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# ── Gate 14 headless app-shell tests (session/control/ui/midi) ────────
file(GLOB SHELL_TEST_SRC tests/integration/*.cpp tests/ui/*.cpp tests/midi/*.cpp tests/performance/*.cpp)
foreach(test_file ${SHELL_TEST_SRC})
    get_filename_component(test_name ${test_file} NAME_WE)
    if(NOT APPLE AND test_name IN_LIST APPLE_ONLY_TESTS)
        continue()
    endif()
    add_executable(${test_name} ${test_file})
    target_link_libraries(${test_name} ${PLATFORM_LIBS} arranger-engine Threads::Threads)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# ── Gate 3 latency trace harness tests ────────────────────────────────
# The MPSC ring is a generic container and is exercised in BOTH build modes;
# tracer/collector assertions are guarded by AIARR_TRACE_ENABLED inside the test
# so the suite passes whether the harness is compiled in or out.
file(GLOB TRACE_TEST_SRC tests/trace/*.cpp)
foreach(test_file ${TRACE_TEST_SRC})
    get_filename_component(test_name ${test_file} NAME_WE)
    add_executable(${test_name} ${test_file})
    target_link_libraries(${test_name} arranger-engine Threads::Threads)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# ── Gate 4 engine/product contract tests ──────────────────────────────
# The contract layer (engine_contract.h, engine_lifecycle, facade queries) is
# portable. These tests link the core ONLY (arranger-engine + Threads, NO
# platform-apple / NO Apple framework) — a link-time assertion of the platform
# boundary: if any contract code reached for CoreMIDI/CoreAudio, these would fail
# to link. Keep this list free of platform-apple.
file(GLOB CONTRACT_TEST_SRC tests/session/*.cpp)
foreach(test_file ${CONTRACT_TEST_SRC})
    get_filename_component(test_name ${test_file} NAME_WE)
    add_executable(${test_name} ${test_file})
    target_link_libraries(${test_name} arranger-engine Threads::Threads)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# ── Gate 4 C ABI product bridge (opaque handle) ───────────────────────
# The single surface the Gate 5 product host (iPad SwiftUI) links against. Pure C
# API over the portable facade; links the core ONLY (NO platform-apple / NO Apple
# framework), so the bridge is provably platform-neutral. Public include dir holds
# the C header a Swift module map will import.
add_library(arranger-bridge STATIC src/bridge/aiarranger_bridge.cpp)
target_include_directories(arranger-bridge PUBLIC include src)
target_link_libraries(arranger-bridge PUBLIC arranger-engine Threads::Threads)

file(GLOB BRIDGE_TEST_SRC tests/bridge/*.cpp)
foreach(test_file ${BRIDGE_TEST_SRC})
    get_filename_component(test_name ${test_file} NAME_WE)
    add_executable(${test_name} ${test_file})
    target_link_libraries(${test_name} arranger-bridge arranger-engine Threads::Threads)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Pure-C consumer: compiles the public header as strict C11 and links through the
# C ABI only — machine-checks that nothing C++ leaks across the bridge. The C++
# runtime comes from the static bridge, so the final link uses the C++ driver.
add_executable(test_bridge_c_consumer tests/bridge/test_bridge_c_consumer.c)
set_target_properties(test_bridge_c_consumer PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF LINKER_LANGUAGE CXX)
target_compile_options(test_bridge_c_consumer PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -pedantic -Werror>)
target_link_libraries(test_bridge_c_consumer arranger-bridge arranger-engine Threads::Threads)
add_test(NAME test_bridge_c_consumer COMMAND test_bridge_c_consumer)

# ── Gate 3 latency report tool (Python, stdlib only) ──────────────────
# Synthetic-data unit test for tools/latency_report; registered so CTest/CI run
# it alongside the C++ suites instead of it being a manual-only check.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_latency_report_py
             COMMAND Python3::Interpreter test_latency_report.py
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tools/latency_report)
endif()

# ── SFF1 parser CLI tool ──────────────────────────────────────────────
add_executable(sff1-parser tools/sff1_parser_cli.cpp)
target_link_libraries(sff1-parser importer-sff1 arranger-engine)

# ── Korg playback CLI (Gate 9 manual Musical Review driver) ───────────
if(APPLE)
    add_executable(korg-playback tools/korg_playback_cli.cpp)
    target_link_libraries(korg-playback platform-apple arranger-engine Threads::Threads)
endif()

# ── KORG validation harness (Gate 11) ─────────────────────────────────
# Pre-hardware, CI-safe measurement tooling. Its CTest targets aggregate into
# the top-level test run (scripts/run_tests.sh + CI).
add_subdirectory(tools/korg_validation)

# ── Gate 14 macOS app shell (JUCE) — CI-optional ──────────────────────
# The headless app-shell modules (src/session|control|ui|midi) are always built
# into arranger-engine and CI-tested above. The JUCE GUI app is built ONLY when
# BUILD_MACOS_APP=ON (default OFF) so CI stays fast and needs no JUCE fetch.
option(BUILD_MACOS_APP "Build the JUCE macOS app shell (Gate 14)" OFF)
if(BUILD_MACOS_APP AND NOT APPLE)
    message(FATAL_ERROR "BUILD_MACOS_APP=ON requires an Apple host")
endif()
if(BUILD_MACOS_APP)
    add_subdirectory(apps/macos)
endif()
