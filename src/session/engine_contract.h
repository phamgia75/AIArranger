#ifndef AI_ARRANGER_SESSION_ENGINE_CONTRACT_H
#define AI_ARRANGER_SESSION_ENGINE_CONTRACT_H

#include <cstdint>

// ── Engine/product contract (Gate 4 — frozen public surface) ─────────────────
//
// This header pins the *value types* of the LiveEngineFacade public contract so
// the portable engine core and any product layer (the Gate 5 iPad SwiftUI app,
// the C ABI bridge, headless tests) agree on one vocabulary. Everything here is
// POD / trivially-copyable and ABI-stable:
//
//   - no pointers, no STL containers, no virtual tables;
//   - enum underlying types are fixed (int32_t / uint8_t) so the same integer
//     value crosses the C ABI bridge unchanged;
//   - additive-only evolution (see kEngineContractVersion) — existing enumerators
//     keep their values forever; new ones are appended.
//
// This is a *definition* header. It changes no engine runtime behaviour and links
// no Apple framework. See docs/architecture/ENGINE_PRODUCT_CONTRACT.md.

namespace ai_arranger::session {

// ── Contract version ─────────────────────────────────────────────────────────
// Bumped only when the public contract changes. MAJOR breaks ABI/behaviour (never
// silently); MINOR appends enumerators/fields with defaults; PATCH is docs/impl.
inline constexpr uint32_t kEngineContractVersionMajor = 1;
inline constexpr uint32_t kEngineContractVersionMinor = 0;
inline constexpr uint32_t kEngineContractVersionPatch = 1;  // 1.0.1: MPSC command queue + lossless drain
inline constexpr uint32_t kEngineContractVersion =
    (kEngineContractVersionMajor << 16) |
    (kEngineContractVersionMinor << 8)  |
     kEngineContractVersionPatch;

// ── Error model ──────────────────────────────────────────────────────────────
// Deterministic, exhaustive result codes. Every fallible contract operation
// returns one of these; the same inputs always produce the same code. int32_t so
// it maps 1:1 onto the C ABI bridge return type. 0 == success (Ok) by convention.
enum class EngineError : int32_t {
    Ok = 0,               // operation succeeded
    NullArgument,         // a required pointer/handle argument was null
    InvalidState,         // operation illegal for the current lifecycle state
    NotConfigured,        // engine not yet configured (see LifecycleState)
    NoStyleLoaded,        // a style is required but none is loaded
    AlreadyStarted,       // start requested while already running
    NotStarted,           // an operation required a running engine
    DeviceUnavailable,    // requested MIDI in/out device index is not present
    QueueFull,            // a command/input event was dropped: its queue was full
    Unsupported,          // operation not supported in this build/configuration
};

// Human-readable, allocation-free name for logs/tests. Returns a static string.
inline const char* toString(EngineError e) noexcept {
    switch (e) {
        case EngineError::Ok:                return "Ok";
        case EngineError::NullArgument:      return "NullArgument";
        case EngineError::InvalidState:      return "InvalidState";
        case EngineError::NotConfigured:     return "NotConfigured";
        case EngineError::NoStyleLoaded:     return "NoStyleLoaded";
        case EngineError::AlreadyStarted:    return "AlreadyStarted";
        case EngineError::NotStarted:        return "NotStarted";
        case EngineError::DeviceUnavailable: return "DeviceUnavailable";
        case EngineError::QueueFull:         return "QueueFull";
        case EngineError::Unsupported:       return "Unsupported";
    }
    return "Unknown";
}

// ── Capability queries ───────────────────────────────────────────────────────
// What this engine build can do. The product layer queries this once (rather than
// hard-coding assumptions) so a Gate-5 host degrades gracefully across builds.
// Trivially-copyable; safe to marshal across the bridge as a plain struct.
struct EngineCapabilities {
    uint32_t contractVersion = kEngineContractVersion;
    bool     hasMidiInput   = false;  // a MIDI input source is wired
    bool     hasMidiOutput  = false;  // a MIDI output provider is wired
    bool     hasChordDetect = true;   // fingered/one-finger chord scan available
    bool     hasSections    = true;   // intro/main/fill/break/ending transitions
    bool     hasSuspendResume = true; // lifecycle suspend/resume is modelled
    bool     latencyTrace   = false;  // AIARR_LATENCY_TRACE compiled in
    uint32_t maxCommandQueue = 256;   // command (MPSC) queue depth, exact
};

} // namespace ai_arranger::session

#endif // AI_ARRANGER_SESSION_ENGINE_CONTRACT_H
