#include "engine/midi/coremidi_out.h"
#include "engine/trace/latency_trace.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>

// MIDIPacketList / MIDIPacketListAdd are deprecated in favour of the newer
// MIDIEventList API, but remain fully functional and are simpler for a
// single-endpoint output path. Silence the deprecation noise locally.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace ai_arranger::midi {

// ── Pure conversion (no CoreMIDI) ────────────────────────────────────

size_t midiEventToBytes(const uasf::MidiEvent& ev, uint8_t* out) noexcept {
    const uint8_t status = static_cast<uint8_t>(ev.type) | (ev.channel & 0x0F);
    switch (ev.type) {
        case uasf::MidiEventType::NoteOff:
        case uasf::MidiEventType::NoteOn:
        case uasf::MidiEventType::PolyPressure:
        case uasf::MidiEventType::ControlChange:
        case uasf::MidiEventType::PitchBend:
            out[0] = status;
            out[1] = ev.data1 & 0x7F;
            out[2] = ev.data2 & 0x7F;
            return 3;
        case uasf::MidiEventType::ProgramChange:
        case uasf::MidiEventType::ChannelPressure:
            out[0] = status;
            out[1] = ev.data1 & 0x7F;
            return 2;
        default:
            return 0;
    }
}

// ── Name resolution helpers (control / dispatch threads only) ────────

namespace {

// Serialises selection changes (control thread) against hot-plug re-resolution
// (dispatch thread), so a deselect can never be undone by an in-flight resolve.
// Neither thread is the audio thread.
std::mutex& selectionMutex() {
    static std::mutex m;
    return m;
}

int32_t endpointUniqueId(MIDIEndpointRef ep) {
    SInt32 uid = 0;
    if (ep == 0 || MIDIObjectGetIntegerProperty(ep, kMIDIPropertyUniqueID, &uid) != noErr) return 0;
    return uid;
}

std::string endpointDisplayName(MIDIEndpointRef ep) {
    if (ep == 0) return {};
    CFStringRef cf = nullptr;
    if (MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &cf) != noErr || !cf) {
        return {};
    }
    char buf[256] = {0};
    CFStringGetCString(cf, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(cf);
    return std::string(buf);
}

} // namespace

// ── Lifecycle ────────────────────────────────────────────────────────

CoreMidiOut::~CoreMidiOut() {
    shutdown();
}

bool CoreMidiOut::initialize(const char* clientName) noexcept {
    if (initialized_.load(std::memory_order_acquire)) return true;

    // Hardware-transmission guard, read once here (never on the realtime path).
    const char* tx = std::getenv("AIARR_MIDI_HW_TX");
    hw_tx_allowed_ = !(tx && std::strcmp(tx, "0") == 0);

    CFStringRef cfClient = CFStringCreateWithCString(
        kCFAllocatorDefault, clientName ? clientName : "AI Arranger", kCFStringEncodingUTF8);
    OSStatus st = MIDIClientCreate(cfClient, &CoreMidiOut::notifyProc, this, &client_);
    if (cfClient) CFRelease(cfClient);
    if (st != noErr) {
        return false; // hard CoreMIDI failure
    }

    CFStringRef cfPort = CFStringCreateWithCString(
        kCFAllocatorDefault, "out", kCFStringEncodingUTF8);
    st = MIDIOutputPortCreate(client_, cfPort, &out_port_);
    if (cfPort) CFRelease(cfPort);
    if (st != noErr) {
        MIDIClientDispose(client_);
        client_ = 0;
        return false;
    }

    // Fail-closed: no destination is selected here, even if devices exist. The
    // caller must pick one explicitly (selectDestination) before anything is sent.

    running_.store(true, std::memory_order_release);
    dispatch_thread_ = std::thread([this]{ dispatchLoop(); });
    initialized_.store(true, std::memory_order_release);
    return true;
}

