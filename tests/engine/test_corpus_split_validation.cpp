// Gate 10B Task F — per-channel/per-role split validation across the corpus.
//
// Task A is verified end-to-end on POP_ACOUSTIC_2; this test extends the
// guarantee to EVERY style under corpus/yamaha/sff1, asserting the structural
// split invariants hold (>=4 per-role tracks, bass + chord + a rhythm track,
// each track single-channel). Soft-skips if the proprietary corpus is absent.

#include "importers/sff1/sff1_reader.h"
#include "importers/sff1/sff1_mapper.h"
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include "../support/genos_corpus.h"

using namespace ai_arranger;
using namespace ai_arranger::importers::sff1;

static int failures = 0;
static int passes = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  \xE2\x9D\x8C FAIL: %s\n", name); failures++; } \
    else { std::printf("  \xE2\x9C\x85 PASS: %s\n", name); passes++; } \
} while(0)

#ifndef CORPUS_DIR
#define CORPUS_DIR "."
#endif

int main() {
    if (const int rc = genos_corpus::require("test_corpus_split_validation"); rc >= 0) return rc;
    std::printf("Test: Gate 10B Task F — corpus-wide split validation\n");
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!fs::exists(CORPUS_DIR, ec)) {
        std::printf("  \xE2\x84\xB9\xEF\xB8\x8F  SKIP: no corpus dir %s\n", CORPUS_DIR);
        std::printf("\nResults: %d passed, %d failed\n", passes, failures);
        return 0;
    }

    int styles = 0;
    for (const auto& e : fs::directory_iterator(CORPUS_DIR, ec)) {
        if (ec) break;
        const std::string fn = e.path().filename().string();
        if (fn.size() < 4 || fn.substr(fn.size() - 4) != ".sty") continue;
        ++styles;

        Sff1Reader reader;
        auto pr = reader.parseFile(e.path().string());
        Sff1ToUasfMapper mapper;
        auto mr = mapper.map(pr);

        const std::string tag = fn.substr(0, 24);
        char name[160];

        std::snprintf(name, sizeof(name), "[%s] mapper succeeds", tag.c_str());
        TEST(name, mr.success);

        size_t maxTracks = 0;
        std::set<uasf::TrackRole> roles;
        bool singleChannel = true;
        for (const auto& sec : mr.style.sections) {
            if (sec.tracks.size() > maxTracks) maxTracks = sec.tracks.size();
            for (const auto& t : sec.tracks) {
                roles.insert(t.role);
                for (const auto& ev : t.events)
                    if (ev.channel != t.midi_channel) singleChannel = false;
            }
        }

        std::snprintf(name, sizeof(name), "[%s] >=4 per-role tracks", tag.c_str());
        TEST(name, maxTracks >= 4);

        std::snprintf(name, sizeof(name), "[%s] each track single-channel", tag.c_str());
        TEST(name, singleChannel);

        std::snprintf(name, sizeof(name), "[%s] bass track present", tag.c_str());
        TEST(name, roles.count(uasf::TrackRole::Bass) > 0);

        std::snprintf(name, sizeof(name), "[%s] chord track present", tag.c_str());
        TEST(name, roles.count(uasf::TrackRole::Chord) > 0);

        std::snprintf(name, sizeof(name), "[%s] rhythm track present (drum|perc)", tag.c_str());
        TEST(name, roles.count(uasf::TrackRole::Drum) > 0 ||
                   roles.count(uasf::TrackRole::Percussion) > 0);
    }

    if (styles == 0) {
        std::printf("  \xE2\x84\xB9\xEF\xB8\x8F  SKIP: no .sty styles under %s\n", CORPUS_DIR);
    } else {
        std::printf("\n  validated %d corpus styles\n", styles);
        TEST("at least one corpus style validated", styles >= 1);
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}
