// SFF1 importer — SMF Set Tempo (FF 51 03 tt tt tt) import (Issue #2).
//
// Contract pinned here:
//   - The reader records every well-formed FF 51 as raw microseconds-per-quarter
//     (MPQN) with its absolute tick; no rounding at the reader layer.
//   - FF 51 with a length other than 3, or MPQN 0, is ignored with a warning.
//     A truncated event stops decoding (existing reader behaviour).
//   - The style tempo is the earliest tempo by absolute tick (file order breaks
//     ties), converted with round-to-nearest integer math. Later tempo changes
//     are ignored with a warning (no tempo map).
//   - No tempo, or a tempo outside 20..400 BPM, falls back to 120 BPM.

#include "importers/sff1/sff1_reader.h"
#include "importers/sff1/sff1_mapper.h"
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace ai_arranger::importers::sff1;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", std::string(name).c_str()); failures++; } \
    else { std::printf("  PASS: %s\n", std::string(name).c_str()); passes++; } \
} while(0)

using Bytes = std::vector<uint8_t>;

static Bytes mtrk(const Bytes& t) {
    Bytes b = {'M', 'T', 'r', 'k'};
    const uint32_t n = static_cast<uint32_t>(t.size());
    b.push_back(n >> 24); b.push_back(n >> 16); b.push_back(n >> 8); b.push_back(n);
    b.insert(b.end(), t.begin(), t.end());
    return b;
}
// SMF with 480 PPQN and the given tracks.
static Bytes smf(const std::vector<Bytes>& tracks) {
    Bytes b = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, static_cast<uint8_t>(tracks.size()), 0x01, 0xE0};
    for (const auto& t : tracks) { const Bytes c = mtrk(t); b.insert(b.end(), c.begin(), c.end()); }
    return b;
}
static Bytes tempo(uint32_t mpqn) {   // delta 0 + FF 51 03 tt tt tt
    return {0x00, 0xFF, 0x51, 0x03, uint8_t(mpqn >> 16), uint8_t(mpqn >> 8), uint8_t(mpqn)};
}
static Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes b; for (const auto& p : parts) b.insert(b.end(), p.begin(), p.end()); return b;
}
static const Bytes kNote = {0x00, 0x90, 0x3C, 0x64, 0x83, 0x60, 0x80, 0x3C, 0x00};  // C4 for 480 ticks
static const Bytes kEot  = {0x00, 0xFF, 0x2F, 0x00};

static ParseResult parse(const Bytes& b) { Sff1Reader r; return r.parseBuffer(b, "synthetic"); }
static uint32_t bpmOf(const ParseResult& p) {
    Sff1ToUasfMapper m; const auto r = m.map(p); return r.success ? r.style.tempo_bpm : 0;
}
static bool warned(const std::vector<std::string>& w, const char* needle) {
    for (const auto& s : w) if (s.find(needle) != std::string::npos) return true;
    return false;
}
static size_t noteCount(const ParseResult& p) {
    size_t n = 0;
    for (const auto& s : p.sections) for (const auto& t : s.tracks) n += t.events.size();
    return n;
}

