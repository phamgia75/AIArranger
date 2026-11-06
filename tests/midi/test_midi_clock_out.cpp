// MIDI Clock out (roadmap P0.6): 24 pulses per quarter note on the engine's own
// tick grid, Start before the first pulse, Stop when the transport stops, never
// touched by the output channel map, and independent of Program Changes (Pa5X
// Control-channel commands and the style's own voice selects).

#include "session/live_engine_facade.h"
#include "engine/midi/coremidi_out.h"
#include "midi/pa5x_control.h"
#include "midi/pa5x_profile.h"
#include "engine/chord/fingered_detector.h"
#include "fake_midi_input_source.h"
#include "fake_midi_output_provider.h"
#include <cstdio>

using namespace ai_arranger;
using uasf::MidiEvent;
using uasf::MidiEventType;
using session::LiveEngineFacade;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static bool isRt(const MidiEvent& e) {
    return e.type == MidiEventType::TimingClock || e.type == MidiEventType::Start ||
           e.type == MidiEventType::Stop;
}
static int count(const std::vector<MidiEvent>& v, MidiEventType t) {
    int n = 0;
    for (const auto& e : v) n += e.type == t;
    return n;
}
// Pulses due by tick `pos` (n = 0, 1, ... with n*ppqn/24 <= pos), computed
// independently of the implementation's loop.
static int64_t expectedPulses(int64_t pos, int64_t ppqn) {
    int64_t n = 0;
    while (n * ppqn / 24 <= pos) ++n;
    return n;
}

struct Rig {
    midi::FakeMidiInputSource  fin;
    midi::FakeMidiOutputProvider fout;
    chord::FingeredDetector det;
    LiveEngineFacade fac{&fin, &fout};
    Rig() {
        fin.devices = {{0, "Pa5X KEYBOARD"}};
        fout.setDevices({{0, "Pa5X SOUND"}});
        fac.setChordScanMode(&det);
        fac.start();
        fac.selectMidiOutput(0);
        fac.tick(48);
        fout.sent.clear();
    }
};

