#include "exporters/korg/genos_to_korg.h"
#include "exporters/korg/cv_splitter.h"
#include "exporters/korg/pa5x_sound_matcher.h"
#include "exporters/korg/timbre_profiler.h"

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
    return korgMarkerFor(t, BreakPolicy::Withhold);
}

const char* korgMarkerFor(SectionType t, BreakPolicy policy) noexcept {
    // One Chord Variation per element: Genos sections have no chord variations.
    // Codes per Pa5X User Manual v1.4 p.1035. Break ("br", no number, no CV) is
    // emitted under BreakPolicy::Yamaha1BarCap (1-bar cap per Wierzba/Bedesem
    // v2.1 §5.1.1.2) and withheld under BreakPolicy::Withhold.
    switch (t) {
        case SectionType::Main1:   return "v1cv1";
        case SectionType::Main2:   return "v2cv1";
        case SectionType::Main3:   return "v3cv1";
        case SectionType::Main4:   return "v4cv1";
        case SectionType::Intro1:  return "i1cv1";
        case SectionType::Intro2:  return "i2cv1";
        case SectionType::Intro3:  return "i3cv1";
        case SectionType::Fill1:   return "f1cv1";
        case SectionType::Fill2:   return "f2cv1";
        case SectionType::Fill3:   return "f3cv1";
        case SectionType::Fill4:   return "f4cv1";
        case SectionType::Ending1: return "e1cv1";
        case SectionType::Ending2: return "e2cv1";
        case SectionType::Ending3: return "e3cv1";
        case SectionType::Break:   return policy == BreakPolicy::Yamaha1BarCap ? "br" : nullptr;
        default: return nullptr;
    }
}

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Instrument family for the sound report. Drum tracks by Korg target; other
// parts by an instrument keyword in the Genos part name, then by role. Part
// names like "Chord1" carry no instrument: those stay "Unidentified" (the
// Yamaha voice number is not a family source; its banks are not GM).
std::string familyOf(const std::string& name, uasf::TrackRole role, uint8_t korgCh) {
    const std::string n = lower(name);
    auto has = [&](std::initializer_list<const char*> ks) {
        for (const char* k : ks) if (n.find(k) != std::string::npos) return true;
        return false;
    };
    if (korgCh == kDrumCh) return "Drum Kit";
    if (korgCh == kPercCh) return "Percussion";
    if (has({"piano", "pno", "e.p", "ep "})) return "Piano/EP";
    if (has({"organ", "org"})) return "Organ";
    if (has({"guitar", "gtr", "gt.", "gt "})) return "Guitar";
    if (has({"str"})) return "Strings";
    if (has({"brass", "tbn", "trb", "tp", "trp", "trumpet", "trombone", "horn", "sax"})) return "Brass";
    if (has({"synth", "syn", "lead"})) return "Synth";
    if (has({"pad"}) || role == uasf::TrackRole::Pad) return "Pad";
    if (has({"bass"}) || role == uasf::TrackRole::Bass) return "Bass";
    return "Unidentified";
}

int rank(const Event& e) {
    const uint8_t hi = e.status & 0xF0;
    if (hi == 0xB0 && e.d1 == 0) return 0;
    if (hi == 0xB0 && e.d1 == 32) return 1;
    if (hi == 0xC0) return 2;
    if (hi == 0x80) return 4;
    if (hi == 0x90) return 5;
    return 3;
}

void addUniqueDiag(std::vector<std::string>& diags, const std::string& code) {
    if (std::find(diags.begin(), diags.end(), code) == diags.end()) {
        diags.push_back(code);
    }
}

struct VoiceSelect {
    int msb = -1;
    int lsb = -1;
    int pc  = -1;
};

uint8_t resolveRoutedChannel(const uasf::TrackDefinition& tr, const MapOptions& opt) noexcept {
    auto rit = opt.route.find(tr.midi_channel);
    if (rit != opt.route.end()) return rit->second;
    if (tr.articulation.has_casm_meta && tr.articulation.dest_channel <= 15) {
        return tr.articulation.dest_channel;
    }
    return tr.midi_channel;
}

