// AIArranger C ABI bridge implementation (Gate 4 skeleton).
//
// Thin, exception-free adapter from the pure C surface (aiarranger_engine.h) to
// the portable LiveEngineFacade. It adds NO engine behaviour: every call forwards
// to an existing noexcept facade method and marshals POD in/out. The facade is
// constructed headless (no MIDI devices) in this gate; device binding is a Gate 5
// addition on the same handle.

#include "aiarranger/aiarranger_engine.h"

#include "session/live_engine_facade.h"
#include "session/engine_contract.h"
#include "session/engine_lifecycle.h"

#include <new>

using ai_arranger::session::LiveEngineFacade;
using ai_arranger::session::EngineError;
using ai_arranger::session::EngineCapabilities;
using ai_arranger::session::EngineSnapshot;
using ai_arranger::session::LifecycleState;

// ── ABI lock: the C enum integers MUST equal the C++ enum integers ───────────
static_assert(AIARR_OK                    == static_cast<int>(EngineError::Ok), "ABI drift");
static_assert(AIARR_ERR_NULL_ARGUMENT     == static_cast<int>(EngineError::NullArgument), "ABI drift");
static_assert(AIARR_ERR_INVALID_STATE     == static_cast<int>(EngineError::InvalidState), "ABI drift");
static_assert(AIARR_ERR_NOT_CONFIGURED    == static_cast<int>(EngineError::NotConfigured), "ABI drift");
static_assert(AIARR_ERR_NO_STYLE_LOADED   == static_cast<int>(EngineError::NoStyleLoaded), "ABI drift");
static_assert(AIARR_ERR_ALREADY_STARTED   == static_cast<int>(EngineError::AlreadyStarted), "ABI drift");
static_assert(AIARR_ERR_NOT_STARTED       == static_cast<int>(EngineError::NotStarted), "ABI drift");
static_assert(AIARR_ERR_DEVICE_UNAVAILABLE== static_cast<int>(EngineError::DeviceUnavailable), "ABI drift");
static_assert(AIARR_ERR_QUEUE_FULL        == static_cast<int>(EngineError::QueueFull), "ABI drift");
static_assert(AIARR_ERR_UNSUPPORTED       == static_cast<int>(EngineError::Unsupported), "ABI drift");

static_assert(AIARR_LIFECYCLE_CREATED    == static_cast<int>(LifecycleState::Created), "ABI drift");
static_assert(AIARR_LIFECYCLE_CONFIGURED == static_cast<int>(LifecycleState::Configured), "ABI drift");
static_assert(AIARR_LIFECYCLE_READY      == static_cast<int>(LifecycleState::Ready), "ABI drift");
static_assert(AIARR_LIFECYCLE_RUNNING    == static_cast<int>(LifecycleState::Running), "ABI drift");
static_assert(AIARR_LIFECYCLE_SUSPENDED  == static_cast<int>(LifecycleState::Suspended), "ABI drift");
static_assert(AIARR_LIFECYCLE_STOPPED    == static_cast<int>(LifecycleState::Stopped), "ABI drift");
static_assert(AIARR_LIFECYCLE_SHUTDOWN   == static_cast<int>(LifecycleState::ShutDown), "ABI drift");

// The opaque handle is just the portable facade, headless in this gate.
struct AiArrEngine {
    LiveEngineFacade facade{nullptr, nullptr};
};

