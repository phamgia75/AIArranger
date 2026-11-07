// pa5x-probe pure helpers: held-note tracking, chord naming, argument parsing.
#include "midi/pa5x_probe.h"
#include "engine/chord/fingered_detector.h"
#include "engine/chord/fingered_on_bass_detector.h"
#include "engine/midi/midi_bytes.h"
#include <cstdio>
#include <vector>

using namespace ai_arranger;
using namespace ai_arranger::midi;
namespace probe = ai_arranger::midi::pa5x::probe;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
                              else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

static MidiInputMessage on(uint8_t ch, uint8_t n, uint8_t v = 100) { return {MidiInMsgType::NoteOn, ch, n, v}; }
static MidiInputMessage off(uint8_t ch, uint8_t n) { return {MidiInMsgType::NoteOff, ch, n, 0}; }

int main() {
    std::printf("Test: pa5x-probe helpers\n");
    probe::HeldNotes h;
    uint8_t buf[128];
    // E G C on ch 3 (C/E first inversion) + a melody note above the split.
    TEST("note on changes held set", h.apply(on(2, 52)) && h.apply(on(2, 55)) && h.apply(on(2, 60)) && h.apply(on(2, 72)));
    TEST("CC does not change held set", !h.apply({MidiInMsgType::ControlChange, 2, 64, 127}));
    size_t n = h.below(2, 61, buf, sizeof buf);
    TEST("below split 61: E3 G3 C4 ascending, melody excluded", n == 3 && buf[0] == 52 && buf[1] == 55 && buf[2] == 60);
    TEST("other channel empty", h.below(0, 128, buf, sizeof buf) == 0);
    const arranger::Chord c = chord::FingeredDetector().detect(buf, n);
    TEST("detector on held notes: C major (inversion)", c.type == arranger::ChordType::Major && c.root % 12 == 0);
    TEST("note on velocity 0 releases", h.apply(on(2, 55, 0)) && h.below(2, 61, buf, sizeof buf) == 2);
    TEST("note off releases", h.apply(off(2, 52)) && h.below(2, 61, buf, sizeof buf) == 1);
    TEST("out-of-range channel/note ignored", !h.apply(on(16, 60)) && !h.apply(on(0, 200)));
    TEST("split 0..: nothing below 1 when only C4 held", h.below(2, 1, buf, sizeof buf) == 0);

    TEST("chord names", std::string(probe::chordTypeName(arranger::ChordType::Min7b5)) == "m7b5" &&
                        std::string(probe::chordTypeName(arranger::ChordType::NoChord)) == "NoChord");
    TEST("pitch names", std::string(probe::pitchName(61)) == "C#" && std::string(probe::pitchName(70)) == "Bb");
    TEST("channel 1 and 16 -> 0 and 15", probe::parseChannel("1") == 0 && probe::parseChannel("16") == 15);
    TEST("channel 0, 17, 1x, empty, -1 refused", !probe::parseChannel("0") && !probe::parseChannel("17") &&
         !probe::parseChannel("1x") && !probe::parseChannel("") && !probe::parseChannel("-1") && !probe::parseChannel("007"));
    TEST("command names map to Pa5X Program Change values",
         probe::commandFromName("variation1") == pa5x::Command::Variation1 &&
         probe::commandFromName("playstop1") == pa5x::Command::PlayStopPlayer1 &&
         static_cast<int>(*probe::commandFromName("variation2")) == 84);
    TEST("unknown command refused", !probe::commandFromName("Variation1") && !probe::commandFromName("reset"));

    // clockPlan: Start, 24 clocks per beat, Stop; exact per-tick offsets.
    const auto cp = probe::clockPlan(120, 2);   // 120 BPM: 1 beat = 500 ms, 1 tick = 20.833 ms
    bool order = cp.size() == 50 && cp.front().status == 0xFA && cp.back().status == 0xFC;
    for (size_t i = 1; order && i + 1 < cp.size(); ++i) order = cp[i].status == 0xF8;
    TEST("clock plan: Start, 48 clocks, Stop, in that order", order);
    bool mono = true;
    for (size_t i = 1; i < cp.size(); ++i) mono = mono && cp[i].ns >= cp[i - 1].ns;
    TEST("clock plan: offsets never go back", mono);
    TEST("clock plan: first clock with Start, clock 24 at exactly 500 ms, Stop at 1 s",
         cp[1].ns == 0 && cp[25].ns == 500000000ull && cp.back().ns == 1000000000ull);
    TEST("clock plan: 97 BPM, tick 72 computed from the start (no cumulative rounding)",
         probe::clockPlan(97, 4)[1 + 72].ns == 72ull * 60000000000ull / (97 * 24));
    TEST("clock plan: out-of-range tempo or length is refused",
         probe::clockPlan(19, 1).empty() && probe::clockPlan(301, 1).empty() &&
         probe::clockPlan(120, 0).empty() && probe::clockPlan(120, 257).empty() && !probe::clockPlan(300, 256).empty());

    // Gate 2 retry (2026-11-07 12:07, evidence/hardware/2026-11-07-pa5x-gate2-chords.md): Note On
    // bytes exactly as the Pa5X KEYBOARD sent them (status 0x93 = ch 4, the lower part), in arrival
    // order, replayed through HeldNotes and the fingered detector as `listen` does.
    {
        const chord::FingeredDetector fd;
        struct Case { const char* name; std::vector<uint8_t> ons; arranger::ChordType type; int root, bass; };
        const Case hw[] = {
            {"hw C2-E2-G2 -> C major, bass C", {0x24, 0x28, 0x2B}, arranger::ChordType::Major, 0, 0},
            {"hw E1-G1-C2 -> C major, root C (fingered ignores the bass)", {0x1C, 0x24, 0x1F}, arranger::ChordType::Major, 0, 0},
            {"hw F1-A1-C2 -> F major, bass F", {0x1D, 0x21, 0x24}, arranger::ChordType::Major, 5, 5},
            {"hw C2-Bb2-E2 (no 5th) -> C7, bass C", {0x24, 0x2E, 0x28}, arranger::ChordType::Dom7, 0, 0},
        };
        for (const auto& c : hw) {
            probe::HeldNotes held;
            for (uint8_t n : c.ons) held.apply(on(3, n));
            const size_t n = held.below(3, 60, buf, sizeof buf);
            const arranger::Chord ch = fd.detect(buf, n);
            const int bass = (ch.bass ? ch.bass : ch.root) % 12;
            TEST(c.name, n == 3 && ch.type == c.type && ch.root % 12 == c.root && bass == c.bass);
        }
        probe::HeldNotes held;  // the same captured inversion in Fingered-on-Bass keeps E as the bass
        for (uint8_t n : {0x1C, 0x24, 0x1F}) held.apply(on(3, n));
        const arranger::Chord ob = chord::FingeredOnBassDetector{}.detect(buf, held.below(3, 60, buf, sizeof buf));
        TEST("hw E1-G1-C2 on-bass -> C major over E1", ob.type == arranger::ChordType::Major && ob.root % 12 == 0 && ob.bass == 0x1C);
    }

    // Phase B 17:30: the tool printed "sent bytes so far: 2" for 2 messages (Note On + Note Off).
    TEST("send summary counts messages, not bytes",
         probe::sentSummary(2).find("2 MIDI messages") != std::string::npos &&
         probe::sentSummary(2).find("bytes") == std::string::npos &&
         probe::sentSummary(1).find("1 MIDI message ") != std::string::npos);
    TEST("send summary says host-side only and that All Sound Off follows",
         probe::sentSummary(2).find("not proof the device sounded") != std::string::npos &&
         probe::sentSummary(2).find("All Sound Off follows") != std::string::npos);

    // Phase B 17:30 command `note --channel 1 --note 60 --vel 64`: CLI channel 1 is wire channel 0.
    {
        const auto c1 = probe::parseChannel("1");
        uint8_t on3[3] = {}, off3[3] = {};
        const size_t a = encodeMidiEvent({uasf::MidiEventType::NoteOn, c1.value_or(99), 60, 64, 0}, on3);
        const size_t b = encodeMidiEvent({uasf::MidiEventType::NoteOff, c1.value_or(99), 60, 0, 0}, off3);
        TEST("--channel 1 --note 60 --vel 64 is 90 3C 40, then 80 3C 00",
             c1 == 0 && a == 3 && on3[0] == 0x90 && on3[1] == 0x3C && on3[2] == 0x40 &&
             b == 3 && off3[0] == 0x80 && off3[1] == 0x3C && off3[2] == 0x00);
        TEST("--channel 16 is status nibble F; 0 and 17 are refused",
             probe::parseChannel("16") == 15 && !probe::parseChannel("0") && !probe::parseChannel("17"));
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
