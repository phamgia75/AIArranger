// UASF model — deterministic default initialization + byte-identical serialization.
//
// Root cause guarded here: the uasf model structs (TrackDefinition,
// ArticulationMetadata, PlaybackRecommendation, SectionDefinition,
// StyleDefinition) hold std::string/std::vector members, so the usual
// `uasf::TrackDefinition track;` is default-initialization and every scalar
// member without an initializer was indeterminate. The SFF1 mapper never sets
// `articulation.recommended.*` or `has_keyswitch`, so UasfSerializer read
// garbage bools (UBSan: serializer.cpp "load of value 165 ... type 'bool'") and
// wrote heap-dependent playback_flags (the committed corpus .uasf fixtures carry
// flags=0x0F on every track). The types now carry documented member defaults.

#include "engine/uasf/types.h"
#include "engine/uasf/serializer.h"
#include "engine/uasf/deserializer.h"
#include "engine/uasf/format.h"
#include "importers/sff1/sff1_reader.h"
#include "importers/sff1/sff1_mapper.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <string>
#include <vector>

using namespace ai_arranger;
using namespace ai_arranger::uasf;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

#ifndef CORPUS_DIR
#define CORPUS_DIR "."
#endif

// Default-initialize T over storage pre-filled with `poison`, so a member with
// no initializer shows the poison byte instead of whatever happened to be there.
template <typename T, typename F>
static bool checkDefaults(unsigned char poison, F&& check) {
    alignas(T) unsigned char buf[sizeof(T)];
    std::memset(buf, poison, sizeof(buf));
    T* p = ::new (static_cast<void*>(buf)) T;   // default-init, NOT value-init
    const bool ok = check(*p);
    p->~T();
    return ok;
}

static bool recommendationIsDefault(const PlaybackRecommendation& r) {
    return !r.external_yamaha && !r.external_midi && !r.premium_library &&
           !r.user_soundfont && !r.degradation_allowed;
}

static bool articulationIsDefault(const ArticulationMetadata& a) {
    return a.profile == ArticulationProfile::Generic &&
           a.fidelity == FidelityRequirement::Low &&
           recommendationIsDefault(a.recommended) &&
           a.ntr == 0 && a.ntt == 0 && !a.has_keyswitch && a.keyswitch_note == 0;
}

// Scrub freed heap with `poison` so indeterminate members of objects allocated
// next are likely to differ between runs.
static void scrubHeap(unsigned char poison) {
    std::vector<void*> blocks;
    for (size_t sz : {32u, 64u, 96u, 128u, 192u, 256u, 512u, 1024u, 4096u})
        for (int i = 0; i < 256; ++i) {
            void* b = std::malloc(sz);
            if (b) { std::memset(b, poison, sz); blocks.push_back(b); }
        }
    for (void* b : blocks) std::free(b);
}

static std::string findCorpusSty(const std::string& needle) {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(CORPUS_DIR, ec)) {
        const std::string fn = e.path().filename().string();
        if (fn.find(needle) != std::string::npos && fn.size() > 4 &&
            fn.substr(fn.size() - 4) == ".sty")
            return e.path().string();
    }
    return {};
}

static std::vector<uint8_t> importAndSerialize(const std::string& sty) {
    importers::sff1::Sff1Reader reader;
    auto pr = reader.parseFile(sty);
    if (!pr.success) return {};
    importers::sff1::Sff1ToUasfMapper mapper;
    auto mr = mapper.map(pr);
    if (!mr.success) return {};
    UasfSerializer ser;
    auto out = ser.serialize(mr.style);
    return out.data;
}

