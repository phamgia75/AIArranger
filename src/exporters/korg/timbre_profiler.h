#ifndef AI_ARRANGER_EXPORTERS_KORG_TIMBRE_PROFILER_H
#define AI_ARRANGER_EXPORTERS_KORG_TIMBRE_PROFILER_H

#include "engine/uasf/types.h"
#include "exporters/korg/genos_to_korg.h"
#include "exporters/korg/korg_style_smf.h"
#include "exporters/korg/megavoice_sanitizer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// Track B Milestone Q4a: Standalone, header-only multi-signal Yamaha Genos/XG
// timbre profiler & per-element voice-change extractor.
//
// Boundaries & Invariants:
//   * Portable C++20 core only: zero JUCE/GUI dependency, zero src/media/ dependency.
//   * 100% deterministic, offline, strictly read-only analysis.
//   * Zero private owner names, bytes, or local file paths.
//   * Safe word/token-boundary keyword matching (no mid-word substring traps).

namespace ai_arranger::exporters::korg::quality {

enum class TimbreFamily : uint8_t {
    Unknown = 0,
    Piano,
    ElectricPiano,
    Organ,
    Accordion,
    AcousticGuitar,
    ElectricGuitar,
    PluckedStrings,
    AcousticBass,
    ElectricBass,
    SynthBass,
    Strings,
    ChoirPad,
    SynthPad,
    Brass,
    SaxReed,
    FlutePipe,
    SynthLead,
    MalletPerc,
    EthnicMelodic,
    DrumKit,
    PercussionKit
};

inline constexpr uint8_t kTimbreFamilyCount = 22;

inline constexpr const char* timbreFamilyName(TimbreFamily family) noexcept {
    switch (family) {
        case TimbreFamily::Unknown:        return "Unidentified";
        case TimbreFamily::Piano:          return "Piano";
        case TimbreFamily::ElectricPiano:  return "Electric Piano";
        case TimbreFamily::Organ:          return "Organ";
        case TimbreFamily::Accordion:      return "Accordion";
        case TimbreFamily::AcousticGuitar: return "Acoustic Guitar";
        case TimbreFamily::ElectricGuitar: return "Electric Guitar";
        case TimbreFamily::PluckedStrings: return "Plucked Strings";
        case TimbreFamily::AcousticBass:   return "Acoustic Bass";
        case TimbreFamily::ElectricBass:   return "Electric Bass";
        case TimbreFamily::SynthBass:      return "Synth Bass";
        case TimbreFamily::Strings:        return "Strings";
        case TimbreFamily::ChoirPad:       return "Choir / Vocal Pad";
        case TimbreFamily::SynthPad:       return "Synth Pad";
        case TimbreFamily::Brass:          return "Brass";
        case TimbreFamily::SaxReed:        return "Sax / Reed";
        case TimbreFamily::FlutePipe:      return "Flute / Pipe";
        case TimbreFamily::SynthLead:      return "Synth Lead";
        case TimbreFamily::MalletPerc:     return "Mallet / Chromatic Perc";
        case TimbreFamily::EthnicMelodic:  return "Ethnic Melodic";
        case TimbreFamily::DrumKit:        return "Drum Kit";
        case TimbreFamily::PercussionKit:  return "Percussion Kit";
    }
    return "Unidentified";
}

inline constexpr bool isPercussiveTimbreFamily(TimbreFamily family) noexcept {
    return family == TimbreFamily::DrumKit || family == TimbreFamily::PercussionKit;
}

inline constexpr uint32_t timbreFamilyBit(TimbreFamily family) noexcept {
    return 1u << static_cast<uint8_t>(family);
}

struct TimbreSubFamily {
    TimbreFamily family = TimbreFamily::Unknown;
    std::string  sub_family;         // e.g. "Steel Acoustic Guitar", "Finger Electric Bass", "Rhodes EP", "Brush Kit"
    bool         prefers_dnc = false; // true when source was S.Art/S.Art2/MegaVoice/Revo!
};

struct TrackTimbreProfile {
    uint8_t         src_ch = 0;
    uint8_t         korg_ch = 0xFF;
    int             yamaha_msb = -1;
    int             yamaha_lsb = -1;
    int             yamaha_pc  = -1;
    TimbreSubFamily inferred;
    uint8_t         confidence = 0;          // 0..100
    // Signal features (computed deterministically over track notes):
    uint8_t         pitch_min    = 0;
    uint8_t         pitch_median = 60;
    uint8_t         pitch_max    = 127;
    uint8_t         max_polyphony = 0;       // 0=empty, 1=mono bass/lead, 3..6=chord/guitar/pad
    double          mean_duration_beats = 0.0; // <0.4 staccato/plucked, >1.5 pad/strings
    double          notes_per_bar       = 0.0;
    bool            has_strum_offsets   = false;
    uint32_t        note_count          = 0;
    // Per-element mid-style voice changes (prevents first-wins drop across sections):
    std::map<uasf::SectionType, std::tuple<int, int, int>> element_yamaha_voices;
    std::string     rationale;
};

struct TimbreProfileOptions {
    uint32_t ppqn       = 480;    // ticks per quarter note (used for duration & strum timing)
    uint32_t tempo_mpqn = 500000; // microseconds per quarter note (120 BPM default)
    uint32_t bar_ticks  = 0;      // 0 -> default 4 * ppqn
    uint32_t bars       = 0;      // 0 -> inferred from note extent
};

namespace timbre_detail {

inline bool isAsciiAlpha(char c) noexcept {
    const unsigned char uc = static_cast<unsigned char>(c);
    return (uc >= 'A' && uc <= 'Z') || (uc >= 'a' && uc <= 'z');
}

inline bool isAsciiDigit(char c) noexcept {
    const unsigned char uc = static_cast<unsigned char>(c);
    return uc >= '0' && uc <= '9';
}

inline bool isAsciiAlnum(char c) noexcept {
    return isAsciiAlpha(c) || isAsciiDigit(c);
}

inline char lowerAsciiChar(char c) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

inline bool isAsciiUpper(char c) noexcept {
    const unsigned char uc = static_cast<unsigned char>(c);
    return uc >= 'A' && uc <= 'Z';
}

inline bool isAsciiLower(char c) noexcept {
    const unsigned char uc = static_cast<unsigned char>(c);
    return uc >= 'a' && uc <= 'z';
}

// Tokenizes an identifier safely along non-alphanumeric separators AND CamelCase
// boundaries, while also preserving dotted abbreviations (e.g. "e.p", "e.piano",
// "s.art", "s.art2", "e.gtr", "a.gtr", "e.bass", "a.bass").
// Never performs arbitrary mid-word substring matching: "tom" will never match
// "bottom", and "harp" will never match "harpsichord" or "harpsi".
inline std::vector<std::string> tokenizeSafeWords(std::string_view input) {
    std::vector<std::string> tokens;
    auto pushUnique = [&](std::string tok) {
        if (tok.empty()) return;
        if (std::find(tokens.begin(), tokens.end(), tok) == tokens.end()) {
            tokens.push_back(std::move(tok));
        }
    };
    // For alphanumeric tokens matching [a-z]+[0-9]+ (e.g. "piano1", "organ1", "guitar1"),
    // also emit the alphabetic prefix (len >= 2) while preserving the full token
    // ("dx7", "b3", "acc1", "chord1", "phrase1").
    auto pushWithAlphaPrefix = [&](std::string tok) {
        if (tok.empty()) return;
        size_t alpha_len = 0;
        while (alpha_len < tok.size() && isAsciiLower(tok[alpha_len])) ++alpha_len;
        bool trailing_digits = (alpha_len >= 2 && alpha_len < tok.size());
        if (trailing_digits) {
            for (size_t d = alpha_len; d < tok.size(); ++d) {
                if (!isAsciiDigit(tok[d])) { trailing_digits = false; break; }
            }
        }
        std::string alpha_prefix = trailing_digits ? tok.substr(0, alpha_len) : std::string{};
        pushUnique(std::move(tok));
        if (!alpha_prefix.empty()) pushUnique(std::move(alpha_prefix));
    };

    // Pass 1: preserve dotted/alnum chunks separated by spaces/slashes/underscores/etc.
    size_t i = 0;
    const size_t n = input.size();
    while (i < n) {
        while (i < n && !isAsciiAlnum(input[i])) ++i;
        if (i >= n) break;
        const size_t start = i;
        while (i < n && (isAsciiAlnum(input[i]) || input[i] == '.')) ++i;
        size_t end = i;
        while (end > start && input[end - 1] == '.') --end;
        const std::string_view rawChunk = input.substr(start, end - start);

        std::string lowerChunk;
        lowerChunk.reserve(rawChunk.size());
        for (char c : rawChunk) lowerChunk.push_back(lowerAsciiChar(c));
        pushWithAlphaPrefix(std::move(lowerChunk));

        // Pass 2: split chunk on '.' into sub-chunks and split CamelCase words
        size_t p = 0;
        while (p < rawChunk.size()) {
            while (p < rawChunk.size() && rawChunk[p] == '.') ++p;
            const size_t subStart = p;
            while (p < rawChunk.size() && rawChunk[p] != '.') ++p;
            const std::string_view sub = rawChunk.substr(subStart, p - subStart);
            if (sub.empty()) continue;

            std::string lowerSub;
            lowerSub.reserve(sub.size());
            for (char c : sub) lowerSub.push_back(lowerAsciiChar(c));
            pushWithAlphaPrefix(std::move(lowerSub));

            // Split CamelCase boundaries inside sub (e.g. "AcousticGuitar" -> "acoustic", "guitar",
            // "FingerBass" -> "finger", "bass", "BottomEnd" -> "bottom", "end").
            size_t wStart = 0;
            for (size_t k = 1; k < sub.size(); ++k) {
                const bool prevLower = isAsciiLower(sub[k - 1]);
                const bool curUpper  = isAsciiUpper(sub[k]);
                const bool nextLower = (k + 1 < sub.size()) && isAsciiLower(sub[k + 1]);
                const bool acronymEnd = isAsciiUpper(sub[k - 1]) && curUpper && nextLower;
                if ((prevLower && curUpper) || acronymEnd) {
                    std::string word;
                    word.reserve(k - wStart);
                    for (size_t j = wStart; j < k; ++j) word.push_back(lowerAsciiChar(sub[j]));
                    pushWithAlphaPrefix(std::move(word));
                    wStart = k;
                }
            }
            if (wStart > 0 && wStart < sub.size()) {
                std::string word;
                word.reserve(sub.size() - wStart);
                for (size_t j = wStart; j < sub.size(); ++j) word.push_back(lowerAsciiChar(sub[j]));
                pushWithAlphaPrefix(std::move(word));
            }
        }
    }
    return tokens;
}

inline bool hasToken(const std::vector<std::string>& tokens, std::string_view exact) noexcept {
    for (const auto& t : tokens) {
        if (t == exact) return true;
    }
    return false;
}

inline bool hasAnyToken(const std::vector<std::string>& tokens,
                        std::initializer_list<const char*> candidates) noexcept {
    for (const char* c : candidates) {
        if (hasToken(tokens, c)) return true;
    }
    return false;
}

inline bool isGenericTrackName(const std::vector<std::string>& tokens) noexcept {
    if (tokens.empty()) return true;
    static constexpr std::array<const char*, 21> kGeneric = {
        "chord", "chord1", "chord2", "chord3", "chord4",
        "phrase", "phrase1", "phrase2",
        "part", "part1", "part2", "part14",
        "track", "channel", "ch",
        "acc", "acc1", "acc2", "acc3", "acc4", "acc5"
    };
    for (const auto& t : tokens) {
        bool isGen = false;
        for (const char* g : kGeneric) {
            if (t == g) { isGen = true; break; }
        }
        if (isGen) continue;
        // Pure digits (e.g. "Channel 8" -> "channel", "8") are also generic.
        bool allDigits = true;
        for (char c : t) {
            if (!isAsciiDigit(c)) { allDigits = false; break; }
        }
        if (allDigits) continue;
        return false;
    }
    return true;
}

inline bool nameSuggestsDncOrArticulation(const std::vector<std::string>& tokens) noexcept {
    return hasAnyToken(tokens, {
        "dnc", "dn1", "dn2", "dn3", "s.art", "s.art2", "sart", "sa2",
        "mega", "megavoice", "revo", "superart", "rx"
    });
}

// Complete GM / XG Program Change (0..127) -> TimbreSubFamily mapping.
inline TimbreSubFamily classifyGmProgramChange(int pc) {
    if (pc < 0 || pc > 127) return {};
    switch (pc) {
        // 0..7 Piano / EP / Harpsichord / Clavinet
        case 0:  return {TimbreFamily::Piano, "Acoustic Grand Piano", false};
        case 1:  return {TimbreFamily::Piano, "Bright Acoustic Piano", false};
        case 2:  return {TimbreFamily::Piano, "Electric Grand Piano", false};
        case 3:  return {TimbreFamily::Piano, "Honky-Tonk Piano", false};
        case 4:  return {TimbreFamily::ElectricPiano, "Rhodes EP", false};
        case 5:  return {TimbreFamily::ElectricPiano, "DX / FM EP", false};
        case 6:  return {TimbreFamily::Piano, "Harpsichord", false};
        case 7:  return {TimbreFamily::ElectricPiano, "Clavinet", false};

        // 8..15 Chromatic Percussion / Mallet
        case 8:  return {TimbreFamily::MalletPerc, "Celesta", false};
        case 9:  return {TimbreFamily::MalletPerc, "Glockenspiel", false};
        case 10: return {TimbreFamily::MalletPerc, "Music Box", false};
        case 11: return {TimbreFamily::MalletPerc, "Vibraphone", false};
        case 12: return {TimbreFamily::MalletPerc, "Marimba", false};
        case 13: return {TimbreFamily::MalletPerc, "Xylophone", false};
        case 14: return {TimbreFamily::MalletPerc, "Tubular Bells", false};
        case 15: return {TimbreFamily::MalletPerc, "Dulcimer", false};

        // 16..20 Organ
        case 16: return {TimbreFamily::Organ, "Drawbar Organ", false};
        case 17: return {TimbreFamily::Organ, "Percussive Organ", false};
        case 18: return {TimbreFamily::Organ, "Rock Organ", false};
        case 19: return {TimbreFamily::Organ, "Church Organ", false};
        case 20: return {TimbreFamily::Organ, "Reed Organ", false};

        // 21..23 Accordion / Harmonica
        case 21: return {TimbreFamily::Accordion, "Accordion", false};
        case 22: return {TimbreFamily::Accordion, "Harmonica", false};
        case 23: return {TimbreFamily::Accordion, "Tango Accordion / Bandoneon", false};

        // 24..25 Acoustic Guitar
        case 24: return {TimbreFamily::AcousticGuitar, "Nylon Acoustic Guitar", false};
        case 25: return {TimbreFamily::AcousticGuitar, "Steel Acoustic Guitar", false};

        // 26..31 Electric Guitar
        case 26: return {TimbreFamily::ElectricGuitar, "Jazz Electric Guitar", false};
        case 27: return {TimbreFamily::ElectricGuitar, "Clean Electric Guitar", false};
        case 28: return {TimbreFamily::ElectricGuitar, "Muted Electric Guitar", false};
        case 29: return {TimbreFamily::ElectricGuitar, "Overdriven Electric Guitar", false};
        case 30: return {TimbreFamily::ElectricGuitar, "Distortion Electric Guitar", false};
        case 31: return {TimbreFamily::ElectricGuitar, "Guitar Harmonics", false};

        // 32..39 Bass
        case 32: return {TimbreFamily::AcousticBass, "Acoustic Upright Bass", false};
        case 33: return {TimbreFamily::ElectricBass, "Finger Electric Bass", false};
        case 34: return {TimbreFamily::ElectricBass, "Pick Electric Bass", false};
        case 35: return {TimbreFamily::ElectricBass, "Fretless Electric Bass", false};
        case 36: return {TimbreFamily::ElectricBass, "Slap Bass 1", false};
        case 37: return {TimbreFamily::ElectricBass, "Slap Bass 2", false};
        case 38: return {TimbreFamily::SynthBass, "Synth Bass 1", false};
        case 39: return {TimbreFamily::SynthBass, "Synth Bass 2", false};

        // 40..51 Strings / Harp / Timpani / Ensemble
        case 40: return {TimbreFamily::Strings, "Solo Violin", false};
        case 41: return {TimbreFamily::Strings, "Solo Viola", false};
        case 42: return {TimbreFamily::Strings, "Solo Cello", false};
        case 43: return {TimbreFamily::Strings, "Contrabass", false};
        case 44: return {TimbreFamily::Strings, "Tremolo Strings", false};
        case 45: return {TimbreFamily::Strings, "Pizzicato Strings", false};
        case 46: return {TimbreFamily::PluckedStrings, "Orchestral Harp", false};
        case 47: return {TimbreFamily::MalletPerc, "Timpani", false};
        case 48: return {TimbreFamily::Strings, "String Ensemble 1", false};
        case 49: return {TimbreFamily::Strings, "String Ensemble 2", false};
        case 50: return {TimbreFamily::Strings, "Synth Strings 1", false};
        case 51: return {TimbreFamily::Strings, "Synth Strings 2", false};

        // 52..55 Choir / Voice
        case 52: return {TimbreFamily::ChoirPad, "Choir Aahs", false};
        case 53: return {TimbreFamily::ChoirPad, "Voice Oohs", false};
        case 54: return {TimbreFamily::ChoirPad, "Synth Choir", false};
        case 55: return {TimbreFamily::ChoirPad, "Orchestra Hit", false};

        // 56..63 Brass
        case 56: return {TimbreFamily::Brass, "Trumpet", false};
        case 57: return {TimbreFamily::Brass, "Trombone", false};
        case 58: return {TimbreFamily::Brass, "Tuba", false};
        case 59: return {TimbreFamily::Brass, "Muted Trumpet", false};
        case 60: return {TimbreFamily::Brass, "French Horn", false};
        case 61: return {TimbreFamily::Brass, "Brass Section", false};
        case 62: return {TimbreFamily::Brass, "Synth Brass 1", false};
        case 63: return {TimbreFamily::Brass, "Synth Brass 2", false};

        // 64..71 Sax / Reed
        case 64: return {TimbreFamily::SaxReed, "Soprano Sax", false};
        case 65: return {TimbreFamily::SaxReed, "Alto Sax", false};
        case 66: return {TimbreFamily::SaxReed, "Tenor Sax", false};
        case 67: return {TimbreFamily::SaxReed, "Baritone Sax", false};
        case 68: return {TimbreFamily::SaxReed, "Oboe", false};
        case 69: return {TimbreFamily::SaxReed, "English Horn", false};
        case 70: return {TimbreFamily::SaxReed, "Bassoon", false};
        case 71: return {TimbreFamily::SaxReed, "Clarinet", false};

        // 72..79 Flute / Pipe
        case 72: return {TimbreFamily::FlutePipe, "Piccolo", false};
        case 73: return {TimbreFamily::FlutePipe, "Flute", false};
        case 74: return {TimbreFamily::FlutePipe, "Recorder", false};
        case 75: return {TimbreFamily::FlutePipe, "Pan Flute", false};
        case 76: return {TimbreFamily::FlutePipe, "Blown Bottle", false};
        case 77: return {TimbreFamily::FlutePipe, "Shakuhachi", false};
        case 78: return {TimbreFamily::FlutePipe, "Whistle", false};
        case 79: return {TimbreFamily::FlutePipe, "Ocarina", false};

        // 80..87 Synth Lead
        case 80: return {TimbreFamily::SynthLead, "Square Lead", false};
        case 81: return {TimbreFamily::SynthLead, "Sawtooth Lead", false};
        case 82: return {TimbreFamily::SynthLead, "Calliope Lead", false};
        case 83: return {TimbreFamily::SynthLead, "Chiff Lead", false};
        case 84: return {TimbreFamily::SynthLead, "Charang Lead", false};
        case 85: return {TimbreFamily::SynthLead, "Voice Lead", false};
        case 86: return {TimbreFamily::SynthLead, "Fifths Lead", false};
        case 87: return {TimbreFamily::SynthLead, "Bass + Lead", false};

        // 88..95 Synth Pad
        case 88: return {TimbreFamily::SynthPad, "New Age Pad", false};
        case 89: return {TimbreFamily::SynthPad, "Warm Pad", false};
        case 90: return {TimbreFamily::SynthPad, "Polysynth Pad", false};
        case 91: return {TimbreFamily::ChoirPad, "Choir Pad", false};
        case 92: return {TimbreFamily::SynthPad, "Bowed Pad", false};
        case 93: return {TimbreFamily::SynthPad, "Metallic Pad", false};
        case 94: return {TimbreFamily::SynthPad, "Halo Pad", false};
        case 95: return {TimbreFamily::SynthPad, "Sweep Pad", false};

        // 96..103 Synth Effects / Atmospheric Pads
        case 96:  return {TimbreFamily::SynthPad, "Rain Pad", false};
        case 97:  return {TimbreFamily::SynthPad, "Soundtrack Pad", false};
        case 98:  return {TimbreFamily::SynthPad, "Crystal Pad", false};
        case 99:  return {TimbreFamily::SynthPad, "Atmosphere Pad", false};
        case 100: return {TimbreFamily::SynthPad, "Brightness Pad", false};
        case 101: return {TimbreFamily::SynthPad, "Goblins Pad", false};
        case 102: return {TimbreFamily::SynthPad, "Echoes Pad", false};
        case 103: return {TimbreFamily::SynthPad, "Sci-Fi Pad", false};

        // 104..111 Ethnic Melodic
        case 104: return {TimbreFamily::EthnicMelodic, "Sitar", false};
        case 105: return {TimbreFamily::EthnicMelodic, "Banjo", false};
        case 106: return {TimbreFamily::EthnicMelodic, "Shamisen", false};
        case 107: return {TimbreFamily::EthnicMelodic, "Koto", false};
        case 108: return {TimbreFamily::EthnicMelodic, "Kalimba", false};
        case 109: return {TimbreFamily::EthnicMelodic, "Bagpipe", false};
        case 110: return {TimbreFamily::EthnicMelodic, "Fiddle", false};
        case 111: return {TimbreFamily::EthnicMelodic, "Shanai", false};

        // 112..119 Melodic Percussion
        case 112: return {TimbreFamily::MalletPerc, "Tinkle Bell", false};
        case 113: return {TimbreFamily::MalletPerc, "Agogo", false};
        case 114: return {TimbreFamily::MalletPerc, "Steel Drums", false};
        case 115: return {TimbreFamily::PercussionKit, "Woodblock", false};
        case 116: return {TimbreFamily::PercussionKit, "Taiko Drum", false};
        case 117: return {TimbreFamily::PercussionKit, "Melodic Tom", false};
        case 118: return {TimbreFamily::PercussionKit, "Synth Drum", false};
        case 119: return {TimbreFamily::PercussionKit, "Reverse Cymbal", false};

        // 120..127 Sound Effects (intentionally Unidentified so SFX never mimics melodic voice)
        default:  return {TimbreFamily::Unknown, "Sound Effect", false};
    }
}

struct KeywordMatch {
    TimbreSubFamily sub;
    uint8_t         confidence = 0;
    std::string     matched_rule;
};

// Word-boundary keyword classifier over tokenized track/sound names.
// Returns TimbreFamily::Unknown when no unambiguous instrument token is present.
inline KeywordMatch classifyByKeywords(std::string_view name,
                                       uasf::TrackRole role,
                                       uint8_t korg_ch) {
    const auto tokens = tokenizeSafeWords(name);
    if (tokens.empty() || isGenericTrackName(tokens)) {
        return {};
    }

    const bool dncHint = nameSuggestsDncOrArticulation(tokens);
    const bool isDrumContext = (korg_ch == kDrumCh || korg_ch == kPercCh ||
                                role == uasf::TrackRole::Drum || role == uasf::TrackRole::Percussion);

    // ── Steel Drums (MalletPerc) checked BEFORE generic Drum/Kit block when not on drum/perc channel ──
    if (!isDrumContext &&
        (hasToken(tokens, "steeldrum") || hasToken(tokens, "steeldrums") ||
         (hasToken(tokens, "steel") && hasAnyToken(tokens, {"drum", "drums"})))) {
        return {{TimbreFamily::MalletPerc, "Steel Drums", dncHint}, 86, "keyword:steel-drum"};
    }

    // ── Drum / Percussion kits (checked first in drum context or explicit kit tokens) ──
    const bool hasKitToken = hasAnyToken(tokens, {
        "kit", "drum", "drums", "drumkit", "dk", "rhythm2", "808", "909", "tr808", "tr909"
    });
    const bool hasPercToken = hasAnyToken(tokens, {
        "perc", "percussion", "rhythm1", "conga", "congas", "bongo", "bongos",
        "shaker", "tambourine", "timbale", "timbales", "latinperc", "darbuka",
        "tabla", "cajon", "cabasa", "claves", "surdo", "agogo"
    });

    if (isDrumContext || hasKitToken || hasPercToken) {
        if (hasAnyToken(tokens, {"brush", "brushes", "brushkit"})) {
            return {{TimbreFamily::DrumKit, "Brush Drum Kit", dncHint}, 85, "keyword:brush-kit"};
        }
        if (hasAnyToken(tokens, {"jazz", "jazzkit"}) && (isDrumContext || hasKitToken)) {
            return {{TimbreFamily::DrumKit, "Jazz Drum Kit", dncHint}, 85, "keyword:jazz-kit"};
        }
        if (hasAnyToken(tokens, {"808", "909", "tr808", "tr909", "electro", "electronic", "edm", "dance", "techno", "house", "hiphop", "analog"}) &&
            (isDrumContext || hasKitToken)) {
            return {{TimbreFamily::DrumKit, "Electronic Drum Kit", dncHint}, 85, "keyword:electronic-kit"};
        }
        if (hasAnyToken(tokens, {"rock", "power", "metal", "heavy", "stadium", "rockkit"}) &&
            (isDrumContext || hasKitToken)) {
            return {{TimbreFamily::DrumKit, "Rock Drum Kit", dncHint}, 85, "keyword:rock-kit"};
        }
        if (hasAnyToken(tokens, {"arab", "arabic", "orient", "oriental", "ethnic", "indian", "turkish", "tabla", "darbuka"}) &&
            (isDrumContext || hasKitToken || hasPercToken)) {
            const TimbreFamily fam = (korg_ch == kPercCh || role == uasf::TrackRole::Percussion || hasPercToken)
                                         ? TimbreFamily::PercussionKit
                                         : TimbreFamily::DrumKit;
            return {{fam, "Ethnic / Oriental Kit", dncHint}, 84, "keyword:ethnic-kit"};
        }
        if (hasAnyToken(tokens, {"latin", "salsa", "samba", "cuban", "bossa", "brazil", "latinperc"}) &&
            (isDrumContext || hasKitToken || hasPercToken)) {
            return {{TimbreFamily::PercussionKit, "Latin Percussion Kit", dncHint}, 85, "keyword:latin-perc"};
        }
        if (hasAnyToken(tokens, {"sfx", "fx"}) && (isDrumContext || hasKitToken)) {
            return {{TimbreFamily::PercussionKit, "SFX Kit", dncHint}, 82, "keyword:sfx-kit"};
        }
        if (hasPercToken && !hasAnyToken(tokens, {"drum", "drums", "drumkit"})) {
            return {{TimbreFamily::PercussionKit, "Standard Percussion Kit", dncHint}, 82, "keyword:perc"};
        }
        if (hasKitToken || isDrumContext) {
            const TimbreFamily fam = (korg_ch == kPercCh || role == uasf::TrackRole::Percussion)
                                         ? TimbreFamily::PercussionKit
                                         : TimbreFamily::DrumKit;
            const std::string sub = (fam == TimbreFamily::PercussionKit)
                                        ? "Standard Percussion Kit"
                                        : (hasAnyToken(tokens, {"pop", "studio", "standard", "std", "acoustic", "live"})
                                               ? "Standard Pop Drum Kit"
                                               : "Drum Kit");
            return {{fam, sub, dncHint}, 82, "keyword:drum-kit"};
        }
    }

    // ── Harpsichord MUST be checked BEFORE Harp (prevents "harp" shadowing "harpsi"/"harpsichord") ──
    if (hasAnyToken(tokens, {"harpsichord", "harpsi", "hpschd", "cembalo"})) {
        return {{TimbreFamily::Piano, "Harpsichord", dncHint}, 86, "keyword:harpsichord"};
    }

    // ── Electric Piano / Clavinet (checked before acoustic Piano) ──
    if (hasAnyToken(tokens, {"rhodes", "mk1", "mki", "mkii", "suitcase", "tine"})) {
        return {{TimbreFamily::ElectricPiano, "Rhodes EP", dncHint}, 88, "keyword:rhodes"};
    }
    if (hasAnyToken(tokens, {"wurlitzer", "wurl", "wurli", "wurlie"})) {
        return {{TimbreFamily::ElectricPiano, "Wurlitzer EP", dncHint}, 88, "keyword:wurlitzer"};
    }
    if (hasAnyToken(tokens, {"clav", "clavinet", "clavi"})) {
        return {{TimbreFamily::ElectricPiano, "Clavinet", dncHint}, 86, "keyword:clavinet"};
    }
    if (hasAnyToken(tokens, {"dx", "dx7", "dxep", "fmep"})) {
        return {{TimbreFamily::ElectricPiano, "DX / FM EP", dncHint}, 86, "keyword:dx-ep"};
    }
    if (hasAnyToken(tokens, {"ep", "e.p", "epno", "e.pno", "epiano", "e.piano"}) ||
        (hasAnyToken(tokens, {"e", "el", "elec", "electric"}) && hasAnyToken(tokens, {"piano", "pno"}))) {
        return {{TimbreFamily::ElectricPiano, "Electric Piano", dncHint}, 86, "keyword:ep"};
    }

    // ── Organ (checked BEFORE generic "reed" so "Organ_Reed" & "Reed Organ" classify as Organ) ──
    if (hasAnyToken(tokens, {"organ", "org", "b3", "hammond", "rotary", "leslie",
                             "drawbar", "churchorg", "pipeorg", "farfisa", "voxorg", "orgel", "jimmy"})) {
        std::string sub = "Drawbar Organ";
        if (hasAnyToken(tokens, {"church", "churchorg", "pipe", "pipeorg"})) sub = "Church Organ";
        else if (hasToken(tokens, "reed")) sub = "Reed Organ";
        else if (hasAnyToken(tokens, {"rock", "overdrive", "dist"})) sub = "Rock Organ";
        else if (hasAnyToken(tokens, {"jazz", "b3", "hammond", "rotary", "jimmy"})) sub = "Jazz / Drawbar Organ";
        return {{TimbreFamily::Organ, std::move(sub), dncHint}, 85, "keyword:organ"};
    }

    // ── Accordion / Harmonica (NEVER bare "acc" / "acc1..5"; checked before generic "reed") ──
    if (hasAnyToken(tokens, {"accordion", "accord", "accdn", "akkordeon", "musette",
                             "bandoneon", "concertina", "harmonica", "bayan"})) {
        std::string sub = hasToken(tokens, "harmonica") ? "Harmonica"
                        : (hasToken(tokens, "bandoneon") ? "Bandoneon" : "Accordion");
        return {{TimbreFamily::Accordion, std::move(sub), dncHint}, 86, "keyword:accordion"};
    }

    // ── English Horn (SaxReed) checked BEFORE generic "horn" -> Brass ──
    if (hasToken(tokens, "englhorn") || (hasToken(tokens, "english") && hasToken(tokens, "horn"))) {
        return {{TimbreFamily::SaxReed, "English Horn", dncHint}, 86, "keyword:english-horn"};
    }

    // ── Brass (checked before generic "bs" suffix so "Tbn Bs" resolves as Trombone Brass!) ──
    if (hasAnyToken(tokens, {"trombone", "tbn", "trb"})) {
        return {{TimbreFamily::Brass, "Trombone", dncHint}, 86, "keyword:trombone"};
    }
    if (hasAnyToken(tokens, {"trumpet", "trp", "tpt", "tp", "cornet", "flugel", "flugelhorn"})) {
        return {{TimbreFamily::Brass, "Trumpet", dncHint}, 86, "keyword:trumpet"};
    }
    if (hasAnyToken(tokens, {"brass", "brs", "horn", "horns", "tuba", "bigband", "fanfare", "blech"})) {
        return {{TimbreFamily::Brass, "Brass Section", dncHint}, 84, "keyword:brass"};
    }

    // ── Sax / Reed ──
    if (hasAnyToken(tokens, {"sax", "saxophone", "altosax", "tenorsax", "sopranosax", "barisax"})) {
        std::string sub = "Saxophone";
        if (hasToken(tokens, "alto") || hasToken(tokens, "altosax")) sub = "Alto Sax";
        else if (hasToken(tokens, "tenor") || hasToken(tokens, "tenorsax")) sub = "Tenor Sax";
        else if (hasToken(tokens, "soprano") || hasToken(tokens, "sopranosax")) sub = "Soprano Sax";
        else if (hasAnyToken(tokens, {"bari", "baritone", "barisax"})) sub = "Baritone Sax";
        return {{TimbreFamily::SaxReed, std::move(sub), dncHint}, 86, "keyword:sax"};
    }
    if (hasAnyToken(tokens, {"oboe", "clarinet", "clar", "cl", "bassoon", "woodwind", "englhorn"}) ||
        (hasToken(tokens, "reed") && !hasAnyToken(tokens, {"organ", "org", "orgel", "accordion", "accord", "harmonica"}))) {
        std::string sub = "Reed / Woodwind";
        if (hasToken(tokens, "oboe")) sub = "Oboe";
        else if (hasAnyToken(tokens, {"clarinet", "clar", "cl"})) sub = "Clarinet";
        else if (hasToken(tokens, "bassoon")) sub = "Bassoon";
        return {{TimbreFamily::SaxReed, std::move(sub), dncHint}, 84, "keyword:reed"};
    }

    // ── Flute / Pipe ──
    if (hasAnyToken(tokens, {"flute", "fl", "flt", "piccolo", "picc", "panflute", "panpipe",
                             "recorder", "whistle", "ocarina", "shakuhachi", "quena", "flote"})) {
        std::string sub = "Flute";
        if (hasAnyToken(tokens, {"piccolo", "picc"})) sub = "Piccolo";
        else if (hasAnyToken(tokens, {"panflute", "panpipe"})) sub = "Pan Flute";
        else if (hasToken(tokens, "shakuhachi")) sub = "Shakuhachi";
        return {{TimbreFamily::FlutePipe, std::move(sub), dncHint}, 85, "keyword:flute"};
    }

    // ── Guitars (Acoustic vs Electric) ──
    if (hasAnyToken(tokens, {"acgt", "acgtr", "agtr", "a.gtr", "nylon", "nyl", "nylgt",
                             "classicgt", "steel", "stl", "stlgt", "folkgt", "12str", "flamenco", "spanishgt"})) {
        std::string sub = "Steel Acoustic Guitar";
        if (hasAnyToken(tokens, {"nylon", "nyl", "nylgt", "classic", "classicgt", "flamenco", "spanishgt"})) {
            sub = "Nylon Acoustic Guitar";
        }
        return {{TimbreFamily::AcousticGuitar, std::move(sub), dncHint}, 86, "keyword:acoustic-guitar"};
    }
    // Do not let bare "overdrive"/"dist"/"crunch" hijack "Overdrive_Lead", "Dist_Synth", or "Overdrive_Bass".
    const bool hasNonGuitarHost = hasAnyToken(tokens, {"lead", "synth", "syn", "bass", "bs", "organ", "org"});
    if (hasAnyToken(tokens, {"elgt", "elgtr", "egtr", "e.gtr", "clngt", "strat", "tele", "lespaul",
                             "jazzgt", "jazzgtr", "distgt", "odgt",
                             "rockgt", "mutedgt", "wahgt", "funkgt", "solidgt"}) ||
        (!hasNonGuitarHost && hasAnyToken(tokens, {"dist", "overdrive", "crunch"}))) {
        std::string sub = "Clean Electric Guitar";
        if (hasAnyToken(tokens, {"dist", "distgt", "overdrive", "odgt", "crunch", "rockgt", "metal"})) {
            sub = "Distortion Electric Guitar";
        } else if (hasAnyToken(tokens, {"jazz", "jazzgt", "jazzgtr", "hollow"})) {
            sub = "Jazz Electric Guitar";
        } else if (hasAnyToken(tokens, {"mute", "muted", "mutedgt", "funk", "funkgt"})) {
            sub = "Muted Electric Guitar";
        }
        return {{TimbreFamily::ElectricGuitar, std::move(sub), dncHint}, 86, "keyword:electric-guitar"};
    }
    if (hasAnyToken(tokens, {"guitar", "gtr", "gt"})) {
        if (hasAnyToken(tokens, {"ac", "acoust", "acoustic", "nylon", "classic", "steel", "folk"})) {
            const std::string sub = hasAnyToken(tokens, {"nylon", "classic"})
                                        ? "Nylon Acoustic Guitar"
                                        : "Steel Acoustic Guitar";
            return {{TimbreFamily::AcousticGuitar, sub, dncHint}, 86, "keyword:acoustic-guitar-pair"};
        }
        if (hasAnyToken(tokens, {"e", "el", "elec", "electric", "clean", "cln", "strat", "jazz", "rock", "dist", "overdrive", "solid"})) {
            std::string sub = "Clean Electric Guitar";
            if (hasAnyToken(tokens, {"dist", "overdrive", "rock"})) sub = "Distortion Electric Guitar";
            else if (hasToken(tokens, "jazz")) sub = "Jazz Electric Guitar";
            return {{TimbreFamily::ElectricGuitar, std::move(sub), dncHint}, 86, "keyword:electric-guitar-pair"};
        }
        return {{TimbreFamily::AcousticGuitar, "Acoustic Guitar", dncHint}, 78, "keyword:guitar-generic"};
    }

    // ── Basses (Synth / Acoustic / Electric) ──
    if (hasAnyToken(tokens, {"synbs", "synthbass", "subbass", "acidbass", "303", "moogbass", "analogbass"}) ||
        (hasAnyToken(tokens, {"synth", "syn", "sub", "analog", "acid", "moog"}) && hasAnyToken(tokens, {"bass", "bs"}))) {
        return {{TimbreFamily::SynthBass, "Synth Bass", dncHint}, 86, "keyword:synth-bass"};
    }
    if (hasAnyToken(tokens, {"acbs", "abass", "a.bass", "doublebass", "contrabass", "woodbass"}) ||
        (hasAnyToken(tokens, {"ac", "acoust", "acoustic", "upright", "wood", "double"}) && hasAnyToken(tokens, {"bass", "bs"}))) {
        return {{TimbreFamily::AcousticBass, "Acoustic Upright Bass", dncHint}, 86, "keyword:acoustic-bass"};
    }
    if (hasAnyToken(tokens, {"fingbs", "pickbs", "slapbs", "ebass", "e.bass", "jbass", "pbass", "fretless"})) {
        std::string sub = "Finger Electric Bass";
        if (hasAnyToken(tokens, {"pick", "pickbs"})) sub = "Pick Electric Bass";
        else if (hasAnyToken(tokens, {"slap", "slapbs"})) sub = "Slap Electric Bass";
        else if (hasToken(tokens, "fretless")) sub = "Fretless Electric Bass";
        return {{TimbreFamily::ElectricBass, std::move(sub), dncHint}, 86, "keyword:electric-bass"};
    }
    if (hasAnyToken(tokens, {"bass", "bs"})) {
        if (hasAnyToken(tokens, {"finger", "pick", "slap", "electric", "elec", "el", "e"})) {
            std::string sub = "Finger Electric Bass";
            if (hasToken(tokens, "pick")) sub = "Pick Electric Bass";
            else if (hasToken(tokens, "slap")) sub = "Slap Electric Bass";
            return {{TimbreFamily::ElectricBass, std::move(sub), dncHint}, 86, "keyword:electric-bass"};
        }
        return {{TimbreFamily::ElectricBass, "Finger Electric Bass", dncHint}, 78, "keyword:bass-generic"};
    }

    // ── Piano (Acoustic) ──
    if (hasAnyToken(tokens, {"piano", "pno", "grand", "honky", "klavier", "austrian"})) {
        std::string sub = "Acoustic Grand Piano";
        if (hasToken(tokens, "bright")) sub = "Bright Acoustic Piano";
        else if (hasToken(tokens, "honky")) sub = "Honky-Tonk Piano";
        return {{TimbreFamily::Piano, std::move(sub), dncHint}, 86, "keyword:piano"};
    }

    // ── Plucked Strings (Harp, Banjo, Mandolin, Ukulele — never shadowed by Harpsichord) ──
    if (hasAnyToken(tokens, {"harp", "banjo", "mandolin", "mando", "ukulele", "uke",
                             "lute", "pizz", "pizzicato", "balalaika", "cavaquinho", "plucked"})) {
        std::string sub = "Plucked Strings";
        if (hasToken(tokens, "harp")) sub = "Orchestral Harp";
        else if (hasToken(tokens, "banjo")) sub = "Banjo";
        else if (hasAnyToken(tokens, {"mandolin", "mando"})) sub = "Mandolin";
        else if (hasAnyToken(tokens, {"ukulele", "uke"})) sub = "Ukulele";
        else if (hasAnyToken(tokens, {"pizz", "pizzicato"})) sub = "Pizzicato Strings";
        return {{TimbreFamily::PluckedStrings, std::move(sub), dncHint}, 84, "keyword:plucked"};
    }

    // ── Strings ──
    if (hasAnyToken(tokens, {"str", "strs", "string", "strings", "orch", "orchestra", "orchestral",
                             "vln", "violin", "violins", "viola", "cel", "cello", "ensemble",
                             "streicher", "legatostr", "stringensemble", "stringensemblednc"})) {
        std::string sub = "String Ensemble";
        if (hasAnyToken(tokens, {"vln", "violin"})) sub = "Solo Violin";
        else if (hasAnyToken(tokens, {"cel", "cello"})) sub = "Solo Cello";
        return {{TimbreFamily::Strings, std::move(sub), dncHint}, 85, "keyword:strings"};
    }

    // ── Choir / Vocal Pad ──
    if (hasAnyToken(tokens, {"choir", "chor", "vox", "vocal", "voice", "aah", "aahs", "ooh", "oohs", "gospel"})) {
        return {{TimbreFamily::ChoirPad, "Choir / Vocal Pad", dncHint}, 85, "keyword:choir"};
    }

    // ── Mallet / Chromatic Percussion ──
    if (hasAnyToken(tokens, {"marimba", "vibe", "vibes", "vibraphone", "xylo", "xylophone",
                             "glock", "glockenspiel", "celesta", "kalimba", "bell", "bells",
                             "tubular", "chimes", "steeldrum", "mallet", "timpani", "dulcimer"})) {
        std::string sub = "Vibraphone / Mallet";
        if (hasToken(tokens, "marimba")) sub = "Marimba";
        else if (hasAnyToken(tokens, {"xylo", "xylophone"})) sub = "Xylophone";
        else if (hasAnyToken(tokens, {"glock", "glockenspiel"})) sub = "Glockenspiel";
        else if (hasToken(tokens, "celesta")) sub = "Celesta";
        else if (hasAnyToken(tokens, {"bell", "bells", "tubular", "chimes"})) sub = "Tubular Bells";
        return {{TimbreFamily::MalletPerc, std::move(sub), dncHint}, 85, "keyword:mallet"};
    }

    // ── Ethnic Melodic ──
    if (hasAnyToken(tokens, {"sitar", "koto", "shamisen", "oud", "kanun", "qanun", "ney",
                             "bouzouki", "bagpipe", "erhu", "guzheng", "pipa", "santoor",
                             "saz", "fiddle", "shanai", "duduk", "ethnic", "oriental"})) {
        std::string sub = "Ethnic Melodic";
        if (hasToken(tokens, "sitar")) sub = "Sitar";
        else if (hasToken(tokens, "oud")) sub = "Oud";
        else if (hasToken(tokens, "koto")) sub = "Koto";
        else if (hasToken(tokens, "bouzouki")) sub = "Bouzouki";
        return {{TimbreFamily::EthnicMelodic, std::move(sub), dncHint}, 84, "keyword:ethnic"};
    }

    // ── Synth Pad (checked before generic Synth Lead) ──
    if (hasAnyToken(tokens, {"pad", "warm", "warmpad", "sweep", "atmos", "atmosphere",
                             "halo", "polysyn", "obpad", "juppad", "synthpad", "synpad", "stringpad"}) ||
        (hasToken(tokens, "ob") && hasToken(tokens, "pad"))) {
        std::string sub = hasAnyToken(tokens, {"warm", "warmpad", "ob", "obpad"}) ? "Warm Analog Pad" : "Synth Pad";
        return {{TimbreFamily::SynthPad, std::move(sub), dncHint}, 84, "keyword:synth-pad"};
    }

    // ── Synth Lead ──
    if (hasAnyToken(tokens, {"lead", "synlead", "sawlead", "sqlead", "square", "saw",
                             "arp", "arpeggio", "synth", "syn", "mini", "moog", "porta", "supersaw", "plucksyn"})) {
        std::string sub = "Synth Lead";
        if (hasAnyToken(tokens, {"saw", "sawlead", "supersaw"})) sub = "Sawtooth Lead";
        else if (hasAnyToken(tokens, {"square", "sqlead"})) sub = "Square Lead";
        else if (hasAnyToken(tokens, {"mini", "moog"})) sub = "Analog Mono Lead";
        return {{TimbreFamily::SynthLead, std::move(sub), dncHint}, 82, "keyword:synth-lead"};
    }

    return {};
}

struct SignalFeatures {
    uint8_t  pitch_min = 0;
    uint8_t  pitch_median = 60;
    uint8_t  pitch_max = 127;
    uint8_t  max_polyphony = 0;
    double   mean_duration_beats = 0.0;
    double   notes_per_bar = 0.0;
    bool     has_strum_offsets = false;
    uint32_t note_count = 0;
};

inline SignalFeatures computeSignalFeatures(const std::vector<NoteSpan>& notes,
                                            const TimbreProfileOptions& opt) {
    SignalFeatures f{};
    if (notes.empty()) return f;

    const uint32_t ppqn = opt.ppqn ? opt.ppqn : 480u;
    const uint32_t tempo_mpqn = opt.tempo_mpqn ? opt.tempo_mpqn : 500000u;
    const uint64_t bar_ticks = opt.bar_ticks
                                   ? uint64_t{opt.bar_ticks}
                                   : std::max<uint64_t>(1u, uint64_t{ppqn} * 4u);

    f.note_count = static_cast<uint32_t>(notes.size());

    std::vector<uint8_t> pitches;
    pitches.reserve(notes.size());

    struct SweepPoint {
        uint64_t tick;
        int8_t   delta; // -1 for NoteOff (ordered BEFORE +1 NoteOn at equal tick)
    };
    std::vector<SweepPoint> sweep;
    sweep.reserve(notes.size() * 2);

    std::vector<NoteSpan> sorted_notes;
    sorted_notes.reserve(notes.size());

    double sum_dur_beats = 0.0;
    uint64_t max_end_tick = 0;

    for (const auto& n : notes) {
        const uint8_t p = std::min<uint8_t>(n.pitch, 127u);
        const uint64_t on64  = uint64_t{n.on_tick};
        const uint64_t off64 = (n.off_tick > n.on_tick) ? uint64_t{n.off_tick} : (on64 + 1u);
        pitches.push_back(p);
        sweep.push_back({on64, +1});
        sweep.push_back({off64, -1});
        sum_dur_beats += static_cast<double>(off64 - on64) / static_cast<double>(ppqn);
        if (off64 > max_end_tick) max_end_tick = off64;
        NoteSpan clean = n;
        clean.pitch = p;
        clean.off_tick = static_cast<uint32_t>(std::min<uint64_t>(off64, UINT32_MAX));
        sorted_notes.push_back(clean);
    }

    std::sort(pitches.begin(), pitches.end());
    f.pitch_min = pitches.front();
    f.pitch_max = pitches.back();
    f.pitch_median = pitches[pitches.size() / 2];

    std::sort(sweep.begin(), sweep.end(), [](const SweepPoint& a, const SweepPoint& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        return a.delta < b.delta; // NoteOff (-1) before NoteOn (+1)
    });
    int cur_poly = 0;
    int max_poly = 0;
    for (const auto& sp : sweep) {
        cur_poly += sp.delta;
        if (cur_poly > max_poly) max_poly = cur_poly;
    }
    f.max_polyphony = static_cast<uint8_t>(std::clamp(max_poly, 0, 255));
    f.mean_duration_beats = sum_dur_beats / static_cast<double>(notes.size());

    const uint64_t bars = opt.bars
                              ? uint64_t{opt.bars}
                              : std::max<uint64_t>(1u, (max_end_tick / bar_ticks) +
                                                           ((max_end_tick % bar_ticks) ? 1u : 0u));
    f.notes_per_bar = static_cast<double>(notes.size()) / static_cast<double>(bars);

    // Detect guitar-like strum offsets: consecutive onset deltas in [5 ms, 60 ms]
    // forming >= 3-note clusters with consistent pitch direction.
    const double ticks_per_ms = (static_cast<double>(ppqn) * 1000.0) / static_cast<double>(tempo_mpqn);
    const double max_u32_d    = static_cast<double>(UINT32_MAX - 1u);
    const double min_strum_d  = std::clamp(std::floor(5.0 * ticks_per_ms), 1.0, max_u32_d);
    const double max_strum_d  = std::clamp(std::ceil(60.0 * ticks_per_ms), 1.0, max_u32_d);
    const uint32_t min_strum_ticks = static_cast<uint32_t>(min_strum_d);
    const uint32_t max_strum_ticks = std::max<uint32_t>(min_strum_ticks + 1u,
                                                        static_cast<uint32_t>(max_strum_d));

    std::sort(sorted_notes.begin(), sorted_notes.end(), [](const NoteSpan& a, const NoteSpan& b) {
        if (a.on_tick != b.on_tick) return a.on_tick < b.on_tick;
        return a.pitch < b.pitch;
    });

    uint32_t strum_clusters = 0;
    size_t idx = 0;
    while (idx < sorted_notes.size()) {
        size_t end = idx + 1;
        uint32_t staggered_steps = 0;
        bool all_up = true;
        bool all_down = true;
        while (end < sorted_notes.size()) {
            const uint32_t dt = sorted_notes[end].on_tick - sorted_notes[end - 1].on_tick;
            if (dt > max_strum_ticks) break;
            if (dt >= min_strum_ticks) ++staggered_steps;
            if (sorted_notes[end].pitch < sorted_notes[end - 1].pitch) all_up = false;
            if (sorted_notes[end].pitch > sorted_notes[end - 1].pitch) all_down = false;
            ++end;
        }
        const size_t cluster_size = end - idx;
        if (cluster_size >= 3 && staggered_steps >= 2 && (all_up || all_down)) {
            ++strum_clusters;
        }
        idx = end;
    }
    f.has_strum_offsets = (strum_clusters >= 1);
    return f;
}

} // namespace timbre_detail

// Extracts the initial Yamaha voice (MSB, LSB, PC) seen in a track's event list.
// Unset components remain -1 (matching genos_to_korg.h TrackRow convention).
inline std::tuple<int, int, int> extractSectionYamahaVoice(const std::vector<uasf::MidiEvent>& events) {
    std::vector<const uasf::MidiEvent*> ordered;
    ordered.reserve(events.size());
    for (const auto& e : events) ordered.push_back(&e);
    std::stable_sort(ordered.begin(), ordered.end(), [](const uasf::MidiEvent* a, const uasf::MidiEvent* b) {
        return a->tick < b->tick;
    });

    int cur_msb = -1;
    int cur_lsb = -1;
    for (const auto* e : ordered) {
        if (e->type == uasf::MidiEventType::ControlChange && e->data1 == 0) {
            cur_msb = static_cast<int>(e->data2);
        } else if (e->type == uasf::MidiEventType::ControlChange && e->data1 == 32) {
            cur_lsb = static_cast<int>(e->data2);
        } else if (e->type == uasf::MidiEventType::ProgramChange) {
            return {cur_msb, cur_lsb, static_cast<int>(e->data1)};
        }
    }
    return {cur_msb, cur_lsb, -1};
}

// Extracts paired NoteSpans from raw UASF MidiEvents.
inline std::vector<NoteSpan> extractNoteSpansFromEvents(const std::vector<uasf::MidiEvent>& events,
                                                        uint32_t default_end_tick = 0) {
    std::vector<const uasf::MidiEvent*> ordered;
    ordered.reserve(events.size());
    for (const auto& e : events) ordered.push_back(&e);
    std::stable_sort(ordered.begin(), ordered.end(), [](const uasf::MidiEvent* a, const uasf::MidiEvent* b) {
        return a->tick < b->tick;
    });

    std::vector<NoteSpan> spans;
    std::map<uint8_t, std::deque<size_t>> open;
    uint32_t max_tick = default_end_tick;
    for (const auto* e : ordered) {
        const uint32_t t = static_cast<uint32_t>(std::min<uint64_t>(e->tick, 0x0FFFFFFFu));
        if (t > max_tick) max_tick = t;
        const bool isOn  = (e->type == uasf::MidiEventType::NoteOn && e->data2 > 0);
        const bool isOff = (e->type == uasf::MidiEventType::NoteOff) ||
                           (e->type == uasf::MidiEventType::NoteOn && e->data2 == 0);
        if (isOn) {
            open[e->data1].push_back(spans.size());
            NoteSpan ns{};
            ns.on_tick = t;
            ns.off_tick = 0; // closed below
            ns.pitch = std::min<uint8_t>(e->data1, 127u);
            ns.vel = std::clamp<uint8_t>(e->data2, 1u, 127u);
            spans.push_back(ns);
        } else if (isOff) {
            auto it = open.find(e->data1);
            if (it != open.end() && !it->second.empty()) {
                const size_t idx = it->second.front();
                it->second.pop_front();
                spans[idx].off_tick = (t > spans[idx].on_tick) ? t : (spans[idx].on_tick + 1u);
            }
        }
    }
    const uint32_t fallback_end = max_tick + 1u;
    for (auto& ns : spans) {
        if (ns.off_tick <= ns.on_tick) {
            ns.off_tick = std::max<uint32_t>(fallback_end, ns.on_tick + 1u);
        }
    }
    return spans;
}

// Extracts per-section Yamaha voice selections for a given source MIDI channel across all sections.
inline std::map<uasf::SectionType, std::tuple<int, int, int>> extractElementYamahaVoices(
    const uasf::StyleDefinition& style,
    uint8_t src_ch) {
    std::map<uasf::SectionType, std::tuple<int, int, int>> out;
    for (const auto& sec : style.sections) {
        for (const auto& tr : sec.tracks) {
            if (tr.midi_channel != src_ch) continue;
            const auto voice = extractSectionYamahaVoice(tr.events);
            const auto [msb, lsb, pc] = voice;
            if (msb >= 0 || lsb >= 0 || pc >= 0) {
                out[sec.type] = voice;
            }
            break;
        }
    }
    return out;
}

// Deterministic multi-signal Yamaha track timbre profiler.
// Resolution priority:
//   1. Yamaha Bank/PC taxonomy (XG Drum 127/0, Revo! Drum 62/100, SFX/Perc 126/0,
//      MegaVoice Guitar 63/120, MegaVoice Bass 63/60, MegaVoice 8/60, GM/XG/Panel/S.Art 0/8/104)
//   2. Safe word-boundary multi-lingual / Yamaha abbreviation keyword matcher
//   3. Musical signal disambiguator (pitch median, polyphony, mean duration, strum offsets) + role fallback
inline TrackTimbreProfile profileTrackTimbre(
    int yamaha_msb,
    int yamaha_lsb,
    int yamaha_pc,
    const std::string& track_name,
    uasf::TrackRole role,
    const std::vector<NoteSpan>& notes,
    const std::map<uasf::SectionType, std::tuple<int, int, int>>& element_yamaha_voices = {},
    uint8_t src_ch = 0,
    uint8_t korg_ch = 0xFF,
    const TimbreProfileOptions& opt = {}) {
    TrackTimbreProfile prof{};
    prof.src_ch = src_ch;
    prof.korg_ch = korg_ch;
    prof.yamaha_msb = yamaha_msb;
    prof.yamaha_lsb = yamaha_lsb;
    prof.yamaha_pc  = yamaha_pc;
    prof.element_yamaha_voices = element_yamaha_voices;

    // Step 0: always compute deterministic musical signal features.
    const auto sig = timbre_detail::computeSignalFeatures(notes, opt);
    prof.pitch_min           = sig.pitch_min;
    prof.pitch_median        = sig.pitch_median;
    prof.pitch_max           = sig.pitch_max;
    prof.max_polyphony       = sig.max_polyphony;
    prof.mean_duration_beats = sig.mean_duration_beats;
    prof.notes_per_bar       = sig.notes_per_bar;
    prof.has_strum_offsets   = sig.has_strum_offsets;
    prof.note_count          = sig.note_count;

    const auto tokens = timbre_detail::tokenizeSafeWords(track_name);
    const auto kw = timbre_detail::classifyByKeywords(track_name, role, korg_ch);
    const bool nameDnc = timbre_detail::nameSuggestsDncOrArticulation(tokens);
    const bool validPc = (yamaha_pc >= 0 && yamaha_pc <= 127);
    const bool validBank = (yamaha_msb >= 0 && yamaha_msb <= 127 && yamaha_lsb >= 0 && yamaha_lsb <= 127);
    const bool isPercTarget = (korg_ch == kPercCh || role == uasf::TrackRole::Percussion);
    const bool isDrumTarget = (korg_ch == kDrumCh || role == uasf::TrackRole::Drum);

    // ── Signal 1: Yamaha Bank/PC Taxonomy ──
    if (validBank && validPc) {
        // 1a. XG Drum Kits (127/0)
        if (yamaha_msb == 127 && yamaha_lsb == 0) {
            TimbreSubFamily sub{};
            sub.family = isPercTarget ? TimbreFamily::PercussionKit : TimbreFamily::DrumKit;
            sub.prefers_dnc = nameDnc;
            if (yamaha_pc <= 7)       sub.sub_family = "Standard Drum Kit";
            else if (yamaha_pc <= 15) sub.sub_family = "Room Drum Kit";
            else if (yamaha_pc <= 23) sub.sub_family = "Rock / Power Drum Kit";
            else if (yamaha_pc <= 31) sub.sub_family = "Electronic / Analog Drum Kit";
            else if (yamaha_pc <= 39) sub.sub_family = "Jazz Drum Kit";
            else if (yamaha_pc <= 47) sub.sub_family = "Brush Drum Kit";
            else if (yamaha_pc <= 55) {
                sub.family = isDrumTarget ? TimbreFamily::DrumKit : TimbreFamily::PercussionKit;
                sub.sub_family = "Orchestra / Latin Kit";
            } else if (yamaha_pc >= 112) {
                sub.sub_family = "Ethnic / Oriental Drum Kit";
            } else {
                sub.sub_family = isPercTarget ? "Standard Percussion Kit" : "Pop / Studio Drum Kit";
            }
            // Refine sub-family with explicit kit keywords when present.
            if (kw.confidence > 0 && isPercussiveTimbreFamily(kw.sub.family) &&
                kw.matched_rule != "keyword:drum-kit" && kw.matched_rule != "keyword:perc") {
                sub.sub_family = kw.sub.sub_family;
            }
            prof.inferred = std::move(sub);
            prof.confidence = 94;
            prof.rationale = "yamaha-bank:127/0-xg-drum";
            return prof;
        }

        // 1b. Genos Revo! Drums (62/100)
        if (yamaha_msb == 62 && yamaha_lsb == 100) {
            TimbreSubFamily sub{};
            sub.family = isPercTarget ? TimbreFamily::PercussionKit : TimbreFamily::DrumKit;
            sub.prefers_dnc = true;
            if (kw.confidence > 0 && isPercussiveTimbreFamily(kw.sub.family) &&
                kw.matched_rule != "keyword:drum-kit" && kw.matched_rule != "keyword:perc") {
                sub.sub_family = "Revo! " + kw.sub.sub_family;
            } else if (yamaha_pc >= 32 && yamaha_pc <= 39) {
                sub.sub_family = "Revo! Jazz Drum Kit";
            } else if (yamaha_pc >= 40 && yamaha_pc <= 47) {
                sub.sub_family = "Revo! Brush Drum Kit";
            } else if (yamaha_pc >= 16 && yamaha_pc <= 23) {
                sub.sub_family = "Revo! Rock Drum Kit";
            } else if (yamaha_pc >= 24 && yamaha_pc <= 31) {
                sub.sub_family = "Revo! Electronic Drum Kit";
            } else {
                sub.sub_family = isPercTarget ? "Revo! Percussion Kit" : "Revo! Standard Pop Drum Kit";
            }
            prof.inferred = std::move(sub);
            prof.confidence = 95;
            prof.rationale = "yamaha-bank:62/100-revo-drum";
            return prof;
        }

        // 1c. Yamaha SFX / Oriental / Percussion Kits (126/0)
        if (yamaha_msb == 126 && yamaha_lsb == 0) {
            TimbreSubFamily sub{};
            sub.family = TimbreFamily::PercussionKit;
            sub.prefers_dnc = nameDnc;
            if (kw.confidence > 0 && isPercussiveTimbreFamily(kw.sub.family) &&
                kw.matched_rule != "keyword:drum-kit" && kw.matched_rule != "keyword:perc") {
                sub.sub_family = kw.sub.sub_family;
            } else if (yamaha_pc <= 1) {
                sub.sub_family = "SFX Percussion Kit";
            } else if (yamaha_pc >= 16 && yamaha_pc <= 63) {
                sub.sub_family = "Ethnic / Oriental Percussion Kit";
            } else {
                sub.sub_family = "Yamaha Percussion Kit";
            }
            prof.inferred = std::move(sub);
            prof.confidence = 92;
            prof.rationale = "yamaha-bank:126/0-sfx-perc";
            return prof;
        }

        // 1d. MegaVoice Guitars (63/120)
        if (yamaha_msb == 63 && yamaha_lsb == 120) {
            TimbreSubFamily sub{};
            sub.prefers_dnc = true;
            if (kw.confidence > 0 && kw.matched_rule != "keyword:guitar-generic" &&
                (kw.sub.family == TimbreFamily::AcousticGuitar ||
                 kw.sub.family == TimbreFamily::ElectricGuitar)) {
                sub.family = kw.sub.family;
                sub.sub_family = "MegaVoice " + kw.sub.sub_family;
            } else if (yamaha_pc <= 7) {
                sub.family = TimbreFamily::AcousticGuitar;
                sub.sub_family = "MegaVoice Nylon Acoustic Guitar";
            } else if (yamaha_pc <= 23) {
                sub.family = TimbreFamily::AcousticGuitar;
                sub.sub_family = "MegaVoice Steel Acoustic Guitar";
            } else if (yamaha_pc <= 39) {
                sub.family = TimbreFamily::ElectricGuitar;
                sub.sub_family = "MegaVoice Clean Electric Guitar";
            } else if (yamaha_pc <= 63) {
                sub.family = TimbreFamily::ElectricGuitar;
                sub.sub_family = "MegaVoice Overdrive / Distortion Guitar";
            } else {
                sub.family = TimbreFamily::AcousticGuitar;
                sub.sub_family = "MegaVoice Guitar";
            }
            prof.inferred = std::move(sub);
            prof.confidence = 92;
            prof.rationale = "yamaha-bank:63/120-megavoice-guitar";
            return prof;
        }

        // 1e. MegaVoice Basses (63/60)
        if (yamaha_msb == 63 && yamaha_lsb == 60) {
            TimbreSubFamily sub{};
            sub.prefers_dnc = true;
            if (kw.confidence > 0 && kw.matched_rule != "keyword:bass-generic" &&
                (kw.sub.family == TimbreFamily::AcousticBass ||
                 kw.sub.family == TimbreFamily::ElectricBass ||
                 kw.sub.family == TimbreFamily::SynthBass)) {
                sub.family = kw.sub.family;
                sub.sub_family = "MegaVoice " + kw.sub.sub_family;
            } else if (yamaha_pc <= 1 || yamaha_pc == 32) {
                sub.family = TimbreFamily::AcousticBass;
                sub.sub_family = "MegaVoice Acoustic Upright Bass";
            } else if (yamaha_pc == 38 || yamaha_pc == 39) {
                sub.family = TimbreFamily::SynthBass;
                sub.sub_family = "MegaVoice Synth Bass";
            } else {
                sub.family = TimbreFamily::ElectricBass;
                sub.sub_family = "MegaVoice Finger Electric Bass";
            }
            prof.inferred = std::move(sub);
            prof.confidence = 92;
            prof.rationale = "yamaha-bank:63/60-megavoice-bass";
            return prof;
        }

        // 1f. Additional MegaVoice Bank (8/60)
        if (yamaha_msb == 8 && yamaha_lsb == 60) {
            TimbreSubFamily sub = timbre_detail::classifyGmProgramChange(yamaha_pc);
            sub.prefers_dnc = true;
            if (sub.family == TimbreFamily::Unknown) {
                if (kw.confidence > 0) sub = kw.sub;
                else if (korg_ch == kBassCh || role == uasf::TrackRole::Bass) {
                    sub.family = TimbreFamily::ElectricBass;
                    sub.sub_family = "MegaVoice Electric Bass";
                } else {
                    sub.family = TimbreFamily::AcousticGuitar;
                    sub.sub_family = "MegaVoice Guitar";
                }
                sub.prefers_dnc = true;
            }
            prof.inferred = std::move(sub);
            prof.confidence = 90;
            prof.rationale = "yamaha-bank:8/60-megavoice";
            return prof;
        }

        // 1g. GM/XG (msb=0), Genos Panel/S.Art (msb=8), Genos Live/Cool/Sweet (msb=104)
        if (yamaha_msb == 0 || yamaha_msb == 8 || yamaha_msb == 104) {
            // Never classify a Drum/Perc channel as a melodic GM sound.
            if (isDrumTarget || isPercTarget) {
                TimbreSubFamily sub{};
                sub.family = isPercTarget ? TimbreFamily::PercussionKit : TimbreFamily::DrumKit;
                sub.sub_family = (kw.confidence > 0 && isPercussiveTimbreFamily(kw.sub.family))
                                     ? kw.sub.sub_family
                                     : (isPercTarget ? "Percussion Kit" : "Drum Kit");
                sub.prefers_dnc = nameDnc || (yamaha_msb == 8 && yamaha_lsb >= 64);
                prof.inferred = std::move(sub);
                prof.confidence = 80;
                prof.rationale = "drum-role-guard";
                return prof;
            }

            TimbreSubFamily gm = timbre_detail::classifyGmProgramChange(yamaha_pc);
            const bool isSArt = (yamaha_msb == 8 && yamaha_lsb >= 64);
            gm.prefers_dnc = isSArt || nameDnc;

            // Yamaha Panel bank (msb==8) occasionally hosts non-GM bass PCs on dedicated Bass tracks
            // (e.g. 8/37/17 Bass). If role/channel is explicitly Bass and GM PC is non-bass, trust Bass role/keyword.
            const bool isBassChannel = (korg_ch == kBassCh || role == uasf::TrackRole::Bass);
            const bool gmIsBass = (gm.family == TimbreFamily::AcousticBass ||
                                   gm.family == TimbreFamily::ElectricBass ||
                                   gm.family == TimbreFamily::SynthBass);
            if (yamaha_msb == 8 && isBassChannel && !gmIsBass) {
                if (kw.confidence > 0 && (kw.sub.family == TimbreFamily::AcousticBass ||
                                          kw.sub.family == TimbreFamily::ElectricBass ||
                                          kw.sub.family == TimbreFamily::SynthBass)) {
                    prof.inferred = kw.sub;
                } else {
                    prof.inferred = {TimbreFamily::ElectricBass, "Finger Electric Bass", gm.prefers_dnc};
                }
                prof.inferred.prefers_dnc = gm.prefers_dnc;
                prof.confidence = 84;
                prof.rationale = "yamaha-bank:8-panel-bass-role";
                return prof;
            }

            if (gm.family != TimbreFamily::Unknown) {
                // Refine sub-family with explicit track-name nuance when family agrees (e.g. Wurlitzer vs Rhodes).
                if (kw.confidence > 0 && kw.sub.family == gm.family) {
                    gm.sub_family = kw.sub.sub_family;
                }
                prof.inferred = std::move(gm);
                prof.confidence = (yamaha_msb == 0) ? 92 : (isSArt ? 90 : 88);
                prof.rationale = (yamaha_msb == 0)
                                     ? "yamaha-bank:0-gm-xg"
                                     : (isSArt ? "yamaha-bank:8-sart"
                                               : (yamaha_msb == 8 ? "yamaha-bank:8-panel" : "yamaha-bank:104-live-cool-sweet"));
                return prof;
            }
        }
    } else if (validPc && yamaha_msb < 0 && !isDrumTarget && !isPercTarget) {
        // Program Change only (no Bank Select CC0/CC32): treat as GM PC with moderate confidence
        // unless explicit keywords give stronger evidence.
        TimbreSubFamily gm = timbre_detail::classifyGmProgramChange(yamaha_pc);
        if (kw.confidence >= 80) {
            prof.inferred = kw.sub;
            prof.confidence = kw.confidence;
            prof.rationale = kw.matched_rule;
            return prof;
        }
        if (gm.family != TimbreFamily::Unknown) {
            gm.prefers_dnc = nameDnc;
            prof.inferred = std::move(gm);
            prof.confidence = 78;
            prof.rationale = "gm-pc-only";
            return prof;
        }
    }

    // ── Signal 2: Safe Token/Word-Boundary Keyword Matcher ──
    if (kw.confidence > 0 && kw.sub.family != TimbreFamily::Unknown) {
        prof.inferred = kw.sub;
        prof.confidence = kw.confidence;
        prof.rationale = kw.matched_rule;
        return prof;
    }

    // ── Signal 3: Musical Signal Disambiguator + Role Fallback ──
    if (isDrumTarget) {
        prof.inferred = {TimbreFamily::DrumKit, "Drum Kit", nameDnc};
        prof.confidence = 70;
        prof.rationale = "role:drum";
        return prof;
    }
    if (isPercTarget) {
        prof.inferred = {TimbreFamily::PercussionKit, "Percussion Kit", nameDnc};
        prof.confidence = 70;
        prof.rationale = "role:percussion";
        return prof;
    }
    if (korg_ch == kBassCh || role == uasf::TrackRole::Bass) {
        prof.inferred = {TimbreFamily::ElectricBass, "Electric Bass", nameDnc};
        prof.confidence = 72;
        prof.rationale = "role:bass";
        return prof;
    }

    if (sig.note_count > 0) {
        if (sig.max_polyphony <= 1 && sig.pitch_median <= 48) {
            prof.inferred = {TimbreFamily::ElectricBass, "Signal-Inferred Bass", nameDnc};
            prof.confidence = 68;
            prof.rationale = "signal:mono-low-bass";
            return prof;
        }
        if (sig.max_polyphony >= 3 && sig.mean_duration_beats >= 1.8) {
            if (role == uasf::TrackRole::Pad) {
                prof.inferred = {TimbreFamily::SynthPad, "Signal-Inferred Warm Pad", nameDnc};
                prof.confidence = 70;
                prof.rationale = "signal:sustained-pad";
            } else {
                prof.inferred = {TimbreFamily::Strings, "Signal-Inferred Sustained Strings", nameDnc};
                prof.confidence = 65;
                prof.rationale = "signal:sustained-strings";
            }
            return prof;
        }
        if (sig.max_polyphony >= 3 && sig.mean_duration_beats < 0.75) {
            if (sig.has_strum_offsets) {
                prof.inferred = {TimbreFamily::AcousticGuitar, "Signal-Inferred Strummed Guitar", nameDnc};
                prof.confidence = 68;
                prof.rationale = "signal:strummed-guitar";
            } else {
                prof.inferred = {TimbreFamily::Piano, "Signal-Inferred Chord Piano", nameDnc};
                prof.confidence = 64;
                prof.rationale = "signal:staccato-poly-piano";
            }
            return prof;
        }
        if (sig.max_polyphony >= 3) {
            if (sig.has_strum_offsets) {
                prof.inferred = {TimbreFamily::AcousticGuitar, "Signal-Inferred Acoustic Guitar", nameDnc};
                prof.confidence = 64;
                prof.rationale = "signal:mid-strum-guitar";
            } else {
                prof.inferred = {TimbreFamily::Piano, "Signal-Inferred Polyphonic Piano", nameDnc};
                prof.confidence = 60;
                prof.rationale = "signal:poly-piano";
            }
            return prof;
        }
    }

    if (role == uasf::TrackRole::Pad) {
        prof.inferred = {TimbreFamily::SynthPad, "Role-Inferred Pad", nameDnc};
        prof.confidence = 58;
        prof.rationale = "role:pad";
        return prof;
    }

    prof.inferred = {TimbreFamily::Unknown, "Unidentified", false};
    prof.confidence = 0;
    prof.rationale = "unidentified";
    return prof;
}

// Profiles every exported/present source channel across a full UASF StyleDefinition,
// aggregating note spans across sections and preserving per-section voice changes
// inside TrackTimbreProfile::element_yamaha_voices.
inline std::vector<TrackTimbreProfile> profileStyleTracks(
    const uasf::StyleDefinition& style,
    const std::map<uint8_t, uint8_t>& route = {}) {
    struct Accum {
        uint8_t src_ch = 0;
        uint8_t korg_ch = 0xFF;
        std::string name;
        uasf::TrackRole role = uasf::TrackRole::Accompaniment;
        int dominant_msb = -1;
        int dominant_lsb = -1;
        int dominant_pc  = -1;
        std::map<std::tuple<int, int, int>, uint32_t> voice_freq;
        std::map<uasf::SectionType, std::tuple<int, int, int>> element_voices;
        std::vector<NoteSpan> notes;
        uint64_t total_bars = 0;
        uint32_t ppqn = 480;
        uint64_t bar_ticks = 1920;
    };

    const uint32_t tempo_mpqn = style.tempo_bpm
                                    ? static_cast<uint32_t>(std::max<uint64_t>(
                                          1u, (60'000'000ULL + uint64_t{style.tempo_bpm} / 2u) / uint64_t{style.tempo_bpm}))
                                    : 500000u;

    std::map<uint8_t, Accum> by_src;
    for (const auto& sec : style.sections) {
        const uint32_t ppqn = sec.resolution ? sec.resolution : (style.resolution ? style.resolution : 480u);
        const uint64_t num  = sec.beats_per_bar ? uint64_t{sec.beats_per_bar} : 4u;
        const uint64_t den  = sec.beat_note ? uint64_t{sec.beat_note} : 4u;
        const uint64_t bar_ticks64 = std::max<uint64_t>(1u, (uint64_t{ppqn} * 4u * num) / den);
        const uint64_t sec_bars64  = std::max<uint64_t>(1u, uint64_t{sec.bars});
        const uint64_t sec_end64   = (sec_bars64 > uint64_t{UINT32_MAX} / bar_ticks64)
                                         ? uint64_t{UINT32_MAX}
                                         : (sec_bars64 * bar_ticks64);

        for (const auto& tr : sec.tracks) {
            const uint8_t src_ch = tr.midi_channel;
            auto rit = route.find(src_ch);
            const uint8_t routed = (rit != route.end())
                                       ? rit->second
                                       : ((tr.articulation.has_casm_meta && tr.articulation.dest_channel <= 15)
                                              ? tr.articulation.dest_channel
                                              : src_ch);
            const uint8_t kch = korgChannelFor(routed);

            auto [it, fresh] = by_src.try_emplace(src_ch);
            Accum& acc = it->second;
            if (fresh) {
                acc.src_ch = src_ch;
                acc.korg_ch = kch;
                acc.name = tr.name;
                acc.role = tr.role;
                acc.ppqn = ppqn;
                acc.bar_ticks = bar_ticks64;
            } else if (acc.name.empty() && !tr.name.empty()) {
                acc.name = tr.name;
            }

            const auto voice = extractSectionYamahaVoice(tr.events);
            const auto [msb, lsb, pc] = voice;
            if (msb >= 0 || lsb >= 0 || pc >= 0) {
                acc.element_voices[sec.type] = voice;
                if (pc >= 0) {
                    acc.voice_freq[voice]++;
                    if (acc.dominant_pc < 0) {
                        acc.dominant_msb = msb;
                        acc.dominant_lsb = lsb;
                        acc.dominant_pc  = pc;
                    }
                }
            }

            const auto spans = extractNoteSpansFromEvents(tr.events, static_cast<uint32_t>(sec_end64));
            const uint64_t offset64 = (acc.bar_ticks > 0 && acc.total_bars > UINT64_MAX / acc.bar_ticks)
                                          ? UINT64_MAX
                                          : (acc.total_bars * acc.bar_ticks);
            for (auto ns : spans) {
                const uint64_t on64  = (offset64 >= uint64_t{UINT32_MAX} - ns.on_tick)
                                           ? uint64_t{UINT32_MAX}
                                           : (offset64 + uint64_t{ns.on_tick});
                const uint64_t off64 = (offset64 >= uint64_t{UINT32_MAX} - ns.off_tick)
                                           ? uint64_t{UINT32_MAX}
                                           : (offset64 + uint64_t{ns.off_tick});
                ns.on_tick  = static_cast<uint32_t>(on64);
                ns.off_tick = static_cast<uint32_t>(off64);
                acc.notes.push_back(ns);
            }
            if (acc.total_bars <= UINT64_MAX - sec_bars64) {
                acc.total_bars += sec_bars64;
            } else {
                acc.total_bars = UINT64_MAX;
            }
        }
    }

    std::vector<TrackTimbreProfile> result;
    result.reserve(by_src.size());
    for (const auto& [src_ch, acc] : by_src) {
        // Choose most frequent voice across sections (Main1..4 preferred on tie via section order).
        int best_msb = acc.dominant_msb;
        int best_lsb = acc.dominant_lsb;
        int best_pc  = acc.dominant_pc;
        uint32_t best_count = 0;
        for (const auto& [voice, count] : acc.voice_freq) {
            if (count > best_count) {
                best_count = count;
                std::tie(best_msb, best_lsb, best_pc) = voice;
            }
        }
        TimbreProfileOptions topt{};
        topt.ppqn       = acc.ppqn;
        topt.tempo_mpqn = tempo_mpqn;
        topt.bar_ticks  = static_cast<uint32_t>(std::min<uint64_t>(acc.bar_ticks, UINT32_MAX));
        topt.bars       = static_cast<uint32_t>(std::min<uint64_t>(acc.total_bars, UINT32_MAX));

        result.push_back(profileTrackTimbre(
            best_msb, best_lsb, best_pc,
            acc.name, acc.role, acc.notes,
            acc.element_voices,
            src_ch, acc.korg_ch, topt));
    }
    return result;
}

} // namespace ai_arranger::exporters::korg::quality

#endif // AI_ARRANGER_EXPORTERS_KORG_TIMBRE_PROFILER_H
