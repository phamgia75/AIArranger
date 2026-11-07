// Live output channel routing (OutputChannelMap) and the Pa5X style-track preset.
//
// Default must be identity (byte-identical output). The Genos preset moves Genos
// style parts onto Pa5X Style-track channels 9-16, drops ch1-8 and Yamaha Bank
// Select / Program Change. The facade refuses a map change while playing.

#include "session/live_engine_facade.h"
#include "midi/pa5x_profile.h"
#include "engine/chord/fingered_detector.h"
#include "fake_midi_input_source.h"
#include "fake_midi_output_provider.h"
#include <cstdio>
#include <set>

using namespace ai_arranger;
using control::MidiOutputBridge;
using control::OutputChannelMap;
using uasf::MidiEvent;
using uasf::MidiEventType;
using session::LiveEngineFacade;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static MidiEvent ev(MidiEventType t, uint8_t ch, uint8_t d1, uint8_t d2 = 100) {
    return MidiEvent{t, ch, d1, d2, 0};
}

int main() {
    std::printf("Test: output channel map\n");

    // ── Bridge: identity by default ──
    {
        MidiOutputBridge b;
        midi::FakeMidiOutputProvider out; out.setDevices({{0, "X"}}); out.select(0);
        bool same = true;
        for (uint8_t ch = 0; ch < 16; ++ch) {
            b.send(ev(MidiEventType::NoteOn, ch, 60));
            b.send(ev(MidiEventType::ProgramChange, ch, 5, 0));
        }
        b.pump(out);
        same = out.sent.size() == 32;
        for (size_t i = 0; same && i < out.sent.size(); ++i) same = out.sent[i].channel == i / 2;
        TEST("default map: every channel unchanged, voice select kept", same && b.routedOut() == 0);
    }

    // ── Bridge: Genos preset ──
    {
        MidiOutputBridge b;
        b.setChannelMap(midi::pa5x::genosStyleTrackMap());
        midi::FakeMidiOutputProvider out; out.setDevices({{0, "Pa5X SOUND"}}); out.select(0);
        b.send(ev(MidiEventType::NoteOn, 8, 42));    // Genos Rhythm1 (ch9)
        b.send(ev(MidiEventType::NoteOn, 9, 36));    // Genos Rhythm2 (ch10)
        b.send(ev(MidiEventType::NoteOn, 10, 36));   // Genos Bass (ch11)
        b.send(ev(MidiEventType::NoteOn, 13, 60));   // Genos Pad (ch14)
        b.send(ev(MidiEventType::NoteOn, 2, 60));    // ch3: not a style part
        b.send(ev(MidiEventType::ControlChange, 10, 0, 0));    // Yamaha bank MSB
        b.send(ev(MidiEventType::ControlChange, 10, 32, 115)); // Yamaha bank LSB
        b.send(ev(MidiEventType::ProgramChange, 10, 33, 0));
        b.send(ev(MidiEventType::ControlChange, 10, 7, 100));  // volume kept
        b.pump(out);
        auto chAt = [&](size_t i) { return i < out.sent.size() ? int(out.sent[i].channel) : -1; };
        TEST("Rhythm1 -> Percussion (ch11)", chAt(0) == 10);
        TEST("Rhythm2 -> Drum (ch10)", chAt(1) == 9);
        TEST("Bass -> Bass (ch9)", chAt(2) == 8);
        TEST("Pad (ch14) -> Acc3 (ch14)", chAt(3) == 13);
        TEST("ch3 dropped; bank/PC dropped; CC7 kept on Bass",
             out.sent.size() == 5 && out.sent[4].type == MidiEventType::ControlChange &&
             out.sent[4].data1 == 7 && chAt(4) == 8 && b.routedOut() == 4);
        bool lowCh = false;
        for (const auto& e : out.sent) lowCh |= e.channel < 8;
        TEST("nothing leaves on ch1-8", !lowCh);
        OutputChannelMap bad; bad.dst[0] = 200;
        b.setChannelMap(bad);
        TEST("out-of-range destination is stored as drop", b.channelMap().dst[0] == OutputChannelMap::kDrop);
    }

    // ── Facade: refused while playing; demo style through the preset ──
    {
        midi::FakeMidiInputSource  fin;  fin.devices = {{0, "Pa5X KEYBOARD"}};
        midi::FakeMidiOutputProvider fout; fout.setDevices({{0, "Pa5X SOUND"}});
        LiveEngineFacade fac(&fin, &fout);
        chord::FingeredDetector det;
        fac.setChordScanMode(&det);
        fac.start();
        fac.selectMidiOutput(0);
        fac.tick(48);
        TEST("facade default map is identity", fac.outputChannelMap().dst[9] == 9 &&
             fac.outputChannelMap().dst[1] == 1 && !fac.outputChannelMap().dropVoiceSelect);

        fac.transportStart();
        for (int i = 0; i < 10; ++i) fac.tick(480);
        TEST("map change refused while playing", !fac.setOutputChannelMap(midi::pa5x::genosStyleTrackMap()) &&
             fac.outputChannelMap().dst[1] == 1);
        fac.transportStop();
        fac.tick(0);
        TEST("map change accepted when stopped", fac.setOutputChannelMap(midi::pa5x::genosStyleTrackMap()));

        fout.sent.clear();
        fac.transportStart();
        for (int i = 0; i < 400; ++i) fac.tick(480);
        fac.transportStop();
        fac.tick(0);
        std::set<int> chans;
        int on = 0, off = 0;
        for (const auto& e : fout.sent) {
            chans.insert(e.channel);
            if (e.type == MidiEventType::NoteOn && e.data2 > 0) ++on;
            if (e.type == MidiEventType::NoteOff || (e.type == MidiEventType::NoteOn && e.data2 == 0)) ++off;
        }
        bool onlyStyle = !chans.empty();
        for (int c : chans) onlyStyle &= c >= 8;
        TEST("demo style through the preset: output only on ch9-16", onlyStyle);
        TEST("demo drums (ch10) still sound", chans.count(9) == 1);
        TEST("every Note On has its Note Off", on > 0 && on == off);
        fac.stop();
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