void CoreMidiOut::shutdown() noexcept {
    if (!initialized_.load(std::memory_order_acquire) &&
        !running_.load(std::memory_order_acquire)) {
        return;
    }
    running_.store(false, std::memory_order_release);
    if (dispatch_thread_.joinable()) dispatch_thread_.join();   // drains the queue

    // Deterministic cleanup: silence the explicitly selected device after the
    // final drain, so a Note On without its Note Off never keeps sounding.
    sendAllSoundOff(endpoint_.load(std::memory_order_acquire));

    if (out_port_) { MIDIPortDispose(out_port_); out_port_ = 0; }
    if (client_)   { MIDIClientDispose(client_);  client_ = 0; }
    endpoint_.store(0, std::memory_order_release);
    // Forget the selection: a later initialize() must start with no destination,
    // not let the hot-plug probe silently reconnect the old device.
    std::lock_guard<std::mutex> lk(selectionMutex());
    selected_index_.store(-1, std::memory_order_release);
    selected_uid_.store(0, std::memory_order_release);
    initialized_.store(false, std::memory_order_release);
}

bool CoreMidiOut::isInitialized() const noexcept {
    return initialized_.load(std::memory_order_acquire);
}

// ── Destination management ───────────────────────────────────────────

int CoreMidiOut::destinationCount() const noexcept {
    return static_cast<int>(MIDIGetNumberOfDestinations());
}

std::vector<MidiDestinationInfo> CoreMidiOut::enumerateDestinations() const noexcept {
    std::vector<MidiDestinationInfo> out;
    ItemCount n = MIDIGetNumberOfDestinations();
    out.reserve(n);
    for (ItemCount i = 0; i < n; ++i) {
        MIDIEndpointRef ep = MIDIGetDestination(i);
        out.push_back({static_cast<int>(i), endpointDisplayName(ep), endpointUniqueId(ep)});
    }
    return out;
}

bool CoreMidiOut::selectDestination(int index) noexcept {
    // Silence the OUTGOING device synchronously before repointing, so a switch (or
    // disconnect) never leaves it with hanging notes and its all-sound-off is never
    // sent to the newly selected device. Validate first so a failed switch leaves
    // both the old device and the current selection untouched.
    if (index < 0) {
        std::lock_guard<std::mutex> lk(selectionMutex());
        MIDIEndpointRef old = endpoint_.exchange(0, std::memory_order_acq_rel);
        if (old != 0) sendAllSoundOff(old);
        selected_uid_.store(0, std::memory_order_release);
        selected_index_.store(-1, std::memory_order_release);
        return true;
    }
    ItemCount n = MIDIGetNumberOfDestinations();
    if (static_cast<ItemCount>(index) >= n) return false;

    MIDIEndpointRef ep = MIDIGetDestination(static_cast<ItemCount>(index));
    // Device vanished between enumeration and selection: fail the switch cleanly
    // rather than repointing to endpoint 0 (which would silence the old device and
    // leave a half-switched state with selected_index_ set but no live endpoint).
    if (ep == 0) return false;
    return selectEndpoint(ep, index);
}

bool CoreMidiOut::selectDestinationByName(const std::string& name) noexcept {
    // One pass over the live list; the endpoint that matched is the one selected
    // (no re-query by index), so a hot-plug between lookup and selection cannot
    // redirect output to a different device.
    MIDIEndpointRef match = 0;
    int matchIndex = -1, matches = 0;
    const ItemCount n = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < n; ++i) {
        MIDIEndpointRef ep = MIDIGetDestination(i);
        if (ep != 0 && endpointDisplayName(ep) == name) { match = ep; matchIndex = static_cast<int>(i); ++matches; }
    }
    if (name.empty() || matches != 1) return false;   // missing or ambiguous: select nothing
    return selectEndpoint(match, matchIndex);
}

bool CoreMidiOut::selectDestinationByUid(int32_t uid) noexcept {
    if (uid == 0) return false;
    const ItemCount n = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < n; ++i) {
        MIDIEndpointRef ep = MIDIGetDestination(i);
        if (endpointUniqueId(ep) == uid) return selectEndpoint(ep, static_cast<int>(i));
    }
    return false;
}

