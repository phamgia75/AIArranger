// Golden corpus: the 4 real Genos styles -> Korg Pa5X Style SMF.
// For each style: convert, write twice (byte-identical), re-parse with the
// independent reader, validate, check tempo/meter, and compare the re-parsed
// notes against the canonical import (musical equivalence of the artifact).

#include "exporters/korg/genos_to_korg.h"
#include "exporters/korg/sff1_route.h"
#include "importers/sff1/sff1_mapper.h"
#include "importers/sff1/sff1_reader.h"
#include "engine/music/ntt.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <map>
#include <tuple>

using namespace ai_arranger;
using namespace ai_arranger::exporters::korg;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

using NoteKey = std::tuple<uint8_t, uint32_t, uint32_t, uint8_t, uint8_t>;   // ch, on, off, pitch, vel

// Notes of a canonical section as the Korg artifact must carry them: routed to
// the Korg track, chord-event-range and last-tick notes removed, ends clamped.
static std::vector<NoteKey> expectedNotes(const uasf::SectionDefinition& sec, const std::map<uint8_t, uint8_t>& route,
                                          uint32_t len, bool& drumOnChordTrack) {
    std::vector<NoteKey> out;
    for (const auto& t : sec.tracks) {
        auto it = route.find(t.midi_channel);
        const uint8_t kch = korgChannelFor(it != route.end() ? it->second : t.midi_channel);
        if (kch == 0xFF) continue;
        const bool drumRole = t.role == uasf::TrackRole::Drum || t.role == uasf::TrackRole::Percussion;
        if (drumRole && isChordDriven(kch)) drumOnChordTrack = true;
        auto evs = t.events;
        std::stable_sort(evs.begin(), evs.end(), [](auto& a, auto& b) { return a.tick < b.tick; });
        std::map<uint8_t, std::deque<std::pair<uint32_t, uint8_t>>> open;
        auto emit = [&](uint8_t p, uint32_t on, uint8_t v, uint32_t off) {
            if (isChordDriven(kch) && p <= kChordEventNoteMax) return;
            if (on >= len - 1) return;
            out.emplace_back(kch, on, std::max(std::min(off, len - 1), on + 1), p, v);
        };
        for (const auto& e : evs) {
            const bool on = e.type == uasf::MidiEventType::NoteOn && e.data2 > 0;
            const bool off = e.type == uasf::MidiEventType::NoteOff || (e.type == uasf::MidiEventType::NoteOn && e.data2 == 0);
            const auto tick = static_cast<uint32_t>(std::min<uint64_t>(e.tick, len));
            if (on) open[e.data1].push_back({tick, e.data2});
            else if (off && !open[e.data1].empty()) {
                auto [t0, v] = open[e.data1].front();
                open[e.data1].pop_front();
                emit(e.data1, t0, v, tick);
            }
        }
        for (auto& [p, q] : open) for (auto& [t0, v] : q) emit(p, t0, v, len);
    }
    std::sort(out.begin(), out.end());
    return out;
}

