// Import / format hardening — Issue #6 items 6.1-6.5.
//
//  6.1 A PPQN/meter whose bar rounds to 0 ticks (1 PPQN in 1/16) used to import
//      "successfully" as a style that never sounds (the sequencer ignores a
//      0-tick bar). It is now an import error, and a validator ERROR.
//  6.2 RealtimeClock::isFirstBeatOfBar divided by a 0-tick bar (SIGFPE).
//  6.3 A section longer than 65535 bars was silently truncated to uint16 by the
//      serializer (and clamped by the legacy import path). Now rejected.
//  6.4 The SMF reader's absolute tick was uint32: crafted deltas wrapped it and
//      later events landed near tick 0. Decoding now stops before the wrap.
//  6.5 Markers from several MTrk chunks were kept in file order, so section
//      ranges could overlap. They are now sorted by tick (file order on ties).

#include "importers/sff1/sff1_reader.h"
#include "importers/sff1/sff1_mapper.h"
#include "engine/uasf/serializer.h"
#include "engine/uasf/validator.h"
#include "engine/realtime/clock.h"
#include "engine/arranger/style_player.h"
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

using namespace ai_arranger;
using namespace ai_arranger::importers::sff1;
using Bytes = std::vector<uint8_t>;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", std::string(name).c_str()); failures++; } \
    else { std::printf("  PASS: %s\n", std::string(name).c_str()); passes++; } \
} while(0)

static Bytes smf(uint16_t division, const std::vector<Bytes>& tracks) {
    Bytes b = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, static_cast<uint8_t>(tracks.size()),
               static_cast<uint8_t>(division >> 8), static_cast<uint8_t>(division)};
    for (const auto& t : tracks) {
        b.insert(b.end(), {'M', 'T', 'r', 'k'});
        const uint32_t n = static_cast<uint32_t>(t.size());
        b.insert(b.end(), {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)});
        b.insert(b.end(), t.begin(), t.end());
    }
    return b;
}
static Bytes vlq(uint32_t v) {
    Bytes out = {uint8_t(v & 0x7F)};
    while (v >>= 7) out.insert(out.begin(), uint8_t(0x80 | (v & 0x7F)));
    return out;
}
static Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes b; for (const auto& p : parts) b.insert(b.end(), p.begin(), p.end()); return b;
}
static Bytes marker(uint32_t delta, const std::string& name) {
    Bytes b = vlq(delta); b.insert(b.end(), {0xFF, 0x06, uint8_t(name.size())});
    b.insert(b.end(), name.begin(), name.end()); return b;
}
static Bytes ev(uint32_t delta, std::initializer_list<uint8_t> msg) {
    Bytes b = vlq(delta); b.insert(b.end(), msg); return b;
}
static const Bytes kEot = {0x00, 0xFF, 0x2F, 0x00};
static bool warned(const std::vector<std::string>& w, const char* needle) {
    for (const auto& s : w) if (s.find(needle) != std::string::npos) return true;
    return false;
}
static SffToUasfResult importOf(const Bytes& b) {
    Sff1Reader r; Sff1ToUasfMapper m; return m.map(r.parseBuffer(b, "synthetic"));
}

