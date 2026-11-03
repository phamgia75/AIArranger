#include "importers/sff1/sff1_mapper.h"
#include "engine/uasf/note_closure.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace ai_arranger::importers::sff1 {

namespace {
// UASF SectionHeader::bars is uint16.
constexpr uint64_t kMaxSectionBars = 0xFFFF;

// Exact Yamaha SFF section marker names. Anything else ("SFF1"/"SFF2" format
// tags, "SInt" setup, unknown text) is not a playable section.
bool markerSectionType(const std::string& m, uasf::SectionType& out) {
    using T = uasf::SectionType;
    static const struct { const char* name; T type; } kMap[] = {
        {"Intro A", T::Intro1}, {"Intro B", T::Intro2}, {"Intro C", T::Intro3},
        {"Main A", T::Main1}, {"Main B", T::Main2}, {"Main C", T::Main3}, {"Main D", T::Main4},
        {"Fill In AA", T::Fill1}, {"Fill In BB", T::Fill2},
        {"Fill In CC", T::Fill3}, {"Fill In DD", T::Fill4},
        {"Fill In BA", T::Break},
        {"Ending A", T::Ending1}, {"Ending B", T::Ending2}, {"Ending C", T::Ending3},
    };
    for (const auto& e : kMap)
        if (m == e.name) { out = e.type; return true; }
    return false;
}
} // namespace