static std::vector<NoteKey> actualNotes(const Element& el) {
    std::vector<NoteKey> out;
    std::map<uint16_t, std::deque<std::pair<uint32_t, uint8_t>>> open;
    for (const auto& e : el.events) {
        const uint8_t hi = e.status & 0xF0, ch = e.status & 0x0F;
        const uint16_t k = static_cast<uint16_t>(ch << 8 | e.d1);
        if (hi == 0x90 && e.d2) open[k].push_back({e.tick, e.d2});
        else if ((hi == 0x80 || hi == 0x90) && !open[k].empty()) {
            out.emplace_back(ch, open[k].front().first, e.tick, e.d1, open[k].front().second);
            open[k].pop_front();
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

int main() {
    std::printf("Test: Genos corpus -> Korg Pa5X Style SMF\n");
    const std::map<std::string, std::pair<uint32_t, uint8_t>> expect = {   // tempo, meter numerator
        {"POP_ACOUSTIC_2", {100, 4}}, {"CLASSIC_6_8", {90, 6}}, {"BALLAD_FOLK", {105, 4}}, {"C_WHISPER", {76, 4}}};

    std::vector<std::filesystem::path> files;
    for (const auto& f : std::filesystem::directory_iterator(CORPUS_DIR))
        if (f.path().extension() == ".sty") files.push_back(f.path());
    std::sort(files.begin(), files.end());
    TEST("corpus has 4 Genos styles", files.size() == 4);

    for (const auto& path : files) {
        const std::string fname = path.filename().string();
        std::string key;
        for (const auto& [k, v] : expect) if (fname.rfind(k, 0) == 0) key = k;
        std::printf("\n== %s\n", key.c_str());
        TEST("known corpus style", !key.empty());
        if (key.empty()) continue;

        importers::sff1::Sff1Reader reader;
        const auto parsed = reader.parseFile(path.string());
        importers::sff1::Sff1ToUasfMapper m;
        const auto canon = m.map(parsed);
        TEST("imports", parsed.success && canon.success);
        if (!canon.success) continue;

        MapOptions opt;
        opt.route = routeFromCasm(parsed);
        const auto r = mapToKorg(canon.style, opt);
        TEST("maps to Korg", r.ok);
        if (!r.ok) continue;
        const auto w1 = write(r.style), w2 = write(r.style);
        TEST("writes", w1.ok);
        TEST("deterministic (byte-identical)", w1.bytes == w2.bytes);
        const auto back = parse(w1.bytes);
        TEST("independent parser reads it", back.ok);
        TEST("validator passes on re-parsed output", back.ok && validate(back.style).empty());
        TEST("tempo matches source", back.style.mpqn == (60'000'000u + expect.at(key).first / 2) / expect.at(key).first);
        TEST("meter matches source", std::all_of(back.style.elements.begin(), back.style.elements.end(),
             [&](const Element& e) { return e.num == expect.at(key).second && e.den == (expect.at(key).second == 6 ? 8 : 4); }));

        // Musical equivalence of the artifact against the canonical import.
        bool lengthsOk = true, notesOk = true, drumOnChord = false;
        size_t notes = 0;
        for (const auto& sec : canon.style.sections) {
            const char* mk = korgMarkerFor(sec.type);
            if (!mk || sec.bars == 0) continue;
            const Element* el = nullptr;
            for (const auto& e : back.style.elements) if (e.marker == mk) el = &e;
            if (!el) { lengthsOk = false; continue; }
            const uint32_t len = sec.bars * sec.resolution * 4 * sec.beats_per_bar / sec.beat_note;
            if (el->length != len) lengthsOk = false;
            const auto want = expectedNotes(sec, opt.route, len, drumOnChord);
            const auto got = actualNotes(*el);
            if (want != got) { notesOk = false; std::fprintf(stderr, "    %s: %zu notes expected, %zu written\n", mk, want.size(), got.size());
                for (size_t i = 0, k = 0; i < want.size() && k < 4; ++i) if (want[i] != got[i]) { ++k;
                    auto [c, a, b, p, v] = want[i]; auto [c2, a2, b2, p2, v2] = got[i];
                    std::fprintf(stderr, "      want ch%d %u-%u p%d v%d | got ch%d %u-%u p%d v%d\n", c + 1, a, b, p, v, c2 + 1, a2, b2, p2, v2); } }
            notes += got.size();
        }
        TEST("section lengths preserved", lengthsOk);
        TEST("every kept note preserved (pitch, velocity, on/off tick, track)", notesOk);
        TEST("no drum part on a chord-driven Korg track (drums never transposed)", !drumOnChord);

        // Chord adaptation input is identical, so the engine reference output is too:
        // spot-check one bass note through the NTR/NTT transform over 4 chords.
        using arranger::Chord;
        using arranger::ChordType;
        const Chord chords[] = {{ChordType::Major, 60, 0, 0}, {ChordType::Minor, 69, 0, 0},
                                {ChordType::Dom7, 67, 0, 0}, {ChordType::Dim, 71, 0, 0}};
        const uint8_t bassC2 = 36;
        const uint8_t want[] = {36, 45, 43, 47};
        bool tr = true;
        for (int i = 0; i < 4; ++i)
            tr = tr && music::transpose(bassC2, chords[i], music::NtrRule::RootTransposition, music::NttMode::Bass) == want[i];
        TEST("bass reference transposition C/Am/G7/Bdim", tr);

        const bool isWhisper = key == "C_WHISPER";
        if (isWhisper) {
            bool tail = false;
            for (const auto& s : r.report.sections)
                if (s.source == "Fill In BA" && s.bars == 10 && s.marker.empty()) tail = true;
            TEST("C_WHISPER 10-bar Fill In BA (Break + unmarked tail, #26) reported, not exported", tail);
        }

        uint32_t dropped = 0;
        for (const auto& [why, n] : r.report.dropped) dropped += n;
        uint32_t unresolved = 0, nonGm = 0;
        for (const auto& t : r.report.tracks) if (t.target == "UNRESOLVED") ++unresolved;
        for (const auto& [n, c] : r.report.non_gm_drum_notes) nonGm += c;
        std::printf("  | sections mapped %zu/%zu | tracks %zu | notes %zu | unresolved sounds %u | non-GM drum notes %u | dropped events %u | warnings %zu | %zu bytes\n",
                    back.style.elements.size(), r.report.sections.size(), r.report.tracks.size(), notes,
                    unresolved, nonGm, dropped, r.report.warnings.size(), w1.bytes.size());
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
