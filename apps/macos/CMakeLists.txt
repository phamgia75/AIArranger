# ── Gate 14 — JUCE macOS app shell (CI-optional) ──────────────────────
# Configured only when BUILD_MACOS_APP=ON (see top-level CMakeLists). Fetches a
# pinned JUCE 7.x. The app is a pure client of arranger-engine: it sends intent
# through the engine bridge and renders ViewModel state. No DSP/audio is done by
# the app. Requires network to fetch JUCE; intended for local/dev builds.

include(FetchContent)
FetchContent_Declare(
    juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4    # macOS 15+/SDK-26 compatible (ScreenCaptureKit)
)
FetchContent_MakeAvailable(juce)

file(GLOB MACOS_APP_SRC Source/*.cpp)

juce_add_gui_app(aiarranger-macos
    PRODUCT_NAME "AI Arranger"
    COMPANY_NAME "AIArranger"
    BUNDLE_ID "com.aiarranger.macos")

target_sources(aiarranger-macos PRIVATE ${MACOS_APP_SRC})

target_compile_definitions(aiarranger-macos PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="AI Arranger"
    JUCE_APPLICATION_VERSION_STRING="0.14.0")

# The shell consumes the engine + the headless app-shell modules (session/
# control/ui/midi) that live in arranger-engine.
target_link_libraries(aiarranger-macos PRIVATE
    # arranger-engine is pulled in transitively (platform-apple PUBLIC +
    # importer-sff1), so it is not listed explicitly — a second, non-adjacent
    # reference makes the linker warn about a duplicate static library.
    platform-apple
    importer-sff1
    juce::juce_gui_extra
    juce::juce_audio_basics
    juce::juce_audio_devices)

# ── P2.5 UI smoke test: the real LiveHostComponent, headless-safe ───────
# Same sources as the app minus Main.cpp. Runs only in BUILD_MACOS_APP builds.
juce_add_console_app(aiarranger-app-smoke PRODUCT_NAME "aiarranger-app-smoke")
set(MACOS_APP_LIB_SRC ${MACOS_APP_SRC})
list(FILTER MACOS_APP_LIB_SRC EXCLUDE REGEX "Main\\.cpp$")
target_sources(aiarranger-app-smoke PRIVATE Tests/app_smoke.cpp ${MACOS_APP_LIB_SRC})
target_compile_definitions(aiarranger-app-smoke PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(aiarranger-app-smoke PRIVATE
    platform-apple importer-sff1
    juce::juce_gui_extra juce::juce_audio_basics juce::juce_audio_devices)
add_test(NAME test_app_smoke COMMAND aiarranger-app-smoke)
set_tests_properties(test_app_smoke PROPERTIES ENVIRONMENT "AIARR_MIDI_HW_TX=0" RUN_SERIAL TRUE)