quality::CasmTrackView buildCasmTrackView(const uasf::TrackDefinition& tr, uint8_t routedCh) noexcept {
    quality::CasmTrackView v{};
    v.src_ch = tr.midi_channel;
    v.dest_ch = routedCh;
    const auto& a = tr.articulation;
    v.has_casm = a.has_casm_meta;
    v.ntr = a.ntr;
    v.ntt = a.ntt;
    v.ntt_bass = a.ntt_bass;
    v.high_key = a.high_key;
    v.note_low = a.note_low;
    v.note_high = a.note_high;
    v.rtr = a.rtr;
    v.source_chord_root = a.source_chord_root;
    v.source_chord_type = a.source_chord_type;
    if (a.has_casm_meta) {
        // Watchpoint #1: legacy SFF2 Ctb2 blocks leave bytes [11..17] all-zero
        // (note_mute_mask == 0 && chord_mute_mask == 0) to signify "unrestricted".
        // Normalize that legacy all-zero pair to all-active so cv_splitter.h
        // never silently drops tracks, while still honoring single-axis mute masks.
        if (a.note_mute_mask == 0 && a.chord_mute_mask == 0) {
            v.note_mute_mask = quality::kMaskAllNotes;
            v.chord_mute_mask = quality::kMaskAllChords;
        } else {
            v.note_mute_mask = a.note_mute_mask;
            v.chord_mute_mask = a.chord_mute_mask;
        }
    } else {
        v.note_mute_mask = quality::kMaskAllNotes;
        v.chord_mute_mask = quality::kMaskAllChords;
    }
    return v;
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

    // Pre-scan initial Yamaha Bank/PC per source channel so sections after SInt/Intro
    // still know their active Yamaha voice (MegaVoice 63/120, 63/60, S.Art 8/64..127,
    // Revo! 62/100, XG Brush/Latin kits, etc.).
    std::map<uint8_t, VoiceSelect> activeVoice;
    for (const auto& sec : style.sections) {
        for (const auto& tr : sec.tracks) {
            auto& vs = activeVoice[tr.midi_channel];
            for (const auto& e : tr.events) {
                if (e.type == MidiEventType::ControlChange && e.data1 == 0 && vs.msb < 0) vs.msb = e.data2;
                else if (e.type == MidiEventType::ControlChange && e.data1 == 32 && vs.lsb < 0) vs.lsb = e.data2;
                else if (e.type == MidiEventType::ProgramChange && vs.pc < 0) vs.pc = e.data1;
            }
        }
    }

    // Style-wide Stage 2a: verify Rhythm1 (ch8) vs Rhythm2 (ch9) drum/perc swap risk
    // consistently across all sections (avoids partial-section flip on sparse Breaks).
    if (opt.enable_drum_remap) {
        std::vector<quality::NoteSpan> ch8All, ch9All;
        for (const auto& sec : style.sections) {
            if (!korgMarkerFor(sec.type, opt.break_policy) || sec.bars == 0) continue;
            for (const auto& tr : sec.tracks) {
                const uint8_t routed = resolveRoutedChannel(tr, opt);
                if (routed != 8 && routed != 9) continue;
                auto& dstVec = (routed == 8) ? ch8All : ch9All;
                for (const auto& e : tr.events) {
                    if (e.type == MidiEventType::NoteOn && e.data2 > 0) {
                        const uint32_t on = static_cast<uint32_t>(std::min<uint64_t>(e.tick, UINT32_MAX - 1));
                        dstVec.push_back({on, on + 1u, e.data1, e.data2, false});
                    }
                }
            }
        }
        const auto swapDecision = quality::detectRhythmTrackSwap(ch8All, ch9All);
        rep.rhythm_tracks_swapped = swapDecision.swap_drum_and_perc;
        if (rep.rhythm_tracks_swapped) {
            rep.warnings.push_back("Rhythm1/Rhythm2 swapped: " + swapDecision.reason);
        }
    }

    auto resolveKorgCh = [&](uint8_t routed) noexcept -> uint8_t {
        if (rep.rhythm_tracks_swapped) {
            if (routed == 8) return kDrumCh;
            if (routed == 9) return kPercCh;
        }
        return korgChannelFor(routed);
    };

    // ── Stage 5 (Q4-Wire): Timbre Profiler -> Pa5X Sound Matcher (post-swap aware, fail-closed) ──
    // Runs whenever sound matching is enabled OR when caller requests `capture_soundmap_dump`
    // (single source of truth so `--dump-soundmap` can NEVER diverge from SMF Bank/PC emission!).
    std::map<uint8_t, SoundRef> effectiveSounds = opt.sounds;
    std::map<std::pair<SectionType, uint8_t>, SoundRef> effectiveElementSounds = opt.element_sounds;
    std::map<uint8_t, quality::TrackSoundMatch> matchBySrcCh;
    std::map<uint8_t, quality::TrackSoundMatch> matchByKorgCh;
    std::map<std::pair<SectionType, uint8_t>, std::string> elementSoundLabels;
    quality::StyleSoundMatchResult stage5StyleMatch;
    quality::SoundMatchOptions stage5MatchOpt{};

    if (opt.enable_sound_matching || opt.capture_soundmap_dump) {
        struct ProfileAccum {
            uint8_t src_ch = 0;
            uint8_t korg_ch = 0xFF;
            std::string name;
            uasf::TrackRole role = uasf::TrackRole::Accompaniment;
            int dominant_msb = -1;
            int dominant_lsb = -1;
            int dominant_pc = -1;
            std::map<std::tuple<int, int, int>, uint32_t> voice_freq;
            std::map<SectionType, std::tuple<int, int, int>> element_voices;
            std::vector<quality::NoteSpan> notes;
            uint64_t total_bars = 0;
            uint32_t ppqn = 480;
            uint64_t bar_ticks = 1920;
        };

        const uint32_t tempoMpqn = r.style.mpqn ? r.style.mpqn : 500000u;
        std::map<uint8_t, ProfileAccum> bySrc;
        std::set<SectionType> seenProfiledSectionTypes;
        for (const auto& sec : style.sections) {
            const bool sectionExportable = (korgMarkerFor(sec.type, opt.break_policy) != nullptr && sec.bars > 0);
            // Match the exporter's first-wins duplicate-section-type rule (P2-1):
            // never let a dropped duplicate SectionType overwrite an already-recorded element voice or pollute profiles.
            if (sectionExportable && !seenProfiledSectionTypes.insert(sec.type).second) {
                continue;
            }
            const uint32_t secPpqn = sec.resolution ? sec.resolution : (style.resolution ? style.resolution : 480u);
            const uint64_t sNum = sec.beats_per_bar ? uint64_t{sec.beats_per_bar} : 4u;
            const uint64_t sDen = sec.beat_note ? uint64_t{sec.beat_note} : 4u;
            const uint64_t barTicks64 = std::max<uint64_t>(1u, (uint64_t{secPpqn} * 4u * sNum) / sDen);
            const uint64_t secBars64 = std::max<uint64_t>(1u, uint64_t{sec.bars});
            const uint64_t secEnd64 = (secBars64 > uint64_t{UINT32_MAX} / barTicks64)
                                          ? uint64_t{UINT32_MAX}
                                          : (secBars64 * barTicks64);

            for (const auto& tr : sec.tracks) {
                const uint8_t srcCh = tr.midi_channel;
                const uint8_t routed = resolveRoutedChannel(tr, opt);
                const uint8_t kch = resolveKorgCh(routed);
                const uasf::TrackRole effRole = (kch == kDrumCh) ? uasf::TrackRole::Drum
                                              : (kch == kPercCh) ? uasf::TrackRole::Percussion
                                                                 : tr.role;

                auto [it, fresh] = bySrc.try_emplace(srcCh);
                ProfileAccum& acc = it->second;
                if (fresh) {
                    acc.src_ch = srcCh;
                    acc.korg_ch = kch;
                    acc.name = tr.name;
                    acc.role = effRole;
                    acc.ppqn = secPpqn;
                    acc.bar_ticks = barTicks64;
                } else if (acc.name.empty() && !tr.name.empty()) {
                    acc.name = tr.name;
                }

                const auto voice = quality::extractSectionYamahaVoice(tr.events);
                const auto [vMsb, vLsb, vPc] = voice;
                if (vMsb >= 0 || vLsb >= 0 || vPc >= 0) {
                    if (sectionExportable) {
                        // First-wins per (srcCh, SectionType): never silently clobber an already-recorded element voice.
                        acc.element_voices.try_emplace(sec.type, voice);
                    }
                    if (vPc >= 0) {
                        acc.voice_freq[voice]++;
                        if (acc.dominant_pc < 0) {
                            acc.dominant_msb = vMsb;
                            acc.dominant_lsb = vLsb;
                            acc.dominant_pc = vPc;
                        }
                    }
                }

                const auto spans = quality::extractNoteSpansFromEvents(tr.events, static_cast<uint32_t>(secEnd64));
                const uint64_t offset64 = (acc.bar_ticks > 0 && acc.total_bars > UINT64_MAX / acc.bar_ticks)
                                              ? UINT64_MAX
                                              : (acc.total_bars * acc.bar_ticks);
                for (auto ns : spans) {
                    const uint64_t on64 = (offset64 >= uint64_t{UINT32_MAX} - ns.on_tick)
                                              ? uint64_t{UINT32_MAX}
                                              : (offset64 + uint64_t{ns.on_tick});
                    const uint64_t off64 = (offset64 >= uint64_t{UINT32_MAX} - ns.off_tick)
                                               ? uint64_t{UINT32_MAX}
                                               : (offset64 + uint64_t{ns.off_tick});
                    ns.on_tick = static_cast<uint32_t>(on64);
                    ns.off_tick = static_cast<uint32_t>(off64);
                    acc.notes.push_back(ns);
                }
                if (acc.total_bars <= UINT64_MAX - secBars64) {
                    acc.total_bars += secBars64;
                } else {
                    acc.total_bars = UINT64_MAX;
                }
            }
        }

        std::vector<quality::TrackTimbreProfile> profiles;
        profiles.reserve(bySrc.size());
        for (const auto& [srcCh, acc] : bySrc) {
            int bestMsb = acc.dominant_msb;
            int bestLsb = acc.dominant_lsb;
            int bestPc = acc.dominant_pc;
            uint32_t bestCount = 0;
            for (const auto& [voice, cnt] : acc.voice_freq) {
                if (cnt > bestCount) {
                    bestCount = cnt;
                    std::tie(bestMsb, bestLsb, bestPc) = voice;
                }
            }
            quality::TimbreProfileOptions topt{};
            topt.ppqn = acc.ppqn;
            topt.tempo_mpqn = tempoMpqn;
            topt.bar_ticks = static_cast<uint32_t>(std::min<uint64_t>(acc.bar_ticks, UINT32_MAX));
            topt.bars = static_cast<uint32_t>(std::min<uint64_t>(acc.total_bars, UINT32_MAX));
            profiles.push_back(quality::profileTrackTimbre(
                bestMsb, bestLsb, bestPc, acc.name, acc.role, acc.notes,
                acc.element_voices, srcCh, acc.korg_ch, topt));
        }

        quality::TargetSoundCatalogView localTargetCatalog;
        const quality::TargetSoundCatalogView* targetCatalogPtr = opt.target_sound_catalog;
        if (!targetCatalogPtr && opt.target_sounds != nullptr) {
            localTargetCatalog.entries = *opt.target_sounds;
            targetCatalogPtr = &localTargetCatalog;
        }

        stage5MatchOpt.enable_factory_catalog = opt.enable_sound_matching;
        stage5MatchOpt.prefer_target_user_sounds = opt.enable_sound_matching && opt.prefer_target_user_sounds;
        stage5MatchOpt.min_auto_select_score = std::max<uint8_t>(opt.min_sound_match_score, uint8_t{40u});

        stage5StyleMatch = quality::matchStyleSounds(
            profiles, opt.sounds, targetCatalogPtr, stage5MatchOpt, opt.element_sounds);

        if (opt.enable_sound_matching) {
            rep.sound_match_stats.unsupported += stage5StyleMatch.target_entries_skipped_oos;
            if (stage5StyleMatch.target_entries_skipped_oos > 0) {
                addUniqueDiag(rep.quality_diagnostics, "STYLE_QUAL_TARGET_KST_OUT_OF_SPEC_SKIPPED");
            }
            if (stage5StyleMatch.target_partial_inspect) {
                addUniqueDiag(rep.quality_diagnostics, "STYLE_QUAL_TARGET_KST_PARTIAL_INSPECT");
                if (!stage5StyleMatch.target_partial_notice.empty()) {
                    rep.warnings.push_back("Target KST partial inspect: " + stage5StyleMatch.target_partial_notice);
                }
            }

            // Defense-in-depth fail-closed audit:
            //   1. NEVER auto-select any non-user sound when profile.confidence < 40.
            //   2. NEVER allow melodic-to-drum (MSB 120 on melodic) or drum-to-melodic (MSB 121 on drum/perc).
            //   3. Explicit user --sound-map / element_sounds ALWAYS take highest precedence.
            effectiveSounds = opt.sounds;
            effectiveElementSounds = opt.element_sounds;

            for (auto& m : stage5StyleMatch.tracks) {
                if (m.korg_ch == 0xFF) continue;
                const bool isDrumKch = (m.korg_ch == kDrumCh || m.korg_ch == kPercCh);

                if (m.selected.has_value() && m.selected_origin != quality::MatchOrigin::UserSoundMap) {
                    const bool lowConf = (m.profile.confidence < 40u);
                    const bool msbOk = isDrumKch ? (m.selected->msb == 120u) : (m.selected->msb == 121u);
                    if (lowConf || !msbOk) {
                        m.selected.reset();
                        m.selected_epistemic_tag.clear();
                        if (lowConf) addUniqueDiag(rep.quality_diagnostics, "STYLE_QUAL_SOUND_LOW_CONFIDENCE_REJECTED");
                        if (!msbOk) addUniqueDiag(rep.quality_diagnostics, "STYLE_QUAL_SOUND_CROSS_FAMILY_REJECTED");
                    }
                }

                if (m.selected.has_value() &&
                    (m.selected_origin == quality::MatchOrigin::UserSoundMap || !effectiveSounds.count(m.korg_ch))) {
                    effectiveSounds[m.korg_ch] = *m.selected;
                }

                for (auto it = m.element_selected.begin(); it != m.element_selected.end(); ) {
                    const SectionType st = it->first;
                    const SoundRef& elRef = it->second;
                    const bool userEl = opt.element_sounds.count({st, m.korg_ch}) > 0;
                    const bool msbOk = isDrumKch ? (elRef.msb == 120u) : (elRef.msb == 121u);
                    if (!userEl && !msbOk) {
                        addUniqueDiag(rep.quality_diagnostics, "STYLE_QUAL_SOUND_CROSS_FAMILY_REJECTED");
                        it = m.element_selected.erase(it);
                        continue;
                    }
                    if (!userEl && !effectiveElementSounds.count({st, m.korg_ch})) {
                        effectiveElementSounds[{st, m.korg_ch}] = elRef;
                        if (const auto cit = m.element_candidates.find(st); cit != m.element_candidates.end()) {
                            elementSoundLabels[{st, m.korg_ch}] = cit->second.display_name;
                        }
                    }
                    ++it;
                }

                matchBySrcCh[m.profile.src_ch] = m;
                auto kit = matchByKorgCh.find(m.korg_ch);
                if (kit == matchByKorgCh.end() ||
                    m.selected_origin == quality::MatchOrigin::UserSoundMap ||
                    (m.selected.has_value() && !kit->second.selected.has_value())) {
                    matchByKorgCh[m.korg_ch] = m;
                }
            }

            // Keep `stage5StyleMatch` resolved maps synchronized with the audited `effective*` maps.
            stage5StyleMatch.resolved_sounds = effectiveSounds;
            stage5StyleMatch.element_sounds = effectiveElementSounds;
        }
    }

    std::set<std::pair<SectionType, uint8_t>> emittedElementSwitches;

    std::map<uint8_t, TrackRow> rows;
    std::set<std::pair<uint8_t, uint8_t>> mergeWarned;
    std::set<SectionType> exportedSectionTypes;
    std::map<uint8_t, quality::Pa5xTrackEditAdvice> adviceByKch;

    for (const auto& sec : style.sections) {
        SectionRow row{sec.name, "", sec.bars, ""};
        const char* baseMarker = korgMarkerFor(sec.type, opt.break_policy);
        size_t secEvents = 0;
        for (const auto& t : sec.tracks) secEvents += t.events.size();
        if (!baseMarker) {
            row.note = "Break not exported: marker form unconfirmed on Pa5X, Genos break import open (#26)";
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
        if (!exportedSectionTypes.insert(sec.type).second) {
            row.note = "duplicate section type";
            rep.dropped[kDropSection] += static_cast<uint32_t>(secEvents);
            rep.sections.push_back(row);
            continue;
        }

        const bool breakTruncated = (sec.type == SectionType::Break &&
                                     opt.break_policy == BreakPolicy::Yamaha1BarCap &&
                                     sec.bars > 1);
        const uint32_t effectiveBars = breakTruncated ? 1u : sec.bars;
        if (breakTruncated) {
            row.bars = 1;
            row.note = std::string(kDiagBreakTailTruncated) + ": truncated " +
                       std::to_string(sec.bars) + "-bar Break to Yamaha 1-bar cap";
            rep.warnings.push_back(std::string(kDiagBreakTailTruncated) +
                                   ": Fill In BA truncated from " +
                                   std::to_string(sec.bars) + " bars to 1 bar");
            addUniqueDiag(rep.quality_diagnostics, kDiagBreakTailTruncated);
        }

        const uint32_t src = sec.resolution ? sec.resolution : style.resolution;
        const uint8_t num = static_cast<uint8_t>(sec.beats_per_bar ? sec.beats_per_bar : 4);
        const uint8_t den = static_cast<uint8_t>(sec.beat_note ? sec.beat_note : 4);
        if (src == 0) { r.error = "section '" + sec.name + "' has no resolution"; return r; }
        const uint64_t barNum = uint64_t{dst} * 4 * num;
        if (barNum % den) { r.error = "meter " + std::to_string(num) + "/" + std::to_string(den) +
                                      " has no whole-tick bar at " + std::to_string(dst) + " PPQN"; return r; }
        const uint64_t len64 = uint64_t{effectiveBars} * (barNum / den);
        if (len64 > 0x0FFFFFFF) { r.error = "section '" + sec.name + "' too long"; return r; }
        const uint32_t len = static_cast<uint32_t>(len64);
        // Exact rational rescale of each absolute tick: no cumulative drift.
        auto conv = [&](uint64_t t) { return (t * dst + src / 2) / src; };

        struct PreparedTrack {
            const uasf::TrackDefinition* track = nullptr;
            uint8_t srcCh = 0;
            uint8_t kch = 0xFF;
            quality::CasmTrackView casm;
            std::vector<quality::NoteSpan> notes;
            std::vector<Event> nonNoteEvents;
        };

        std::vector<PreparedTrack> prepared;
        prepared.reserve(sec.tracks.size());

        // Extract per-track NoteSpan + CC streams and run Stages 2 & 3 ONCE per source track
        // (strictly before CV splitting/cloning so no track is ever double-remapped or double-sanitized).
        for (const auto& tr : sec.tracks) {
            const uint8_t srcCh = tr.midi_channel;
            const uint8_t routed = resolveRoutedChannel(tr, opt);
            const uint8_t kch = resolveKorgCh(routed);

            auto [rowIt, fresh] = rows.try_emplace(srcCh);
            TrackRow& tr_row = rowIt->second;
            if (fresh) {
                tr_row.src_ch = srcCh;
                tr_row.korg_ch = kch;
                tr_row.name = tr.name;
                tr_row.role = tr.role;
                tr_row.ntr = tr.articulation.ntr;
                tr_row.ntt = tr.articulation.ntt;
                tr_row.family = familyOf(tr.name, tr.role, kch);
            }

            if (kch == 0xFF) {
                rep.dropped[kDropChannel] += static_cast<uint32_t>(tr.events.size());
                continue;
            }

            std::vector<const uasf::MidiEvent*> evs;
            evs.reserve(tr.events.size());
            for (const auto& e : tr.events) evs.push_back(&e);
            std::stable_sort(evs.begin(), evs.end(),
                             [](const uasf::MidiEvent* a, const uasf::MidiEvent* b) { return a->tick < b->tick; });

            PreparedTrack pt;
            pt.track = &tr;
            pt.srcCh = srcCh;
            pt.kch = kch;
            pt.casm = buildCasmTrackView(tr, routed);

            // Track section-local Bank/PC updates while preserving active style-wide voice.
            VoiceSelect secVoice = activeVoice[srcCh];
            bool sawSecMsb = false, sawSecLsb = false, sawSecPc = false;

            std::vector<quality::NoteSpan> rawNotes;
            std::map<uint8_t, std::deque<size_t>> open;
            for (const auto* e : evs) {
                const uint64_t t = conv(e->tick);
                const bool isOn = e->type == MidiEventType::NoteOn && e->data2 > 0;
                const bool isOff = e->type == MidiEventType::NoteOff ||
                                   (e->type == MidiEventType::NoteOn && e->data2 == 0);
                if (isOn) {
                    if (breakTruncated && t >= len) {
                        ++rep.dropped[kDropBreakTail];
                        continue;
                    }
                    open[e->data1].push_back(rawNotes.size());
                    rawNotes.push_back({static_cast<uint32_t>(std::min<uint64_t>(t, len)),
                                        UINT32_MAX, e->data1, e->data2, false});
                } else if (isOff) {
                    auto& q = open[e->data1];
                    if (q.empty()) {
                        if (breakTruncated && t >= len) { ++rep.dropped[kDropBreakTail]; continue; }
                        ++rep.dropped[kDropOrphanOff];
                        continue;
                    }
                    rawNotes[q.front()].off_tick = static_cast<uint32_t>(std::min<uint64_t>(t, len));
                    q.pop_front();
                } else if (e->type == MidiEventType::ProgramChange ||
                           (e->type == MidiEventType::ControlChange && (e->data1 == 0 || e->data1 == 32))) {
                    // Yamaha voice numbers mean nothing on Pa5X: never forward them.
                    if (e->type == MidiEventType::ProgramChange) {
                        if (tr_row.pc < 0) tr_row.pc = e->data1;
                        if (!sawSecPc) { secVoice.pc = e->data1; sawSecPc = true; }
                        activeVoice[srcCh].pc = e->data1;
                    } else if (e->data1 == 0) {
                        if (tr_row.msb < 0) tr_row.msb = e->data2;
                        if (!sawSecMsb) { secVoice.msb = e->data2; sawSecMsb = true; }
                        activeVoice[srcCh].msb = e->data2;
                    } else {
                        if (tr_row.lsb < 0) tr_row.lsb = e->data2;
                        if (!sawSecLsb) { secVoice.lsb = e->data2; sawSecLsb = true; }
                        activeVoice[srcCh].lsb = e->data2;
                    }
                    ++rep.dropped[kDropVoiceSelect];
                } else if (t >= len) {
                    ++rep.dropped[breakTruncated ? kDropBreakTail : kDropPastEnd];
                } else {
                    pt.nonNoteEvents.push_back({static_cast<uint32_t>(t),
                                                static_cast<uint8_t>(static_cast<uint8_t>(e->type) | kch),
                                                e->data1, e->data2});
                }
            }

            const bool drum = (kch == kDrumCh || kch == kPercCh);
            std::vector<quality::NoteSpan> boundedNotes;
            boundedNotes.reserve(rawNotes.size());
            for (auto n : rawNotes) {
                if (!drum) {
                    tr_row.velocity_zones |= n.vel <= 64 ? 1 : n.vel <= 90 ? 2 : n.vel <= 100 ? 4 : 8;
                    if (n.pitch >= 96) ++tr_row.high_notes;
                }
                if (!drum && n.pitch <= kChordEventNoteMax) {
                    // Preserve 0..11 spans briefly into Stage 3 so MegaVoice classifier S4 sees them;
                    // Stage 3 strips all 0..11 unconditionally in every mode (including Preserve).
                    const uint32_t safeOn = (len >= 2) ? std::min<uint32_t>(n.on_tick, len - 2) : 0u;
                    n.on_tick = safeOn;
                    n.off_tick = safeOn + 1u;
                    boundedNotes.push_back(n);
                    continue;
                }
                if (len < 2 || n.on_tick >= len - 1) {
                    ++rep.dropped[kDropLastTick];
                    continue;
                }
                // A note past the last tick makes Pa append a measure: end it inside.
                n.off_tick = std::max(std::min(n.off_tick, len - 1), n.on_tick + 1);
                boundedNotes.push_back(n);
            }

            if (drum) {
                // ── Stage 2 (Q3 drum_remap.h) — ONCE per source Drum/Perc track ──
                if (opt.enable_drum_remap) {
                    const uasf::TrackRole drumRole =
                        (kch == kPercCh) ? uasf::TrackRole::Percussion : uasf::TrackRole::Drum;
                    const quality::DrumKitCategory cat = quality::classifyDrumKitCategory(
                        secVoice.msb, secVoice.lsb, secVoice.pc, tr.name, drumRole);
                    const bool isRevo = (secVoice.msb == 62 && secVoice.lsb == 100);
                    quality::FactoryPa5xDrumKitKeyMap factoryKit(cat);
                    // P1-2: Scope target_drum_key_map strictly to kDrumCh, and target_perc_key_map strictly to kPercCh.
                    // When target_drum_key_maps (Track A read-only bridge) is provided, bind each channel independently
                    // ONLY when that channel's own resolved sound points to a comparable target USERDK kit.
                    const quality::IDrumKitKeyMap* resolvedTargetKit = nullptr;
                    auto lookupBridgeKit = [&](uint8_t ch) -> const quality::IDrumKitKeyMap* {
                        if (!opt.target_drum_key_maps) return nullptr;
                        const SoundRef* refPtr = nullptr;
                        if (const auto elit = effectiveElementSounds.find({sec.type, ch});
                            elit != effectiveElementSounds.end()) {
                            refPtr = &elit->second;
                        } else if (const auto sit = effectiveSounds.find(ch); sit != effectiveSounds.end()) {
                            refPtr = &sit->second;
                        }
                        if (!refPtr) return nullptr;
                        const auto kitIt = opt.target_drum_key_maps->find(
                            std::make_tuple(refPtr->msb, refPtr->lsb, refPtr->pc));
                        return kitIt == opt.target_drum_key_maps->end() ? nullptr : &kitIt->second;
                    };
                    if (kch == kDrumCh) {
                        resolvedTargetKit = opt.target_drum_key_map ? opt.target_drum_key_map : lookupBridgeKit(kDrumCh);
                    } else if (kch == kPercCh) {
                        resolvedTargetKit = opt.target_perc_key_map ? opt.target_perc_key_map : lookupBridgeKit(kPercCh);
                    }
                    const quality::IDrumKitKeyMap& targetKit =
                        resolvedTargetKit ? *resolvedTargetKit
                                          : static_cast<const quality::IDrumKitKeyMap&>(factoryKit);
                    const auto dr = quality::remapDrumTrack(boundedNotes, cat, targetKit, isRevo, dst);
                    rep.drum_notes_remapped_exact += dr.stats.passthrough_notes;
                    rep.drum_notes_remapped_approx += dr.stats.remapped_notes;
                    rep.drum_notes_unsupported += dr.stats.notes_dropped;
                    for (const auto& code : dr.diagnostic_codes) addUniqueDiag(rep.quality_diagnostics, code);
                    pt.notes = dr.notes;
                } else {
                    pt.notes = std::move(boundedNotes);
                }
                for (auto& n : pt.notes) {
                    if (len >= 2) {
                        n.off_tick = std::max(std::min(n.off_tick, len - 1), n.on_tick + 1);
                    }
                    if (n.pitch < 35 || n.pitch > 81) ++rep.non_gm_drum_notes[n.pitch];
                }
            } else {
                // ── Stage 3 (Q2 megavoice_sanitizer.h) — ONCE per source chord-driven track ──
                quality::SanitizeOptions sopt;
                sopt.mode = opt.megavoice_mode;
                sopt.ppqn = dst;
                sopt.tempo_mpqn = r.style.mpqn ? r.style.mpqn : 500000u;
                sopt.drop_noise_zone = true;
                sopt.emit_guitar_mode_hint = true;

                const auto sr = quality::sanitizeMegaVoiceTrack(
                    boundedNotes, secVoice.msb, secVoice.lsb, secVoice.pc,
                    tr.name, tr.role, pt.casm, sopt);
                if (sr.stats.chord_range_stripped > 0) {
                    rep.dropped[kDropChordRange] += sr.stats.chord_range_stripped;
                    tr_row.chord_range_dropped += sr.stats.chord_range_stripped;
                }
                const uint32_t fxStripped = sr.stats.fx_keys_stripped + sr.stats.noise_zone_stripped;
                rep.megavoice_fx_notes_stripped += fxStripped;
                rep.megavoice_vel_reshaped += sr.stats.vel_reshaped_notes;
                if (opt.megavoice_mode != quality::MegaVoiceMode::Preserve &&
                    (fxStripped > 0 || sr.stats.vel_reshaped_notes > 0 ||
                     sr.stats.guitar_voices_trimmed > 0 || sr.stats.guitar_span_folded > 0)) {
                    ++rep.megavoice_tracks_sanitized;
                }
                for (const auto& code : sr.diagnostic_codes) addUniqueDiag(rep.quality_diagnostics, code);

                pt.notes.reserve(sr.notes.size());
                for (auto n : sr.notes) {
                    // Defensive chord-event-range guard for every chord-driven Korg track (!drum):
                    // covers Drum/Percussion-role tracks routed onto Acc1..Acc5/Bass (where
                    // sanitizeMegaVoiceTrack returns early) in both quality and raw() modes.
                    if (n.pitch <= kChordEventNoteMax) {
                        ++rep.dropped[kDropChordRange];
                        ++tr_row.chord_range_dropped;
                        continue;
                    }
                    if (len < 2 || n.on_tick >= len - 1) continue;
                    n.off_tick = std::max(std::min(n.off_tick, len - 1), n.on_tick + 1);
                    pt.notes.push_back(n);
                }
            }

            prepared.push_back(std::move(pt));
        }

        // ── Stage 1 (Q2 cv_splitter.h) — Per-Section CASM Route + Chord-Mute -> CV1..CV6 ──
        std::vector<quality::SectionTrackInput> splitInputs;
        splitInputs.reserve(prepared.size());
        std::map<const quality::SectionTrackInput*, size_t> inputToPreparedIdx;
        for (size_t i = 0; i < prepared.size(); ++i) {
            splitInputs.push_back({prepared[i].track, prepared[i].casm, prepared[i].kch});
        }
        for (size_t i = 0; i < splitInputs.size(); ++i) {
            inputToPreparedIdx[&splitInputs[i]] = i;
        }

        quality::CvSplitPlan splitPlan;
        if (opt.enable_cv_split) {
            splitPlan = quality::planSectionCvs(sec.type, sec.name, splitInputs);
            if (splitPlan.buckets.size() > 1) {
                rep.cv_splits_created += static_cast<uint32_t>(splitPlan.buckets.size() - 1);
            }
            rep.shadowed_tracks_dropped += splitPlan.shadowed_tracks_dropped;
            for (const auto& d : splitPlan.diagnostics) {
                addUniqueDiag(rep.quality_diagnostics, d.code);
                if (d.code == "STYLE_QUAL_CV_SLOTS_EXHAUSTED") {
                    rep.warnings.push_back(d.code + " (" + d.section + "): " + d.detail);
                }
            }
        } else {
            quality::CvBucket single;
            single.cv_index = (sec.type == SectionType::Break) ? 0 : 1;
            single.marker = baseMarker;
            single.reference_chord = arranger::ChordType::Major;
            single.tracks.reserve(splitInputs.size());
            for (const auto& inp : splitInputs) single.tracks.push_back(&inp);
            splitPlan.buckets.push_back(std::move(single));
        }

        // ── Stage 4 (Q3 source_chord_normalizer.h) + Stage 7 Element Assembly ──
        for (const auto& bucket : splitPlan.buckets) {
            if (bucket.marker.empty()) continue;

            Element el;
            el.marker = bucket.marker;
            el.num = num;
            el.den = den;
            el.length = len;

            std::map<uint8_t, uint8_t> owner; // Korg channel -> first source channel in this CV bucket
            for (const auto* inp : bucket.tracks) {
                const auto idxIt = inputToPreparedIdx.find(inp);
                if (idxIt == inputToPreparedIdx.end()) continue;
                const auto& pt = prepared[idxIt->second];

                auto [own, first] = owner.try_emplace(pt.kch, pt.srcCh);
                if (!first && own->second != pt.srcCh && mergeWarned.insert({own->second, pt.srcCh}).second) {
                    rep.warnings.push_back("Genos channels " + std::to_string(own->second + 1) + " and " +
                                           std::to_string(pt.srcCh + 1) + " both play Korg " + korgTrackName(pt.kch) +
                                           " (merged; their NTR/NTT may differ)");
                }

                for (const auto& ev : pt.nonNoteEvents) {
                    el.events.push_back(ev);
                }

                const std::vector<quality::NoteSpan>* emitNotes = &pt.notes;
                quality::NormalizeResult normRes;
                if (opt.enable_source_chord_normalization && isChordDriven(pt.kch)) {
                    // Normalized ONCE per (source_track, target_cv_chord) — never double-transposed!
                    normRes = quality::normalizeTrackToPa5xReference(
                        pt.notes, pt.kch, pt.track->role, pt.casm, bucket.reference_chord);
                    emitNotes = &normRes.notes;
                    if (normRes.normalized_from_non_c || normRes.notes_transposed > 0 || normRes.quality_remapped > 0) {
                        ++rep.tracks_source_chord_normalized;
                    }
                    rep.register_violations_fixed += normRes.register_violations_fixed;
                    // Record primary Pa5X Style Edit advice per Korg track (prefer Main1 cv1, else first seen).
                    if (adviceByKch.find(pt.kch) == adviceByKch.end() ||
                        (sec.type == SectionType::Main1 && bucket.cv_index == 1)) {
                        adviceByKch[pt.kch] = normRes.advice;
                    }
                }

                const uint8_t onSt = static_cast<uint8_t>(0x90 | pt.kch);
                const uint8_t offSt = static_cast<uint8_t>(0x80 | pt.kch);
                for (auto n : *emitNotes) {
                    if (len < 2 || n.on_tick >= len - 1) continue;
                    n.off_tick = std::max(std::min(n.off_tick, len - 1), n.on_tick + 1);
                    el.events.push_back({n.on_tick, onSt, n.pitch, n.vel});
                    el.events.push_back({n.off_tick, offSt, n.pitch, 0});
                }
            }

            std::map<uint8_t, SoundRef> sounds = effectiveSounds;
            for (const auto& [key, s] : effectiveElementSounds) {
                if (key.first == sec.type) {
                    sounds[key.second] = s;
                    emittedElementSwitches.insert(key);
                    const bool isUserEl = opt.element_sounds.count(key) > 0;
                    std::string line = bucket.marker + " " + korgTrackName(key.second) + ": MSB " +
                                       std::to_string(s.msb) + " LSB " + std::to_string(s.lsb) +
                                       " PC " + std::to_string(s.pc);
                    if (!isUserEl) {
                        if (const auto lit = elementSoundLabels.find(key);
                            lit != elementSoundLabels.end() && !lit->second.empty()) {
                            line += " (" + lit->second + ", auto match)";
                        } else {
                            line += " (auto match)";
                        }
                    }
                    rep.element_sounds.push_back(std::move(line));
                }
            }
            for (const auto& [kch, s] : sounds) {
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

            SectionRow cvRow = row;
            cvRow.marker = bucket.marker;
            rep.sections.push_back(std::move(cvRow));
            r.style.elements.push_back(std::move(el));
        }
    }

    for (auto& [kch, adv] : adviceByKch) {
        (void)kch;
        rep.track_edit_advice.push_back(std::move(adv));
    }

    rep.resolved_sounds = effectiveSounds;
    rep.resolved_element_sounds = effectiveElementSounds;
    rep.sound_match_stats.element_voice_switches = static_cast<uint32_t>(emittedElementSwitches.size());

    for (auto& [ch, row] : rows) {
        auto s = row.korg_ch == 0xFF ? opt.sounds.end() : opt.sounds.find(row.korg_ch);
        if (row.korg_ch == 0xFF) {
            row.target = "not exported";
        } else {
            ++rep.sound_match_stats.tracks_evaluated;
            if (s != opt.sounds.end()) {
                row.target = "MSB " + std::to_string(s->second.msb) + " LSB " + std::to_string(s->second.lsb) +
                             " PC " + std::to_string(s->second.pc) + " (user map)";
                ++rep.sound_match_stats.user_overrides;
            } else if (opt.enable_sound_matching) {
                const quality::TrackSoundMatch* mPtr = nullptr;
                if (const auto mit = matchBySrcCh.find(ch); mit != matchBySrcCh.end()) {
                    mPtr = &mit->second;
                } else if (const auto kit = matchByKorgCh.find(row.korg_ch); kit != matchByKorgCh.end()) {
                    mPtr = &kit->second;
                }

                if (mPtr && row.family == "Unidentified" &&
                    mPtr->profile.inferred.family != quality::TimbreFamily::Unknown &&
                    mPtr->profile.confidence >= 40u) {
                    row.family = quality::timbreFamilyName(mPtr->profile.inferred.family);
                }

                if (mPtr && mPtr->selected.has_value()) {
                    const SoundRef& sel = *mPtr->selected;
                    const bool isDrumKch = (row.korg_ch == kDrumCh || row.korg_ch == kPercCh);
                    const bool msbOk = isDrumKch ? (sel.msb == 120u) : (sel.msb == 121u);
                    if (!msbOk) ++rep.sound_match_stats.cross_family_contaminations;
                    if (mPtr->profile.confidence < 40u) ++rep.sound_match_stats.low_confidence_auto_selects;

                    std::string candName;
                    uint8_t candScore = 0;
                    std::string originLabel = (mPtr->selected_origin == quality::MatchOrigin::TargetKstUserSound)
                                                  ? "target-kst"
                                                  : "auto-factory";
                    for (const auto& c : mPtr->top_candidates) {
                        if (c.origin == mPtr->selected_origin &&
                            c.ref.msb == sel.msb && c.ref.lsb == sel.lsb && c.ref.pc == sel.pc) {
                            candName = c.display_name;
                            candScore = c.score;
                            if (c.target_slot.has_value()) {
                                originLabel += " " + c.target_slot->shortLabel();
                            }
                            break;
                        }
                    }

                    row.target = "MSB " + std::to_string(sel.msb) + " LSB " + std::to_string(sel.lsb) +
                                 " PC " + std::to_string(sel.pc) + " (" +
                                 (candName.empty() ? std::string("Pa5X Sound") : candName) + ", " +
                                 originLabel + ", score=" + std::to_string(candScore) + ", " +
                                 mPtr->selected_epistemic_tag + ")";

                    if (candScore >= 85u) ++rep.sound_match_stats.exact_matches;
                    else ++rep.sound_match_stats.approx_matches;
                    if (mPtr->selected_origin == quality::MatchOrigin::TargetKstUserSound) {
                        ++rep.sound_match_stats.target_kst_matches;
                    } else {
                        ++rep.sound_match_stats.factory_matches;
                    }
                } else {
                    row.target = "UNRESOLVED";
                    ++rep.sound_match_stats.unresolved;
                    if (row.pc >= 120 && row.pc <= 127 && (row.msb <= 0)) {
                        ++rep.sound_match_stats.unsupported;
                        addUniqueDiag(rep.quality_diagnostics, "STYLE_QUAL_SOUND_UNSUPPORTED_SFX");
                    } else {
                        ++rep.sound_match_stats.low_confidence_fallbacks;
                        addUniqueDiag(rep.quality_diagnostics, "STYLE_QUAL_SOUND_UNRESOLVED_LOW_CONFIDENCE");
                    }
                }
            } else {
                row.target = "UNRESOLVED";
                ++rep.sound_match_stats.unresolved;
            }
        }

        const bool drumTrack = row.korg_ch == kDrumCh || row.korg_ch == kPercCh;
        // Keys at C7+ are MegaVoice noise/FX keys on plucked parts; a part with
        // no instrument name is checked too. Velocity zones alone are ordinary
        // dynamics, so they only support a flag, never raise one.
        const bool plucked = row.family == "Guitar" || row.family == "Bass" || row.family == "Unidentified";
        int zones = 0;
        for (uint8_t z = row.velocity_zones; z; z &= z - 1) ++zones;
        std::string why;
        auto add = [&](const std::string& x) { why += (why.empty() ? "" : "; ") + x; };
        if (!drumTrack && row.chord_range_dropped)
            add(std::to_string(row.chord_range_dropped) + " notes in C-1..B-1 (dropped)");
        if (!drumTrack && plucked && row.high_notes)
            add(std::to_string(row.high_notes) + " notes at C7 or above (" + row.family + " part)");
        row.megavoice = !why.empty();
        if (row.megavoice && zones >= 3) add("velocities span " + std::to_string(zones) + " MegaVoice zones");
        row.megavoice_why = why;
        rep.tracks.push_back(row);
    }
    for (const auto& [kch, s] : opt.sounds)
        if (std::none_of(rep.tracks.begin(), rep.tracks.end(), [&](const TrackRow& t) { return t.korg_ch == kch; }))
            rep.warnings.push_back(std::string("sound map sets Korg ") + korgTrackName(kch) +
                                   " but no Genos part plays it (bank/PC still written)");
    if (opt.capture_soundmap_dump) {
        const std::string dumpName = opt.soundmap_style_name.empty() ? style.name : opt.soundmap_style_name;
        rep.soundmap_dump = quality::formatSoundMapTemplateWithRecommendations(
            rep, stage5StyleMatch, dumpName, stage5MatchOpt);
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
    if (!r.element_sounds.empty()) {
        bool anyAuto = false;
        bool anyUser = false;
        for (const auto& e : r.element_sounds) {
            if (e.find("auto match)") != std::string::npos) anyAuto = true;
            else anyUser = true;
        }
        const char* originDesc = (anyAuto && anyUser) ? "user map / auto match"
                               : anyAuto              ? "auto match"
                                                      : "user map";
        line(std::string("Element-specific Pa5X sounds (") + originDesc + ", in that element only):");
        for (const auto& e : r.element_sounds) line("  " + e);
        line("");
    }
    line("MegaVoice / articulation candidates (Yamaha articulations do not carry over; confirm by ear):");
    bool anyMega = false;
    for (const auto& t : r.tracks)
        if (t.megavoice) { anyMega = true; line("  Genos ch " + std::to_string(t.src_ch + 1) + " '" + t.name + "': " + t.megavoice_why); }
    if (!anyMega) line("  none");
    line("");
    line("Confirm on Pa5X (sound choice by ear; unresolved = no sound assigned):");
    for (const auto& t : r.tracks)
        if (t.korg_ch != 0xFF)
            line("  " + std::string(korgTrackName(t.korg_ch)) + " <- Genos ch " + std::to_string(t.src_ch + 1) + " '" +
                 t.name + "' (" + t.family + "): " + t.target);
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
    line("Quality pipeline (Stages 1-5; offline software-verified only, hardwareVerified=false):");
    line("  CV splits created: " + std::to_string(r.cv_splits_created) +
         ", shadowed tracks dropped: " + std::to_string(r.shadowed_tracks_dropped));
    line("  MegaVoice/S.Art: " + std::to_string(r.megavoice_tracks_sanitized) + " section-tracks sanitized, " +
         std::to_string(r.megavoice_fx_notes_stripped) + " FX/noise notes stripped, " +
         std::to_string(r.megavoice_vel_reshaped) + " velocities reshaped");
    line("  Drum remap: exact=" + std::to_string(r.drum_notes_remapped_exact) +
         ", approx=" + std::to_string(r.drum_notes_remapped_approx) +
         ", unsupported=" + std::to_string(r.drum_notes_unsupported) +
         ", Rhythm1/2 swapped=" + (r.rhythm_tracks_swapped ? "yes" : "no"));
    line("  Source-chord normalization: " + std::to_string(r.tracks_source_chord_normalized) +
         " track patterns normalized, " + std::to_string(r.register_violations_fixed) +
         " register/LIL violations fixed");
    line("  Sound matching (Stage 5, L3=" + r.sound_match_stats.l3_status + "): evaluated=" +
         std::to_string(r.sound_match_stats.tracks_evaluated) +
         ", user=" + std::to_string(r.sound_match_stats.user_overrides) +
         ", exact=" + std::to_string(r.sound_match_stats.exact_matches) +
         ", approx=" + std::to_string(r.sound_match_stats.approx_matches) +
         ", target_kst=" + std::to_string(r.sound_match_stats.target_kst_matches) +
         ", factory=" + std::to_string(r.sound_match_stats.factory_matches) +
         ", elem_switches=" + std::to_string(r.sound_match_stats.element_voice_switches) +
         ", low_conf_fallback=" + std::to_string(r.sound_match_stats.low_confidence_fallbacks) +
         ", unresolved=" + std::to_string(r.sound_match_stats.unresolved) +
         ", unsupported=" + std::to_string(r.sound_match_stats.unsupported));
    if (!r.track_edit_advice.empty()) {
        line("Pa5X Style Edit advice (per Korg track):");
        for (const auto& a : r.track_edit_advice) {
            line("  " + std::string(korgTrackName(a.korg_ch)) + ": NTT=" + a.pa5x_ntt_type + "/" +
                 a.pa5x_ntt_table + ", Wrap=" + std::to_string(a.wrap_around) +
                 ", KeyRange=" + std::to_string(a.key_range_low) + ".." + std::to_string(a.key_range_high) +
                 ", Retrigger=" + a.retrigger_mode);
        }
    }
    line("");
    line("Warnings:");
    if (r.warnings.empty()) line("  none");
    for (const auto& w : r.warnings) line("  " + w);
    return o;
}

} // namespace ai_arranger::exporters::korg