SffToUasfResult Sff1ToUasfMapper::map(const ParseResult& parseResult) noexcept {
    SffToUasfResult result;
    result.success = false;

    if (!parseResult.success ||
        (parseResult.sections.empty() && parseResult.casm_sections.empty())) {
        result.error = "No sections to map";
        return result;
    }

    auto& style = result.style;
    style.name = "Imported from " + parseResult.file_path;
    style.format_version = "1.0";

    // Style tempo = the earliest Set Tempo by absolute tick (file order breaks
    // ties: min_element keeps the first). Whole BPM, rounded to nearest:
    // Yamaha writes 105 BPM as 571429 us/q (104.9999), so truncation is wrong.
    // No tempo map: later changes are reported and ignored.
    style.tempo_bpm = uasf::kDefaultTempoBpm;
    if (!parseResult.tempos.empty()) {
        const auto first = std::min_element(
            parseResult.tempos.begin(), parseResult.tempos.end(),
            [](const SmfTempo& a, const SmfTempo& b) { return a.tick < b.tick; });
        const uint64_t bpm = (60'000'000ull + first->mpqn / 2) / first->mpqn;
        if (bpm >= uasf::kMinTempoBpm && bpm <= uasf::kMaxTempoBpm) {
            style.tempo_bpm = static_cast<uint32_t>(bpm);
        } else {
            result.warnings.push_back("Set Tempo " + std::to_string(first->mpqn) +
                                      " us/quarter (" + std::to_string(bpm) +
                                      " BPM) out of range; using 120 BPM");
        }
        for (const auto& t : parseResult.tempos) {
            if (t.mpqn != first->mpqn) {
                result.warnings.push_back("Later tempo changes ignored (no tempo map); "
                                          "style tempo is the initial tempo");
                break;
            }
        }
    }

    // Resolution (PPQN) must match the source so the playback clock plays the
    // event ticks at the correct tempo. The MThd-derived section carries the
    // real SMF division; fall back to 480 only if no section reports one.
    style.resolution = 480;
    for (const auto& s : parseResult.sections) {
        if (s.resolution > 0) { style.resolution = s.resolution; break; }
    }

    // ── Source-channel → CASM config map (Task A: per-role splitting) ──
    // The SFF1 reader collapses the whole style into a single mixed-channel
    // SMF MTrk (one `Phrase1` track). Each MIDI event still carries its
    // source channel, and each CASM Ctb2 entry maps a source channel to a
    // named role (Rhythm1/Bass/Chord1/…). We rebuild per-role UASF tracks by
    // bucketing events by channel and resolving the role from CASM — NOT from
    // a channel heuristic, because drums are not always on GM channel 9
    // (e.g. POP_ACOUSTIC_2 puts Rhythm1 on channel 8). Splitting keeps the
    // per-channel NoteOn-dedupe from collapsing unrelated tracks and stops
    // drums from being chord-transposed, recovering retriggers dropped when
    // everything shared one track (PR #7 known limitation).
    std::map<uint8_t, const CasmTrackConfig*> chanConfig;
    for (const auto& cfg : parseResult.casm_configs) {
        // Defensive (Task D): a Ctb2 source-channel byte must be a valid MIDI
        // channel (0-15). A corrupt/abnormal config byte can hold anything; it
        // can never bind to a real event channel, so drop it with a warning
        // rather than letting it shadow a valid config or wrap silently.
        if (cfg.source_channel > 15) {
            result.warnings.push_back(
                "CASM track '" + cfg.name + "' has out-of-range source channel " +
                std::to_string(static_cast<int>(cfg.source_channel)) +
                " — ignored (expected 0-15)");
            continue;
        }
        // First config for a channel wins — keeps the role stable across the
        // section variants that repeat the same source channel.
        chanConfig.emplace(cfg.source_channel, &cfg);
    }

    // Track channels we have already warned about so the fallback message is
    // emitted once per channel, not once per section.
    std::set<uint8_t> fallbackWarned;

    // One UASF track per MIDI channel; role/NTR/NTT from the CASM config for
    // that channel (section-specific `cfgs` first, then the style-wide map).
    auto makeTrack = [&](uint8_t ch, const std::map<uint8_t, const CasmTrackConfig*>* cfgs) {
        uasf::TrackDefinition track;
        track.midi_channel = ch;
        track.articulation.profile = uasf::ArticulationProfile::Generic;
        track.articulation.fidelity = uasf::FidelityRequirement::High;

        const CasmTrackConfig* cfg = nullptr;
        if (cfgs) { auto it = cfgs->find(ch); if (it != cfgs->end()) cfg = it->second; }
        if (!cfg) { auto it = chanConfig.find(ch); if (it != chanConfig.end()) cfg = it->second; }
        if (cfg) {
            track.name = cfg->name;
            track.role = mapCasmTrackRole(*cfg);
            track.articulation.ntr = cfg->ntr;
            track.articulation.ntt = cfg->ntt;
        } else {
            // No CASM metadata for this channel: fall back to the
            // GM drum convention, else treat as a melodic phrase.
            track.name = "Channel " + std::to_string(static_cast<int>(ch));
            track.role = (ch == 9) ? uasf::TrackRole::Drum
                                   : uasf::TrackRole::Phrase1;
            if (fallbackWarned.insert(ch).second) {
                result.warnings.push_back(
                    "Channel " + std::to_string(static_cast<int>(ch)) +
                    " has events but no CASM metadata — using " +
                    (ch == 9 ? "drum" : "melodic") + " fallback role");
            }
        }
        track.is_drum = (track.role == uasf::TrackRole::Drum ||
                         track.role == uasf::TrackRole::Percussion);
        if (track.role == uasf::TrackRole::Bass ||
            track.role == uasf::TrackRole::Percussion) {
            track.articulation.fidelity = uasf::FidelityRequirement::Medium;
        }
        return track;
    };

    // Bucket (event, section-relative tick) pairs into per-channel tracks, in
    // ascending channel order (std::map) for deterministic output.
    auto fillTracks = [&](uasf::SectionDefinition& section,
                          const std::vector<std::pair<const SffMidiEvent*, uint64_t>>& evs,
                          const std::map<uint8_t, const CasmTrackConfig*>* cfgs) {
        std::map<uint8_t, uasf::TrackDefinition> byChannel;
        for (const auto& [sffEv, rel] : evs) {
            const uint8_t ch = sffEv->status & 0x0F;
            auto it = byChannel.find(ch);
            if (it == byChannel.end()) it = byChannel.emplace(ch, makeTrack(ch, cfgs)).first;
            uasf::MidiEvent ev = mapMidiEvent(*sffEv);
            ev.tick = rel;
            it->second.events.push_back(ev);
        }
        for (auto& [ch, track] : byChannel) section.tracks.push_back(std::move(track));
    };

    // ── Marker-based section split (Yamaha SMF: "SInt", "Intro A", "Main A",
    // "Fill In AA", "Fill In BA" = Break, "Ending A", ...) ──────────────────
    // Each recognised marker opens a section that runs to the next later
    // marker (or End of Track). Ticks become section-relative; a NoteOff
    // exactly on the end tick closes the section (dispatched at its end). The
    // SInt block's setup messages (program/bank/CC, not notes) are placed at
    // tick 0 of every Intro, so a Start re-initialises the parts. Styles with
    // no recognised markers keep the single-section import below.
    // Meter from the SMF time signature (FF 58); 4/4 if absent or unusable.
    const uint8_t tsNum = parseResult.time_sig_num ? parseResult.time_sig_num : 4;
    const uint8_t tsDen = (parseResult.time_sig_den == 2 || parseResult.time_sig_den == 4 ||
                           parseResult.time_sig_den == 8 || parseResult.time_sig_den == 16)
                              ? parseResult.time_sig_den : 4;

    // A grid whose bar rounds to 0 ticks (e.g. 1 PPQN in 1/16) cannot be played:
    // the sequencer ignores a 0-tick bar, so the style would import "fine" and
    // never sound. Reject it instead.
    if (static_cast<uint64_t>(style.resolution) * 4 * tsNum / tsDen == 0) {
        result.error = "Unplayable grid: " + std::to_string(style.resolution) + " PPQN in " +
                       std::to_string(tsNum) + "/" + std::to_string(tsDen) +
                       " gives a bar of 0 ticks";
        return result;
    }

    std::vector<const SffMidiEvent*> allEvents;
    for (const auto& sffSection : parseResult.sections)
        for (const auto& t : sffSection.tracks)
            for (const auto& e : t.events) allEvents.push_back(&e);

    struct Range { uasf::SectionType type; std::string name; uint32_t start; uint32_t end; };
    std::vector<Range> ranges;
    uint32_t sintStart = 0, sintEnd = 0;
    bool haveSInt = false;
    uint32_t lastTick = parseResult.end_of_track_tick;
    for (const auto* e : allEvents) lastTick = std::max(lastTick, e->tick);
    const auto& marks = parseResult.markers;
    for (size_t i = 0; i < marks.size(); ++i) {
        uint32_t end = lastTick;
        for (size_t j = i + 1; j < marks.size(); ++j)
            if (marks[j].tick > marks[i].tick) { end = marks[j].tick; break; }
        if (marks[i].name == "SInt") {
            haveSInt = true; sintStart = marks[i].tick; sintEnd = end;
            continue;
        }
        uasf::SectionType type;
        if (!markerSectionType(marks[i].name, type)) continue;
        if (end <= marks[i].tick) continue;          // empty range
        bool dup = false;
        for (const auto& r : ranges) if (r.type == type) dup = true;
        if (dup) {
            result.warnings.push_back("Duplicate section marker '" + marks[i].name + "' ignored");
            continue;
        }
        ranges.push_back({type, marks[i].name, marks[i].tick, end});
    }

    if (!ranges.empty()) {
        // Canonical order (enum order): Intros, Mains, Fills, Endings, Break —
        // so section 0 is Intro A when present (Start plays section 0).
        std::stable_sort(ranges.begin(), ranges.end(),
                         [](const Range& a, const Range& b) { return a.type < b.type; });
        const bool anyIntro = std::any_of(ranges.begin(), ranges.end(), [](const Range& r) {
            return r.type >= uasf::SectionType::Intro1 && r.type <= uasf::SectionType::Intro3;
        });
        const uint64_t barTicks = static_cast<uint64_t>(style.resolution) * 4 * tsNum / tsDen;

        for (size_t ri = 0; ri < ranges.size(); ++ri) {
            const Range& r = ranges[ri];
            const uint32_t len = r.end - r.start;
            std::vector<std::pair<const SffMidiEvent*, uint64_t>> evs;

            const bool isIntro = r.type >= uasf::SectionType::Intro1 &&
                                 r.type <= uasf::SectionType::Intro3;
            if (haveSInt && (isIntro || (!anyIntro && ri == 0))) {
                for (const auto* e : allEvents) {
                    if (e->tick < sintStart || e->tick >= sintEnd) continue;
                    const uint8_t hi = e->status & 0xF0;
                    if (hi == 0x80 || hi == 0x90 || hi == 0xA0) continue;   // not notes
                    evs.emplace_back(e, 0);
                }
            }
            for (const auto* e : allEvents) {
                const uint8_t hi = e->status & 0xF0;
                const bool isOff = hi == 0x80 || (hi == 0x90 && e->data2 == 0);
                if ((e->tick >= r.start && e->tick < r.end) || (isOff && e->tick == r.end))
                    evs.emplace_back(e, e->tick - r.start);
            }

            // Section-specific CASM configs (same name as the marker).
            std::map<uint8_t, const CasmTrackConfig*> secCfg;
            for (const auto& cs : parseResult.casm_sections) {
                if (cs.name != r.name) continue;
                for (const auto& t : cs.tracks)
                    if (t.source_channel <= 15) secCfg.emplace(t.source_channel, &t);
                break;
            }

            uasf::SectionDefinition section;
            section.type = r.type;
            section.name = r.name;
            section.resolution = style.resolution;
            section.beats_per_bar = tsNum;
            section.beat_note = tsDen;
            const uint64_t bars = std::max<uint64_t>(1, (len + barTicks - 1) / barTicks);
            if (bars > kMaxSectionBars) {   // UASF stores bars as uint16
                result.error = "Section '" + r.name + "' is " + std::to_string(bars) +
                               " bars long (max " + std::to_string(kMaxSectionBars) + ")";
                return result;
            }
            section.bars = static_cast<uint32_t>(bars);
            fillTracks(section, evs, secCfg.empty() ? nullptr : &secCfg);
            // A note whose NoteOff fell past this marker range (into the next
            // section) would never be closed by this section: end it here.
            for (auto& t : section.tracks) uasf::closeOpenNotes(t, len);
            style.sections.push_back(std::move(section));
        }
    } else {
        for (const auto& sffSection : parseResult.sections) {
            uasf::SectionDefinition section;
            section.type = mapSectionType(sffSection.type);
            section.name = sffSection.name;
            section.bars = sffSection.bars;
            section.resolution = sffSection.resolution;
            section.beats_per_bar = tsNum;
            section.beat_note = tsDen;

            std::vector<std::pair<const SffMidiEvent*, uint64_t>> evs;
            for (const auto& sffTrack : sffSection.tracks)
                for (const auto& sffEv : sffTrack.events) evs.emplace_back(&sffEv, sffEv.tick);
            fillTracks(section, evs, nullptr);

            // Section length from its content. The reader only knows a placeholder
            // (4 bars); the engine loops / hands over at bars x bar length, so
            // a placeholder would loop just the first 4 bars of a 24+ bar SMF.
            // Round up to whole bars of the style meter (the sequencer grid); a final
            // NoteOff exactly on a bar line ends that bar (it is dispatched at the
            // section end), so it does not add an extra bar.
            uint64_t maxTick = 0;
            bool anyEvent = false;
            for (const auto& t : section.tracks)
                for (const auto& e : t.events) { anyEvent = true; if (e.tick > maxTick) maxTick = e.tick; }
            if (anyEvent && section.resolution > 0) {
                // >= 1 keeps the division safe even for a section whose own
                // resolution differs from the style's (checked above).
                const uint64_t barTicks = std::max<uint64_t>(
                    1, static_cast<uint64_t>(section.resolution) * 4 * tsNum / tsDen);
                const uint64_t bars = std::max<uint64_t>(1, (maxTick + barTicks - 1) / barTicks);
                if (bars > kMaxSectionBars) {   // was clamped: a silently shortened section
                    result.error = "Section is " + std::to_string(bars) + " bars long (max " +
                                   std::to_string(kMaxSectionBars) + ")";
                    return result;
                }
                section.bars = static_cast<uint32_t>(bars);
            }

            style.sections.push_back(std::move(section));
        }
    }

    // ── CASM-derived section structure (Task D) ────────────────────────
    // Build UASF sections from the CASM Sdec/Ctb2 data: real section names,
    // per-track roles and channels (metadata only — the playable, event-bearing
    // sections are built above from the SMF markers).
    for (const auto& cs : parseResult.casm_sections) {
        uasf::SectionDefinition sec;
        sec.type = mapCasmSectionType(cs.name);
        sec.name = cs.name;
        sec.bars = 0;
        sec.resolution = style.resolution;
        sec.beats_per_bar = 4;
        sec.beat_note = 4;

        for (const auto& t : cs.tracks) {
            uasf::TrackDefinition track;
            track.name = t.name;
            track.midi_channel = t.source_channel;
            track.role = mapCasmTrackRole(t);
            track.is_drum = (track.role == uasf::TrackRole::Drum ||
                             track.role == uasf::TrackRole::Percussion);
            track.articulation.profile = uasf::ArticulationProfile::Generic;
            track.articulation.fidelity = uasf::FidelityRequirement::High;

            // NTR/NTT come from this section's own Ctb2 entry: the same track
            // name can carry a different rule in another section (C_WHISPER's
            // Strings), so never look it up by name across the whole file.
            track.articulation.ntr = t.ntr;
            track.articulation.ntt = t.ntt;

            sec.tracks.push_back(std::move(track));
        }
        result.casm_sections.push_back(std::move(sec));
    }

    // Check for unmapped features
    if (!parseResult.unsupported_features.empty()) {
        result.unmapped_features = parseResult.unsupported_features;
    }

    // NTR/NTT values passed to UASF articulation metadata (Gate 8)
    if (!parseResult.casm_configs.empty()) {
        // Count mapped tracks
        size_t mappedNtr = 0;
        for (const auto& sec : result.casm_sections) {
            for (const auto& trk : sec.tracks) {
                if (trk.articulation.ntr != 0) mappedNtr++;
            }
        }
        result.warnings.push_back(
            "NTR/NTT mapped for " + std::to_string(mappedNtr) +
            " tracks (supports Root, Fifth, Chord, Bass, Fixed)");
    }

    result.success = true;
    return result;
}

