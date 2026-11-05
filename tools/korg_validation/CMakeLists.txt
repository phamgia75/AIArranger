# ── KORG validation harness (Gate 11) ────────────────────────────────
# Vendor-agnostic, CI-safe measurement tooling. CONSUMES the engine; never
# modifies it. Synthetic-only — no real-device I/O. Tests register with the
# top-level CTest so scripts/run_tests.sh (and CI) cover them automatically.

file(GLOB KORG_VALIDATION_SRC src/*.cpp)

add_library(korg-validation STATIC ${KORG_VALIDATION_SRC})
target_include_directories(korg-validation PUBLIC include)
# Read-only consumer of the engine (UASF types, StylePlayer, MIDI).
target_link_libraries(korg-validation PUBLIC arranger-engine)

# CLI front-end.
add_executable(korg-validate korg_validate_cli.cpp)
target_link_libraries(korg-validate korg-validation arranger-engine)

# Per-file test executables (same pattern as the engine suite).
file(GLOB KORG_VALIDATION_TEST_SRC tests/*.cpp)
foreach(test_file ${KORG_VALIDATION_TEST_SRC})
    get_filename_component(test_name ${test_file} NAME_WE)
    add_executable(${test_name} ${test_file})
    target_link_libraries(${test_name} korg-validation arranger-engine Threads::Threads)
    target_compile_definitions(${test_name} PRIVATE
        KORG_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
        KORG_REPO_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Hardware-transmission guard (see the top-level CMakeLists): no test here may
# drive a real MIDI device.
get_property(_kv_tests DIRECTORY PROPERTY TESTS)
if(_kv_tests)
    set_property(TEST ${_kv_tests} APPEND PROPERTY ENVIRONMENT "AIARR_MIDI_HW_TX=0")
endif()
