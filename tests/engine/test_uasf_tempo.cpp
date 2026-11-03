// UASF style tempo persistence (Issue #2) — a backward-compatible v1.x extension.
//
// Layout (format.h): FileHeader.flags bit 0 (kFlagExtensions) says tagged
// blocks {u32 tag, u32 length, payload} follow the last section. The tempo is
// block 'TMPO' with a u32 whole-BPM payload. FileHeader.version stays 1, so a
// v1.0 reader (which ignores flags and trailing bytes) still loads new files at
// its 120 BPM default, and every existing file (flags 0, no trailer) still loads
// here at 120 BPM.

#include "engine/uasf/deserializer.h"
#include "engine/uasf/serializer.h"
#include "engine/arranger/style_player.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ai_arranger;
using namespace ai_arranger::uasf;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", std::string(name).c_str()); failures++; } \
    else { std::printf("  PASS: %s\n", std::string(name).c_str()); passes++; } \
} while(0)

using Bytes = std::vector<uint8_t>;

static StyleDefinition styleAt(uint32_t bpm) {
    auto s = arranger::buildDemoStyle();
    s.tempo_bpm = bpm;
    return s;
}
static Bytes ser(const StyleDefinition& s) { UasfSerializer w; return w.serialize(s).data; }
static DeserializeResult des(const Bytes& b) { UasfDeserializer r; return r.deserialize(b); }
static uint32_t u32At(const Bytes& b, size_t at) {
    uint32_t v; std::memcpy(&v, b.data() + at, 4); return v;
}
static void putU32(Bytes& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
// Bytes of a style with no tempo (flags 0, no trailer) plus the given trailer.
static Bytes withTrailer(const Bytes& trailer) {
    Bytes b = ser(styleAt(0));
    b[8] |= format::kFlagExtensions;
    b.insert(b.end(), trailer.begin(), trailer.end());
    return b;
}
static Bytes block(uint32_t tag, const Bytes& payload) {
    Bytes b; putU32(b, tag); putU32(b, static_cast<uint32_t>(payload.size()));
    b.insert(b.end(), payload.begin(), payload.end()); return b;
}
static Bytes u32Bytes(uint32_t v) { Bytes b; putU32(b, v); return b; }

int main() {
    std::printf("Test: UASF style tempo persistence\n");

    {   // 1-3. round trip: 90, 120, real corpus tempos, range boundaries
        for (uint32_t bpm : {90u, 120u, 76u, 100u, 105u, kMinTempoBpm, kMaxTempoBpm}) {
            const auto r = des(ser(styleAt(bpm)));
            TEST("round trip " + std::to_string(bpm) + " BPM", r.success && r.style.tempo_bpm == bpm);
        }
    }
    {   // Exact layout of a tempo-bearing file.
        const Bytes b = ser(styleAt(90));
        TEST("layout: version stays 1", u32At(b, 4) == 1);
        TEST("layout: flags = kFlagExtensions", u32At(b, 8) == format::kFlagExtensions);
        const size_t n = b.size();
        TEST("layout: file ends with TMPO block {'TMPO', 4, 90}",
             n >= 12 && std::memcmp(b.data() + n - 12, "TMPO", 4) == 0 &&
             u32At(b, n - 8) == 4 && u32At(b, n - 4) == 90);
        // Everything before the trailer is the unchanged v1 body.
        Bytes body(b.begin(), b.end() - 12);
        body[8] = 0;
        TEST("layout: body before the trailer == the tempo-less v1 encoding", body == ser(styleAt(0)));
    }
    {   // 4. legacy: no flag / no trailer -> 120 (the pre-tempo default)
        const Bytes legacy = ser(styleAt(0));
        TEST("tempo-less style writes flags 0 and no trailer", u32At(legacy, 8) == 0);
        const auto r = des(legacy);
        TEST("legacy file (flags 0) loads at 120 BPM", r.success && r.style.tempo_bpm == kDefaultTempoBpm);
        const auto g = UasfDeserializer().deserializeFromFile(
            std::string(REPO_DIR) + "/tests/golden/uasf/gate-3-demo-v1.uasf");
        TEST("committed v1 fixture gate-3-demo-v1.uasf loads at 120 BPM",
             g.success && g.style.tempo_bpm == kDefaultTempoBpm);
        // v1 readers ignored trailing bytes when flags is 0; so does this one.
        Bytes junk = legacy; junk.push_back(0xAB); junk.push_back(0xCD);
        const auto j = des(junk);
        TEST("flags 0: trailing bytes ignored as before", j.success && j.style.tempo_bpm == kDefaultTempoBpm);
    }
    {   // 5. committed tempo-aware corpus fixtures carry the imported tempo
        const auto pop = UasfDeserializer().deserializeFromFile(std::string(CORPUS_DIR) + "/POP_ACOUSTIC_2.uasf");
        const auto cls = UasfDeserializer().deserializeFromFile(std::string(CORPUS_DIR) + "/CLASSIC_6_8.uasf");
        TEST("POP_ACOUSTIC_2.uasf loads at 100 BPM", pop.success && pop.style.tempo_bpm == 100);
        TEST("CLASSIC_6_8.uasf loads at 90 BPM", cls.success && cls.style.tempo_bpm == 90);
    }
    {   // 6. determinism
        TEST("repeated serialization is byte-identical", ser(styleAt(105)) == ser(styleAt(105)));
        const auto r = des(ser(styleAt(105)));
        TEST("serialize(deserialize(x)) == x", r.success && ser(r.style) == ser(styleAt(105)));
    }
    {   // 7. malformed extension data is rejected, not misread
        TEST("flag set, no blocks: loads at 120", des(withTrailer({})).success &&
             des(withTrailer({})).style.tempo_bpm == kDefaultTempoBpm);
        TEST("truncated block header rejected", !des(withTrailer({'T', 'M', 'P', 'O'})).success);
        Bytes cut = block(format::kExtTempo, u32Bytes(90)); cut.pop_back();
        TEST("truncated TMPO payload rejected", !des(withTrailer(cut)).success);
        TEST("TMPO with length 2 rejected", !des(withTrailer(block(format::kExtTempo, {90, 0}))).success);
        TEST("TMPO 0 BPM rejected", !des(withTrailer(block(format::kExtTempo, u32Bytes(0)))).success);
        TEST("TMPO 19 BPM rejected", !des(withTrailer(block(format::kExtTempo, u32Bytes(19)))).success);
        TEST("TMPO 401 BPM rejected", !des(withTrailer(block(format::kExtTempo, u32Bytes(401)))).success);
        Bytes dup = block(format::kExtTempo, u32Bytes(90));
        const Bytes second = block(format::kExtTempo, u32Bytes(100));
        dup.insert(dup.end(), second.begin(), second.end());
        TEST("duplicate TMPO rejected", !des(withTrailer(dup)).success);
        // Forward compatibility: an unknown block is skipped by its length.
        Bytes fwd = block(0x5A5A5A5A, {1, 2, 3});
        const Bytes tmpo = block(format::kExtTempo, u32Bytes(76));
        fwd.insert(fwd.end(), tmpo.begin(), tmpo.end());
        const auto f = des(withTrailer(fwd));
        TEST("unknown block skipped, following TMPO read (76)", f.success && f.style.tempo_bpm == 76);
        Bytes overlong; putU32(overlong, 0x5A5A5A5A); putU32(overlong, 1000);   // no payload
        TEST("unknown block longer than the file rejected", !des(withTrailer(overlong)).success);
    }
    {   // 8. writer refuses a tempo the reader would reject
        UasfSerializer w;
        TEST("serialize rejects 401 BPM", !w.serialize(styleAt(401)).success);
        TEST("serialize rejects 19 BPM", !w.serialize(styleAt(19)).success);
    }
    {   // Writer caps sections/tracks to the header counts, so the trailer is
        // always where the reader looks for it.
        auto big = styleAt(90);
        while (big.sections.size() < format::kMaxSections + 1) big.sections.push_back(big.sections[0]);
        while (big.sections[0].tracks.size() < format::kMaxTracksPerSection + 1)
            big.sections[0].tracks.push_back(big.sections[0].tracks[0]);
        const auto r = des(ser(big));
        TEST("33 sections / 17 tracks: loads, capped to 32 / 16, tempo kept",
             r.success && r.style.sections.size() == format::kMaxSections &&
             r.style.sections[0].tracks.size() == format::kMaxTracksPerSection &&
             r.style.tempo_bpm == 90);
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