uasf::SectionType Sff1ToUasfMapper::mapSectionType(SffSectionType sffType) noexcept {
    switch (sffType) {
        case SffSectionType::Intro1:  return uasf::SectionType::Intro1;
        case SffSectionType::Intro2:  return uasf::SectionType::Intro2;
        case SffSectionType::Intro3:  return uasf::SectionType::Intro3;
        case SffSectionType::Main1:   return uasf::SectionType::Main1;
        case SffSectionType::Main2:   return uasf::SectionType::Main2;
        case SffSectionType::Main3:   return uasf::SectionType::Main3;
        case SffSectionType::Main4:   return uasf::SectionType::Main4;
        case SffSectionType::Fill1:   return uasf::SectionType::Fill1;
        case SffSectionType::Fill2:   return uasf::SectionType::Fill2;
        case SffSectionType::Fill3:   return uasf::SectionType::Fill3;
        case SffSectionType::Fill4:   return uasf::SectionType::Fill4;
        case SffSectionType::Ending1: return uasf::SectionType::Ending1;
        case SffSectionType::Ending2: return uasf::SectionType::Ending2;
        case SffSectionType::Ending3: return uasf::SectionType::Ending3;
        case SffSectionType::Break:   return uasf::SectionType::Break;
        default: return uasf::SectionType::Main1;
    }
}

