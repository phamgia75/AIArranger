#ifndef AI_ARRANGER_EXPORTERS_GENOS_TO_KORG_H
#define AI_ARRANGER_EXPORTERS_GENOS_TO_KORG_H

#include "engine/uasf/types.h"
#include "exporters/korg/drum_remap.h"
#include "exporters/korg/korg_style_smf.h"
#include "exporters/korg/megavoice_sanitizer.h"
#include "exporters/korg/source_chord_normalizer.h"

#include <map>
#include <string>
#include <tuple>
#include <vector>

// Canonical (UASF) style -> Korg Pa5X style semantic model. Input is the
// vendor-neutral UASF model, never raw Genos bytes. Every lossy decision is
// counted in the Report; nothing is dropped silently.

namespace ai_arranger::exporters::korg {

namespace quality {
struct TargetSoundEntry;
struct TargetSoundCatalogView;
} // namespace quality

struct SoundRef { uint8_t msb = 0, lsb = 0, pc = 0; };

enum class BreakPolicy : uint8_t {
    Withhold = 0,       // Legacy default: withhold Fill In BA (keeps legacy direct callers/tests unchanged)
    Yamaha1BarCap = 1,  // Emit validated 1-bar "br" element, truncating post-bar-1 residue (#26)
};

struct MapOptions {
    uint16_t ppqn = 0;                    // 0 = keep the source resolution
    std::map<uint8_t, uint8_t> route;     // source channel -> CASM destination channel (0-based)
    std::map<uint8_t, SoundRef> sounds;   // Korg channel (0-based) -> user-supplied Pa5X sound
    // (section type, Korg channel) -> that element's sound, written instead of `sounds` in that element only.
    std::map<std::pair<uasf::SectionType, uint8_t>, SoundRef> element_sounds;
    BreakPolicy break_policy = BreakPolicy::Withhold;

    // ── Wave-2B Q23-Wire & Wave-3 Q4-Wire Quality Pipeline Controls (enabled by default; use raw() for legacy passthrough) ──
    bool enable_cv_split = true;                                    // Stage 1: CASM chord-mute -> Pa5X CV1..CV6
    bool enable_drum_remap = true;                                  // Stage 2: Rhythm1/2 swap + XG/Revo! drum remap
    quality::MegaVoiceMode megavoice_mode = quality::MegaVoiceMode::Auto; // Stage 3: MegaVoice/S.Art/Guitar
    bool enable_source_chord_normalization = true;                  // Stage 4: Ctb2[18..19] -> C Maj/Min + register audit
    bool enable_sound_matching = true;                              // Stage 5: TimbreProfiler + Pa5XSoundMatcher (fail-closed <40 / <min_score)
    bool prefer_target_user_sounds = false;                         // Stage 5: prefer in-spec read-only Target KST User sounds when supplied
    uint8_t min_sound_match_score = 70;                             // Stage 5: fail-closed minimum auto-select score (0..100)
    const quality::IDrumKitKeyMap* target_drum_key_map = nullptr;   // Optional read-only target kit for kDrumCh ONLY (nullptr -> lookup/Factory)
    const quality::IDrumKitKeyMap* target_perc_key_map = nullptr;   // Optional read-only target kit for kPercCh ONLY (nullptr -> lookup/Factory)
    const std::map<std::tuple<uint8_t, uint8_t, uint8_t>, quality::UserdkDrumKitKeyMap>* target_drum_key_maps = nullptr; // Optional read-only (msb,lsb,pc)->UserdkDrumKitKeyMap lookup (Track A bridge)
    const quality::TargetSoundCatalogView* target_sound_catalog = nullptr; // Optional read-only Target KST catalog view
    const std::vector<quality::TargetSoundEntry>* target_sounds = nullptr; // Optional direct read-only Target KST sound entries
    bool capture_soundmap_dump = false;                             // When true, populate Report::soundmap_dump from exact Stage-5 matches
    std::string soundmap_style_name;                                // Display filename for Report::soundmap_dump (basename only)