int main() {
    std::printf("Test: SFF1 SMF Set Tempo import\n");

    {   // 1-3. valid FF 51: exact raw MPQN at its tick, and the BPM conversion
        const auto p = parse(smf({cat({tempo(600000), kNote, kEot})}));
        TEST("valid: one tempo recorded", p.success && p.tempos.size() == 1);
        TEST("valid: raw MPQN 600000 at tick 0",
             p.tempos.size() == 1 && p.tempos[0].mpqn == 600000 && p.tempos[0].tick == 0);
        TEST("valid: 600000 us/q -> 100 BPM", bpmOf(p) == 100);
        TEST("valid: 500000 us/q -> 120 BPM", bpmOf(parse(smf({cat({tempo(500000), kNote, kEot})}))) == 120);
        // Non-integer quotients round to nearest (truncation would give 104 / 89).
        TEST("rounding: 571429 us/q (104.9999) -> 105 BPM",
             bpmOf(parse(smf({cat({tempo(571429), kNote, kEot})}))) == 105);
        TEST("rounding: 666667 us/q (89.99996) -> 90 BPM",
             bpmOf(parse(smf({cat({tempo(666667), kNote, kEot})}))) == 90);
        TEST("rounding: 789473 us/q (76.0001) -> 76 BPM",
             bpmOf(parse(smf({cat({tempo(789473), kNote, kEot})}))) == 76);
        TEST("max 24-bit field is accepted by the reader",
             parse(smf({cat({tempo(0xFFFFFF), kNote, kEot})})).tempos.size() == 1);
    }
    {   // 4. missing tempo -> 120 fallback
        const auto p = parse(smf({cat({kNote, kEot})}));
        TEST("missing: no tempo recorded", p.success && p.tempos.empty());
        TEST("missing: style falls back to 120 BPM", bpmOf(p) == 120);
    }
    {   // 5. malformed length: ignored, warned, rest of the track still decoded
        const Bytes bad2 = {0x00, 0xFF, 0x51, 0x02, 0x09, 0x27};
        const Bytes bad4 = {0x00, 0xFF, 0x51, 0x04, 0x00, 0x07, 0xA1, 0x20};
        const auto p = parse(smf({cat({bad2, bad4, kNote, kEot})}));
        TEST("bad length: ignored", p.success && p.tempos.empty());
        TEST("bad length: warned", warned(p.warnings, "Set Tempo"));
        TEST("bad length: following note still decoded", noteCount(p) == 2);
        TEST("bad length: style falls back to 120 BPM", bpmOf(p) == 120);
    }
    {   // 6. truncated FF 51 at the end of the track: no tempo, no crash
        const Bytes trunc = {0x00, 0xFF, 0x51, 0x03, 0x09, 0x27};
        const auto p = parse(smf({cat({kNote, trunc})}));
        TEST("truncated: parse survives", p.success);
        TEST("truncated: no tempo recorded", p.tempos.empty());
        TEST("truncated: earlier note kept", noteCount(p) == 2);
    }
    {   // 7. MPQN 0 is not a tempo
        const auto p = parse(smf({cat({tempo(0), kNote, kEot})}));
        TEST("zero MPQN: ignored", p.success && p.tempos.empty());
        TEST("zero MPQN: warned", warned(p.warnings, "Set Tempo"));
        TEST("zero MPQN: style falls back to 120 BPM", bpmOf(p) == 120);
    }
    {   // 8. multiple tempo events: all recorded; the initial one is the style tempo
        const Bytes later = {0x83, 0x60, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20};  // tick 480: 120 BPM
        const auto p = parse(smf({cat({tempo(600000), kNote, later, kEot})}));
        TEST("multiple: both recorded in order",
             p.tempos.size() == 2 && p.tempos[0].mpqn == 600000 && p.tempos[1].mpqn == 500000 &&
             p.tempos[1].tick == 960);
        Sff1ToUasfMapper m; const auto r = m.map(p);
        TEST("multiple: style tempo is the initial tempo (100)", r.success && r.style.tempo_bpm == 100);
        TEST("multiple: later tempo change warned", warned(r.warnings, "tempo change"));
        // Same tick, two values: file order decides.
        const auto q = parse(smf({cat({tempo(600000), tempo(500000), kNote, kEot})}));
        TEST("same tick: first in file order wins (100)", bpmOf(q) == 100);
    }
    {   // 9. tempo after other meta events and a non-zero delta
        const Bytes marker = {0x00, 0xFF, 0x06, 0x04, 'S', 'I', 'n', 't'};
        const Bytes ts     = {0x00, 0xFF, 0x58, 0x04, 0x03, 0x02, 0x18, 0x08};
        const Bytes late   = {0x83, 0x60, 0xFF, 0x51, 0x03, 0x0B, 0x71, 0xB0};  // tick 480: 750000 = 80 BPM
        const auto p = parse(smf({cat({marker, ts, late, kNote, kEot})}));
        TEST("after meta: tempo recorded at its absolute tick",
             p.tempos.size() == 1 && p.tempos[0].tick == 480 && p.tempos[0].mpqn == 750000);
        TEST("after meta: time signature still read (3/4)", p.time_sig_num == 3 && p.time_sig_den == 4);
        TEST("after meta: style tempo 80 BPM", bpmOf(p) == 80);
    }
    {   // 10. running status around a tempo meta: tempo bytes are never note data,
        //     and explicit statuses after it decode normally.
        const Bytes notes = {0x00, 0x90, 0x3C, 0x64,      // NoteOn (sets running status)
                             0x10, 0x3E, 0x64,            // running-status NoteOn
                             0x00, 0xFF, 0x51, 0x03, 0x08, 0x2C, 0xA2,  // 535714 us/q = 112 BPM
                             0x10, 0x80, 0x3C, 0x00,      // explicit NoteOff
                             0x00, 0x80, 0x3E, 0x00};
        const auto p = parse(smf({cat({notes, kEot})}));
        TEST("running status: tempo recorded", p.tempos.size() == 1 && p.tempos[0].mpqn == 535714);
        TEST("running status: all 4 channel events decoded, none from tempo bytes", noteCount(p) == 4);
        TEST("running status: style tempo 112 BPM", bpmOf(p) == 112);
    }
    {   // 11. tempo in several MTrk chunks: earliest absolute tick wins, not file order
        const Bytes late = {0x87, 0x40, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20};    // tick 960: 120 BPM
        const auto p = parse(smf({cat({late, kNote, kEot}), cat({tempo(600000), kNote, kEot})}));
        TEST("multi-track: both tempos recorded", p.tempos.size() == 2);
        TEST("multi-track: earliest tick (track 2, tick 0) wins -> 100 BPM", bpmOf(p) == 100);
    }
    {   // 12. out-of-range BPM falls back to 120 with a warning
        const auto slow = parse(smf({cat({tempo(0xFFFFFF), kNote, kEot})}));      // ~3.6 BPM
        Sff1ToUasfMapper m; const auto r = m.map(slow);
        TEST("range: 3.6 BPM falls back to 120", r.success && r.style.tempo_bpm == 120);
        TEST("range: out-of-range tempo warned", warned(r.warnings, "out of range"));
        TEST("range: 600 BPM falls back to 120", bpmOf(parse(smf({cat({tempo(100000), kNote, kEot})}))) == 120);
        TEST("range: 20 BPM (3000000 us/q) accepted", bpmOf(parse(smf({cat({tempo(3000000), kNote, kEot})}))) == 20);
        TEST("range: 400 BPM (150000 us/q) accepted", bpmOf(parse(smf({cat({tempo(150000), kNote, kEot})}))) == 400);
    }

    {   // 13. real Genos corpus: one FF 51 at tick 0 each (independently decoded
        //     from the files: format 0, single MTrk, 1920 PPQN).
        struct Want { const char* needle; uint32_t mpqn; uint32_t bpm; };
        const Want want[] = {{"POP_ACOUSTIC_2", 600000, 100}, {"CLASSIC_6_8", 666667, 90},
                             {"BALLAD_FOLK", 571429, 105},    {"C_WHISPER", 789473, 76}};
        for (const auto& w : want) {
            std::string path;
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(CORPUS_DIR, ec)) {
                const std::string fn = e.path().filename().string();
                if (fn.find(w.needle) != std::string::npos && e.path().extension() == ".sty")
                    path = e.path().string();
            }
            const std::string tag = w.needle;
            TEST(tag + ": corpus .sty present", !path.empty());
            if (path.empty()) continue;
            Sff1Reader r; const auto p = r.parseFile(path);
            TEST(tag + ": exactly one Set Tempo, at tick 0, MPQN " + std::to_string(w.mpqn),
                 p.success && p.tempos.size() == 1 && p.tempos[0].tick == 0 && p.tempos[0].mpqn == w.mpqn);
            TEST(tag + ": imported tempo " + std::to_string(w.bpm) + " BPM", bpmOf(p) == w.bpm);
        }
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
