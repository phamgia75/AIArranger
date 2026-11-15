// CASM register limits (roadmap P1.5): High Key and Note Limit, from the Ctb2
// middle zone through the mapper and the UASF 'TRNS' block into NTT playback.
// Semantics: Wierzba/Bedesem "Style Files" v2.1 §5.2.6-5.2.8.

#include "engine/music/ntt.h"
#include "engine/uasf/serializer.h"
#include "engine/uasf/deserializer.h"
#include "engine/uasf/format.h"
#include "importers/sff1/sff1_mapper.h"
#include <cstdio>
#include <cstring>

using namespace ai_arranger;
using music::NtrRule;
using music::NttMode;
using music::RegisterLimits;
using arranger::Chord;
using arranger::ChordType;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static Chord chord(ChordType t, uint8_t root) { return Chord{t, root, 0, 0}; }

static uasf::StyleDefinition oneTrackStyle() {
    uasf::StyleDefinition s;
    uasf::SectionDefinition sec;
    sec.type = uasf::SectionType::Main1; sec.bars = 1; sec.resolution = 480;
    sec.beats_per_bar = 4; sec.beat_note = 4;
    uasf::TrackDefinition t;
    t.name = "Bass"; t.midi_channel = 10; t.role = uasf::TrackRole::Bass;
    t.events.push_back({uasf::MidiEventType::NoteOn, 10, 36, 100, 0});
    t.events.push_back({uasf::MidiEventType::NoteOff, 10, 36, 0, 240});
    sec.tracks.push_back(t);
    s.sections.push_back(sec);
    return s;
}

