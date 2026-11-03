// UASF deserializer — structural limits from format.h are enforced on read.
//
// The serializer clamps to kMaxSections / kMaxTracksPerSection /
// kMaxTrackNameLength / kMaxEvents, so a file above any of them was not written
// by us and is corrupt or crafted. The deserializer used to accept any count:
// 12-byte track headers expand to ~170-byte TrackDefinitions, so a 10 MB file
// could allocate hundreds of MB inside a noexcept function. Limits are exact:
// the maximum is accepted, maximum + 1 is rejected with an error.

#include "engine/uasf/deserializer.h"
#include "engine/uasf/format.h"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace ai_arranger::uasf;
namespace fmt = ai_arranger::uasf::format;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

template <class T> static void put(std::vector<uint8_t>& b, const T& v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(T));
}
static void fileHeader(std::vector<uint8_t>& b, uint32_t sections) {
    fmt::FileHeader h{}; h.magic = fmt::kMagic; h.version = fmt::kVersion; h.section_count = sections;
    put(b, h);
}
static void sectionHeader(std::vector<uint8_t>& b, uint8_t tracks) {
    fmt::SectionHeader s{}; s.bars = 4; s.resolution = 480; s.beats_per_bar = 4; s.beat_note = 4;
    s.track_count = tracks;
    put(b, s);
}
static void track(std::vector<uint8_t>& b, uint8_t nameLen, uint32_t events) {
    fmt::TrackHeader t{}; t.name_length = nameLen; t.event_count = events;
    put(b, t);
    b.insert(b.end(), nameLen, 'n');
    fmt::MidiEventSerialized ev{}; ev.type_channel = 0x90; ev.data1 = 60; ev.data2 = 100;
    for (uint32_t i = 0; i < events; ++i) put(b, ev);
}

static bool parses(const std::vector<uint8_t>& b) {
    UasfDeserializer d;
    return d.deserialize(b).success;
}

int main() {
    std::printf("Test: UASF deserializer structural limits\n");

    auto sections = [](uint32_t n) {
        std::vector<uint8_t> b; fileHeader(b, n);
        for (uint32_t i = 0; i < n; ++i) sectionHeader(b, 0);
        return b;
    };
    TEST("kMaxSections sections accepted", parses(sections(fmt::kMaxSections)));
    TEST("kMaxSections + 1 sections rejected", !parses(sections(fmt::kMaxSections + 1)));

    auto tracks = [](uint32_t n) {
        std::vector<uint8_t> b; fileHeader(b, 1); sectionHeader(b, static_cast<uint8_t>(n));
        for (uint32_t i = 0; i < n; ++i) track(b, 0, 0);
        return b;
    };
    TEST("kMaxTracksPerSection tracks accepted", parses(tracks(fmt::kMaxTracksPerSection)));
    TEST("kMaxTracksPerSection + 1 tracks rejected", !parses(tracks(fmt::kMaxTracksPerSection + 1)));
    TEST("255 tracks (u8 max) rejected", !parses(tracks(255)));

    auto named = [](uint32_t len) {
        std::vector<uint8_t> b; fileHeader(b, 1); sectionHeader(b, 1);
        track(b, static_cast<uint8_t>(len), 0);
        return b;
    };
    TEST("kMaxTrackNameLength name accepted", parses(named(fmt::kMaxTrackNameLength)));
    TEST("kMaxTrackNameLength + 1 name rejected", !parses(named(fmt::kMaxTrackNameLength + 1)));

    auto events = [](uint32_t n) {
        std::vector<uint8_t> b; fileHeader(b, 1); sectionHeader(b, 1);
        track(b, 0, n);
        return b;
    };
    TEST("kMaxEvents events accepted", parses(events(fmt::kMaxEvents)));
    TEST("kMaxEvents + 1 events rejected", !parses(events(fmt::kMaxEvents + 1)));

    {
        UasfDeserializer d;
        const auto r = d.deserialize(sections(fmt::kMaxSections + 1));
        TEST("rejection carries a descriptive error", !r.success && !r.error.empty());
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
