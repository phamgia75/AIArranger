// Gate 4 — C ABI bridge round-trip (headless).
//
// Drives the engine purely through the pure-C surface (aiarranger_engine.h): the
// opaque handle lifecycle, POD snapshot/capabilities reads, transport + tick,
// deterministic error codes, null-handle safety, and repeated create/destroy.
// Proves the product-bridge boundary compiles and behaves without any C++ type
// crossing the line — the shape the Gate 5 Swift host will call.

#include "aiarranger/aiarranger_engine.h"
#include <cstdio>

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

int main() {
    std::printf("Test: Gate 4 C ABI bridge round-trip\n");

    // ── Contract version reachable without a handle ──
    TEST("contract version non-zero", aiarr_contract_version() != 0u);

    // ── Null-handle safety: every entry point rejects NULL deterministically ──
    {
        AiArrSnapshot snap;
        AiArrCapabilities caps;
        AiArrError err;
        TEST("null start -> NULL_ARGUMENT",      aiarr_engine_start(nullptr) == AIARR_ERR_NULL_ARGUMENT);
        TEST("null stop -> NULL_ARGUMENT",       aiarr_engine_stop(nullptr) == AIARR_ERR_NULL_ARGUMENT);
        TEST("null tick -> NULL_ARGUMENT",       aiarr_engine_tick(nullptr, 48) == AIARR_ERR_NULL_ARGUMENT);
        TEST("null snapshot -> NULL_ARGUMENT",   aiarr_engine_snapshot(nullptr, &snap) == AIARR_ERR_NULL_ARGUMENT);
        TEST("null caps -> NULL_ARGUMENT",       aiarr_engine_capabilities(nullptr, &caps) == AIARR_ERR_NULL_ARGUMENT);
        TEST("null last_error -> NULL_ARGUMENT", aiarr_engine_last_error(nullptr, &err) == AIARR_ERR_NULL_ARGUMENT);
        aiarr_engine_destroy(nullptr);   // must not crash
        TEST("destroy(NULL) is safe", true);
    }

    // ── Create + capabilities ──
    AiArrEngine* eng = aiarr_engine_create();
    TEST("create returns a handle", eng != nullptr);

    AiArrCapabilities caps;
    TEST("capabilities Ok", aiarr_engine_capabilities(eng, &caps) == AIARR_OK);
    TEST("caps contract version matches free function",
         caps.contractVersion == aiarr_contract_version());
    // Contract 1.1.0: the bridge owns host-fed MIDI ports (both start disconnected).
    TEST("caps: bridge has host MIDI in/out ports",
         caps.hasMidiInput == 1 && caps.hasMidiOutput == 1);

    // ── NULL out pointer rejected even with a valid handle ──
    TEST("snapshot NULL out -> NULL_ARGUMENT",
         aiarr_engine_snapshot(eng, nullptr) == AIARR_ERR_NULL_ARGUMENT);

    // ── Lifecycle: created -> running -> stopped via the C ABI ──
    TEST("lifecycle starts Created",
         aiarr_engine_lifecycle_state(eng) == AIARR_LIFECYCLE_CREATED);
    TEST("start Ok", aiarr_engine_start(eng) == AIARR_OK);
    TEST("lifecycle Running after start",
         aiarr_engine_lifecycle_state(eng) == AIARR_LIFECYCLE_RUNNING);

    AiArrSnapshot snap;
    TEST("snapshot Ok", aiarr_engine_snapshot(eng, &snap) == AIARR_OK);
    TEST("snapshot mirrors lifecycle (Running)",
         snap.lifecycleState == AIARR_LIFECYCLE_RUNNING);

    // ── Tempo applies through the bridge (checked before playback, since a
    //    running style reasserts its own tempo — matches facade semantics) ──
    TEST("set tempo Ok", aiarr_engine_set_tempo(eng, 100) == AIARR_OK);
    aiarr_engine_tick(eng, 48);
    aiarr_engine_snapshot(eng, &snap);
    TEST("tempo applied through bridge", snap.tempoBpm == 100u);

    // ── Transport + tick advance the engine into playback ──
    TEST("transport start Ok", aiarr_engine_transport_start(eng) == AIARR_OK);
    for (int i = 0; i < 100; ++i) aiarr_engine_tick(eng, 48);
    aiarr_engine_snapshot(eng, &snap);
    TEST("engine playing after transport start", snap.playing == 1);

    // ── set_variation input validation is deterministic ──
    TEST("set variation 2 Ok",     aiarr_engine_set_variation(eng, 2) == AIARR_OK);
    TEST("set variation 9 -> UNSUPPORTED", aiarr_engine_set_variation(eng, 9) == AIARR_ERR_UNSUPPORTED);

    // ── Panic is accepted, then stop ──
    TEST("panic Ok", aiarr_engine_panic(eng) == AIARR_OK);
    for (int i = 0; i < 16; ++i) aiarr_engine_tick(eng, 48);
    TEST("stop Ok", aiarr_engine_stop(eng) == AIARR_OK);
    TEST("lifecycle Stopped after stop",
         aiarr_engine_lifecycle_state(eng) == AIARR_LIFECYCLE_STOPPED);

    // ── last_error is readable and Ok on a clean run ──
    AiArrError le;
    TEST("last_error readable", aiarr_engine_last_error(eng, &le) == AIARR_OK);

    aiarr_engine_destroy(eng);

    // ── Repeated create/destroy is stable ──
    {
        bool ok = true;
        for (int i = 0; i < 200; ++i) {
            AiArrEngine* e = aiarr_engine_create();
            if (!e) { ok = false; break; }
            aiarr_engine_start(e);
            aiarr_engine_tick(e, 48);
            aiarr_engine_stop(e);
            aiarr_engine_destroy(e);
        }
        TEST("200x create/start/tick/stop/destroy stable", ok);
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}