int main() {
    std::printf("Test: import / format hardening (Issue #6)\n");

    {   // 6.1 zero-tick bar: an import error, never a silent style
        const Bytes ts116 = {0x00, 0xFF, 0x58, 0x04, 0x01, 0x04, 0x18, 0x08};   // 1/16
        const auto unmarked = importOf(smf(1, {cat({ts116, ev(0, {0x90, 60, 100}), ev(5, {0x80, 60, 0}), kEot})}));
        TEST("6.1 1 PPQN in 1/16 (no markers): import rejected", !unmarked.success);
        TEST("6.1 the error names the zero-tick bar", unmarked.error.find("0 ticks") != std::string::npos);
        const auto marked = importOf(smf(1, {cat({ts116, marker(0, "Main A"), ev(0, {0x90, 60, 100}),
                                                  ev(5, {0x80, 60, 0}), kEot})}));
        TEST("6.1 1 PPQN in 1/16 (marker split): import rejected", !marked.success);
        TEST("6.1 a normal grid still imports",
             importOf(smf(480, {cat({ev(0, {0x90, 60, 100}), ev(480, {0x80, 60, 0}), kEot})})).success);

        auto st = arranger::buildDemoStyle();
        st.sections[0].resolution = 1; st.sections[0].beats_per_bar = 1; st.sections[0].beat_note = 16;
        uasf::UasfValidator v;
        TEST("6.1 validator: a 0-tick bar is an ERROR", !v.validate(st).valid);
        auto st2 = arranger::buildDemoStyle();
        st2.sections[0].beat_note = 0;
        TEST("6.1 validator: beat_note 0 is an ERROR", !uasf::UasfValidator().validate(st2).valid);
        TEST("6.1 validator: the demo style is still valid",
             uasf::UasfValidator().validate(arranger::buildDemoStyle()).valid);
    }
    {   // 6.2 isFirstBeatOfBar on a 0-tick bar
        realtime::RealtimeClock c;
        c.setResolution(1);
        c.setTimeSignature(1, 16);                       // bar and beat round to 0 ticks
        TEST("6.2 0-tick bar: ticksPerBar() == 0 (precondition)", c.ticksPerBar() == 0);
        TEST("6.2 isFirstBeatOfBar() returns false instead of dividing by 0", !c.isFirstBeatOfBar());
        c.setResolution(480); c.setTimeSignature(4, 4);
        TEST("6.2 normal grid: tick 0 is the first beat", c.isFirstBeatOfBar());
    }
    {   // 6.3 > 65535 bars: rejected, never truncated
        // 1 PPQN 4/4 = 4-tick bar; 300000 ticks = 75000 bars.
        const auto marked = importOf(smf(1, {cat({marker(0, "Main A"), ev(0, {0x90, 60, 100}),
                                                  ev(300000, {0x80, 60, 0}), kEot})}));
        TEST("6.3 marker split: 75000-bar section rejected", !marked.success);
        TEST("6.3 the error names the bar limit", marked.error.find("65535") != std::string::npos);
        const auto unmarked = importOf(smf(1, {cat({ev(0, {0x90, 60, 100}), ev(300000, {0x80, 60, 0}), kEot})}));
        TEST("6.3 unmarked import: 75000-bar section rejected (was clamped)", !unmarked.success);
        const auto ok = importOf(smf(1, {cat({marker(0, "Main A"), ev(0, {0x90, 60, 100}),
                                              ev(4 * 65535, {0x80, 60, 0}), kEot})}));
        TEST("6.3 exactly 65535 bars still imports", ok.success);

        uasf::UasfSerializer w;
        auto big = arranger::buildDemoStyle(); big.sections[0].bars = 65536;
        TEST("6.3 serializer rejects bars > 65535 (was cast to uint16)", !w.serialize(big).success);
        auto res = arranger::buildDemoStyle(); res.sections[0].resolution = 65536;
        TEST("6.3 serializer rejects resolution > 65535", !w.serialize(res).success);
        auto edge = arranger::buildDemoStyle(); edge.sections[0].bars = 65535;
        TEST("6.3 serializer accepts 65535 bars", w.serialize(edge).success);
    }
    {   // 6.4 absolute-tick overflow: decoding stops before the wrap
        // 17 deltas of 0x0FFFFFFF (~4.56e9 ticks) on empty text metas, then a note.
        Bytes t = ev(0, {0x90, 60, 100});
        for (int i = 0; i < 17; ++i) t = cat({t, vlq(0x0FFFFFFF), {0xFF, 0x01, 0x00}});
        t = cat({t, ev(0, {0x90, 62, 100}), kEot});
        Sff1Reader r;
        const auto p = r.parseBuffer(smf(480, {t}), "overflow");
        TEST("6.4 overflow: parse survives", p.success);
        TEST("6.4 overflow: warned", warned(p.warnings, "too long"));
        bool sawLate = false, monotonic = true;
        uint32_t last = 0;
        for (const auto& s : p.sections) for (const auto& tr : s.tracks) for (const auto& e : tr.events) {
            if (e.data1 == 62) sawLate = true;
            if (e.tick < last) monotonic = false;
            last = e.tick;
        }
        TEST("6.4 overflow: no event past the wrap point (no wrapped tick)", !sawLate && monotonic);
    }
    {   // 6.5 markers merged from several MTrk chunks are ordered by tick
        // Track 1: "Main A" at 1920; track 2: "Intro A" at 0 (480 PPQN, 4/4 = 1920-tick bar).
        const Bytes t1 = cat({marker(1920, "Main A"), ev(0, {0x99, 36, 100}), ev(240, {0x89, 36, 0}),
                              ev(1680, {0xFF, 0x2F, 0x00})});
        const Bytes t2 = cat({marker(0, "Intro A"), ev(0, {0x99, 38, 100}), ev(240, {0x89, 38, 0}), kEot});
        Sff1Reader r;
        const auto p = r.parseBuffer(smf(480, {t1, t2}), "multitrack");
        TEST("6.5 markers sorted by tick", p.markers.size() == 2 && p.markers[0].name == "Intro A" &&
                                           p.markers[0].tick == 0 && p.markers[1].tick == 1920);
        Sff1ToUasfMapper m;
        const auto mr = m.map(p);
        const uasf::SectionDefinition* intro = nullptr;
        for (const auto& s : mr.style.sections) if (s.type == uasf::SectionType::Intro1) intro = &s;
        TEST("6.5 Intro A ends where Main A starts (1 bar, not overlapping it)",
             mr.success && intro && intro->bars == 1);
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