uasf::TrackRole Sff1ToUasfMapper::mapTrackRole(SffTrackRole sffRole) noexcept {
    switch (sffRole) {
        case SffTrackRole::Rhythm1:  return uasf::TrackRole::Drum;
        case SffTrackRole::Rhythm2:  return uasf::TrackRole::Percussion;
        case SffTrackRole::Bass:     return uasf::TrackRole::Bass;
        case SffTrackRole::Chord1:   return uasf::TrackRole::Chord;
        case SffTrackRole::Chord2:   return uasf::TrackRole::Chord;
        case SffTrackRole::Pad:      return uasf::TrackRole::Pad;
        case SffTrackRole::Phrase1:  return uasf::TrackRole::Phrase1;
        case SffTrackRole::Phrase2:  return uasf::TrackRole::Phrase2;
        default:                     return uasf::TrackRole::Accompaniment;
    }
}

uasf::TrackRole Sff1ToUasfMapper::mapCasmTrackRole(const CasmTrackConfig& cfg) noexcept {
    std::string n;
    n.reserve(cfg.name.size());
    for (char c : cfg.name)
        n.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    auto has = [&](const char* s) { return n.find(s) != std::string::npos; };

    // Drums live on the GM percussion channel (0-based 9) regardless of name.
    if (cfg.source_channel == 9 || has("rhythm") || has("drum")) {
        // Rhythm2 = percussion overlay, Rhythm1 / main kit = drums
        // (consistent with mapTrackRole's SffTrackRole convention).
        if (has("rhythm2")) return uasf::TrackRole::Percussion;
        return uasf::TrackRole::Drum;
    }
    // Bass: explicit name or NTT bass-note conversion flag.
    if (has("bass") || cfg.ntt_bass) return uasf::TrackRole::Bass;
    if (has("chord"))                return uasf::TrackRole::Chord;
    if (has("pad"))                  return uasf::TrackRole::Pad;
    if (has("phrase2"))              return uasf::TrackRole::Phrase2;
    if (has("phrase"))               return uasf::TrackRole::Phrase1;

    // Named instrument (Piano, Strings, Tbn, ...) with no structural keyword.
    // NTR/NTT alone do not determine a musical role, so we do not guess.
    return uasf::TrackRole::Accompaniment;
}