int main() {
    std::printf("Test: MIDI clock out\n");

    // ── Encoding ──
    {
        uint8_t b[3] = {0, 0, 0};
        bool ok = true;
        for (auto t : {MidiEventType::TimingClock, MidiEventType::Start, MidiEventType::Stop}) {
            const MidiEvent e{t, 9, 0, 0, 0};
            ok &= midi::midiEventToBytes(e, b) == 1 && b[0] == static_cast<uint8_t>(t);
        }
        TEST("F8/FA/FC encode as one byte, channel ignored", ok);
    }

    // ── Off by default ──
    {
        Rig r;
        TEST("clock out off by default", !r.fac.midiClockOut());
        r.fac.transportStart();
        for (int i = 0; i < 100; ++i) r.fac.tick(480);
        r.fac.transportStop(); r.fac.tick(0);
        bool none = true;
        for (const auto& e : r.fout.sent) none &= !isRt(e);
        TEST("off: no real-time messages while playing", none && !r.fout.sent.empty());
        r.fac.stop();
    }

    // ── Timing, Start/Stop ordering ──
    {
        Rig r;
        TEST("enable while stopped", r.fac.setMidiClockOut(true));
        r.fac.transportStart();
        r.fac.tick(0);
        const auto& s = r.fout.sent;
        TEST("first message is Start, then a pulse",
             s.size() >= 2 && s[0].type == MidiEventType::Start && s[1].type == MidiEventType::TimingClock);
        TEST("refused while playing", !r.fac.setMidiClockOut(false) && r.fac.midiClockOut());

        const int64_t ppqn = r.fac.session().clock().getResolution();
        bool grid = true;
        for (int i = 0; i < 400; ++i) {   // 400 x 480 samples = 4 s at 48 kHz
            r.fac.tick(480);
            const int64_t pos = r.fac.snapshot().positionTicks;
            grid &= count(s, MidiEventType::TimingClock) == expectedPulses(pos, ppqn);
        }
        TEST("after every tick: pulse count == floor grid of 24 per quarter", grid);
        const int pulses = count(s, MidiEventType::TimingClock);
        // 4 s at 120 bpm = 8 quarter notes = 192 pulses (+1 for pulse 0).
        TEST("120 bpm: 193 pulses in 4 s", r.fac.snapshot().tempoBpm != 120 || pulses == 193);
        TEST("one Start, no Stop yet",
             count(s, MidiEventType::Start) == 1 && count(s, MidiEventType::Stop) == 0);

        r.fac.transportStop();
        r.fac.tick(480);
        TEST("Stop is the last real-time message",
             count(s, MidiEventType::Stop) == 1 && [&] {
                 for (size_t i = s.size(); i-- > 0;) if (isRt(s[i])) return s[i].type == MidiEventType::Stop;
                 return false; }());
        const size_t afterStop = s.size();
        for (int i = 0; i < 20; ++i) r.fac.tick(480);
        bool quiet = true;
        for (size_t i = afterStop; i < s.size(); ++i) quiet &= !isRt(s[i]);
        TEST("no pulses while stopped", quiet);

        // Restart: a fresh Start and pulse 0 again.
        r.fout.sent.clear();
        r.fac.transportStart();
        r.fac.tick(0);
        TEST("restart sends Start + pulse",
             s.size() >= 2 && s[0].type == MidiEventType::Start && s[1].type == MidiEventType::TimingClock);
        r.fac.stop();   // facade stop while playing
        TEST("facade stop() while playing sends Stop", count(s, MidiEventType::Stop) == 1);
    }

    // ── Tempo follows ──
    {
        Rig r;
        r.fac.setMidiClockOut(true);
        r.fac.setTempo(60);
        r.fac.transportStart();
        r.fac.tick(0);
        for (int i = 0; i < 100; ++i) r.fac.tick(480);   // 1 s
        const int slow = count(r.fout.sent, MidiEventType::TimingClock);
        r.fout.sent.clear();
        r.fac.setTempo(240);
        r.fac.tick(0);
        for (int i = 0; i < 100; ++i) r.fac.tick(480);   // 1 s
        const int fast = count(r.fout.sent, MidiEventType::TimingClock);
        TEST("60 bpm: 24-25 pulses per second", slow >= 24 && slow <= 25);
        TEST("240 bpm: 96 +-1 pulses per second", fast >= 95 && fast <= 97);
        r.fac.stop();
    }

    // ── Channel independence and Program Change interaction ──
    {
        Rig r;
        r.fac.setMidiClockOut(true);
        r.fac.setOutputChannelMap(midi::pa5x::genosStyleTrackMap());   // drops ch1-8 + every PC
        const uint64_t routedBefore = 0;
        r.fac.transportStart();
        r.fac.tick(0);
        for (int i = 0; i < 50; ++i) r.fac.tick(480);
        const auto& s = r.fout.sent;
        const int before = count(s, MidiEventType::TimingClock);
        TEST("clock passes the Genos map", before > 0 && count(s, MidiEventType::Start) == 1);

        // Pa5X Control-channel PC mid-play: out on its channel, ahead of the
        // tick's pulses, and the pulse grid is unaffected.
        const size_t mark = s.size();
        r.fac.sendDeviceCommand(midi::pa5x::controlEvent(midi::pa5x::Command::Variation2, 15));
        r.fac.tick(480);
        bool pcFirst = mark < s.size() && s[mark].type == MidiEventType::ProgramChange &&
                       s[mark].channel == 15 && s[mark].data1 == 84;
        TEST("control PC goes out before that tick's pulses", pcFirst);
        const int64_t ppqn = r.fac.session().clock().getResolution();
        TEST("pulse grid unchanged by the PC",
             count(s, MidiEventType::TimingClock) == expectedPulses(r.fac.snapshot().positionTicks, ppqn));

        // The style's own voice selects are still dropped by the map; no other PC leaves.
        int pcs = 0;
        for (const auto& e : s) pcs += e.type == MidiEventType::ProgramChange;
        TEST("only the control PC left (style PCs still dropped)", pcs == 1);
        bool lowCh = false;
        for (const auto& e : s) lowCh |= !isRt(e) && e.channel < 8 && e.type != MidiEventType::ProgramChange;
        TEST("notes still only on ch9-16", !lowCh);
        (void)routedBefore;
        r.fac.stop();
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
