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
 * Gate 4 scope bound no MIDI. Contract 1.1.0 (P2.4) adds UASF style loading,
 * chord mode, and host-fed MIDI in/out ports, additively (see the end of file).
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
    int32_t  hasSuspendResume;  /* 1 only when suspend/resume entry points exist (0 today) */
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
 *                          Tempo is not queued: it holds one pending value and
 *                          the latest accepted one before a tick() wins (an
 *                          earlier one may never apply; tempo never QUEUE_FULL).
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

/* ── Contract 1.1.0 additions (P2.4, additive) ─────────────────────────────
 * The bridge owns one HOST MIDI port in each direction. Physical device choice
 * (CoreMIDI, USB) stays in the host app: it feeds received bytes in and drains
 * outgoing messages out. Port index 0 = the host port, -1 = disconnected; any
 * other index -> AIARR_ERR_DEVICE_UNAVAILABLE. Both ports start disconnected. */

/* Owner thread, not while playing (AIARR_ERR_INVALID_STATE). `data` is a
 * serialized UASF file. Invalid or truncated data -> AIARR_ERR_UNSUPPORTED and
 * the current style is kept. */
AiArrError aiarr_engine_load_style_uasf(AiArrEngine* engine, const uint8_t* data, uint32_t size);

/* Chord recognition mode for notes below the split (owner thread). */
typedef enum AiArrChordMode {
    AIARR_CHORD_MODE_FINGERED = 0,          /* default */
    AIARR_CHORD_MODE_FINGERED_ON_BASS,
    AIARR_CHORD_MODE_SINGLE_FINGER
} AiArrChordMode;
AiArrError aiarr_engine_set_chord_mode(AiArrEngine* engine, int32_t mode /* AiArrChordMode */);

/* Changing the output port while playing -> AIARR_ERR_INVALID_STATE (the
 * NoteOffs of sounding notes would be lost); stop first. */
AiArrError aiarr_engine_select_midi_input(AiArrEngine* engine, int32_t index);
AiArrError aiarr_engine_select_midi_output(AiArrEngine* engine, int32_t index);

/* Raw MIDI bytes received by the host, at most 256 per call (more ->
 * AIARR_ERR_UNSUPPORTED, nothing injected). Running status is allowed within
 * one call; an incomplete trailing message is dropped. One injecting thread at
 * a time; it may differ from the tick thread. AIARR_ERR_DEVICE_UNAVAILABLE when
 * the input port is not selected. Input the engine had to drop (tick() not
 * draining) shows as AIARR_ERR_QUEUE_FULL in aiarr_engine_last_error(). */
AiArrError aiarr_engine_send_midi_in(AiArrEngine* engine, const uint8_t* bytes, uint32_t length);

/* One outgoing short message: `length` bytes of status/data1/data2 (1-3). */
typedef struct AiArrMidiMessage {
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
    uint8_t length;
} AiArrMidiMessage;

/* Drain up to `capacity` messages the engine sent to the output port since the
 * last call (one draining thread at a time). `*count` receives the number
 * written. While the port is disconnected the engine sends nothing. If the host
 * stops draining, messages beyond the port's buffer (1024) are dropped; once the
 * backlog is drained, a call with room for 32 more messages then receives CC 120
 * + CC 123 on all 16 channels, so a lost NoteOff never leaves a stuck note. */
AiArrError aiarr_engine_read_midi_out(AiArrEngine* engine, AiArrMidiMessage* out,
                                      uint32_t capacity, uint32_t* count);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AIARRANGER_ENGINE_H */
