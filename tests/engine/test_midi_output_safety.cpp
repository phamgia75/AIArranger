// Fail-closed MIDI output policy (hardware safety regression).
//
// A regression run once sent notes to an attached Korg Pa5X because
// CoreMidiOut::initialize() auto-selected the first destination. This test proves
// the fixed policy against isolated endpoints only: it creates its OWN virtual
// CoreMIDI destinations (one decoy named exactly like the Pa5X, one with a unique
// name), and never selects any endpoint it did not create. Endpoints are picked by
// MIDIEndpointRef identity, never by name, so a real device with the same name is
// never touched.

#include "engine/midi/coremidi_out.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

using namespace ai_arranger::midi;
using ai_arranger::uasf::MidiEvent;
using ai_arranger::uasf::MidiEventType;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

namespace {

struct Sink {                       // what a virtual destination received
    std::mutex m;
    std::vector<uint8_t> status;    // status byte of every message
    size_t count() { std::lock_guard<std::mutex> l(m); return status.size(); }
};

void readProc(const MIDIPacketList* pl, void* refCon, void*) {
    auto* s = static_cast<Sink*>(refCon);
    std::lock_guard<std::mutex> l(s->m);
    const MIDIPacket* p = &pl->packet[0];
    for (UInt32 i = 0; i < pl->numPackets; ++i, p = MIDIPacketNext(p))
        for (UInt16 b = 0; b < p->length; ++b)
            if (p->data[b] & 0x80) s->status.push_back(p->data[b]);
}

int indexOf(MIDIEndpointRef ep) {   // identity, not name
    for (int w = 0; w < 200; ++w) {
        for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); ++i)
            if (MIDIGetDestination(i) == ep) return static_cast<int>(i);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return -1;
}

MidiEvent ev(MidiEventType t, uint8_t d1, uint8_t d2) { return {t, 0, d1, d2, 0}; }

template <class F> void waitFor(F done) {
    for (int i = 0; i < 500 && !done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
}
void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }

} // namespace

int main() {
    std::printf("Test: fail-closed MIDI output policy\n");
    setenv("AIARR_MIDI_HW_TX", "0", 1);   // deterministic outside CTest too

    MIDIClientRef client = 0;
    if (MIDIClientCreate(CFSTR("AIARR safety test"), nullptr, nullptr, &client) != noErr) {
        std::printf("  SKIP: CoreMIDI client unavailable (#21)\n");
        return 0;
    }
    Sink decoy, sink;
    const std::string uniq = "AIARR SAFETY SINK " + std::to_string(getpid());
    CFStringRef uniqCf = CFStringCreateWithCString(nullptr, uniq.c_str(), kCFStringEncodingUTF8);
    MIDIEndpointRef decoyEp = 0, sinkEp = 0;
    const bool made = MIDIDestinationCreate(client, CFSTR("Pa5X SOUND"), readProc, &decoy, &decoyEp) == noErr &&
                      MIDIDestinationCreate(client, uniqCf, readProc, &sink, &sinkEp) == noErr;
    CFRelease(uniqCf);
    const int sinkIdx = made ? indexOf(sinkEp) : -1;
    if (!made || indexOf(decoyEp) < 0 || sinkIdx < 0) {
        std::printf("  SKIP: cannot create virtual destinations\n");
        MIDIClientDispose(client);
        return 0;
    }

    // 1. A device named like the Pa5X is present: nothing is selected or sent.
    {
        CoreMidiOut out;
        TEST("initialize succeeds with devices present", out.initialize("AIARR safety 1"));
        TEST("initialize selects no destination", out.selectedDestination() == -1);
        TEST("no live destination after initialize", !out.hasLiveDestination());
        for (int i = 0; i < 8; ++i) out.send(ev(MidiEventType::NoteOn, 60, 100));
        waitFor([&] { return out.dispatchedCount() >= 8; });
        TEST("no destination: every event dropped, none sent",
             out.droppedCount() >= 8 && out.sentCount() == 0);
        out.shutdown();
        settle();
        TEST("decoy 'Pa5X SOUND' received nothing (events or cleanup)", decoy.count() == 0);
        TEST("unselected sink received nothing", sink.count() == 0);
    }

    // 2. Explicit selection is required, and the guard blocks it under test.
    {
        CoreMidiOut out;
        out.initialize("AIARR safety 2");
        TEST("guard active (AIARR_MIDI_HW_TX=0)", !out.hardwareTransmitAllowed());
        TEST("out-of-range select rejected, selection unchanged",
             !out.selectDestination(9999) && out.selectedDestination() == -1);
        TEST("explicit select", out.selectDestination(sinkIdx) && out.hasLiveDestination());
        out.send(ev(MidiEventType::NoteOn, 60, 100));
        out.send(ev(MidiEventType::NoteOff, 60, 0));
        waitFor([&] { return out.dispatchedCount() >= 2; });
        TEST("guard blocks events at the MIDISend boundary", out.sentCount() == 0 && out.blockedCount() == 2);
        out.selectDestination(-1);   // would send CC123/CC120 x16 to the old endpoint
        TEST("guard blocks deselect cleanup", out.blockedCount() == 2 + 32);
        out.shutdown();
        settle();
        TEST("selected sink received nothing while guarded", sink.count() == 0);
    }

    // 3. Authorized transmission (own isolated endpoint only): Note Off and
    //    shutdown cleanup are delivered in order.
    {
        setenv("AIARR_MIDI_HW_TX", "1", 1);
        CoreMidiOut out;
        out.initialize("AIARR safety 3");
        setenv("AIARR_MIDI_HW_TX", "0", 1);
        TEST("authorized when AIARR_MIDI_HW_TX=1", out.hardwareTransmitAllowed());
        out.selectDestination(sinkIdx);
        out.send(ev(MidiEventType::NoteOn, 60, 100));
        out.send(ev(MidiEventType::NoteOff, 60, 0));
        waitFor([&] { return sink.count() >= 2; });
        out.shutdown();
        waitFor([&] { return sink.count() >= 34; });
        bool ordered = false, cleanup = false;
        {
            std::lock_guard<std::mutex> l(sink.m);
            const auto& s = sink.status;
            ordered = s.size() >= 2 && s[0] == 0x90 && s[1] == 0x80;
            cleanup = s.size() == 34;
            for (size_t i = 2; cleanup && i < s.size(); ++i) cleanup = s[i] == (0xB0 | ((i - 2) / 2));
        }
        TEST("authorized: Note On then explicit Note Off delivered", ordered);
        TEST("shutdown cleanup: CC123+CC120 on all 16 channels", cleanup);
        TEST("decoy still received nothing", decoy.count() == 0);
    }

    MIDIEndpointDispose(decoyEp);
    MIDIEndpointDispose(sinkEp);
    MIDIClientDispose(client);
    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}

#pragma clang diagnostic pop
