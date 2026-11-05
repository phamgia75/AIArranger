#include "exporters/korg/genos_to_korg.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <set>
#include <tuple>

namespace ai_arranger::exporters::korg {

using uasf::MidiEventType;
using uasf::SectionType;

const char* korgTrackName(uint8_t ch) noexcept {
    switch (ch) {
        case kBassCh: return "Bass";
        case kDrumCh: return "Drum";
        case kPercCh: return "Percussion";
        case 11: return "Acc1";
        case 12: return "Acc2";
        case 13: return "Acc3";
        case 14: return "Acc4";
        case 15: return "Acc5";
        default: return "-";
    }
}

uint8_t korgChannelFor(uint8_t g) noexcept {
    // Genos part channels 9..16: Rhythm1, Rhythm2, Bass, Chord1, Chord2, Pad,
    // Phrase1, Phrase2. Rhythm2 carries the main kit in every corpus style.
    switch (g) {
        case 8:  return kPercCh;   // Rhythm1 -> Percussion
        case 9:  return kDrumCh;   // Rhythm2 -> Drum
        case 10: return kBassCh;   // Bass    -> Bass
        case 11: case 12: case 13: case 14: case 15:
            return g;              // Chord1, Chord2, Pad, Phrase1, Phrase2 -> Acc1..Acc5
        default: return 0xFF;
    }
}

const char* korgMarkerFor(SectionType t) noexcept {
    // One Chord Variation per element: Genos sections have no chord variations.
    // Intro/Fill/Ending numbers above 2 and Break have no documented marker
    // code (Korg tutorial table) and are reported, not guessed.
    switch (t) {
        case SectionType::Main1:   return "v1cv1";
        case SectionType::Main2:   return "v2cv1";
        case SectionType::Main3:   return "v3cv1";
        case SectionType::Main4:   return "v4cv1";
        case SectionType::Intro1:  return "i1cv1";
        case SectionType::Intro2:  return "i2cv1";
        case SectionType::Fill1:   return "f1cv1";
        case SectionType::Fill2:   return "f2cv1";
        case SectionType::Ending1: return "e1cv1";
        case SectionType::Ending2: return "e2cv1";
        default: return nullptr;
    }
}

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Instrument family for the sound report (priority order of the milestone).
std::string familyOf(const std::string& name, uasf::TrackRole role) {
    using R = uasf::TrackRole;
    const std::string n = lower(name);
    auto has = [&](const char* k) { return n.find(k) != std::string::npos; };
    if (role == R::Drum || role == R::Percussion) return "Drum Kit";
    if (role == R::Bass || has("bass")) return "Bass";
    if (has("piano") || has("e.p") || has("pno")) return "Piano/EP";
    if (has("gt") || has("guit")) return "Guitar";
    if (has("str")) return "Strings";
    if (has("brass") || has("tp") || has("tb") || has("trump") || has("horn")) return "Brass";
    if (role == R::Pad || has("pad")) return "Pad";
    return "Generic";
}

struct Note { uint32_t on, off; uint8_t pitch, vel; };

int rank(const Event& e) {
    const uint8_t hi = e.status & 0xF0;
    if (hi == 0xB0 && e.d1 == 0) return 0;
    if (hi == 0xB0 && e.d1 == 32) return 1;
    if (hi == 0xC0) return 2;
    if (hi == 0x80) return 4;
    if (hi == 0x90) return 5;
    return 3;
}
} // namespace

