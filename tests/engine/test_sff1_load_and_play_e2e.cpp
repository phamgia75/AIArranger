// Gate 2 — SFF1 .sty → importer → StyleDefinition → StylePlayer → MIDI events,
// end-to-end, headless and deterministic, over the real Genos corpus.
//
// Scope: this asserts behaviour ONLY on the 4 Genos SFF1 fixtures committed under
// corpus/yamaha/sff1 (POP_ACOUSTIC_2, CLASSIC_6_8, BALLAD_FOLK, C_WHISPER). It is
// NOT a general Yamaha/Genos/SFF1 compatibility claim — see
// docs/PROJECT_REVIEW_AND_CONTINUATION_PLAN.md hardware discipline.
//
// For each fixture it proves the whole load-and-play loop:
//   Sff1Reader.parseFile → Sff1ToUasfMapper.map → StyleDefinition
//     → StylePlayer.loadStyle → start → setChord → tick → scheduler dispatch
// and asserts:
//   - reader + mapper succeed, note events survive, PPQN division is preserved
//   - playback dispatches note AND non-note (CC/program) events on ≥2 channels
//   - a drum-role track is voiced (drums are not always MIDI channel 9 here)
//   - STOP flushes every sounding note → NoteOn/NoteOff balance is exactly 0
//     (the no-stuck-notes invariant)
//   - playback is deterministic (same style+chord → byte-identical event stream)
//   - the chord input actually drives the output (a different chord transposes
//     the pitched tracks → a different event stream)
//
// These 4 fixtures are a COMMITTED acceptance corpus (corpus/yamaha/sff1, see
// docs/governance/IMPORTER_GOLDEN_CORPUS_POLICY.md — "Every importer change must
// pass all golden files before merge"). A missing fixture or a wrong CORPUS_DIR
// therefore FAILS the test (return non-zero); it must never silently soft-skip
// to green, or a broken checkout / renamed corpus would hide a real regression.

#include "importers/sff1/sff1_reader.h"
#include "importers/sff1/sff1_mapper.h"
#include "engine/arranger/style_player.h"
#include "engine/arranger/chord_input.h"
#include <algorithm>
#include "engine/realtime/clock.h"
#include "engine/midi/scheduler.h"
#include "engine/midi/panic.h"
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

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

// ── Captured playback result ─────────────────────────────────────────────────
struct Captured {
    long noteOn = 0;    // NoteOn with velocity > 0
    long noteOff = 0;   // NoteOff, or NoteOn velocity 0
    long cc = 0;        // ControlChange
    long other = 0;     // ProgramChange / pitchbend / etc.
    int  balance = 0;   // +1 per sounding NoteOn, -1 per release — 0 == balanced
    std::set<int> channels;             // channels that carried note events
    std::set<int> harmonicChannels;     // note-event channels that are NOT drums
    uint64_t noteStreamHash = 1469598103934665603ull;  // FNV-1a over every note
    uint64_t harmonicHash   = 1469598103934665603ull;  // FNV-1a over pitched notes
    int bassMin = 128, bassMax = -1;    // note-on range on Bass-role channels
};

static void fnvMix(uint64_t& h, uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
}

static uint64_t maxEventTick(const uasf::StyleDefinition& s) {
    uint64_t m = 0;
    for (const auto& sec : s.sections)
        for (const auto& t : sec.tracks)
            for (const auto& e : t.events)
                if (e.tick > m) m = e.tick;
    return m;
}

static std::set<int> drumChannels(const uasf::StyleDefinition& s) {
    std::set<int> d;
    for (const auto& sec : s.sections)
        for (const auto& t : sec.tracks)
            if (t.role == uasf::TrackRole::Drum ||
                t.role == uasf::TrackRole::Percussion)
                d.insert(t.midi_channel);
    return d;
}

