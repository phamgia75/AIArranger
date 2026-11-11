// Phase C Style playback validation package, OFFLINE: the built-in demo style at a fixed tempo,
// for a bounded 2 bars, through the explicit Pa5X demo channel map, into a fake provider (never
// CoreMIDI, never a device). Expected events are written out by hand from the demo style data, not
// computed by the engine, and compared with what reached the provider; timing and order are checked
// on the captured ticks. A PASS here is SOFTWARE PASS only, never a hardware PASS.

#include "control/engine_midi_route.h"
#include "control/midi_output_bridge.h"
#include "midi/pa5x_profile.h"
#include "session/engine_session.h"
#include "engine/arranger/style_player.h"
#include "fake_midi_output_provider.h"
#include "engine/control/control_events.h"
#include <algorithm>
#include <cstdio>
#include <tuple>
#include <vector>

using namespace ai_arranger;
using uasf::MidiEventType;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

namespace {

constexpr int64_t kBar = 1920;                  // 4/4 at 480 ppq
constexpr int64_t kWindow = 2 * kBar + 100;     // bounded: ~2 bars (~4.1 s at 120 BPM); stops while bar-3 downbeat notes sound
constexpr uint8_t kBass = 8, kDrum = 9, kAcc1 = 11;   // wire indexes: ch 9, 10, 12

using Ev = std::tuple<uint64_t, uint8_t, uint8_t>;   // tick, channel index, note

// Hand-written from buildDemoStyle(): per bar, Bass C3 beat 1 + G3 beat 3; Chord C4 E4 G4 beat 1;
// kick beats 1, 3; snare 2, 4; hi-hat every 8th. Under C major the NTT leaves every note as is.
std::vector<Ev> expectedNoteOns() {
    std::vector<Ev> v;
    for (int64_t bar = 0; bar < 3; ++bar) {
        const uint64_t b = bar * kBar;
        v.push_back({b, kBass, 48}); v.push_back({b + 960, kBass, 55});
        for (uint8_t n : {60, 64, 67}) v.push_back({b, kAcc1, n});
        v.push_back({b, kDrum, 36}); v.push_back({b + 960, kDrum, 36});
        v.push_back({b + 480, kDrum, 38}); v.push_back({b + 1440, kDrum, 38});
        for (int e = 0; e < 8; ++e) v.push_back({b + e * 240, kDrum, 42});
    }
    v.erase(std::remove_if(v.begin(), v.end(), [](const Ev& e) { return std::get<0>(e) >= static_cast<uint64_t>(kWindow); }), v.end());
    std::sort(v.begin(), v.end());
    return v;
}

struct Run {
    std::vector<uasf::MidiEvent> sent;   // what reached the provider during play
    std::vector<uasf::MidiEvent> afterStop;
};

Run play(uint16_t chordParam) {
    session::EngineSession s; s.boot();
    // Known-good style = the demo's Main 1 (drums, bass, chord): Start plays section 0, and the
    // demo Intro has no chord track, so the Intro is removed to cover the Acc1 route as well.
    auto style = arranger::buildDemoStyle();
    style.sections.erase(style.sections.begin());
    if (style.sections.empty() || style.sections[0].type != uasf::SectionType::Main1 || !s.loadStyle(style)) return {};
    s.clock().setTempo(120);
    control::MidiOutputBridge bridge;
    bridge.setChannelMap(midi::pa5x::demoStyleTrackMap());
    midi::FakeMidiOutputProvider provider;
    provider.setDevices({{0, "fake (offline)"}}); provider.select(0);
    control::attachEngineOutput(s.scheduler(), bridge);

    s.adapter().postEvent({control::ControlAction::Chord, chordParam, 0});
    s.adapter().postEvent({control::ControlAction::Start, 0, 0});
    // 256-sample blocks at 48 kHz; stop on the first block that reaches the window end.
    while (true) {
        s.tick();
        s.clock().advance(256);
        const int64_t pos = s.clock().getPosition();
        s.scheduler().advanceTo(std::min(pos, kWindow - 1));
        bridge.pump(provider);
        if (pos >= kWindow) break;
    }
    Run r;
    r.sent = provider.sent;
    s.adapter().postEvent({control::ControlAction::Stop, 0, 0});
    for (int i = 0; i < 50; ++i) {   // keep pumping after Stop: nothing may start
        s.tick(); s.clock().advance(256);
        s.scheduler().advanceTo(s.clock().getPosition() + kWindow);
        bridge.pump(provider);
    }
    r.afterStop.assign(provider.sent.begin() + static_cast<long>(r.sent.size()), provider.sent.end());
    s.shutdown();
    return r;
}

} // namespace

