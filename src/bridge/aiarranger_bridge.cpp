// AIArranger C ABI bridge implementation (Gate 4 skeleton; P2.4 host MIDI ports).
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

#include "engine/chord/fingered_detector.h"
#include "engine/chord/fingered_on_bass_detector.h"
#include "engine/chord/single_finger_detector.h"
#include "engine/midi/midi_bytes.h"
#include "engine/trace/mpsc_ring.h"
#include "engine/uasf/deserializer.h"
#include "midi/midi_input_parser.h"
#include "midi/midi_input_source.h"
#include "midi/midi_output_provider.h"

#include <atomic>
#include <mutex>
#include <new>
#include <vector>

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

namespace {
namespace midi = ai_arranger::midi;
namespace uasf = ai_arranger::uasf;

// Host-fed input port (index 0). The host thread injects bytes; the mutex only
// orders inject() against setSink() from start()/stop() (neither is realtime).
class HostMidiInput final : public midi::IMidiInputSource {
public:
    void setSink(MessageSink sink) noexcept override {
        std::lock_guard<std::mutex> lk(m_);
        sink_ = std::move(sink);
    }
    std::vector<midi::MidiSourceInfo> enumerateSources() const noexcept override {
        try { return {midi::MidiSourceInfo{0, "Host"}}; } catch (...) { return {}; }
    }
    int  sourceCount() const noexcept override { return 1; }
    bool selectSource(int index) noexcept override {
        if (index != 0 && index != -1) return false;
        selected_.store(index, std::memory_order_relaxed);
        return true;
    }
    int  selectedSource() const noexcept override { return selected_.load(std::memory_order_relaxed); }
    bool hasLiveSource() const noexcept override { return selectedSource() == 0; }
    uint64_t receivedCount() const noexcept override { return received_.load(std::memory_order_relaxed); }

    static constexpr size_t kMaxBytes = 256;   // every message is >= 1 byte

