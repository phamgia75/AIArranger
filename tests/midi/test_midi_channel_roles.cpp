// Chord-recognition MIDI channel roles (Pa5X-style Global / Chord / Ignore).
//
// Default (no roles set) must be the old behaviour: every channel is Global and
// goes through the split. A Chord channel feeds chord detection whatever the
// split; an Ignore channel routes nothing. The Pa5X "Default" preset roles take
// chords from the Lower part (ch4) only. Headless: fake input + output.

#include "session/live_engine_facade.h"
#include "midi/pa5x_profile.h"
#include "engine/chord/fingered_detector.h"
#include "fake_midi_input_source.h"
#include "fake_midi_output_provider.h"
#include <cstdio>
#include <vector>

using namespace ai_arranger;
using session::LiveEngineFacade;
using arranger::ChordType;
using midi::ChannelRole;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

struct Rec {
    std::vector<control::ControlEvent> ev;
    bool postEvent(const control::ControlEvent& e) { ev.push_back(e); return true; }
};

// Three-note chord on `ch` (0-based), then release.
static void chordOn(midi::FakeMidiInputSource& in, uint8_t ch, uint8_t a, uint8_t b, uint8_t c) {
    const uint8_t s = static_cast<uint8_t>(0x90 | ch);
    const uint8_t on[] = {s, a, 100, s, b, 100, s, c, 100};
    in.inject(on, sizeof(on));
}
static void chordOff(midi::FakeMidiInputSource& in, uint8_t ch, uint8_t a, uint8_t b, uint8_t c) {
    const uint8_t s = static_cast<uint8_t>(0x80 | ch);
    const uint8_t off[] = {s, a, 0, s, b, 0, s, c, 0};
    in.inject(off, sizeof(off));
}

int main() {
    std::printf("Test: MIDI input channel roles for chord recognition\n");

    // ── Router ──
    {
        Rec r;
        midi::routeMidiInput({midi::MidiInMsgType::NoteOn, 5, 72, 100}, r);
        TEST("router default: Global, no bypass flag",
             r.ev.size() == 1 && r.ev[0].param == 72);
        midi::ChannelRoles roles;
        roles.role[3] = ChannelRole::Chord;
        roles.role[0] = ChannelRole::Ignore;
        r.ev.clear();
        midi::routeMidiInput({midi::MidiInMsgType::NoteOn, 3, 72, 100}, r, roles);
        midi::routeMidiInput({midi::MidiInMsgType::NoteOff, 3, 72, 0}, r, roles);
        TEST("router Chord channel: NoteOn carries the bypass flag, NoteOff does not",
             r.ev.size() == 2 && r.ev[0].param == (72 | control::kNoteBypassSplit) && r.ev[1].param == 72);
        r.ev.clear();
        midi::routeMidiInput({midi::MidiInMsgType::NoteOn, 0, 48, 100}, r, roles);
        midi::routeMidiInput({midi::MidiInMsgType::ControlChange, 0, midi::kCcSustain, 127}, r, roles);
        midi::routeMidiInput({midi::MidiInMsgType::ControlChange, 0, midi::kCcAllNotesOff, 0}, r, roles);
        TEST("router Ignore channel: notes, sustain and panic dropped", r.ev.empty());
    }

    midi::FakeMidiInputSource  fin;  fin.devices = {{0, "Pa5X KEYBOARD"}};
    midi::FakeMidiOutputProvider fout; fout.setDevices({{0, "Pa5X SOUND"}});
    LiveEngineFacade fac(&fin, &fout);
    chord::FingeredDetector det;
    fac.setChordScanMode(&det);
    fac.start();
    fac.selectMidiInput(0);
    auto chord = [&]() { fac.tick(48); auto s = fac.snapshot(); return std::pair<int, int>{s.chordType, s.chordRoot}; };
    const int kMaj = static_cast<int>(ChordType::Major), kMin = static_cast<int>(ChordType::Minor);

    // ── Default roles: unchanged behaviour ──
    {
        midi::ChannelRoles def = fac.inputChannelRoles();
        bool allGlobal = true;
        for (auto r : def.role) allGlobal &= r == ChannelRole::Global;
        TEST("facade default roles: all Global", allGlobal);
        chordOn(fin, 0, 72, 76, 79);   // C5 E5 G5, above the C4 split
        auto c = chord();
        TEST("default: chord above the split is ignored (old behaviour)", c.first != kMaj || c.second != 72);
        chordOff(fin, 0, 72, 76, 79);
        chordOn(fin, 0, 48, 52, 55);   // C3 E3 G3
        c = chord();
        TEST("default: lower-zone chord on any channel detected", c.first == kMaj && c.second == 48);
        chordOff(fin, 0, 48, 52, 55); chord();
    }

    // ── Pa5X Default preset: ch4 (Lower) = Chord, ch1-3 Upper ignored ──
    {
        fac.setInputChannelRoles(midi::pa5x::defaultPresetKeyboardRoles());
        TEST("preset: ch4 Chord, ch1 Ignore",
             fac.inputChannelRoles().role[3] == ChannelRole::Chord && fac.inputChannelRoles().role[0] == ChannelRole::Ignore);
        chordOn(fin, 3, 69, 72, 76);   // A4 C5 E5 on Lower, above our own split
        auto c = chord();
        TEST("preset: Lower-channel chord above the split detected (Am)", c.first == kMin && c.second == 69);
        chordOn(fin, 0, 48, 52, 55);   // right hand on Upper 1 below our split
        c = chord();
        TEST("preset: Upper-channel notes never change the chord", c.first == kMin && c.second == 69);
        chordOff(fin, 0, 48, 52, 55);
        chordOff(fin, 3, 69, 72, 76); chord();
    }

    // ── Back to defaults restores the old behaviour exactly ──
    {
        fac.setInputChannelRoles({});
        chordOn(fin, 3, 74, 77, 81);   // D5 F5 A5 on ch4, above the split
        auto c = chord();
        // The latched chord is still Am; a Dm here would mean the split was bypassed.
        TEST("roles reset: above-split chord ignored again (no Dm)", !(c.first == kMin && c.second == 74));
        chordOff(fin, 3, 74, 77, 81); chord();
    }
    fac.stop();

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