bool CoreMidiOut::selectEndpoint(MIDIEndpointRef ep, int index) noexcept {
    const int32_t uid = endpointUniqueId(ep);
    if (uid == 0) return false;   // cannot be re-identified on hot-plug: refuse (fail closed)
    std::lock_guard<std::mutex> lk(selectionMutex());
    MIDIEndpointRef old = endpoint_.load(std::memory_order_acquire);
    if (old != 0 && old != ep) sendAllSoundOff(old);
    selected_uid_.store(uid, std::memory_order_release);
    selected_index_.store(index, std::memory_order_release);
    endpoint_.store(ep, std::memory_order_release);
    // Non-RT context: record the active output endpoint id (0 = none). +1 so a
    // valid index never collides with the "none" sentinel.
    AIARR_TRACE_SET_OUTPUT_DEVICE(index + 1);
    return true;
}

void CoreMidiOut::sendAllSoundOff(MIDIEndpointRef ep) noexcept {
    if (ep == 0 || out_port_ == 0) return;
    if (!hw_tx_allowed_) {   // guard: cleanup never reaches an unauthorized endpoint
        blocked_count_.fetch_add(32, std::memory_order_relaxed);
        return;
    }
    for (uint8_t ch = 0; ch < 16; ++ch) {
        const uint8_t ctrls[2] = {123 /*All Notes Off*/, 120 /*All Sound Off*/};
        for (uint8_t cc : ctrls) {
            uint8_t bytes[3] = {static_cast<uint8_t>(0xB0 | ch), cc, 0};
            uint8_t listBuf[64];
            MIDIPacketList* pl = reinterpret_cast<MIDIPacketList*>(listBuf);
            MIDIPacket* pkt = MIDIPacketListInit(pl);
            pkt = MIDIPacketListAdd(pl, sizeof(listBuf), pkt, /*timestamp=*/0, 3, bytes);
            if (pkt) MIDISend(out_port_, ep, pl);
        }
    }
}

int CoreMidiOut::selectedDestination() const noexcept {
    return selected_index_.load(std::memory_order_acquire);
}

bool CoreMidiOut::hasLiveDestination() const noexcept {
    return endpoint_.load(std::memory_order_acquire) != 0;
}

// ── Realtime-safe send ───────────────────────────────────────────────

bool CoreMidiOut::send(const uasf::MidiEvent& ev) noexcept {
    const bool pushed = queue_.push(ev);
    if (!pushed) {
        dropped_count_.fetch_add(1, std::memory_order_relaxed);
    }
    // Trace point (4): event enqueued onto the MIDI output queue. `pushed` false
    // means the SPSC queue was full (dropped) — recorded as a queue overflow.
    // Compiled out entirely when the build gate is off.
    AIARR_TRACE_OUTPUT_ENQUEUE(ev.channel, ev.data1, ev.data2, pushed);
    return pushed;
}

// ── Diagnostics ──────────────────────────────────────────────────────

void CoreMidiOut::setDispatchTap(DispatchTap tap) noexcept {
    dispatch_tap_ = std::move(tap);
}

uint64_t CoreMidiOut::sentCount() const noexcept { return sent_count_.load(std::memory_order_acquire); }
uint64_t CoreMidiOut::dispatchedCount() const noexcept { return dispatched_count_.load(std::memory_order_acquire); }
uint64_t CoreMidiOut::droppedCount() const noexcept { return dropped_count_.load(std::memory_order_acquire); }
uint64_t CoreMidiOut::reconnectCount() const noexcept { return reconnect_count_.load(std::memory_order_acquire); }
uint64_t CoreMidiOut::blockedCount() const noexcept { return blocked_count_.load(std::memory_order_acquire); }

// ── Hotplug re-resolution (dispatch / control thread) ────────────────

void CoreMidiOut::resolveSelectedEndpoint() noexcept {
    // Re-resolve by unique ID only: the exact device that was selected, or none.
    std::lock_guard<std::mutex> lk(selectionMutex());
    const int32_t want = selected_uid_.load(std::memory_order_acquire);
    if (selected_index_.load(std::memory_order_acquire) < 0 || want == 0) {
        endpoint_.store(0, std::memory_order_release);
        return;
    }

    MIDIEndpointRef found = 0;
    ItemCount n = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < n; ++i) {
        MIDIEndpointRef ep = MIDIGetDestination(i);
        if (endpointUniqueId(ep) == want) {
            found = ep;
            selected_index_.store(static_cast<int>(i), std::memory_order_release);
            break;
        }
    }

    MIDIEndpointRef prev = endpoint_.load(std::memory_order_acquire);
    endpoint_.store(found, std::memory_order_release);
    if (found != 0 && prev == 0) {
        reconnect_count_.fetch_add(1, std::memory_order_relaxed);
        AIARR_TRACE_COUNT_RECONNECT();
        AIARR_TRACE_LIFECYCLE(::ai_arranger::trace::LifecycleTag::kHotplugAdd);
    }
}