// Load the style into a fresh engine graph, start, apply `chord`, run past the
// end of the material, then STOP (which flushes sounding notes). Captures every
// dispatched event via the scheduler output callback.
static Captured playAndCapture(const uasf::StyleDefinition& style,
                               const arranger::Chord& chord) {
    const std::set<int> drums = drumChannels(style);
    std::set<int> bass;
    for (const auto& sec : style.sections)
        for (const auto& t : sec.tracks)
            if (t.role == uasf::TrackRole::Bass) bass.insert(t.midi_channel);
    Captured cap;

    realtime::RealtimeClock clk;
    midi::MidiScheduler      sch;
    midi::PanicHandler       pn;
    arranger::StylePlayer    pl(clk, sch, pn);

    sch.setOutputCallback([&](const uasf::MidiEvent& e) {
        const bool isOn  = e.type == uasf::MidiEventType::NoteOn && e.data2 > 0;
        const bool isOff = e.type == uasf::MidiEventType::NoteOff ||
                           (e.type == uasf::MidiEventType::NoteOn && e.data2 == 0);
        if (isOn || isOff) {
            cap.channels.insert(e.channel);
            const bool isDrum = drums.count(e.channel) > 0;
            if (!isDrum) cap.harmonicChannels.insert(e.channel);
            // Order-sensitive hash of the whole note stream (proves determinism).
            const uint64_t packed = (static_cast<uint64_t>(e.channel) << 24) |
                                    (static_cast<uint64_t>(e.data1) << 16) |
                                    (static_cast<uint64_t>(e.data2) << 8)  |
                                    (isOn ? 1u : 0u);
            fnvMix(cap.noteStreamHash, packed);
            if (!isDrum) fnvMix(cap.harmonicHash, packed);
            if (isOn && bass.count(e.channel)) {
                cap.bassMin = std::min<int>(cap.bassMin, e.data1);
                cap.bassMax = std::max<int>(cap.bassMax, e.data1);
            }
            if (isOn)  { ++cap.noteOn;  ++cap.balance; }
            else       { ++cap.noteOff; --cap.balance; }
        } else if (e.type == uasf::MidiEventType::ControlChange) {
            ++cap.cc;
        } else {
            ++cap.other;
        }
    });

    const uint64_t maxTick = maxEventTick(style);

    clk.setSampleRate(48000);
    clk.setTempo(style.tempo_bpm ? style.tempo_bpm : 120);
    clk.setResolution(style.resolution ? style.resolution : 480);

    pl.loadStyle(style);
    pl.start(0);
    pl.setChord(chord);

    const int64_t target = static_cast<int64_t>(maxTick) + 8192;
    for (int i = 0; i < 3'000'000 && clk.getPosition() < target; ++i) {
        clk.advance(2048);
        pl.tick();
        sch.advanceTo(clk.getPosition());
    }

    // STOP flushes every note still sounding as a NoteOff (tick 0); the queue is
    // already drained of section events (we ran past maxTick), so one more
    // advanceTo dispatches those flush note-offs.
    pl.stop();
    sch.advanceTo(clk.getPosition());

    return cap;
}

