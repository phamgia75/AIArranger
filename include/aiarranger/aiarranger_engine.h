#ifndef AIARRANGER_ENGINE_H
#define AIARRANGER_ENGINE_H

/* ── AIArranger C ABI bridge (Gate 4 skeleton) ───────────────────────────────
 *
 * A pure C, opaque-handle boundary between the portable C++ engine core and a
 * product host (the Gate 5 iPad SwiftUI app, or any C/Swift/other caller). This
 * is the ONE surface the product layer links against.
 *
 * Bridge contract (frozen — see docs/architecture/ENGINE_PRODUCT_CONTRACT.md):
 *   - Nothing but C crosses this line: no C++ types, no STL containers, no
 *     exceptions. Every entry point is `noexcept` on the C++ side; a failure is
 *     an AiArrError return code, never a thrown exception.
 *   - The engine is an OPAQUE handle (AiArrEngine*). The caller never sees the
 *     C++ layout; ownership is explicit (create / destroy).
 *   - State is read as plain POD structs the caller owns (AiArrSnapshot,
 *     AiArrCapabilities) — no callbacks retaining C++ ownership.
 *   - Enum integer values match ai_arranger::session::EngineError /
 *     LifecycleState 1:1 (static_assert-locked in the .cpp), so the same integer
 *     means the same thing on both sides.
 *
 * Threading: an AiArrEngine* is NOT internally synchronised. Drive lifecycle +
 * aiarr_engine_tick() from one owner thread; commands (tempo/variation/transport)
 * may come from another thread (they enqueue lock-free); read snapshots from any
 * thread. See the doc for the full thread-ownership table.
 *
 * Gate 4 scope: this skeleton binds NO MIDI hardware (headless). Device binding,
 * style-file loading, and input event injection are Gate 5 additions layered on
 * the same handle — the ABI is designed to extend additively.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque engine handle. */
typedef struct AiArrEngine AiArrEngine;

/* Result codes. 0 == success. Values match ai_arranger::session::EngineError. */
typedef enum AiArrError {
    AIARR_OK = 0,
    AIARR_ERR_NULL_ARGUMENT,
    AIARR_ERR_INVALID_STATE,
    AIARR_ERR_NOT_CONFIGURED,
    AIARR_ERR_NO_STYLE_LOADED,
    AIARR_ERR_ALREADY_STARTED,
    AIARR_ERR_NOT_STARTED,
    AIARR_ERR_DEVICE_UNAVAILABLE,
    AIARR_ERR_QUEUE_FULL,
    AIARR_ERR_UNSUPPORTED
} AiArrError;

/* High-level lifecycle state. Values match session::LifecycleState. */
typedef enum AiArrLifecycleState {
    AIARR_LIFECYCLE_CREATED = 0,
    AIARR_LIFECYCLE_CONFIGURED,
    AIARR_LIFECYCLE_READY,
    AIARR_LIFECYCLE_RUNNING,
    AIARR_LIFECYCLE_SUSPENDED,
    AIARR_LIFECYCLE_STOPPED,
    AIARR_LIFECYCLE_SHUTDOWN
} AiArrLifecycleState;

/* UI-facing engine snapshot — POD mirror of session::EngineSnapshot.
 * Booleans are int32_t (0/1) for stable C ABI. */
typedef struct AiArrSnapshot {
    int32_t  playing;
    uint8_t  chordRoot;
    uint8_t  chordType;
    uint8_t  chordBass;
    int32_t  section;
    uint32_t tempoBpm;
    int64_t  positionTicks;
    int32_t  performerState;
    int32_t  lifecycleState;    /* AiArrLifecycleState */
    int32_t  midiInLive;
    int32_t  midiOutLive;
    uint64_t receivedMessages;
    uint64_t dispatchedNotes;
} AiArrSnapshot;

/* Engine build/instance capabilities — POD mirror of session::EngineCapabilities. */
typedef struct AiArrCapabilities {
    uint32_t contractVersion;
    int32_t  hasMidiInput;
    int32_t  hasMidiOutput;
    int32_t  hasChordDetect;
    int32_t  hasSections;
    int32_t  hasSuspendResume;
    int32_t  latencyTrace;
    uint32_t maxCommandQueue;
} AiArrCapabilities;

/* Packed contract version (MAJOR<<16 | MINOR<<8 | PATCH). Callable without a
 * handle so a host can gate on ABI compatibility before creating an engine. */
uint32_t aiarr_contract_version(void);

/* ── Lifecycle ─────────────────────────────────────────────────────────────
 * create() returns NULL only on allocation failure. destroy(NULL) is a no-op. */
AiArrEngine* aiarr_engine_create(void);
void         aiarr_engine_destroy(AiArrEngine* engine);

/* Boot / teardown the live loop. Idempotent on the C++ side. */
AiArrError aiarr_engine_start(AiArrEngine* engine);
AiArrError aiarr_engine_stop(AiArrEngine* engine);

/* Emergency all-notes-off (safe from any active state). */
AiArrError aiarr_engine_panic(AiArrEngine* engine);

/* Transport + parameter commands (enqueued lock-free, applied in tick()).
 * The result says whether the engine ACCEPTED the command (contract 1.0.2):
 *   AIARR_OK               queued; it is applied on a following tick().
 *   AIARR_ERR_QUEUE_FULL   dropped: the command queue (maxCommandQueue) is full
 *                          because tick() has not drained it. Nothing changes.
 *   AIARR_ERR_UNSUPPORTED  argument outside the engine range (tempo 20..400 BPM,
 *                          variation 0..3). Nothing changes.
 *   AIARR_ERR_NULL_ARGUMENT  NULL handle.
 * Acceptance is not the outcome: what the engine did with an accepted command
 * (e.g. Start with nothing to play) shows in the next snapshot, and
 * aiarr_engine_stop() discards accepted commands not yet applied (a session
 * boundary). Before 1.0.2
 * these returned AIARR_OK even when the command was dropped or ignored. */
AiArrError aiarr_engine_transport_start(AiArrEngine* engine);
AiArrError aiarr_engine_transport_stop(AiArrEngine* engine);
AiArrError aiarr_engine_set_tempo(AiArrEngine* engine, uint32_t bpm);
AiArrError aiarr_engine_set_variation(AiArrEngine* engine, int32_t index /* 0..3 */);

/* Advance engine time by numSamples of wall clock. The host clock/timer adapter
 * drives this (e.g. from a CADisplayLink / audio callback on iPad). */
AiArrError aiarr_engine_tick(AiArrEngine* engine, uint32_t numSamples);

/* ── Reads (fill a caller-owned POD; NULL out -> AIARR_ERR_NULL_ARGUMENT) ── */
AiArrError aiarr_engine_snapshot(const AiArrEngine* engine, AiArrSnapshot* out);
AiArrError aiarr_engine_capabilities(const AiArrEngine* engine, AiArrCapabilities* out);
AiArrError aiarr_engine_last_error(const AiArrEngine* engine, AiArrError* out);
AiArrLifecycleState aiarr_engine_lifecycle_state(const AiArrEngine* engine);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AIARRANGER_ENGINE_H */
