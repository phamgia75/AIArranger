// Gate 10 Task A — per-channel / per-role track splitting.
//
// PR #7 left the whole style collapsed into a single mixed-channel `Phrase1`
// track. Chord transpose was then applied uniformly (even to drums) and the
// per-channel NoteOn dedupe collapsed unrelated voices, dropping ~30% of
// retriggers at dispatch (4876 source note events -> 3384 dispatched).
//
// Task A splits the mixed SMF MTrk back into one UASF track per MIDI channel,
// resolving each channel's role from the CASM Ctb2 metadata (NOT a channel
// heuristic — drums are not always on GM channel 9; POP_ACOUSTIC_2 puts
// Rhythm1 on channel 8). This test asserts the structural split and that the
// dispatched NoteOn/NoteOff count recovers above the PR #7 baseline.
//
// Soft-skips if the proprietary corpus is absent.

#include "importers/sff1/sff1_reader.h"
#include "importers/sff1/sff1_mapper.h"
#include "engine/uasf/serializer.h"
#include "engine/uasf/deserializer.h"
#include "engine/arranger/style_player.h"
#include "engine/realtime/clock.h"
#include "engine/midi/scheduler.h"
#include "engine/midi/panic.h"
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>

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

static std::string findCorpusSty(const std::string& needle) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(CORPUS_DIR, ec)) return {};
    for (const auto& e : fs::directory_iterator(CORPUS_DIR, ec)) {
        if (ec) break;
        const std::string fn = e.path().filename().string();
        if (fn.find(needle) != std::string::npos &&
            fn.size() >= 4 && fn.substr(fn.size() - 4) == ".sty") {
            return e.path().string();
        }
    }
    return {};
}

int main() {
    std::printf("Test: Gate 10 Task A — per-channel / per-role splitting\n");

    const std::string sty = findCorpusSty("POP_ACOUSTIC_2");
    if (sty.empty()) {
        std::printf("  \xE2\x84\xB9\xEF\xB8\x8F  SKIP: no POP_ACOUSTIC_2 corpus .sty under %s\n", CORPUS_DIR);
        std::printf("\nResults: %d passed, %d failed\n", passes, failures);
        return 0;
    }
    std::printf("  corpus: %s\n", sty.c_str());

    Sff1Reader reader;
    auto pr = reader.parseFile(sty);
    TEST("Reader succeeds", pr.success);

    Sff1ToUasfMapper mapper;
    auto mr = mapper.map(pr);
    TEST("Mapper succeeds", mr.success);
    TEST("Mapper produced at least one section", !mr.style.sections.empty());

    // ── Structural split ─────────────────────────────────────────────
    // The single SMF section must now expose multiple per-role tracks.
    size_t maxTracks = 0;
    std::set<uasf::TrackRole> roles;
    bool eachTrackSingleChannel = true;
    for (const auto& sec : mr.style.sections) {
        if (sec.tracks.size() > maxTracks) maxTracks = sec.tracks.size();
        for (const auto& t : sec.tracks) {
            roles.insert(t.role);
            for (const auto& e : t.events) {
                if (e.channel != t.midi_channel) eachTrackSingleChannel = false;
            }
        }
    }
    TEST("At least 4 tracks after split (drum/bass/chord/phrase)", maxTracks >= 4);
    TEST("Each split track holds exactly one MIDI channel", eachTrackSingleChannel);
    TEST("Drum role present (CASM Rhythm1, ch 8)",
         roles.count(uasf::TrackRole::Drum) > 0);
    TEST("Bass role present (CASM Bass, ch 10)",
         roles.count(uasf::TrackRole::Bass) > 0);
    TEST("Chord role present (CASM Chord1)",
         roles.count(uasf::TrackRole::Chord) > 0);

    // ── Dispatch recovery ────────────────────────────────────────────
    // Roundtrip through serialize/deserialize, then play and count dispatched
    // note events. PR #7 baseline (single mixed track) dispatched 3384; the
    // split must strictly improve on that (drums no longer chord-transposed).
    uasf::UasfSerializer ser;
    auto sr = ser.serialize(mr.style);
    TEST("Serializer succeeds", sr.success);
    uasf::UasfDeserializer de;
    auto dr = de.deserialize(sr.data);
    TEST("Deserializer succeeds", dr.success);

    // The import is split into its marker sections (Intro A, Main A-D, fills,
    // break, ending), and sections now loop / hand over at their length. Play
    // every section exactly once for its own length and sum the dispatches —
    // the same "whole style once" coverage the single-section import had.
    int dispatched = 0;
    for (size_t si = 0; si < dr.style.sections.size(); ++si) {
        realtime::RealtimeClock clk;
        midi::MidiScheduler sch;
        midi::PanicHandler pn;
        arranger::StylePlayer pl(clk, sch, pn);
        sch.setOutputCallback([&](const uasf::MidiEvent& e) {
            if (e.type == uasf::MidiEventType::NoteOn ||
                e.type == uasf::MidiEventType::NoteOff) dispatched++;
        });
        pl.loadStyle(dr.style);
        clk.setSampleRate(48000);
        clk.setTempo(dr.style.tempo_bpm ? dr.style.tempo_bpm : 120);
        clk.setResolution(dr.style.resolution ? dr.style.resolution : 480);
        pl.start(static_cast<int>(si));
        clk.start();
        const int64_t target =
            static_cast<int64_t>(dr.style.sections[si].bars) * clk.ticksPerBar() - 1;
        for (int i = 0; i < 2000000 && clk.isRunning() && clk.getPosition() < target; ++i) {
            clk.advance(256);
            pl.tick();
            sch.advanceTo(clk.getPosition());
        }
        pl.stop();
        sch.advanceTo(INT64_MAX);
    }
    // Strict improvement over the PR #7 single-track baseline of 3384.
    TEST("Dispatched note events recover above PR #7 baseline (3384)",
         dispatched > 3384);

    std::printf("\n  [counts] tracks=%zu roles=%zu dispatched=%d (PR#7 baseline 3384)\n",
                maxTracks, roles.size(), dispatched);
    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}