    // False when the port is not selected. len <= kMaxBytes (caller checks).
    bool inject(const uint8_t* bytes, size_t len) noexcept {
        if (!hasLiveSource()) return false;
        midi::MidiInputMessage msgs[kMaxBytes];
        const size_t n = midi::parseMidiInput(bytes, len, msgs, kMaxBytes);
        std::lock_guard<std::mutex> lk(m_);
        for (size_t i = 0; i < n; ++i) if (sink_) sink_(msgs[i]);
        received_.fetch_add(n, std::memory_order_relaxed);
        return true;
    }

private:
    std::mutex m_;
    MessageSink sink_;
    std::atomic<int> selected_{-1};
    std::atomic<uint64_t> received_{0};
};

// Host-drained output port (index 0). Producer: the engine's tick thread.
class HostMidiOutput final : public midi::IMidiOutputProvider {
public:
    std::vector<midi::MidiOutDeviceInfo> enumerate() override { return {midi::MidiOutDeviceInfo{0, "Host"}}; }
    bool select(int index) override {
        if (index != 0 && index != -1) return false;
        selected_.store(index, std::memory_order_relaxed);
        return true;
    }
    int  selected() const override { return selected_.load(std::memory_order_relaxed); }
    bool hasLiveDestination() const override { return selected() == 0; }
    bool send(const uasf::MidiEvent& ev) override {
        if (!hasLiveDestination()) return false;
        if (!ring_.try_push(ev)) {   // host not draining: a lost NoteOff would stick
            overflow_.store(true, std::memory_order_release);
            return false;
        }
        dispatched_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    uint64_t dispatchedCount() const override { return dispatched_.load(std::memory_order_relaxed); }
    bool pop(uasf::MidiEvent& ev) noexcept { return ring_.try_pop(ev); }
    // True once after messages were lost; the drainer then appends silence.
    bool takeOverflow() noexcept { return overflow_.exchange(false, std::memory_order_acq_rel); }

private:
    ai_arranger::trace::MpscRing<uasf::MidiEvent, 1024> ring_;
    std::atomic<bool> overflow_{false};
    std::atomic<int> selected_{-1};
    std::atomic<uint64_t> dispatched_{0};
};
} // namespace

// The opaque handle: the portable facade wired to the host MIDI ports.
struct AiArrEngine {
    HostMidiInput  in;
    HostMidiOutput out;
    ai_arranger::chord::FingeredDetector       fingered;
    ai_arranger::chord::FingeredOnBassDetector onBass;
    ai_arranger::chord::SingleFingerDetector   singleFinger;
    LiveEngineFacade facade{&in, &out};
    AiArrEngine() noexcept { facade.setChordScanMode(&fingered); }
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

AiArrError aiarr_engine_load_style_uasf(AiArrEngine* engine, const uint8_t* data, uint32_t size) {
    if (!engine || !data) return AIARR_ERR_NULL_ARGUMENT;
    try {
        const auto r = ai_arranger::uasf::UasfDeserializer().deserialize(std::vector<uint8_t>(data, data + size));
        if (!r.success) return AIARR_ERR_UNSUPPORTED;
        return static_cast<AiArrError>(static_cast<int>(engine->facade.loadStyle(r.style)));
    } catch (...) {   // only the input copy above can throw here
        return AIARR_ERR_UNSUPPORTED;
    }
    // shortcut: the deserializer and the loadStyle chain are noexcept, so an
    // allocation failure inside them terminates (engine-wide, pre-existing);
    // upgrade if hosts must survive OOM during a style load.
}

AiArrError aiarr_engine_set_chord_mode(AiArrEngine* engine, int32_t mode) {
    if (!engine) return AIARR_ERR_NULL_ARGUMENT;
    const ai_arranger::chord::IChordScanMode* m = nullptr;
    switch (mode) {
        case AIARR_CHORD_MODE_FINGERED:         m = &engine->fingered; break;
        case AIARR_CHORD_MODE_FINGERED_ON_BASS: m = &engine->onBass; break;
        case AIARR_CHORD_MODE_SINGLE_FINGER:    m = &engine->singleFinger; break;
        default: return AIARR_ERR_UNSUPPORTED;
    }
    engine->facade.setChordScanMode(m);
    return AIARR_OK;
}

AiArrError aiarr_engine_select_midi_input(AiArrEngine* engine, int32_t index) {
    if (!engine) return AIARR_ERR_NULL_ARGUMENT;
    return engine->facade.selectMidiInput(index) ? AIARR_OK : AIARR_ERR_DEVICE_UNAVAILABLE;
}

AiArrError aiarr_engine_select_midi_output(AiArrEngine* engine, int32_t index) {
    if (!engine) return AIARR_ERR_NULL_ARGUMENT;
    // Switching away mid-play would strand the NoteOffs of sounding notes.
    if (index != engine->out.selected() && engine->facade.snapshot().playing) return AIARR_ERR_INVALID_STATE;
    return engine->facade.selectMidiOutput(index) ? AIARR_OK : AIARR_ERR_DEVICE_UNAVAILABLE;
}

AiArrError aiarr_engine_send_midi_in(AiArrEngine* engine, const uint8_t* bytes, uint32_t length) {
    if (!engine || (!bytes && length)) return AIARR_ERR_NULL_ARGUMENT;
    if (length > HostMidiInput::kMaxBytes) return AIARR_ERR_UNSUPPORTED;
    return engine->in.inject(bytes, length) ? AIARR_OK : AIARR_ERR_DEVICE_UNAVAILABLE;
}

AiArrError aiarr_engine_read_midi_out(AiArrEngine* engine, AiArrMidiMessage* out,
                                      uint32_t capacity, uint32_t* count) {
    if (!engine || !count || (!out && capacity)) return AIARR_ERR_NULL_ARGUMENT;
    uint32_t n = 0;
    ai_arranger::uasf::MidiEvent ev{};
    while (n < capacity && engine->out.pop(ev)) {
        uint8_t b[3] = {0, 0, 0};
        const size_t len = ai_arranger::midi::encodeMidiEvent(ev, b);
        if (len == 0) continue;
        out[n++] = AiArrMidiMessage{b[0], b[1], b[2], static_cast<uint8_t>(len)};
    }
    // Messages were lost (buffer full): once the backlog is out, silence every
    // channel (All Sound Off + All Notes Off) so no lost NoteOff leaves a stuck note.
    constexpr uint32_t kSilence = 32;
    if (n + kSilence <= capacity && engine->out.takeOverflow()) {
        for (uint8_t ch = 0; ch < 16; ++ch) {
            out[n++] = AiArrMidiMessage{static_cast<uint8_t>(0xB0 | ch), 120, 0, 3};
            out[n++] = AiArrMidiMessage{static_cast<uint8_t>(0xB0 | ch), 123, 0, 3};
        }
    }
    *count = n;
    return AIARR_OK;
}

} // extern "C"
