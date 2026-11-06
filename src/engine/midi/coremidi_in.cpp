#include "engine/midi/coremidi_in.h"
#include "engine/trace/latency_trace.h"

#include <thread>

// The MIDIPacketList / MIDIPacketNext read-callback API is deprecated in favour
// of MIDIEventList, but remains fully functional and is simpler for a single
// input port. Silence the deprecation noise locally (matches coremidi_out.cpp).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace ai_arranger::midi {

namespace {

std::string sourceDisplayName(MIDIEndpointRef ep) {
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

int32_t sourceUniqueId(MIDIEndpointRef ep) {
    SInt32 uid = 0;
    if (ep == 0 || MIDIObjectGetIntegerProperty(ep, kMIDIPropertyUniqueID, &uid) != noErr) return 0;
    return static_cast<int32_t>(uid);
}

} // namespace

CoreMidiIn::~CoreMidiIn() {
    shutdown();
}

bool CoreMidiIn::initialize(const char* clientName) noexcept {
    if (initialized_.load(std::memory_order_acquire)) return true;

    CFStringRef cfClient = CFStringCreateWithCString(
        kCFAllocatorDefault, clientName ? clientName : "AI Arranger", kCFStringEncodingUTF8);
    OSStatus st = MIDIClientCreate(cfClient, &CoreMidiIn::notifyProc, this, &client_);
    if (cfClient) CFRelease(cfClient);
    if (st != noErr) return false;   // hard CoreMIDI failure

    CFStringRef cfPort = CFStringCreateWithCString(
        kCFAllocatorDefault, "in", kCFStringEncodingUTF8);
    st = MIDIInputPortCreate(client_, cfPort, &CoreMidiIn::readProc, this, &in_port_);
    if (cfPort) CFRelease(cfPort);
    if (st != noErr) {
        MIDIClientDispose(client_);
        client_ = 0;
        return false;
    }

    initialized_.store(true, std::memory_order_release);
    return true;   // zero sources is a valid headless state
}

void CoreMidiIn::shutdown() noexcept {
    if (!initialized_.load(std::memory_order_acquire)) return;
    // Disconnect any live source before tearing the port down.
    MIDIEndpointRef ep = source_.exchange(0, std::memory_order_acq_rel);
    if (ep != 0 && in_port_ != 0) MIDIPortDisconnectSource(in_port_, ep);
    // Source disconnected but MIDIPortDisconnectSource does not join an in-flight
    // readProc — wait it out before disposing the port so no callback survives it.
    quiesceReadCallback();
    if (in_port_) { MIDIPortDispose(in_port_); in_port_ = 0; }
    if (client_)  { MIDIClientDispose(client_); client_ = 0; }
    selected_index_.store(-1, std::memory_order_release);
    selected_uid_.store(0, std::memory_order_release);
    initialized_.store(false, std::memory_order_release);
}

bool CoreMidiIn::isInitialized() const noexcept {
    return initialized_.load(std::memory_order_acquire);
}

int CoreMidiIn::sourceCount() const noexcept {
    return static_cast<int>(MIDIGetNumberOfSources());
}

std::vector<MidiSourceInfo> CoreMidiIn::enumerateSources() const noexcept {
    std::vector<MidiSourceInfo> out;
    ItemCount n = MIDIGetNumberOfSources();
    out.reserve(n);
    for (ItemCount i = 0; i < n; ++i) {
        MIDIEndpointRef ep = MIDIGetSource(i);
        out.push_back({static_cast<int>(i), sourceDisplayName(ep)});
    }
    return out;
}

bool CoreMidiIn::selectSource(int index) noexcept {
    if (!initialized_.load(std::memory_order_acquire)) return false;

    // Disconnect the previous source first.
    MIDIEndpointRef prev = source_.exchange(0, std::memory_order_acq_rel);
    if (prev != 0) MIDIPortDisconnectSource(in_port_, prev);

    if (index < 0) {   // explicit "none"
        selected_index_.store(-1, std::memory_order_release);
        selected_uid_.store(0, std::memory_order_release);
        selected_name_.clear();
        return true;
    }
    // A failed selection below leaves nothing selected (no stale reconnect target).
    selected_index_.store(-1, std::memory_order_release);
    selected_uid_.store(0, std::memory_order_release);
    if (index >= static_cast<int>(MIDIGetNumberOfSources())) return false;

    MIDIEndpointRef ep = MIDIGetSource(static_cast<ItemCount>(index));
    if (ep == 0) return false;
    if (MIDIPortConnectSource(in_port_, ep, nullptr) != noErr) return false;

    source_.store(ep, std::memory_order_release);
    selected_index_.store(index, std::memory_order_release);
    selected_uid_.store(sourceUniqueId(ep), std::memory_order_release);
    selected_name_ = sourceDisplayName(ep);
    return true;
}

bool CoreMidiIn::pollReconnect() noexcept {
    if (!initialized_.load(std::memory_order_acquire)) return false;
    const int32_t want = selected_uid_.load(std::memory_order_acquire);
    if (want == 0) return false;   // nothing selected: never auto-connect

    MIDIEndpointRef found = 0;
    int foundIndex = -1;
    const ItemCount n = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < n; ++i) {
        MIDIEndpointRef ep = MIDIGetSource(i);
        if (sourceUniqueId(ep) == want) { found = ep; foundIndex = static_cast<int>(i); break; }
    }
    // An offline endpoint (device unplugged but object retained) is treated as gone.
    if (found != 0) {
        SInt32 offline = 0;
        if (MIDIObjectGetIntegerProperty(found, kMIDIPropertyOffline, &offline) == noErr && offline)
            found = 0;
    }

