#include "engine/uasf/validator.h"
#include "engine/uasf/format.h"
#include "engine/uasf/deserializer.h"

namespace ai_arranger::uasf {

ValidationResult UasfValidator::validate(const StyleDefinition& style) noexcept {
    issues_.clear();

    // ── Style-level checks ─────────────────────────────────────────
    if (style.name.empty()) {
        addIssue(ValidationIssue::WARNING, "Style has no name");
    }
    if (style.tempo_bpm == 0) {
        addIssue(ValidationIssue::ERROR, "Tempo must be > 0");
    }
    if (style.tempo_bpm > 300) {
        addIssue(ValidationIssue::WARNING, "Tempo > 300 BPM may cause playback issues");
    }
    // Upper bound = the highest real source PPQN the importer preserves (Yamaha
    // Genos/PSR styles are 1920). It was 960, which rejected every real Genos
    // import. NOTE: uasf-spec-v0.9 §"ppq" still says "normalise to 480"; the
    // implementation keeps source resolution — open decision, see
    // docs/PROJECT_REVIEW_AND_CONTINUATION_PLAN.md.
    if (style.resolution == 0 || style.resolution > 1920) {
        addIssue(ValidationIssue::ERROR, "Resolution must be 1-1920 ticks/quarter");
    }
    if (style.sections.empty()) {
        addIssue(ValidationIssue::ERROR, "Style must have at least one section");
    }

    // ── Section-level checks ───────────────────────────────────────
    for (size_t s = 0; s < style.sections.size(); ++s) {
        const auto& section = style.sections[s];
        std::string loc = "Section " + std::to_string(s + 1);

        if (section.bars == 0) {
            addIssue(ValidationIssue::ERROR, "Section has 0 bars", loc);
        }
        if (section.bars > 256) {
            addIssue(ValidationIssue::WARNING, "Section has > 256 bars", loc);
        }
        if (section.resolution == 0) {
            addIssue(ValidationIssue::ERROR, "Section resolution is 0", loc);
        }
        if (section.beats_per_bar == 0) {
            addIssue(ValidationIssue::ERROR, "Beats per bar = 0", loc);
        }

        if (section.tracks.empty()) {
            addIssue(ValidationIssue::WARNING, "Section has no tracks", loc);
        }

        // ── Track-level checks ─────────────────────────────────────
        for (size_t t = 0; t < section.tracks.size(); ++t) {
            const auto& track = section.tracks[t];
            std::string trackLoc = loc + " / Track " + std::to_string(t + 1);

            if (track.midi_channel > 15) {
                addIssue(ValidationIssue::ERROR, "MIDI channel invalid (> 15)", trackLoc);
            }
            if (track.is_drum && track.midi_channel != 9) {
                addIssue(ValidationIssue::WARNING, "Drum track not on channel 10 (9)", trackLoc);
            }
            if (track.events.empty()) {
                addIssue(ValidationIssue::WARNING, "Track has no events", trackLoc);
            }
            if (track.events.size() > format::kMaxEvents) {
                addIssue(ValidationIssue::ERROR, "Track exceeds max event count", trackLoc);
            }

            // Check for orphaned NoteOn events
            int noteOnCount = 0;
            int noteOffCount = 0;
            for (const auto& ev : track.events) {
                if (ev.type == MidiEventType::NoteOn && ev.data2 > 0) noteOnCount++;
                if (ev.type == MidiEventType::NoteOff ||
                    (ev.type == MidiEventType::NoteOn && ev.data2 == 0)) noteOffCount++;
            }
            if (noteOnCount != noteOffCount && section.type >= SectionType::Main1) {
                addIssue(ValidationIssue::WARNING,
                         "Unmatched NoteOn/Off: " + std::to_string(noteOnCount) +
                         " vs " + std::to_string(noteOffCount), trackLoc);
            }

            // Check articulation metadata
            if (track.articulation.profile == ArticulationProfile::MegaLike &&
                track.articulation.fidelity == FidelityRequirement::Low) {
                addIssue(ValidationIssue::WARNING,
                         "MegaLike articulation mapped to Low fidelity — audible degradation",
                         trackLoc);
            }
        }
    }

    bool hasErrors = false;
    for (const auto& issue : issues_) {
        if (issue.severity == ValidationIssue::ERROR) {
            hasErrors = true;
            break;
        }
    }

    return {!hasErrors, std::move(issues_)};
}

ValidationResult UasfValidator::validateBinary(const std::vector<uint8_t>& data) noexcept {
    issues_.clear();

    UasfDeserializer deserializer;
    auto result = deserializer.deserialize(data);
    if (!result.success) {
        addIssue(ValidationIssue::ERROR, "Binary parse failed: " + result.error);
        return {false, std::move(issues_)};
    }

    return validate(result.style);
}

void UasfValidator::addIssue(ValidationIssue::Severity sev, const std::string& msg,
                              const std::string& loc) noexcept {
    issues_.push_back({sev, msg, loc});
}

} // namespace ai_arranger::uasf
