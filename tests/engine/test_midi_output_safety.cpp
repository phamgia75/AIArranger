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
        waitFor([&] { return out.droppedCount() >= 8; });   // counted after dispatch
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
        TEST("explicit select", out.selectDestinationByName(uniq) && out.hasLiveDestination());
        out.send(ev(MidiEventType::NoteOn, 60, 100));
        out.send(ev(MidiEventType::NoteOff, 60, 0));
        waitFor([&] { return out.blockedCount() >= 2; });   // counted after dispatch
        TEST("guard blocks events at the MIDISend boundary", out.sentCount() == 0 && out.blockedCount() == 2);
        out.selectDestination(-1);   // would send CC123/CC120 x16 to the old endpoint
        TEST("guard blocks deselect cleanup", out.blockedCount() == 2 + 32);

        // Selection by exact name: unique -> that endpoint; missing or ambiguous ->
        // controlled failure, selection unchanged (never a fallback device).
        const std::string dupName = "AIARR SAFETY DUP " + std::to_string(getpid());
        CFStringRef dupCf = CFStringCreateWithCString(nullptr, dupName.c_str(), kCFStringEncodingUTF8);
        Sink dup;
        MIDIEndpointRef dupA = 0, dupB = 0;
        MIDIDestinationCreate(client, dupCf, readProc, &dup, &dupA);
        MIDIDestinationCreate(client, dupCf, readProc, &dup, &dupB);
        CFRelease(dupCf);
        indexOf(dupA); indexOf(dupB);
        TEST("by name: missing name rejected, nothing selected",
             !out.selectDestinationByName("AIARR NO SUCH DEVICE") && out.selectedDestination() == -1);
        TEST("by name: ambiguous name rejected, nothing selected",
             !out.selectDestinationByName(dupName) && out.selectedDestination() == -1);
        TEST("by name: unique name selects that endpoint",
             out.selectDestinationByName(uniq) && out.hasLiveDestination() &&
             MIDIGetDestination(out.selectedDestination()) == sinkEp);
        TEST("by name: failed lookup keeps the current selection",
             !out.selectDestinationByName(dupName) && MIDIGetDestination(out.selectedDestination()) == sinkEp);
        out.selectDestination(-1);
        if (dupA) MIDIEndpointDispose(dupA);
        if (dupB) MIDIEndpointDispose(dupB);

        // Hot-plug cannot hand the output to a different device with the same
        // name: select A, a same-named B appears, A disappears -> no destination.
        const std::string hopName = "AIARR SAFETY HOP " + std::to_string(getpid());
        CFStringRef hopCf = CFStringCreateWithCString(nullptr, hopName.c_str(), kCFStringEncodingUTF8);
        Sink hop;
        MIDIEndpointRef hopA = 0, hopB = 0;
        MIDIDestinationCreate(client, hopCf, readProc, &hop, &hopA);
        indexOf(hopA);
        TEST("hot-plug: select A by name", out.selectDestinationByName(hopName) && out.hasLiveDestination());
        MIDIDestinationCreate(client, hopCf, readProc, &hop, &hopB);
        CFRelease(hopCf);
        indexOf(hopB);
        MIDIEndpointDispose(hopA);
        // CoreMIDI delivers setup notifications on the run loop of the thread that
        // created the client (this one): pump it, as the app's event loop would,
        // so the output re-resolves after A disappears.
        for (int w = 0; w < 10; ++w) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));   // > 2 probe periods
        for (int w = 0; w < 4; ++w) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
        TEST("hot-plug: A gone -> disconnected, same-named B never adopted", !out.hasLiveDestination());
        out.selectDestination(-1);
        if (hopB) MIDIEndpointDispose(hopB);
        out.shutdown();
        settle();
        TEST("selected sink received nothing while guarded", sink.count() == 0);
    }

    // 3. Shutdown cleanup is deterministic (attempted for the selected device,
    //    blocked here by the guard), and shutdown forgets the selection: a re-
    //    initialized object has no destination and the hot-plug probe (~100 ms)
    //    does not reconnect the old one. No automated test ever transmits: a
    //    by-index selection can race with a device being plugged in.
    {
        CoreMidiOut out;
        out.initialize("AIARR safety 3");
        out.selectDestinationByName(uniq);
        out.send(ev(MidiEventType::NoteOn, 60, 100));   // deliberately no Note Off
        waitFor([&] { return out.blockedCount() >= 1; });
        out.shutdown();
        TEST("shutdown cleanup attempted for the selected device (32 CCs, guarded)",
             out.blockedCount() == 1 + 32);
        TEST("re-initialize succeeds", out.initialize("AIARR safety 3b"));
        TEST("re-initialized: no destination selected", out.selectedDestination() == -1);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));   // > 2 probe periods
        TEST("re-initialized: old destination not reconnected by the probe",
             !out.hasLiveDestination() && out.selectedDestination() == -1);
        out.shutdown();
        settle();
        TEST("sink and decoy received nothing", sink.count() == 0 && decoy.count() == 0);
    }

    MIDIEndpointDispose(decoyEp);
    MIDIEndpointDispose(sinkEp);
    MIDIClientDispose(client);
    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}

#pragma clang diagnostic pop