static std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Every TrackHeader.playback_flags in a serialized UASF buffer (empty on a
// malformed walk).
static std::vector<uint16_t> playbackFlags(const std::vector<uint8_t>& b) {
    std::vector<uint16_t> flags;
    size_t o = sizeof(format::FileHeader);
    if (b.size() < o) return {};
    format::FileHeader fh;
    std::memcpy(&fh, b.data(), sizeof(fh));
    for (uint32_t s = 0; s < fh.section_count; ++s) {
        if (o + sizeof(format::SectionHeader) > b.size()) return {};
        format::SectionHeader sh;
        std::memcpy(&sh, b.data() + o, sizeof(sh));
        o += sizeof(sh);
        for (uint32_t t = 0; t < sh.track_count; ++t) {
            if (o + sizeof(format::TrackHeader) > b.size()) return {};
            format::TrackHeader th;
            std::memcpy(&th, b.data() + o, sizeof(th));
            o += sizeof(th) + th.name_length +
                 size_t(th.event_count) * sizeof(format::MidiEventSerialized);
            flags.push_back(th.playback_flags);
        }
    }
    // v1.1: flagged extension blocks (e.g. the style tempo) follow the sections.
    if (fh.flags & format::kFlagExtensions) {
        while (o + sizeof(format::ExtensionHeader) <= b.size()) {
            format::ExtensionHeader eh;
            std::memcpy(&eh, b.data() + o, sizeof(eh));
            o += sizeof(eh) + eh.length;
        }
    }
    return o == b.size() ? flags : std::vector<uint16_t>{};
}

int main() {
    std::printf("Test: UASF deterministic default init + serialization\n");

    // ── 1. Documented member defaults, independent of prior memory contents ──
    for (unsigned char poison : {0x00, 0xA5, 0xFF}) {
        TEST("PlaybackRecommendation defaults: all false",
             (checkDefaults<PlaybackRecommendation>(poison, recommendationIsDefault)));
        TEST("ArticulationMetadata defaults: Generic/Low, no flags, ntr=ntt=0, no keyswitch",
             (checkDefaults<ArticulationMetadata>(poison, articulationIsDefault)));
        TEST("TrackDefinition defaults: ch 0, Accompaniment, not drum",
             (checkDefaults<TrackDefinition>(poison, [](const TrackDefinition& t) {
                 return t.midi_channel == 0 && t.role == TrackRole::Accompaniment &&
                        !t.is_drum && articulationIsDefault(t.articulation);
             })));
        TEST("SectionDefinition defaults: zero (validator rejects unset bars/resolution/meter)",
             (checkDefaults<SectionDefinition>(poison, [](const SectionDefinition& s) {
                 return s.type == SectionType::Intro1 && s.bars == 0 && s.resolution == 0 &&
                        s.beats_per_bar == 0 && s.beat_note == 0;
             })));
        TEST("StyleDefinition defaults: zero tempo/resolution/ottava",
             (checkDefaults<StyleDefinition>(poison, [](const StyleDefinition& st) {
                 return st.tempo_bpm == 0 && st.resolution == 0 &&
                        st.ottava_bass == 0 && st.ottava_chord == 0;
             })));
    }

    // ── 2. Real Genos imports serialize byte-identically across runs ──
    const char* needles[] = {"POP_ACOUSTIC_2", "CLASSIC_6_8", "BALLAD_FOLK", "C_WHISPER"};
    int found = 0;
    for (const char* needle : needles) {
        const std::string sty = findCorpusSty(needle);
        if (sty.empty()) continue;
        ++found;
        scrubHeap(0xA5);
        const auto a = importAndSerialize(sty);
        scrubHeap(0x5A);
        const auto b = importAndSerialize(sty);
        const auto c = importAndSerialize(sty);
        const std::string tag = needle;
        TEST((tag + ": import+serialize succeeds").c_str(), !a.empty());
        TEST((tag + ": byte-identical across 3 runs (heap scrubbed between)").c_str(),
             a == b && b == c);
        const auto flags = playbackFlags(a);
        bool allZero = !flags.empty();
        for (uint16_t f : flags) allZero = allZero && f == 0;
        TEST((tag + ": importer sets no playback_flags (none are known for SFF1)").c_str(),
             allZero);

        UasfDeserializer des;
        const auto r = des.deserialize(a);
        bool meta = r.success;
        for (const auto& s : r.style.sections)
            for (const auto& t : s.tracks)
                meta = meta && recommendationIsDefault(t.articulation.recommended) &&
                       !t.articulation.has_keyswitch;
        TEST((tag + ": round-tripped tracks carry default playback/keyswitch metadata").c_str(),
             meta);
    }
    // Corpus is committed (IMPORTER_GOLDEN_CORPUS_POLICY): missing = hard failure.
    TEST("all 4 committed Genos fixtures found", found == 4);

    // ── 3. Committed importer fixtures equal a fresh deterministic import ──
    const std::pair<const char*, const char*> fixtures[] = {
        {"POP_ACOUSTIC_2", "/POP_ACOUSTIC_2.uasf"},
        {"CLASSIC_6_8", "/CLASSIC_6_8.uasf"},
    };
    for (const auto& [needle, file] : fixtures) {
        const auto committed = readFile(std::string(CORPUS_DIR) + file);
        const auto fresh = importAndSerialize(findCorpusSty(needle));
        const std::string tag = needle;
        TEST((tag + ".uasf: committed fixture == fresh import (no garbage bytes)").c_str(),
             !committed.empty() && committed == fresh);
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
