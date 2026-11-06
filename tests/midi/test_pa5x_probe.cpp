// pa5x-probe pure helpers: held-note tracking, chord naming, argument parsing.
#include "midi/pa5x_probe.h"
#include "engine/chord/fingered_detector.h"
#include <cstdio>

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

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
