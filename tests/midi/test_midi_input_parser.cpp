// MIDI input parser + router unit tests (headless, no CoreMIDI).
// Verifies raw-byte decoding (running status, vel-0 = NoteOff, CC handling,
// realtime/sysex skipping, incomplete trailing) and the vendor-neutral routing
// of chord-relevant messages to ControlEvents.

#include "midi/midi_input_parser.h"
#include "midi/midi_input_router.h"
#include "engine/control/control_events.h"
#include <cstdio>
#include <vector>

using namespace ai_arranger::midi;
using ai_arranger::control::ControlAction;
using ai_arranger::control::ControlEvent;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

// Capturing sink implementing the `postEvent` contract the router expects.
struct CaptureSink {
    std::vector<ControlEvent> events;
    bool postEvent(const ControlEvent& e) { events.push_back(e); return true; }
};

int main() {
    std::printf("Test: MIDI input parser + router\n");

    // ── Parser: basic NoteOn / NoteOff ──
    {
        const uint8_t b[] = {0x90, 60, 100,  0x80, 60, 0};
        MidiInputMessage m[8];
        size_t n = parseMidiInput(b, sizeof(b), m, 8);
        TEST("two messages parsed", n == 2);
        TEST("noteon type",  m[0].type == MidiInMsgType::NoteOn && m[0].data1 == 60 && m[0].data2 == 100);
        TEST("noteoff type", m[1].type == MidiInMsgType::NoteOff && m[1].data1 == 60);
        TEST("channel decoded", m[0].channel == 0);
    }

    // ── NoteOn velocity 0 normalizes to NoteOff ──
    {
        const uint8_t b[] = {0x90, 64, 0};
        MidiInputMessage m[4];
        size_t n = parseMidiInput(b, sizeof(b), m, 4);
        TEST("vel0 -> one msg", n == 1);
        TEST("vel0 -> NoteOff", m[0].type == MidiInMsgType::NoteOff && m[0].data1 == 64);
    }

    // ── Running status: 0x90 then note pairs with no repeated status ──
    {
        const uint8_t b[] = {0x90, 60, 100,  62, 100,  64, 100};
        MidiInputMessage m[8];
        size_t n = parseMidiInput(b, sizeof(b), m, 8);
        TEST("running status: 3 notes", n == 3);
        TEST("running status notes", m[0].data1 == 60 && m[1].data1 == 62 && m[2].data1 == 64);
        TEST("running status all NoteOn", m[2].type == MidiInMsgType::NoteOn);
    }

    // ── Channel nibble + CC decoding ──
    {
        const uint8_t b[] = {0xB3, 64, 127,   0xB3, 123, 0};
        MidiInputMessage m[8];
        size_t n = parseMidiInput(b, sizeof(b), m, 8);
        TEST("two CC parsed", n == 2);
        TEST("cc channel 3", m[0].channel == 3);
        TEST("cc64 sustain",  m[0].type == MidiInMsgType::ControlChange && m[0].data1 == 64 && m[0].data2 == 127);
        TEST("cc123 allnotesoff", m[1].data1 == 123);
    }

    // ── Realtime byte (0xF8 clock) interleaved is ignored, running status kept ──
    {
        const uint8_t b[] = {0x90, 60, 100,  0xF8,  62, 100};
        MidiInputMessage m[8];
        size_t n = parseMidiInput(b, sizeof(b), m, 8);
        TEST("realtime ignored, 2 notes", n == 2 && m[1].data1 == 62 && m[1].type == MidiInMsgType::NoteOn);
    }

    // ── SysEx block is skipped entirely ──
    {
        const uint8_t b[] = {0xF0, 0x7E, 0x00, 0xF7,  0x90, 60, 100};
        MidiInputMessage m[8];
        size_t n = parseMidiInput(b, sizeof(b), m, 8);
        TEST("sysex skipped, note after parsed", n == 1 && m[0].data1 == 60);
    }

    // ── Incomplete trailing message is dropped, not misread ──
    {
        const uint8_t b[] = {0x90, 60, 100,  0x90, 62};   // last note missing velocity
        MidiInputMessage m[8];
        size_t n = parseMidiInput(b, sizeof(b), m, 8);
        TEST("incomplete trailing dropped", n == 1 && m[0].data1 == 60);
    }

    // ── maxOut respected ──
    {
        const uint8_t b[] = {0x90, 60, 100,  62, 100,  64, 100};
        MidiInputMessage m[2];
        size_t n = parseMidiInput(b, sizeof(b), m, 2);
        TEST("maxOut caps output", n == 2);
    }

    // ── Router: NoteOn/NoteOff -> ControlEvents ──
    {
        CaptureSink sink;
        routeMidiInput({MidiInMsgType::NoteOn, 0, 48, 100}, sink);
        routeMidiInput({MidiInMsgType::NoteOff, 0, 48, 0}, sink);
        TEST("router 2 events", sink.events.size() == 2);
        TEST("router NoteOn action", sink.events[0].action == ControlAction::NoteOn && sink.events[0].param == 48);
        TEST("router NoteOff action", sink.events[1].action == ControlAction::NoteOff && sink.events[1].param == 48);
    }

    // ── Router: CC64 -> Sustain down/up; CC123 -> Panic; other CC ignored ──
    {
        CaptureSink sink;
        routeMidiInput({MidiInMsgType::ControlChange, 0, kCcSustain, 127}, sink);
        routeMidiInput({MidiInMsgType::ControlChange, 0, kCcSustain, 0}, sink);
        routeMidiInput({MidiInMsgType::ControlChange, 0, kCcAllNotesOff, 0}, sink);
        routeMidiInput({MidiInMsgType::ControlChange, 0, 7 /*volume*/, 100}, sink);
        TEST("router 3 events (volume ignored)", sink.events.size() == 3);
        TEST("sustain down param 1", sink.events[0].action == ControlAction::Sustain && sink.events[0].param == 1);
        TEST("sustain up param 0",   sink.events[1].action == ControlAction::Sustain && sink.events[1].param == 0);
        TEST("cc123 -> panic",       sink.events[2].action == ControlAction::Panic);
    }

    // ── routeMidiBytes: full byte buffer -> routed events ──
    {
        CaptureSink sink;
        const uint8_t b[] = {0x90, 48, 100,  0xB0, 64, 127};   // NoteOn + sustain down
        size_t n = routeMidiBytes(b, sizeof(b), sink);
        TEST("routeMidiBytes count", n == 2);
        TEST("routeMidiBytes NoteOn then Sustain",
             sink.events.size() == 2 &&
             sink.events[0].action == ControlAction::NoteOn &&
             sink.events[1].action == ControlAction::Sustain);
    }

    // ── More than 64 messages in one packet (CoreMidiIn::readProc used a fixed msgs[64]) ──
    {
        std::vector<uint8_t> big;   // 70 Note Offs on ch 4, explicit status (as the Pa5X sends)
        for (int k = 0; k < 70; ++k) { big.push_back(0x83); big.push_back(static_cast<uint8_t>(30 + k)); big.push_back(0x40); }
        std::vector<MidiInputMessage> got;
        const size_t n = forEachMidiInput(big.data(), big.size(), [&](const MidiInputMessage& m) { got.push_back(m); });
        bool all = n == 70 && got.size() == 70;
        for (size_t k = 0; all && k < got.size(); ++k)
            all = got[k].type == MidiInMsgType::NoteOff && got[k].channel == 3 && got[k].data1 == 30 + k;
        TEST("forEachMidiInput: 70 messages in one packet, none lost, in order", all);
        MidiInputMessage cap[64];
        TEST("parseMidiInput keeps its maxOut contract (64 of 70)", parseMidiInput(big.data(), big.size(), cap, 64) == 64);
        CaptureSink sink;
        TEST("routeMidiBytes routes all 70", routeMidiBytes(big.data(), big.size(), sink) == 70 && sink.events.size() == 70);
        // The same parse as before for running status, realtime inside a message, and a partial tail.
        const uint8_t rs[] = {0x93, 36, 50, 40, 51, 0xFE, 43, 52, 0x83, 36};
        std::vector<MidiInputMessage> r;
        forEachMidiInput(rs, sizeof rs, [&](const MidiInputMessage& m) { r.push_back(m); });
        TEST("forEachMidiInput: running status + FE kept, partial tail dropped",
             r.size() == 3 && r[0].data1 == 36 && r[1].data1 == 40 && r[2].data1 == 43 && r[2].type == MidiInMsgType::NoteOn);
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}
