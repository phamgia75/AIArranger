// Gate 9 bugfix — Yamaha .sty -> .uasf note-preservation roundtrip.
//
// Regression for: a converted Yamaha style played silent (sent=0). The note
// events survived serialization, but the SMF division (PPQN) was misread
// (little-endian instead of SMF big-endian) and the mapper hardcoded the UASF
// resolution to 480, so the playback clock ran the 1920-PPQN events at the
// wrong tempo — effectively silent.
//
// This test loads a real corpus .sty through the full pipeline
// (reader -> mapper -> serializer -> deserializer -> StylePlayer) and asserts
// the notes are preserved AND the resolution matches the source division so
// playback timing is correct.
//
// If the corpus is not present, the test soft-skips (CI without proprietary
// styles still passes).

#include "importers/sff1/sff1_reader.h"
#include "importers/sff1/sff1_mapper.h"
#include "engine/uasf/serializer.h"
#include "engine/uasf/deserializer.h"
#include "engine/arranger/style_player.h"
#include "engine/realtime/clock.h"
#include "engine/midi/scheduler.h"
#include "engine/midi/panic.h"
#include <cstdint>
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
    if (!(expr)) { std::fprintf(stderr, "  ❌ FAIL: %s\n", name); failures++; } \
    else { std::printf("  ✅ PASS: %s\n", name); passes++; } \
} while(0)

#ifndef CORPUS_DIR
#define CORPUS_DIR "."
#endif

// Find a corpus .sty whose filename contains `needle` (UUID suffix varies).
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

static int countNoteEvents(const uasf::StyleDefinition& s) {
    int n = 0;
    for (const auto& sec : s.sections)
        for (const auto& t : sec.tracks)
            for (const auto& e : t.events)
                if (e.type == uasf::MidiEventType::NoteOn ||
                    e.type == uasf::MidiEventType::NoteOff) n++;
    return n;
}

int main() {
    if (const int rc = genos_corpus::require("test_sty_to_uasf_roundtrip"); rc >= 0) return rc;
    std::printf("Test: .sty -> .uasf note-preservation roundtrip\n");

    const std::string sty = findCorpusSty("POP_ACOUSTIC_2");
    if (sty.empty()) {
        // The corpus is configured (genos_corpus::require), so a missing style is a failure, not a skip.
        std::fprintf(stderr, "  FAIL: no POP_ACOUSTIC_2 corpus .sty under %s\n", CORPUS_DIR);
        return 1;
    }
    std::printf("  corpus: %s\n", sty.c_str());

    // ── Reader ───────────────────────────────────────────────────────
    Sff1Reader reader;
    auto pr = reader.parseFile(sty);
    TEST("Reader succeeds", pr.success);
    TEST("Reader has source MIDI events", pr.parsed_events > 0);

    // SMF division must be read big-endian: these Genos styles are 1920 PPQN.
    uint32_t readerRes = 0;
    for (const auto& s : pr.sections)
        if (s.resolution > readerRes) readerRes = s.resolution;
    TEST("Reader division parsed (>= 96 PPQN, not garbage)", readerRes >= 96);
    TEST("Reader division == 1920 (SMF big-endian)", readerRes == 1920);

    // ── Mapper -> Serialize -> Deserialize ───────────────────────────
    Sff1ToUasfMapper mapper;
    auto mr = mapper.map(pr);
    TEST("Mapper succeeds", mr.success);
    TEST("Mapper preserves note events", countNoteEvents(mr.style) > 0);

    uasf::UasfSerializer ser;
    auto sr = ser.serialize(mr.style);
    TEST("Serializer succeeds", sr.success);

    uasf::UasfDeserializer de;
    auto dr = de.deserialize(sr.data);
    TEST("Deserializer succeeds", dr.success);

    const int notes = countNoteEvents(dr.style);
    TEST("Roundtrip preserves note events (> 0)", notes > 0);   // PRIMARY

    // Resolution must survive to the playable style and match the source
    // division, or the playback clock runs at the wrong tempo (the bug).
    TEST("UASF resolution matches source division (1920)",
         dr.style.resolution == readerRes && dr.style.resolution == 1920);

    // ── Playback dispatch ────────────────────────────────────────────
    int dispatched = 0;
    std::set<int> channels;
    {
        realtime::RealtimeClock clk;
        midi::MidiScheduler sch;
        midi::PanicHandler pn;
        arranger::StylePlayer pl(clk, sch, pn);
        sch.setOutputCallback([&](const uasf::MidiEvent& e) {
            if (e.type == uasf::MidiEventType::NoteOn ||
                e.type == uasf::MidiEventType::NoteOff) {
                dispatched++;
                channels.insert(e.channel);
            }
        });
        uint64_t maxTick = 0;
        for (const auto& sec : dr.style.sections)
            for (const auto& t : sec.tracks)
                for (const auto& e : t.events) if (e.tick > maxTick) maxTick = e.tick;

        pl.loadStyle(dr.style);
        clk.setSampleRate(48000);
        clk.setTempo(dr.style.tempo_bpm ? dr.style.tempo_bpm : 120);
        clk.setResolution(dr.style.resolution ? dr.style.resolution : 480);
        pl.start(0);
        clk.start();
        const int64_t target = static_cast<int64_t>(maxTick) + 8192;
        for (int i = 0; i < 2000000 && clk.getPosition() < target; ++i) {
            clk.advance(2048);
            pl.tick();
            sch.advanceTo(clk.getPosition());
        }
    }
    TEST("Playback dispatches note events (sent > 0)", dispatched > 0);
    TEST("Playback covers multiple instrument channels", channels.size() >= 2);
    TEST("Drum channel (9) present in playback", channels.count(9) > 0);

    std::printf("\n  [counts] reader_events=%u readerRes=%u roundtrip_notes=%d dispatched=%d channels=%zu\n",
                pr.parsed_events, readerRes, notes, dispatched, channels.size());
    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}