uasf::SectionType Sff1ToUasfMapper::mapCasmSectionType(const std::string& name) noexcept {
    std::string n;
    n.reserve(name.size());
    for (char c : name)
        n.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    auto has = [&](const char* s) { return n.find(s) != std::string::npos; };

    if (has("intro")) {
        if (has(" c")) return uasf::SectionType::Intro3;
        if (has(" b")) return uasf::SectionType::Intro2;
        return uasf::SectionType::Intro1;
    }
    if (has("ending")) {
        if (has(" c")) return uasf::SectionType::Ending3;
        if (has(" b")) return uasf::SectionType::Ending2;
        return uasf::SectionType::Ending1;
    }
    if (has("break")) return uasf::SectionType::Break;
    if (has("fill in ba")) return uasf::SectionType::Break;   // Yamaha's Break
    if (has("fill")) {
        // "Fill In AA/BB/CC/DD" map to the matching main variant.
        if (has("bb")) return uasf::SectionType::Fill2;
        if (has("cc")) return uasf::SectionType::Fill3;
        if (has("dd")) return uasf::SectionType::Fill4;
        return uasf::SectionType::Fill1;
    }
    if (has("main")) {
        if (has(" d")) return uasf::SectionType::Main4;
        if (has(" c")) return uasf::SectionType::Main3;
        if (has(" b")) return uasf::SectionType::Main2;
        return uasf::SectionType::Main1;
    }
    return uasf::SectionType::Main1;
}

uasf::MidiEvent Sff1ToUasfMapper::mapMidiEvent(const SffMidiEvent& sffEv) noexcept {
    uasf::MidiEvent ev;
    ev.type = static_cast<uasf::MidiEventType>(sffEv.status & 0xF0);
    ev.channel = sffEv.status & 0x0F;
    ev.data1 = sffEv.data1;
    ev.data2 = sffEv.data2;
    ev.tick = sffEv.tick;
    return ev;
}

} // namespace ai_arranger::importers::sff1