int main() {
    std::printf("Test: CASM register limits\n");

    // ── Transform ──
    // Spec example (High Key = F): over F the pattern stays up (C3 -> F3), over
    // F# it drops (C3 -> F#2).
    const RegisterLimits hkF{5, 0, 127};
    TEST("High Key F: F chord keeps the upper octave",
         music::transpose(48, chord(ChordType::Major, 65), NtrRule::RootTransposition, NttMode::Bass, 60, hkF) == 53);
    TEST("High Key F: F# chord plays an octave down",
         music::transpose(48, chord(ChordType::Major, 66), NtrRule::RootTransposition, NttMode::Bass, 60, hkF) == 42);
    TEST("Root Fixed ignores High Key",
         music::transpose(48, chord(ChordType::Major, 66), NtrRule::RootFixed, NttMode::Chord, 60, hkF) ==
         music::transpose(48, chord(ChordType::Major, 66), NtrRule::RootFixed, NttMode::Chord, 60));

    const ChordType types[] = {ChordType::Major, ChordType::Minor, ChordType::Dom7, ChordType::Maj7,
                               ChordType::Min7, ChordType::Dim, ChordType::Aug, ChordType::Sus4};
    const NttMode modes[] = {NttMode::Melody, NttMode::Chord, NttMode::Bass};
    bool defaults = true, highKey = true, folded = true;
    for (auto ty : types)
        for (auto mode : modes)
            for (int root = 48; root < 60; ++root)
                for (int n = 12; n < 116; ++n) {
                    const Chord c = chord(ty, static_cast<uint8_t>(root));
                    const auto note = static_cast<uint8_t>(n);
                    const int base = music::transpose(note, c, NtrRule::RootTransposition, mode);
                    defaults &= music::transpose(note, c, NtrRule::RootTransposition, mode, 60, {}) == base;
                    for (uint8_t hk = 0; hk <= 11; ++hk) {
                        const int got = music::transpose(note, c, NtrRule::RootTransposition, mode, 60, {hk, 0, 127});
                        const bool drops = (root % 12) > hk;
                        highKey &= got == (drops ? base - 12 : base);
                    }
                    for (int lo : {24, 28, 36}) {
                        const int hi = lo + 23;
                        const int got = music::transpose(note, c, NtrRule::RootTransposition, mode, 60,
                                                         {11, static_cast<uint8_t>(lo), static_cast<uint8_t>(hi)});
                        folded &= got >= lo && got <= hi && (got - base) % 12 == 0;
                    }
                }
    TEST("default limits change nothing (exhaustive)", defaults);
    TEST("High Key: exactly the roots above it drop one octave (exhaustive)", highKey);
    TEST("Note Limit: result inside the range, pitch class kept (exhaustive)", folded);
    TEST("Note Limit under an octave is ignored",
         music::transpose(40, chord(ChordType::Major, 60), NtrRule::RootTransposition, NttMode::Bass, 60, {11, 50, 55}) ==
         music::transpose(40, chord(ChordType::Major, 60), NtrRule::RootTransposition, NttMode::Bass, 60));

    // ── UASF 'TRNS' block ──
    {
        uasf::UasfSerializer ser;
        const auto plain = ser.serialize(oneTrackStyle());
        uasf::format::FileHeader h{};
        std::memcpy(&h, plain.data.data(), sizeof(h));
        TEST("no limits: no extension flag (file unchanged from before)", plain.success && h.flags == 0);

        auto s = oneTrackStyle();
        s.sections[0].tracks[0].articulation.high_key = 3;
        s.sections[0].tracks[0].articulation.note_low = 28;
        const auto out = uasf::UasfSerializer().serialize(s);
        const auto back = uasf::UasfDeserializer().deserialize(out.data);
        const auto& a = back.style.sections.empty() || back.style.sections[0].tracks.empty()
                            ? uasf::ArticulationMetadata{} : back.style.sections[0].tracks[0].articulation;
        TEST("limits round-trip through TRNS",
             back.success && a.high_key == 3 && a.note_low == 28 && a.note_high == 127 &&
             back.style.unknown_extensions.empty());
        TEST("serialize is deterministic", uasf::UasfSerializer().serialize(s).data == out.data);

        // The writer refuses what the reader would reject (no unloadable files).
        auto inval = oneTrackStyle();
        inval.sections[0].tracks[0].articulation.note_low = 120;
        TEST("serializer rejects a range under an octave", !uasf::UasfSerializer().serialize(inval).success);
        inval.sections[0].tracks[0].articulation.note_low = 0;
        inval.sections[0].tracks[0].articulation.high_key = 12;
        TEST("serializer rejects High Key > 11", !uasf::UasfSerializer().serialize(inval).success);

        // Corrupt the 5-byte entry (last bytes of the file): bad High Key, bad range, bad index.
        auto bad = out.data;
        const size_t e = bad.size() - 5;
        bad[e + 2] = 12;
        TEST("High Key > 11 rejected", !uasf::UasfDeserializer().deserialize(bad).success);
        bad = out.data; bad[e + 3] = 120;
        TEST("range under an octave rejected", !uasf::UasfDeserializer().deserialize(bad).success);
        bad = out.data; bad[e + 1] = 5;
        TEST("track index out of range rejected", !uasf::UasfDeserializer().deserialize(bad).success);
        bad = out.data; bad.pop_back();
        TEST("truncated block rejected", !uasf::UasfDeserializer().deserialize(bad).success);
    }

    // ── Mapper: High Key only under Root Trans; out-of-spec limits ignored ──
    {
        using namespace importers::sff1;
        auto cfg = [](const char* name, uint8_t ch, uint8_t ntr, uint8_t hk, uint8_t lo, uint8_t hi) {
            CasmTrackConfig c{};
            c.name = name; c.source_channel = ch; c.dest_channel = ch; c.ntr = ntr; c.ntt = 1;
            c.ntt_bass = true; c.high_key = hk; c.note_low = lo; c.note_high = hi;
            return c;
        };
        ParseResult p{};
        p.success = true;
        SffSection smf{}; smf.resolution = 1920; smf.bars = 4; smf.name = "SMF";
        p.sections.push_back(smf);
        CasmSection sec; sec.name = "Main A";
        sec.tracks = {cfg("Bass", 10, 0, 3, 28, 127), cfg("Bass2", 11, 1, 3, 28, 127),
                      cfg("Bass3", 12, 0, 20, 60, 65)};
        p.casm_sections = {sec};
        p.casm_configs = sec.tracks;
        const auto r = Sff1ToUasfMapper().map(p);
        const bool ok = r.success && r.casm_sections.size() == 1 && r.casm_sections[0].tracks.size() == 3;
        TEST("mapper: three tracks", ok);
        if (ok) {
            const auto& t = r.casm_sections[0].tracks;
            TEST("Root Trans: High Key + limits kept",
                 t[0].articulation.high_key == 3 && t[0].articulation.note_low == 28);
            TEST("Root Fixed: High Key not applied, limits kept",
                 t[1].articulation.high_key == 11 && t[1].articulation.note_low == 28);
            TEST("out-of-spec High Key and range fall back to defaults",
                 !t[2].articulation.hasRegisterLimits());
        }
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
