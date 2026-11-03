// Stop / panic send explicit NoteOffs (Issue #14).
//
// EngineSession::shutdown() ran player.stop() (which schedules a NoteOff for
// every sounding note) and then player.panic(), which cleared the scheduler —
// discarding those NoteOffs — and sent only CC 120 / CC 123 on all channels.
// Compliant receivers go silent on the CCs, but one that tracks notes by
// NoteOff only (some soft synths, MIDI recorders) kept them on. The panic now
// sends a NoteOff for every tracked note before the CCs.

#include "engine/midi/panic.h"
#include "engine/midi/scheduler.h"
#include "session/live_engine_facade.h"
#include "engine/arranger/style_player.h"
#include "../midi/fake_midi_output_provider.h"
#include <cstdio>
#include <set>
#include <vector>

using namespace ai_arranger;
using uasf::MidiEventType;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

// Notes left on when counting NoteOffs only (CCs ignored on purpose).
static std::set<int> heldByNoteOffs(const std::vector<uasf::MidiEvent>& sent) {
    std::set<int> on;
    for (const auto& e : sent) {
        const int k = (e.channel << 8) | e.data1;
        if (e.type == MidiEventType::NoteOn && e.data2 > 0) on.insert(k);
        else if (e.type == MidiEventType::NoteOff ||
                 (e.type == MidiEventType::NoteOn && e.data2 == 0)) on.erase(k);
    }
    return on;
}

int main() {
    std::printf("Test: stop / panic send explicit NoteOffs\n");

    {   // PanicHandler::panic: NoteOff per tracked note, then the CCs
        midi::PanicHandler ph;
        midi::MidiScheduler sch;
        ph.noteOn(0, 60); ph.noteOn(0, 64); ph.noteOn(9, 36);
        std::vector<uasf::MidiEvent> got;
        ph.panic(sch, [&](const uasf::MidiEvent& e) { got.push_back(e); });
        size_t offs = 0, firstCc = got.size(), lastOff = 0;
        for (size_t i = 0; i < got.size(); ++i) {
            if (got[i].type == MidiEventType::NoteOff) { ++offs; lastOff = i; }
            if (got[i].type == MidiEventType::ControlChange && firstCc == got.size()) firstCc = i;
        }
        TEST("panic: one NoteOff per tracked note (3)", offs == 3);
        TEST("panic: NoteOffs come before the CCs", offs > 0 && lastOff < firstCc);
        TEST("panic: still 16 x (CC120 + CC123), CC123 last",
             got.size() == 3 + 32 && got.back().type == MidiEventType::ControlChange &&
             got.back().data1 == 123);
        TEST("panic: tracking cleared", !ph.hasActiveNotes());
    }
    {   // LiveEngineFacade::stop(): every sounding note gets a NoteOff
        midi::FakeMidiOutputProvider out; out.setDevices({{0, "synth"}});
        session::LiveEngineFacade f(nullptr, &out);
        f.selectMidiOutput(0);
        f.loadStyle(arranger::buildDemoStyle());
        f.start(); f.transportStart();
        for (int i = 0; i < 300; ++i) f.tick(48);
        TEST("facade: notes sounding before stop() (precondition)", !heldByNoteOffs(out.sent).empty());
        f.stop();    // no tick afterwards: stop() itself must deliver the silence
        TEST("facade stop(): every sounding note got an explicit NoteOff", heldByNoteOffs(out.sent).empty());
    }
    {   // Panic command (facade.panic()): same guarantee
        midi::FakeMidiOutputProvider out; out.setDevices({{0, "synth"}});
        session::LiveEngineFacade f(nullptr, &out);
        f.selectMidiOutput(0);
        f.loadStyle(arranger::buildDemoStyle());
        f.start(); f.transportStart();
        for (int i = 0; i < 300; ++i) f.tick(48);
        f.panic();
        f.tick(0);
        TEST("facade panic(): every sounding note got an explicit NoteOff", heldByNoteOffs(out.sent).empty());
        f.stop();
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