    // Default quality pipeline (Stages 1–5 enabled).
    static MapOptions quality() noexcept {
        return MapOptions{};
    }

    // Raw passthrough mode: disables Q2–Q4 musical transforms while keeping structural mapping intact.
    static MapOptions raw() noexcept {
        MapOptions o;
        o.enable_cv_split = false;
        o.enable_drum_remap = false;
        o.megavoice_mode = quality::MegaVoiceMode::Preserve;
        o.enable_source_chord_normalization = false;
        o.enable_sound_matching = false;
        o.prefer_target_user_sounds = false;
        return o;
    }
};

struct SectionRow {
    std::string source;     // UASF section name ("Main A", "Fill In BA", ...)
    std::string marker;     // Korg marker, empty = not exported
    uint32_t bars = 0;
    std::string note;       // why it was not exported
};

struct TrackRow {
    uint8_t src_ch = 0;
    uint8_t korg_ch = 0xFF;   // 0xFF = not exported
    std::string name;
    uasf::TrackRole role = uasf::TrackRole::Accompaniment;
    uint8_t ntr = 0, ntt = 0;
    int msb = -1, lsb = -1, pc = -1;   // first Yamaha voice select seen
    std::string family;                // instrument family hint
    std::string target;                // Pa5X sound, or UNRESOLVED
    // MegaVoice heuristic (Yamaha velocity/key-switched articulations, which a
    // Pa5X sound does not reproduce). A candidate needs confirmation by ear.
    uint32_t chord_range_dropped = 0;  // notes 0..11 removed from this part
    uint32_t high_notes = 0;           // notes >= 96 (C7) on a non-drum part
    uint8_t velocity_zones = 0;        // bitmask of Mega zones 1-64/65-90/91-100/101-127
    bool megavoice = false;
    std::string megavoice_why;
};

struct SoundMatchStats {
    uint32_t tracks_evaluated = 0;            // Exported Korg tracks evaluated by Stage 5
    uint32_t user_overrides = 0;              // Tracks resolved via explicit user --sound-map (highest precedence)
    uint32_t exact_matches = 0;               // Auto-matched exact high-specificity sound (score >= 85)
    uint32_t approx_matches = 0;              // Auto-matched family/signal-inferred sound (min_score <= score < 85)
    uint32_t target_kst_matches = 0;          // Subset of exact/approx selected from read-only Target KST User slots
    uint32_t factory_matches = 0;             // Subset of exact/approx selected from curated Pa5X Factory catalog
    uint32_t element_voice_switches = 0;      // Unique (section, korg_ch) mid-style voice switches emitted
    uint32_t low_confidence_fallbacks = 0;    // Tracks rejected fail-closed due to confidence < 40 or score < threshold
    uint32_t unresolved = 0;                  // Exported tracks left UNRESOLVED
    uint32_t unsupported = 0;                 // Out-of-spec Target KST slots skipped (e.g. USERDK07..12) or unmappable SFX
    uint32_t cross_family_contaminations = 0; // Invariant audit: melodic<->drum cross-assignments (always 0)
    uint32_t low_confidence_auto_selects = 0; // Invariant audit: auto-selections with confidence < 40 (always 0)
    bool     hardware_verified = false;       // Always false offline (epistemic boundary)
    std::string l3_status = "NOT_TESTED";     // Always NOT_TESTED offline
};

struct Report {
    std::vector<SectionRow> sections;
    std::vector<TrackRow> tracks;
    std::map<std::string, uint32_t> dropped;       // reason -> event count
    std::map<uint8_t, uint32_t> non_gm_drum_notes;  // drum/perc note outside GM 35..81 -> count
    std::vector<std::string> warnings;
    std::vector<std::string> element_sounds;       // "marker Track: msb/lsb/pc" written for one element only

