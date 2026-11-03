// UASF serializer — delta encoding must survive unsorted tracks and the event cap.
//
// tick_delta is an unsigned uint32 "ticks from previous event". The serializer
// wrote events in container order and computed tick - last_tick in uint64: an
// event earlier than its predecessor underflowed and was clamped to 0xFFFFFFFF,
// so every later event of that track came back ~4.29e9 ticks late (the demo
// style's drum tracks are stored per instrument, not by time — after a save /
// load its drums never played again; tests/golden/uasf/gate-3-demo-v1.uasf shows
// drum ticks up to 34,359,755,520). Separately, event_count was clamped to
// kMaxEvents but every event was still written, corrupting the rest of the file.

#include "engine/uasf/serializer.h"
#include "engine/uasf/deserializer.h"
#include "engine/uasf/format.h"
#include "engine/arranger/style_player.h"
#include <algorithm>
#include <cstdio>
#include <tuple>
#include <vector>

using namespace ai_arranger;
using namespace ai_arranger::uasf;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

using Key = std::tuple<uint64_t, int, int, int, int>;
static std::vector<Key> keys(const TrackDefinition& t) {
    std::vector<Key> k;
    for (const auto& e : t.events)
        k.emplace_back(e.tick, static_cast<int>(e.type), e.channel, e.data1, e.data2);
    std::sort(k.begin(), k.end());
    return k;
}

int main() {
    std::printf("Test: UASF round-trip of unsorted tracks + event cap\n");

    // ── 1. Demo style (unsorted drum tracks) round-trips every event exactly ──
    {
        const StyleDefinition style = arranger::buildDemoStyle();
        bool anyUnsorted = false;
        for (const auto& s : style.sections)
            for (const auto& t : s.tracks)
                for (size_t i = 1; i < t.events.size(); ++i)
                    if (t.events[i].tick < t.events[i - 1].tick) anyUnsorted = true;
        TEST("fixture: demo style really has an unsorted track", anyUnsorted);

        UasfSerializer ser;
        const auto bytes = ser.serialize(style);
        UasfDeserializer des;
        const auto r = des.deserialize(bytes.data);
        bool same = r.success && r.style.sections.size() == style.sections.size();
        for (size_t s = 0; same && s < style.sections.size(); ++s) {
            const auto& a = style.sections[s].tracks;
            const auto& b = r.style.sections[s].tracks;
            same = a.size() == b.size();
            for (size_t t = 0; same && t < a.size(); ++t) same = keys(a[t]) == keys(b[t]);
        }
        TEST("demo style: every (tick, event) survives serialize -> deserialize", same);

        bool sorted = r.success;
        for (const auto& s : r.style.sections)
            for (const auto& t : s.tracks)
                for (size_t i = 1; i < t.events.size(); ++i)
                    if (t.events[i].tick < t.events[i - 1].tick) sorted = false;
        TEST("demo style: decoded tracks are in non-decreasing tick order", sorted);
    }

    // ── 2. Same-tick events keep their relative order (stable) ─────────────
    {
        StyleDefinition st; st.resolution = 480;
        SectionDefinition sec; sec.resolution = 480; sec.bars = 1;
        TrackDefinition tr; tr.midi_channel = 0;
        auto ev = [](uint64_t tick, MidiEventType ty, uint8_t d1) {
            MidiEvent e; e.tick = tick; e.type = ty; e.channel = 0; e.data1 = d1; e.data2 = 64; return e;
        };
        tr.events = {ev(480, MidiEventType::NoteOff, 60), ev(480, MidiEventType::NoteOn, 60),
                     ev(0, MidiEventType::NoteOn, 60)};
        sec.tracks.push_back(tr); st.sections.push_back(sec);
        UasfSerializer ser; UasfDeserializer des;
        const auto r = des.deserialize(ser.serialize(st).data);
        const auto& out = r.style.sections[0].tracks[0].events;
        TEST("stable: NoteOn@0, then NoteOff@480 before NoteOn@480",
             r.success && out.size() == 3 && out[0].tick == 0 &&
             out[1].tick == 480 && out[1].type == MidiEventType::NoteOff &&
             out[2].tick == 480 && out[2].type == MidiEventType::NoteOn);
    }

    // ── 3. A track above kMaxEvents is truncated, not corrupting the file ──
    {
        StyleDefinition st; st.resolution = 480;
        SectionDefinition sec; sec.resolution = 480; sec.bars = 1;
        TrackDefinition big; big.midi_channel = 1;
        for (uint32_t i = 0; i < format::kMaxEvents + 10; ++i) {
            MidiEvent e; e.tick = i; e.type = MidiEventType::NoteOn; e.channel = 1; e.data1 = 60; e.data2 = 1;
            big.events.push_back(e);
        }
        TrackDefinition after; after.midi_channel = 2; after.name = "after";
        MidiEvent e; e.tick = 7; e.type = MidiEventType::NoteOn; e.channel = 2; e.data1 = 64; e.data2 = 99;
        after.events.push_back(e);
        sec.tracks = {big, after}; st.sections.push_back(sec);
        UasfSerializer ser; UasfDeserializer des;
        const auto r = des.deserialize(ser.serialize(st).data);
        const bool ok = r.success && r.style.sections.size() == 1 &&
                        r.style.sections[0].tracks.size() == 2;
        TEST("cap: file with an over-cap track still parses", ok);
        TEST("cap: over-cap track truncated to exactly kMaxEvents",
             ok && r.style.sections[0].tracks[0].events.size() == format::kMaxEvents);
        TEST("cap: the following track is intact",
             ok && r.style.sections[0].tracks[1].name == "after" &&
             r.style.sections[0].tracks[1].events.size() == 1 &&
             r.style.sections[0].tracks[1].events[0].tick == 7 &&
             r.style.sections[0].tracks[1].events[0].data2 == 99);
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