MapResult mapToKorg(const uasf::StyleDefinition& style, const MapOptions& opt) {
    MapResult r;
    Report& rep = r.report;

    const uint32_t dst = opt.ppqn ? opt.ppqn : style.resolution;
    if (dst == 0 || dst > 0x7FFF) { r.error = "target PPQN " + std::to_string(dst) + " out of range 1..32767"; return r; }
    r.style.ppqn = static_cast<uint16_t>(dst);
    if (style.tempo_bpm) r.style.mpqn = (60'000'000u + style.tempo_bpm / 2) / style.tempo_bpm;
    else rep.warnings.push_back("style has no tempo; set it on Pa5X");

    std::map<uint8_t, TrackRow> rows;
    std::set<std::pair<uint8_t, uint8_t>> mergeWarned;

    for (const auto& sec : style.sections) {
        SectionRow row{sec.name, "", sec.bars, ""};
        const char* marker = korgMarkerFor(sec.type);
        size_t secEvents = 0;
        for (const auto& t : sec.tracks) secEvents += t.events.size();
        if (!marker) {
            row.note = sec.type == SectionType::Break
                ? "no documented Korg marker code for Break"
                : "Korg marker spec documents element 1-2 only for intro/fill/ending";
            rep.dropped[kDropSection] += static_cast<uint32_t>(secEvents);
            rep.sections.push_back(row);
            continue;
        }
        if (sec.bars == 0) {
            row.note = "zero-length section";
            rep.dropped[kDropSection] += static_cast<uint32_t>(secEvents);
            rep.sections.push_back(row);
            continue;
        }
        if (std::any_of(r.style.elements.begin(), r.style.elements.end(),
                        [&](const Element& e) { return e.marker == marker; })) {
            row.note = "duplicate section type";
            rep.dropped[kDropSection] += static_cast<uint32_t>(secEvents);
            rep.sections.push_back(row);
            continue;
        }

        const uint32_t src = sec.resolution ? sec.resolution : style.resolution;
        const uint8_t num = static_cast<uint8_t>(sec.beats_per_bar ? sec.beats_per_bar : 4);
        const uint8_t den = static_cast<uint8_t>(sec.beat_note ? sec.beat_note : 4);
        if (src == 0) { r.error = "section '" + sec.name + "' has no resolution"; return r; }
        const uint64_t barNum = uint64_t{dst} * 4 * num;
        if (barNum % den) { r.error = "meter " + std::to_string(num) + "/" + std::to_string(den) +
                                      " has no whole-tick bar at " + std::to_string(dst) + " PPQN"; return r; }
        const uint64_t len64 = uint64_t{sec.bars} * (barNum / den);
        if (len64 > 0x0FFFFFFF) { r.error = "section '" + sec.name + "' too long"; return r; }
        const uint32_t len = static_cast<uint32_t>(len64);
        // Exact rational rescale of each absolute tick: no cumulative drift.
        auto conv = [&](uint64_t t) { return (t * dst + src / 2) / src; };

        Element el;
        el.marker = marker;
        el.num = num;
        el.den = den;
        el.length = len;

        std::map<uint8_t, uint8_t> owner;   // Korg channel -> first source channel this section
        for (const auto& tr : sec.tracks) {
            const uint8_t srcCh = tr.midi_channel;
            auto rit = opt.route.find(srcCh);
            const uint8_t routed = rit != opt.route.end() ? rit->second : srcCh;
            const uint8_t kch = korgChannelFor(routed);

            auto [rowIt, fresh] = rows.try_emplace(srcCh);
            TrackRow& tr_row = rowIt->second;
            if (fresh) {
                tr_row.src_ch = srcCh;
                tr_row.korg_ch = kch;
                tr_row.name = tr.name;
                tr_row.role = tr.role;
                tr_row.ntr = tr.articulation.ntr;
                tr_row.ntt = tr.articulation.ntt;
                tr_row.family = familyOf(tr.name, tr.role);
            }

            if (kch == 0xFF) { rep.dropped[kDropChannel] += static_cast<uint32_t>(tr.events.size()); continue; }
            auto [own, first] = owner.try_emplace(kch, srcCh);
            if (!first && own->second != srcCh && mergeWarned.insert({own->second, srcCh}).second)
                rep.warnings.push_back("Genos channels " + std::to_string(own->second + 1) + " and " +
                                       std::to_string(srcCh + 1) + " both play Korg " + korgTrackName(kch) +
                                       " (merged; their NTR/NTT may differ)");

            std::vector<const uasf::MidiEvent*> evs;
            for (const auto& e : tr.events) evs.push_back(&e);
            std::stable_sort(evs.begin(), evs.end(),
                             [](const uasf::MidiEvent* a, const uasf::MidiEvent* b) { return a->tick < b->tick; });

            std::vector<Note> notes;
            std::map<uint8_t, std::deque<size_t>> open;
            for (const auto* e : evs) {
                const uint64_t t = conv(e->tick);
                const bool isOn = e->type == MidiEventType::NoteOn && e->data2 > 0;
                const bool isOff = e->type == MidiEventType::NoteOff ||
                                   (e->type == MidiEventType::NoteOn && e->data2 == 0);
                if (isOn) {
                    open[e->data1].push_back(notes.size());
                    notes.push_back({static_cast<uint32_t>(std::min<uint64_t>(t, len)), UINT32_MAX, e->data1, e->data2});
                } else if (isOff) {
                    auto& q = open[e->data1];
                    if (q.empty()) { ++rep.dropped[kDropOrphanOff]; continue; }
                    notes[q.front()].off = static_cast<uint32_t>(std::min<uint64_t>(t, len));
                    q.pop_front();
                } else if (e->type == MidiEventType::ProgramChange ||
                           (e->type == MidiEventType::ControlChange && (e->data1 == 0 || e->data1 == 32))) {
                    // Yamaha voice numbers mean nothing on Pa5X: never forward them.
                    if (e->type == MidiEventType::ProgramChange) { if (tr_row.pc < 0) tr_row.pc = e->data1; }
                    else if (e->data1 == 0) { if (tr_row.msb < 0) tr_row.msb = e->data2; }
                    else if (tr_row.lsb < 0) tr_row.lsb = e->data2;
                    ++rep.dropped[kDropVoiceSelect];
                } else if (t >= len) {
                    ++rep.dropped[kDropPastEnd];
                } else {
                    el.events.push_back({static_cast<uint32_t>(t),
                                         static_cast<uint8_t>(static_cast<uint8_t>(e->type) | kch),
                                         e->data1, e->data2});
                }
            }
            const bool drum = kch == kDrumCh || kch == kPercCh;
            for (auto n : notes) {
                if (!drum && n.pitch <= kChordEventNoteMax) { ++rep.dropped[kDropChordRange]; continue; }
                if (len < 2 || n.on >= len - 1) { ++rep.dropped[kDropLastTick]; continue; }
                // A note past the last tick makes Pa append a measure: end it inside.
                n.off = std::max(std::min(n.off, len - 1), n.on + 1);
                if (drum && (n.pitch < 35 || n.pitch > 81)) ++rep.non_gm_drum_notes[n.pitch];
                const uint8_t on = static_cast<uint8_t>(0x90 | kch), off = static_cast<uint8_t>(0x80 | kch);
                el.events.push_back({n.on, on, n.pitch, n.vel});
                el.events.push_back({n.off, off, n.pitch, 0});
            }
        }
        for (const auto& [kch, s] : opt.sounds) {
            el.events.push_back({0, static_cast<uint8_t>(0xB0 | kch), 0, s.msb});
            el.events.push_back({0, static_cast<uint8_t>(0xB0 | kch), 32, s.lsb});
            el.events.push_back({0, static_cast<uint8_t>(0xC0 | kch), s.pc, 0});
        }
        // Stable and without d2: same-tick duplicates (a doubled pitch with two
        // velocities, two CC values) keep source order, so NoteOff pairing and
        // "last value wins" match the source.
        std::stable_sort(el.events.begin(), el.events.end(), [](const Event& a, const Event& b) {
            return std::make_tuple(a.tick, rank(a), a.status, a.d1) <
                   std::make_tuple(b.tick, rank(b), b.status, b.d1);
        });
        row.marker = marker;
        rep.sections.push_back(row);
        r.style.elements.push_back(std::move(el));
    }

    for (auto& [ch, row] : rows) {
        auto s = row.korg_ch == 0xFF ? opt.sounds.end() : opt.sounds.find(row.korg_ch);
        if (row.korg_ch == 0xFF) row.target = "not exported";
        else if (s != opt.sounds.end())
            row.target = "MSB " + std::to_string(s->second.msb) + " LSB " + std::to_string(s->second.lsb) +
                         " PC " + std::to_string(s->second.pc) + " (user map)";
        else row.target = "UNRESOLVED";
        rep.tracks.push_back(row);
    }
    if (r.style.elements.empty()) { r.error = "no section maps to a Korg style element"; return r; }
    r.ok = true;
    return r;
}