    // ── Wave-2B Q23-Wire Structured Quality Counters & Pa5X Style Edit Advice ──
    uint32_t cv_splits_created = 0;                // additional CV elements emitted beyond cv1
    uint32_t shadowed_tracks_dropped = 0;          // mutually exclusive tracks dropped when CV slots exhausted
    uint32_t megavoice_tracks_sanitized = 0;       // section-tracks modified by MegaVoice/S.Art/Guitar sanitization
    uint32_t megavoice_fx_notes_stripped = 0;      // FX/key-switch/noise-zone notes stripped (excluding 0..11 chord range)
    uint32_t megavoice_vel_reshaped = 0;           // notes whose velocity zone curve was reshaped
    uint32_t drum_notes_remapped_exact = 0;        // drum notes kept 1:1 on exact populated GM1/GM2/native key
    uint32_t drum_notes_remapped_approx = 0;       // extended XG/Revo! notes remapped to family-preserving approximate/fallback key
    uint32_t drum_notes_unsupported = 0;           // unmappable extended/SFX drum hits dropped cleanly
    bool     rhythm_tracks_swapped = false;        // true iff Rhythm1 (ch8) & Rhythm2 (ch9) were swapped to keep main kit on Drum
    uint32_t tracks_source_chord_normalized = 0;   // (section, cv, track) patterns normalized from non-C / non-target chord
    uint32_t register_violations_fixed = 0;        // 12-root register / LIL mud violations fixed during normalization
    std::vector<quality::Pa5xTrackEditAdvice> track_edit_advice; // per-Korg-track Pa5X Style Edit recommendations
    std::vector<std::string> quality_diagnostics;  // deduplicated STYLE_QUAL_* diagnostic codes emitted

    // ── Wave-3 Q4-Wire Sound Matching Summary & Resolved Assignments ──
    SoundMatchStats sound_match_stats;
    std::map<uint8_t, SoundRef> resolved_sounds;
    std::map<std::pair<uasf::SectionType, uint8_t>, SoundRef> resolved_element_sounds;
    std::string soundmap_dump; // Populated when MapOptions::capture_soundmap_dump == true (exact Stage-5 single source of truth)
};

struct MapResult {
    bool ok = false;
    std::string error;
    Style style;
    Report report;
};

const char* korgTrackName(uint8_t korgCh) noexcept;
// Genos style part channel (0-based, after CASM routing) -> Korg track channel; 0xFF = none.
uint8_t korgChannelFor(uint8_t genosCh) noexcept;
// Korg marker for a canonical section, or nullptr when the Korg spec has no code for it.
const char* korgMarkerFor(uasf::SectionType t) noexcept;
const char* korgMarkerFor(uasf::SectionType t, BreakPolicy policy) noexcept;

MapResult mapToKorg(const uasf::StyleDefinition& style, const MapOptions& opt);

// Human-readable conversion report (section map, tracks, loss).
std::string formatReport(const Report& r, const Style& s);

// Drop-reason keys (stable strings; tests and the report use them).
inline constexpr const char* kDropSection = "section has no Korg marker code";
inline constexpr const char* kDropChannel = "channel outside the 8 Korg style tracks";
inline constexpr const char* kDropVoiceSelect = "Yamaha bank select / program change";
inline constexpr const char* kDropChordRange = "note C-1..B-1 on a chord-driven track";
inline constexpr const char* kDropLastTick = "note starts in the last tick of the section";
inline constexpr const char* kDropPastEnd = "event at or after the section end";
inline constexpr const char* kDropOrphanOff = "NoteOff without a NoteOn";
inline constexpr const char* kDropBreakTail = "post-bar-1 Break residue truncated (Yamaha 1-bar cap)";
inline constexpr const char* kDiagBreakTailTruncated = "STYLE_QUAL_BREAK_TAIL_TRUNCATED";

} // namespace ai_arranger::exporters::korg

#endif // AI_ARRANGER_EXPORTERS_GENOS_TO_KORG_H
