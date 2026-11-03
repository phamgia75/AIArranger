#include "importers/sff1/sff1_report.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace ai_arranger::importers::sff1 {

CompatibilityScore Sff1ReportGenerator::computeScore(const ParseResult& result) noexcept {
    CompatibilityScore score{};

    score.total_chunks = result.total_chunks;
    score.known_chunks = result.total_chunks - result.unknown_chunks;
    score.unknown_chunks = result.unknown_chunks;
    score.parsed_sections = result.parsed_sections;
    score.total_events = result.parsed_events;

    // Parse success: at least one known chunk found
    score.parse_success = (result.success && score.total_chunks > 0) ? 1.0 : 0.0;

    // Structural fidelity: ratio of known to unknown chunks
    if (score.total_chunks > 0) {
        score.structural_fidelity = static_cast<double>(score.known_chunks) /
                                     static_cast<double>(score.total_chunks);
    }

    // Event parse rate: sections with events / total sections
    uint32_t sectionsWithEvents = 0;
    for (const auto& section : result.sections) {
        for (const auto& track : section.tracks) {
            if (!track.events.empty()) {
                sectionsWithEvents++;
                break;
            }
        }
    }
    if (result.sections.size() > 0) {
        score.event_parse_rate = static_cast<double>(sectionsWithEvents) /
                                 static_cast<double>(result.sections.size());
    }

    // Detect MegaVoice candidates (tracks with dense velocity variation)
    for (const auto& section : result.sections) {
        for (const auto& track : section.tracks) {
            if (track.events.empty()) continue;
            std::vector<uint8_t> velocities;
            for (const auto& ev : track.events) {
                if ((ev.status & 0xF0) == 0x90 && ev.data2 > 0) {
                    velocities.push_back(ev.data2);
                }
            }
            if (velocities.size() > 10) {
                // Check velocity range
                auto [min, max] = std::minmax_element(velocities.begin(), velocities.end());
                if (*max - *min > 60) {
                    score.megavoice_candidates++;
                }
            }
        }
    }

    // ── Harmonic / arranger fidelity from CASM (Gate 7) ────────────────
    score.casm_sections = static_cast<uint32_t>(result.casm_sections.size());
    score.casm_tracks   = static_cast<uint32_t>(result.casm_configs.size());

    uint32_t chordAware = 0;
    for (const auto& cfg : result.casm_configs) {
        if (cfg.ntt_bass) score.bass_tracks++;
        // A track follows the chord when it has a non-bypass transposition
        // table or the bass-note flag. Pure drum tracks (NTT bypass, no bass)
        // do not, and are excluded from harmonic coverage.
        if (cfg.ntt != 0 || cfg.ntt_bass) chordAware++;
    }
    if (score.casm_tracks > 0) {
        score.harmonic_fidelity = static_cast<double>(chordAware) /
                                  static_cast<double>(score.casm_tracks);
    }

    score.unsupported = result.unsupported_features;

    return score;
}

std::string Sff1ReportGenerator::generateReport(const ParseResult& result) noexcept {
    auto score = computeScore(result);
    std::ostringstream ss;

    ss << "═══════════════════════════════════════════════\n";
    ss << "  SFF1 Parser Report\n";
    ss << "  File: " << result.file_path << "\n";
    ss << "═══════════════════════════════════════════════\n\n";

    ss << "  Parse: " << (result.success ? "SUCCESS" : "FAILED") << "\n";
    if (!result.success) {
        ss << "  Error: " << result.error << "\n\n";
        return ss.str();
    }

    ss << "  Total chunks:  " << score.total_chunks << "\n";
    ss << "  Known chunks:  " << score.known_chunks << "\n";
    ss << "  Unknown chunks: " << score.unknown_chunks << "\n";
    ss << "  Sections:      " << score.parsed_sections << "\n";
    ss << "  CASM sections: " << result.sections_parsed.size() << "\n";
    ss << "  Events:        " << score.total_events << "\n";
    ss << "  MegaVoice candidates: " << score.megavoice_candidates << "\n";
    ss << "  CASM sections: " << score.casm_sections << "\n";
    ss << "  CASM tracks:   " << score.casm_tracks
       << " (" << score.bass_tracks << " bass)\n\n";

    // CASM arranger section listing (from Sdec/Ctb2)
    if (!result.casm_sections.empty()) {
        ss << "  Arranger sections (CASM):\n";
        for (const auto& cs : result.casm_sections) {
            ss << "    " << cs.name << " (" << cs.tracks.size() << " tracks)\n";
        }
        ss << "\n";
    }

    // Chunk listing
    ss << "  Chunks:\n";
    for (const auto& chunk : result.chunks) {
        ss << "    [" << chunk.chunk_id << "] " << chunk.size << " bytes\n";
    }
    ss << "\n";

    // Section listing
    if (!result.sections.empty()) {
        ss << "  Sections:\n";
        for (size_t i = 0; i < result.sections.size(); ++i) {
            const auto& section = result.sections[i];
            uint32_t trackEvents = 0;
            for (const auto& t : section.tracks) {
                for (const auto& e : t.events) trackEvents++;
            }
            ss << "    Section " << i << ": " << section.name
               << " (" << section.tracks.size() << " tracks, "
               << trackEvents << " events)\n";
        }
        ss << "\n";
    }

    // Warnings
    if (!result.warnings.empty()) {
        ss << "  Warnings:\n";
        for (const auto& w : result.warnings) {
            ss << "    [WARN] " << w << "\n";
        }
        ss << "\n";
    }

    // Unsupported
    if (!result.unsupported_features.empty()) {
        ss << "  Unsupported features:\n";
        for (const auto& u : result.unsupported_features) {
            ss << "    [REJECTED] " << u << "\n";
        }
        ss << "\n";
    }

    // Compatibility score
    ss << generateCompatibilityTable(score);

    return ss.str();
}

std::string Sff1ReportGenerator::generateCompatibilityTable(const CompatibilityScore& score) noexcept {
    std::ostringstream ss;
    ss << "  ┌──────────────────────────────┬───────┐\n";
    ss << "  │ Metric                       │ Score │\n";
    ss << "  ├──────────────────────────────┼───────┤\n";

    auto fmt = [](double v) -> std::string {
        std::ostringstream s;
        s << std::fixed << std::setprecision(1) << (v * 100.0) << "%";
        return s.str();
    };

    ss << "  │ Parse success                │ " << std::setw(5) << fmt(score.parse_success) << " │\n";
    ss << "  │ Structural fidelity          │ " << std::setw(5) << fmt(score.structural_fidelity) << " │\n";
    ss << "  │ Event parse rate             │ " << std::setw(5) << fmt(score.event_parse_rate) << " │\n";
    ss << "  │ Harmonic fidelity (CASM)     │ " << std::setw(5) << fmt(score.harmonic_fidelity) << " │\n";
    ss << "  └──────────────────────────────┴───────┘\n";

    return ss.str();
}

} // namespace ai_arranger::importers::sff1