int main() {
    std::printf("Test: Pa5X Style playback package (offline, fake provider)\n");

    TEST("demo map: bass -> ch 9, drums -> ch 10, chord -> ch 12, rest dropped, voice select dropped", [] {
        const auto m = midi::pa5x::demoStyleTrackMap();
        int kept = 0;
        for (auto d : m.dst) kept += d != control::OutputChannelMap::kDrop;
        return m.dst[1] == kBass && m.dst[9] == kDrum && m.dst[2] == kAcc1 && kept == 3 && m.dropVoiceSelect;
    }());

    const Run r = play(60 | (static_cast<uint16_t>(arranger::ChordType::Major) << 8));

    std::vector<Ev> actual;
    bool onlyNotes = true, ordered = true, inWindow = true;
    for (size_t i = 0; i < r.sent.size(); ++i) {
        const auto& e = r.sent[i];
        onlyNotes &= e.type == MidiEventType::NoteOn || e.type == MidiEventType::NoteOff;
        if (i) ordered &= e.tick >= r.sent[i - 1].tick;
        inWindow &= e.tick < static_cast<uint64_t>(kWindow);
        if (e.type == MidiEventType::NoteOn && e.data2 > 0) actual.push_back({e.tick, e.channel, e.data1});
    }
    std::sort(actual.begin(), actual.end());
    const auto expected = expectedNoteOns();

    // Expected-vs-actual log (kept in the CI test output as evidence).
    std::printf("    expected %zu Note On, actual %zu\n", expected.size(), actual.size());
    for (size_t i = 0; i < std::max(expected.size(), actual.size()); ++i) {
        auto show = [](const std::vector<Ev>& v, size_t k, char* b) {
            if (k < v.size()) std::snprintf(b, 32, "%5llu ch%-2d %3d", (unsigned long long)std::get<0>(v[k]),
                                             std::get<1>(v[k]) + 1, std::get<2>(v[k]));
            else std::snprintf(b, 32, "%-17s", "-");
        };
        char a[32], b[32];
        show(expected, i, a); show(actual, i, b);
        const bool same = i < expected.size() && i < actual.size() && expected[i] == actual[i];
        std::printf("    %s | %s %s\n", a, b, same ? "" : "<-- MISMATCH");
    }

    TEST("C major, 2 bars + 100 ticks: every expected Note On (tick, channel, pitch) and nothing else", actual == expected);
    TEST("only Note On/Off reach the provider (no PC, CC, clock, SysEx)", onlyNotes);
    TEST("provider order follows tick order", ordered);
    TEST("nothing dispatched past the window", inWindow);
    TEST("Stop came while notes were sounding (cleanup is exercised)", !r.afterStop.empty());

    int balance[16][128] = {};
    for (const auto* v : {&r.sent, &r.afterStop})
        for (const auto& e : *v) {
            if (e.type == MidiEventType::NoteOn && e.data2 > 0) ++balance[e.channel & 15][e.data1 & 127];
            else if (e.type == MidiEventType::NoteOff || e.type == MidiEventType::NoteOn) --balance[e.channel & 15][e.data1 & 127];
        }
    bool balanced = true;
    for (auto& ch : balance) for (int n : ch) balanced &= n <= 0;
    TEST("after Stop every Note On has its Note Off (no stuck notes)", balanced);
    bool noNewOn = true, onlyMapped = true;
    for (const auto& e : r.afterStop) {
        noNewOn &= !(e.type == MidiEventType::NoteOn && e.data2 > 0);
        onlyMapped &= e.channel == kBass || e.channel == kDrum || e.channel == kAcc1;
    }
    TEST("after Stop: no new Note On", noNewOn);
    TEST("after Stop: output stays on the 3 mapped channels", onlyMapped);

    const Run a = play(60 | (static_cast<uint16_t>(arranger::ChordType::Major) << 8));
    bool same = a.sent.size() == r.sent.size();
    for (size_t i = 0; same && i < a.sent.size(); ++i)
        same = a.sent[i].tick == r.sent[i].tick && a.sent[i].channel == r.sent[i].channel &&
               a.sent[i].data1 == r.sent[i].data1 && a.sent[i].type == r.sent[i].type;
    TEST("two identical runs give byte-identical event lists (deterministic)", same);

    {   // G major: bass plays G/D, chord plays G B D, drums untouched
        const Run g = play(67 | (static_cast<uint16_t>(arranger::ChordType::Major) << 8));
        bool bass = true, acc = true, drums = true; int n = 0;
        for (const auto& e : g.sent) {
            if (e.type != MidiEventType::NoteOn || e.data2 == 0) continue;
            ++n;
            const int pc = e.data1 % 12;
            if (e.channel == kBass) bass &= pc == 7 || pc == 2;
            else if (e.channel == kAcc1) acc &= pc == 7 || pc == 11 || pc == 2;
            else drums &= e.data1 == 36 || e.data1 == 38 || e.data1 == 42;
        }
        TEST("G major: bass G/D, Acc1 G-B-D, drums unchanged, same event count", bass && acc && drums && n == (int)expected.size());
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