static void runFixture(const std::string& label, const std::string& needle) {
    std::printf("\n── Fixture: %s ──\n", label.c_str());
    const std::string sty = findCorpusSty(needle);
    if (sty.empty()) {
        std::printf("  ℹ️  SKIP: no %s corpus .sty under %s\n", needle.c_str(), CORPUS_DIR);
        return;
    }
    std::printf("  corpus: %s\n", sty.c_str());

    // ── Reader ──
    Sff1Reader reader;
    ParseResult pr = reader.parseFile(sty);
    TEST((label + ": reader succeeds").c_str(), pr.success);
    TEST((label + ": reader parsed source events").c_str(), pr.parsed_events > 0);

    uint32_t readerRes = 0;
    for (const auto& s : pr.sections)
        if (s.resolution > readerRes) readerRes = s.resolution;
    TEST((label + ": PPQN division sane (>= 96, not garbage)").c_str(), readerRes >= 96);

    // ── Mapper ──
    Sff1ToUasfMapper mapper;
    SffToUasfResult mr = mapper.map(pr);
    TEST((label + ": mapper succeeds").c_str(), mr.success);
    TEST((label + ": mapped style has sections").c_str(), !mr.style.sections.empty());

    long noteEvents = 0;
    for (const auto& sec : mr.style.sections)
        for (const auto& t : sec.tracks)
            for (const auto& e : t.events)
                if (e.type == uasf::MidiEventType::NoteOn ||
                    e.type == uasf::MidiEventType::NoteOff) ++noteEvents;
    TEST((label + ": mapper preserves note events").c_str(), noteEvents > 0);
    TEST((label + ": mapper preserves PPQN division").c_str(),
         mr.style.resolution == readerRes);

    const bool hasDrumRole = !drumChannels(mr.style).empty();
    TEST((label + ": style has a drum-role track").c_str(), hasDrumRole);

    // ── Play (C major default) ──
    const Captured c = playAndCapture(mr.style, arranger::chords::CMaj);
    TEST((label + ": playback dispatched note-ons").c_str(), c.noteOn > 0);
    TEST((label + ": playback covers >= 2 channels").c_str(), c.channels.size() >= 2);
    TEST((label + ": drum-role channel voiced in playback").c_str(),
         [&]{ for (int ch : drumChannels(mr.style)) if (c.channels.count(ch)) return true;
              return false; }());
    TEST((label + ": non-note events (CC/program) dispatched").c_str(),
         (c.cc + c.other) > 0);
    TEST((label + ": no stuck notes after STOP (NoteOn == NoteOff)").c_str(),
         c.balance == 0 && c.noteOn == c.noteOff);

    // ── Determinism: identical run → byte-identical event stream ──
    const Captured c2 = playAndCapture(mr.style, arranger::chords::CMaj);
    TEST((label + ": deterministic note stream (stable hash)").c_str(),
         c2.noteStreamHash == c.noteStreamHash && c2.noteOn == c.noteOn);

    // ── Chord input drives output: a fifth-up chord → different pitched stream ──
    const Captured g = playAndCapture(mr.style, arranger::chords::GMaj);
    TEST((label + ": chord input changes the pitched event stream").c_str(),
         !c.harmonicChannels.empty() && g.harmonicHash != c.harmonicHash);
    TEST((label + ": chord change keeps notes balanced (no stuck notes)").c_str(),
         g.balance == 0 && g.noteOn == g.noteOff);

    // ── The chord's octave never moves the pattern (Pa5X 2026-11-05) ──
    // A left-hand F2 and an F4 are the same chord: identical output, and the bass
    // stays in its recorded register instead of dropping ~1.5 octaves.
    const Captured f2 = playAndCapture(mr.style, {arranger::ChordType::Major, 41, 0, 0});
    const Captured f4 = playAndCapture(mr.style, {arranger::ChordType::Major, 65, 0, 0});
    TEST((label + ": F2 and F4 chords give an identical note stream").c_str(),
         f2.noteStreamHash == f4.noteStreamHash && f2.noteOn == f4.noteOn);
    if (c.bassMax >= 0)
        TEST((label + ": bass over F2 stays within C-chord bass range +0..11").c_str(),
             f2.bassMin >= c.bassMin - 2 && f2.bassMax <= c.bassMax + 11);

    std::printf("  [bass range] C:%d..%d F2:%d..%d F4:%d..%d\n", c.bassMin, c.bassMax,
                f2.bassMin, f2.bassMax, f4.bassMin, f4.bassMax);
    std::printf("  [counts] events=%u res=%u notes=%ld | onC=%ld ccC=%ld othC=%ld ch=%zu harmCh=%zu\n",
                pr.parsed_events, readerRes, noteEvents,
                c.noteOn, c.cc, c.other, c.channels.size(), c.harmonicChannels.size());
}

int main() {
    std::printf("Test: SFF1 load-and-play end-to-end (4 Genos fixtures)\n");

    struct { const char* label; const char* needle; } fixtures[] = {
        {"POP_ACOUSTIC_2", "POP_ACOUSTIC_2"},
        {"CLASSIC_6_8",    "CLASSIC_6_8"},
        {"BALLAD_FOLK",    "BALLAD_FOLK"},
        {"C_WHISPER",      "C_WHISPER"},
    };

    int found = 0;
    for (const auto& f : fixtures) {
        if (!findCorpusSty(f.needle).empty()) ++found;
        runFixture(f.label, f.needle);
    }

    // The corpus is committed (acceptance golden files), so a missing fixture is
    // a hard failure — never a silent soft-skip to green. This catches a broken
    // checkout, a wrong CORPUS_DIR, or an accidentally-deleted/renamed fixture.
    if (found < 4) {
        std::fprintf(stderr,
            "\n  ❌ FAIL: expected all 4 committed Genos SFF1 fixtures under %s, "
            "found only %d/4.\n     They are a committed acceptance corpus "
            "(docs/governance/IMPORTER_GOLDEN_CORPUS_POLICY.md); a missing fixture "
            "or wrong\n     CORPUS_DIR must fail the build, not pass silently.\n",
            CORPUS_DIR, found);
        ++failures;
    }

    std::printf("\nResults: %d passed, %d failed (fixtures found: %d/4)\n",
                passes, failures, found);
    return failures > 0 ? 1 : 0;
}