extern "C" {

uint32_t aiarr_contract_version(void) {
    return ai_arranger::session::kEngineContractVersion;
}

AiArrEngine* aiarr_engine_create(void) {
    return new (std::nothrow) AiArrEngine();   // NULL only on allocation failure
}

void aiarr_engine_destroy(AiArrEngine* engine) {
    delete engine;   // delete nullptr is a no-op
}

AiArrError aiarr_engine_start(AiArrEngine* engine) {
    if (!engine) return AIARR_ERR_NULL_ARGUMENT;
    engine->facade.start();
    return AIARR_OK;
}

AiArrError aiarr_engine_stop(AiArrEngine* engine) {
    if (!engine) return AIARR_ERR_NULL_ARGUMENT;
    engine->facade.stop();
    return AIARR_OK;
}

AiArrError aiarr_engine_panic(AiArrEngine* engine) {
    if (!engine) return AIARR_ERR_NULL_ARGUMENT;
    return engine->facade.panic() ? AIARR_OK : AIARR_ERR_QUEUE_FULL;
}

AiArrError aiarr_engine_transport_start(AiArrEngine* engine) {
    if (!engine) return AIARR_ERR_NULL_ARGUMENT;
    return engine->facade.transportStart() ? AIARR_OK : AIARR_ERR_QUEUE_FULL;
}

AiArrError aiarr_engine_transport_stop(AiArrEngine* engine) {
    if (!engine) return AIARR_ERR_NULL_ARGUMENT;
    return engine->facade.transportStop() ? AIARR_OK : AIARR_ERR_QUEUE_FULL;
}

AiArrError aiarr_engine_set_tempo(AiArrEngine* engine, uint32_t bpm) {
    if (!engine) return AIARR_ERR_NULL_ARGUMENT;
    return engine->facade.setTempo(bpm) ? AIARR_OK : AIARR_ERR_UNSUPPORTED;   // outside 20..400
}

AiArrError aiarr_engine_set_variation(AiArrEngine* engine, int32_t index) {
    if (!engine) return AIARR_ERR_NULL_ARGUMENT;
    if (index < 0 || index > 3) return AIARR_ERR_UNSUPPORTED;
    return engine->facade.setVariation(index) ? AIARR_OK : AIARR_ERR_QUEUE_FULL;
}

AiArrError aiarr_engine_tick(AiArrEngine* engine, uint32_t numSamples) {
    if (!engine) return AIARR_ERR_NULL_ARGUMENT;
    engine->facade.tick(numSamples);
    return AIARR_OK;
}

AiArrError aiarr_engine_snapshot(const AiArrEngine* engine, AiArrSnapshot* out) {
    if (!engine || !out) return AIARR_ERR_NULL_ARGUMENT;
    const EngineSnapshot s = engine->facade.snapshot();
    out->playing          = s.playing ? 1 : 0;
    out->chordRoot        = s.chordRoot;
    out->chordType        = s.chordType;
    out->chordBass        = s.chordBass;
    out->section          = s.section;
    out->tempoBpm         = s.tempoBpm;
    out->positionTicks    = s.positionTicks;
    out->performerState   = s.performerState;
    out->lifecycleState   = s.lifecycleState;
    out->midiInLive       = s.midiInLive ? 1 : 0;
    out->midiOutLive      = s.midiOutLive ? 1 : 0;
    out->receivedMessages = s.receivedMessages;
    out->dispatchedNotes  = s.dispatchedNotes;
    return AIARR_OK;
}

AiArrError aiarr_engine_capabilities(const AiArrEngine* engine, AiArrCapabilities* out) {
    if (!engine || !out) return AIARR_ERR_NULL_ARGUMENT;
    const EngineCapabilities c = engine->facade.capabilities();
    out->contractVersion  = c.contractVersion;
    out->hasMidiInput     = c.hasMidiInput ? 1 : 0;
    out->hasMidiOutput    = c.hasMidiOutput ? 1 : 0;
    out->hasChordDetect   = c.hasChordDetect ? 1 : 0;
    out->hasSections      = c.hasSections ? 1 : 0;
    out->hasSuspendResume = c.hasSuspendResume ? 1 : 0;
    out->latencyTrace     = c.latencyTrace ? 1 : 0;
    out->maxCommandQueue  = c.maxCommandQueue;
    return AIARR_OK;
}

AiArrError aiarr_engine_last_error(const AiArrEngine* engine, AiArrError* out) {
    if (!engine || !out) return AIARR_ERR_NULL_ARGUMENT;
    *out = static_cast<AiArrError>(static_cast<int>(engine->facade.lastError()));
    return AIARR_OK;
}

AiArrLifecycleState aiarr_engine_lifecycle_state(const AiArrEngine* engine) {
    if (!engine) return AIARR_LIFECYCLE_CREATED;
    return static_cast<AiArrLifecycleState>(static_cast<int>(engine->facade.lifecycleState()));
}

} // extern "C"