std::string formatReport(const Report& r, const Style& s) {
    std::string o;
    auto line = [&](const std::string& x) { o += x; o += '\n'; };
    line("PPQN " + std::to_string(s.ppqn) + ", tempo " +
         (s.mpqn ? std::to_string((60'000'000u + s.mpqn / 2) / s.mpqn) + " BPM (" + std::to_string(s.mpqn) + " us/q)"
                 : std::string("none")));
    line("");
    line("Section map (Genos -> Korg marker):");
    for (const auto& x : r.sections)
        line("  " + x.source + " (" + std::to_string(x.bars) + " bars) -> " +
             (x.marker.empty() ? "NOT EXPORTED: " + x.note : x.marker));
    line("");
    line("Tracks: Genos ch | name | NTR/NTT | Yamaha voice MSB/LSB/PC | family -> Korg track | Pa5X sound");
    for (const auto& t : r.tracks)
        line("  " + std::to_string(t.src_ch + 1) + " | " + t.name + " | " + std::to_string(t.ntr) + "/" +
             std::to_string(t.ntt) + " | " + std::to_string(t.msb) + "/" + std::to_string(t.lsb) + "/" +
             std::to_string(t.pc) + " | " + t.family + " -> " + korgTrackName(t.korg_ch) + " | " + t.target);
    line("");
    line("Dropped events:");
    if (r.dropped.empty()) line("  none");
    for (const auto& [why, n] : r.dropped) line("  " + std::to_string(n) + "  " + why);
    uint32_t nonGm = 0;
    std::string notes;
    for (const auto& [n, c] : r.non_gm_drum_notes) { nonGm += c; notes += " " + std::to_string(n); }
    line("Drum/percussion notes outside GM 35..81 (kept; Pa5X kit sound unverified): " +
         std::to_string(nonGm) + (notes.empty() ? "" : " (notes" + notes + ")"));
    line("");
    line("Warnings:");
    if (r.warnings.empty()) line("  none");
    for (const auto& w : r.warnings) line("  " + w);
    return o;
}

} // namespace ai_arranger::exporters::korg