    const MIDIEndpointRef cur = source_.load(std::memory_order_acquire);
    if (found == cur) {
        if (found != 0) selected_index_.store(foundIndex, std::memory_order_release);
        return found != 0;
    }
    if (cur != 0) MIDIPortDisconnectSource(in_port_, cur);
    source_.store(0, std::memory_order_release);
    if (found == 0) return false;   // gone: keep the uid, wait for it to return
    if (MIDIPortConnectSource(in_port_, found, nullptr) != noErr) return false;
    source_.store(found, std::memory_order_release);
    selected_index_.store(foundIndex, std::memory_order_release);
    reconnects_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

int CoreMidiIn::selectedSource() const noexcept {
    return selected_index_.load(std::memory_order_acquire);
}

bool CoreMidiIn::hasLiveSource() const noexcept {
    return source_.load(std::memory_order_acquire) != 0;
}

// ── Read callback (CoreMIDI read thread) ─────────────────────────────
void CoreMidiIn::readProc(const MIDIPacketList* pktList, void* readRefCon, void* /*srcRefCon*/) {
    auto* self = static_cast<CoreMidiIn*>(readRefCon);
    if (!self || !pktList) return;

    // Enter the guarded region BEFORE touching sink_. quiesceReadCallback() clears
    // accepting_ then waits for in_flight_ to hit 0, so once it returns the sink is
    // provably idle and safe to destroy. Ordering: increment → re-check accepting_
    // → use sink_ → decrement (the decrement always runs, even when not accepting).
    self->in_flight_.fetch_add(1, std::memory_order_acquire);
    // StoreLoad barrier: this increment + the accepting_ load below, paired against
    // quiesceReadCallback()'s accepting_ store + in_flight_ load, form a store-buffer
    // (Dekker) pattern. Acquire/release alone permits BOTH sides to read stale — the
    // reader seeing accepting_==true while the writer sees in_flight_==0 — which would
    // let quiesce return and the caller destroy sink_ underneath this callback (UAF on
    // the Apple Silicon / ARM64 target; x86 masks it via the lock-prefixed RMW). The
    // matching seq_cst fences here and in quiesceReadCallback() forbid that outcome.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    // The sink is user-supplied. A throw from it must neither skip the in_flight_
    // decrement below (that would wedge quiesceReadCallback()'s spin forever) nor
    // unwind across this C callback back into CoreMIDI (undefined behaviour). Swallow
    // it so the decrement always runs and the read thread stays live.
    try {
        if (self->accepting_.load(std::memory_order_acquire) && self->sink_) {
            const MIDIPacket* pkt = &pktList->packet[0];
            for (UInt32 i = 0; i < pktList->numPackets; ++i) {
                MidiInputMessage msgs[64];
                const size_t n = parseMidiInput(pkt->data, pkt->length, msgs, 64);
                for (size_t j = 0; j < n; ++j) {
                    // Trace point (1): CoreMIDI input received a NoteOn. Compiled
                    // out entirely when AIARR_LATENCY_TRACE is off; when on it is a
                    // wait-free ring push, adding no work to the sink path itself.
                    if (msgs[j].type == MidiInMsgType::NoteOn) {
                        AIARR_TRACE_INPUT_NOTEON(msgs[j].channel, msgs[j].data1,
                                                 msgs[j].data2);
                    }
                    self->sink_(msgs[j]);
                }
                self->received_.fetch_add(n, std::memory_order_relaxed);
                pkt = MIDIPacketNext(pkt);
            }
        }
    } catch (...) {
        // A MIDI read callback must never propagate an exception.
    }
    self->in_flight_.fetch_sub(1, std::memory_order_release);
}

void CoreMidiIn::quiesceReadCallback() noexcept {
    // Stop new deliveries, then wait for any readProc already past the accepting_
    // check to exit. Bounded and brief (readProc does no malloc/lock). Runs only on
    // non-realtime threads (setSink / shutdown), never on the read thread itself,
    // so this never deadlocks against its own callback.
    accepting_.store(false, std::memory_order_release);
    // Matching half of the store-buffer barrier in readProc: force this accepting_
    // store to be globally ordered before the in_flight_ load, so we can never read
    // in_flight_==0 while a readProc that already saw accepting_==true is still in the
    // sink. Without it, acquire/release permits that stale-stale outcome on ARM64.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    while (in_flight_.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
}

void CoreMidiIn::notifyProc(const MIDINotification* /*msg*/, void* /*refCon*/) {
    // Notifications only arrive when the creating thread runs a CFRunLoop, so
    // hot-plug is handled by polling instead: the host calls pollReconnect().
}

} // namespace ai_arranger::midi

#pragma clang diagnostic pop