void CoreMidiOut::notifyProc(const MIDINotification* msg, void* refCon) {
    auto* self = static_cast<CoreMidiOut*>(refCon);
    if (!self || !msg) return;
    if (msg->messageID == kMIDIMsgSetupChanged ||
        msg->messageID == kMIDIMsgObjectAdded ||
        msg->messageID == kMIDIMsgObjectRemoved) {
        self->needs_reresolve_.store(true, std::memory_order_release);
    }
}

// ── Consumer thread ──────────────────────────────────────────────────

void CoreMidiOut::dispatchLoop() noexcept {
    using namespace std::chrono;
    constexpr auto kIdleSleep = microseconds(250);
    // ~100ms periodic reconnect probe when a device is selected but absent.
    constexpr int kProbeEvery = 400;
    int idleTicks = 0;

    while (running_.load(std::memory_order_acquire)) {
        // Hotplug: explicit notification, or periodic probe when disconnected.
        bool wantResolve = needs_reresolve_.exchange(false, std::memory_order_acq_rel);
        if (!wantResolve && selected_index_.load(std::memory_order_acquire) >= 0 &&
            endpoint_.load(std::memory_order_acquire) == 0) {
            if (++idleTicks >= kProbeEvery) { wantResolve = true; idleTicks = 0; }
        }
        if (wantResolve) resolveSelectedEndpoint();

        uasf::MidiEvent ev;
        bool any = false;
        while (queue_.pop(ev)) { any = true; dispatchOne(ev); }

        if (!any) std::this_thread::sleep_for(kIdleSleep);
    }

    // Final drain: send anything still queued (e.g. a close-time all-sound-off /
    // note-off flush) to the still-valid endpoint before shutdown() disposes the
    // port, so stopping the thread never truncates queued events on the wire.
    uasf::MidiEvent ev;
    while (queue_.pop(ev)) dispatchOne(ev);
}

void CoreMidiOut::dispatchOne(const uasf::MidiEvent& ev) noexcept {
    dispatched_count_.fetch_add(1, std::memory_order_relaxed);

    if (dispatch_tap_) dispatch_tap_(ev);

    // Hold the selection lock from reading the endpoint until MIDISend returns:
    // a deselect/switch (which silences the old device under the same lock) is
    // then strictly before or after this send, so an in-flight Note On can
    // never reach the old device after its cleanup. Dispatch thread, not RT.
    std::lock_guard<std::mutex> lk(selectionMutex());
    MIDIEndpointRef ep = endpoint_.load(std::memory_order_acquire);
    if (ep == 0) {
        // Graceful no-op: nowhere to send (headless / disconnected).
        dropped_count_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    uint8_t bytes[3];
    size_t nbytes = midiEventToBytes(ev, bytes);
    if (nbytes == 0) return;

    if (!hw_tx_allowed_) {   // guard at the MIDISend boundary
        blocked_count_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    uint8_t listBuf[256];
    MIDIPacketList* pktList = reinterpret_cast<MIDIPacketList*>(listBuf);
    MIDIPacket* pkt = MIDIPacketListInit(pktList);
    pkt = MIDIPacketListAdd(pktList, sizeof(listBuf), pkt,
                            /*timestamp=*/0, nbytes, bytes);
    if (pkt && MIDISend(out_port_, ep, pktList) == noErr) {
        sent_count_.fetch_add(1, std::memory_order_relaxed);
        // Trace point (5): CoreMIDI MIDISend actually invoked (last waypoint of
        // the input→output journey). Compiled out when the build gate is off.
        AIARR_TRACE_MIDI_SEND(ev.channel, ev.data1, ev.data2);
    }
}

} // namespace ai_arranger::midi

#pragma clang diagnostic pop
