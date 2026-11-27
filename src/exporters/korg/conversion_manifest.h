#ifndef AI_ARRANGER_EXPORTERS_KORG_CONVERSION_MANIFEST_H
#define AI_ARRANGER_EXPORTERS_KORG_CONVERSION_MANIFEST_H

#include "exporters/korg/genos_to_korg.h"
#include "exporters/korg/sound_map.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// Reproducible Conversion Manifest (JSON), Unresolved Instrument Report (Markdown),
// and Batch Conversion Summary (JSON + Markdown) for aiarranger-convert.
// Header-only, ODR-safe (`namespace manifest_detail`), zero external dependencies,
// 100% deterministic offline provenance with strict epistemic honesty:
//   hardwareVerified: false
//   l3Status: "NOT_TESTED"
//   outputFormat: "PA5X_MAPPED_SMF_IMPORT_ARTIFACT"
//   nativeStyBinaryVerified: false

namespace ai_arranger::exporters::korg {

namespace manifest_detail {

inline constexpr std::array<uint32_t, 64> kSha256K = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

inline uint32_t rotr32(uint32_t x, uint32_t n) noexcept {
    return (x >> n) | (x << (32u - n));
}

class Sha256 {
public:
    Sha256() noexcept { reset(); }

    void reset() noexcept {
        h_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
              0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        buf_len_ = 0;
        total_bits_ = 0;
    }

    void update(const uint8_t* data, size_t len) noexcept {
        if (!data || len == 0) return;
        total_bits_ += static_cast<uint64_t>(len) * 8ULL;
        size_t i = 0;
        if (buf_len_ > 0) {
            while (i < len && buf_len_ < 64) {
                buf_[buf_len_++] = data[i++];
            }
            if (buf_len_ == 64) {
                compress(buf_.data());
                buf_len_ = 0;
            }
        }
        while (i + 64 <= len) {
            compress(data + i);
            i += 64;
        }
        while (i < len) {
            buf_[buf_len_++] = data[i++];
        }
    }

    std::string finalHex() noexcept {
        buf_[buf_len_++] = 0x80u;
        if (buf_len_ > 56) {
            while (buf_len_ < 64) buf_[buf_len_++] = 0u;
            compress(buf_.data());
            buf_len_ = 0;
        }
        while (buf_len_ < 56) buf_[buf_len_++] = 0u;
        for (int s = 56; s >= 0; s -= 8) {
            buf_[buf_len_++] = static_cast<uint8_t>((total_bits_ >> static_cast<unsigned>(s)) & 0xFFu);
        }
        compress(buf_.data());
        buf_len_ = 0;

        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (uint32_t w : h_) {
            for (int shift = 28; shift >= 0; shift -= 4) {
                out.push_back(kHex[(w >> static_cast<unsigned>(shift)) & 0x0Fu]);
            }
        }
        return out;
    }

private:
    void compress(const uint8_t block[64]) noexcept {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        uint32_t e = h_[4], f = h_[5], g = h_[6], hh = h_[7];

        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = hh + S1 + ch + kSha256K[static_cast<size_t>(i)] + w[i];
            const uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = S0 + maj;

            hh = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
        h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += hh;
    }

    std::array<uint32_t, 8> h_{};
    std::array<uint8_t, 64> buf_{};
    size_t buf_len_ = 0;
    uint64_t total_bits_ = 0;
};

inline std::string sha256Hex(const uint8_t* data, size_t len) noexcept {
    Sha256 h;
    h.update(data, len);
    return h.finalHex();
}

inline std::string sha256Hex(std::string_view text) noexcept {
    return sha256Hex(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

inline std::string sha256FileHex(const std::filesystem::path& path, uint64_t& out_size) noexcept {
    out_size = 0;
    try {
        std::ifstream f(path, std::ios::binary);
        if (!f) return {};
        Sha256 h;
        std::array<uint8_t, 16384> buf{};
        while (f) {
            f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
            const std::streamsize got = f.gcount();
            if (got > 0) {
                h.update(buf.data(), static_cast<size_t>(got));
                out_size += static_cast<uint64_t>(got);
            }
        }
        if (f.bad()) return {};
        return h.finalHex();
    } catch (...) {
        return {};
    }
}

inline std::string jsonEscape(std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() + 8);
    for (const char ch : s) {
        const unsigned char uc = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (uc < 0x20u) {
                    out += "\\u00";
                    out.push_back(kHex[(uc >> 4) & 0x0Fu]);
                    out.push_back(kHex[uc & 0x0Fu]);
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    return out;
}

inline std::string jsonString(std::string_view s) {
    return "\"" + jsonEscape(s) + "\"";
}

inline std::string mdCellEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char ch : s) {
        const unsigned char uc = static_cast<unsigned char>(ch);
        if (ch == '\r' || ch == '\n' || uc < 0x20u || uc == 0x7Fu) {
            out.push_back(' ');
        } else if (ch == '|') {
            out += "\\|";
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

inline std::string sanitizeOneLine(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool prev_space = false;
    for (const char ch : s) {
        const unsigned char uc = static_cast<unsigned char>(ch);
        if (uc < 0x20u || uc == 0x7Fu || ch == ' ') {
            if (!out.empty() && !prev_space) {
                out.push_back(' ');
                prev_space = true;
            }
        } else {
            out.push_back(ch);
            prev_space = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

inline void writeJsonStringArray(std::ostringstream& out,
                                 const std::vector<std::string>& items,
                                 const std::string& indent) {
    if (items.empty()) {
        out << "[]";
        return;
    }
    out << "[\n";
    for (size_t i = 0; i < items.size(); ++i) {
        out << indent << "  " << jsonString(items[i]);
        if (i + 1 < items.size()) out << ",";
        out << "\n";
    }
    out << indent << "]";
}

} // namespace manifest_detail

struct ArtifactEntry {
    std::string role;          // e.g. "smf", "uasf", "soundmap", "style_edit_sheet", "report", "unresolved_sounds"
    std::string relative_path; // e.g. "converted.mid"
    uint64_t    size_bytes = 0;
    std::string sha256;
};

struct ConversionOptionsRecord {
    std::string mode = "quality";        // "quality" | "raw"
    uint16_t    ppqn = 0;                // 0 = source passthrough, else target PPQN
    std::string break_policy = "withhold"; // "withhold" | "cap"
    bool        auto_sound_map = true;
    bool        prefer_target_user_sounds = false;
    bool        bake_rubato = false;
    bool        target_kst_used = false;
    bool        manual_soundmap_used = false;
};

struct TrackSoundResolution {
    uint8_t     src_ch = 0;
    uint8_t     korg_ch = 0xFF;
    std::string track_key;               // "bass" | "drum" | "perc" | "acc1".."acc5"
    std::string source_name;
    std::string family;
    int         yamaha_msb = -1;
    int         yamaha_lsb = -1;
    int         yamaha_pc = -1;
    std::string status;                  // "USER_OVERRIDE" | "EXACT" | "APPROX" | "UNRESOLVED_LOW_CONFIDENCE" | "UNSUPPORTED"
    int         target_msb = -1;
    int         target_lsb = -1;
    int         target_pc = -1;
    std::string target_name;
    uint8_t     score = 0;
    std::string epistemic_tag;
    std::string reason;
    std::string suggested_soundmap_line;
};

struct ConversionManifest {
    std::string schema = "aiarranger-conversion-manifest/1";
    std::string input_filename;          // basename only (no private host paths)
    uint64_t    input_size_bytes = 0;
    std::string input_sha256;
    bool        ok = false;
    std::string error;

    ConversionOptionsRecord options;

    // Honest provenance & safety boundary (always false / NOT_TESTED offline).
    bool        hardware_verified = false;
    std::string l3_status = "NOT_TESTED";
    std::string output_format = "PA5X_MAPPED_SMF_IMPORT_ARTIFACT";
    bool        native_sty_binary_verified = false;

    std::vector<ArtifactEntry> artifacts;
    std::vector<TrackSoundResolution> track_resolutions;

    // Quality & safety summary counters.
    uint32_t elements_exported = 0;
    uint32_t cv_splits_created = 0;
    uint32_t same_pitch_overlaps_fixed = 0;
    uint32_t zero_length_notes_fixed = 0;
    uint32_t anticipations_preserved = 0;
    uint32_t loop_sustain_resets = 0;
    uint32_t loop_pitchbend_resets = 0;
    uint32_t loop_cc11_resets = 0;
    uint32_t proprietary_ccs_stripped = 0;
    uint32_t megavoice_tracks_sanitized = 0;
    uint32_t megavoice_fx_stripped = 0;
    uint32_t megavoice_vel_reshaped = 0;
    uint32_t xg_sysex_params_translated = 0;
    uint32_t xg_sysex_seen = 0;
    uint32_t xg_sysex_recognized = 0;
    uint32_t xg_sysex_malformed = 0;
    bool     rhythm_tracks_swapped = false;
    uint32_t rubato_elements_baked = 0;

    std::vector<std::string> warnings;
    std::vector<std::string> reader_warnings;
    std::vector<std::string> quality_diagnostics;
};

struct BatchItemSummary {
    std::string input_relative_path;
    std::string output_subdir;
    uint64_t    input_size_bytes = 0;
    std::string input_sha256;
    bool        ok = false;
    std::string error;
    uint32_t    elements_exported = 0;
    uint32_t    resolved_tracks = 0;
    uint32_t    unresolved_tracks = 0;
    uint32_t    unsupported_tracks = 0;
    uint32_t    warning_count = 0;
    std::string manifest_relative_path;
};

struct BatchManifest {
    std::string schema = "aiarranger-batch-conversion-manifest/1";
    ConversionOptionsRecord options;
    bool        hardware_verified = false;
    std::string l3_status = "NOT_TESTED";
    std::string output_format = "PA5X_MAPPED_SMF_IMPORT_ARTIFACT";
    bool        native_sty_binary_verified = false;
    uint32_t    total_files = 0;
    uint32_t    succeeded = 0;
    uint32_t    failed = 0;
    uint32_t    total_unresolved_tracks = 0;
    std::vector<BatchItemSummary> items;
};

inline std::vector<TrackSoundResolution> buildTrackSoundResolutions(const Report& rep) {
    std::vector<TrackSoundResolution> out;
    for (const auto& tr : rep.tracks) {
        if (tr.korg_ch == 0xFF) continue;
        TrackSoundResolution res{};
        res.src_ch = tr.src_ch;
        res.korg_ch = tr.korg_ch;
        const char* tk = soundMapKey(tr.korg_ch);
        res.track_key = tk ? tk : "unknown";
        res.source_name = tr.name;
        res.family = tr.family;
        res.yamaha_msb = tr.msb;
        res.yamaha_lsb = tr.lsb;
        res.yamaha_pc = tr.pc;
        res.target_msb = tr.target_msb;
        res.target_lsb = tr.target_lsb;
        res.target_pc = tr.target_pc;
        res.target_name = tr.target_sound_name;
        res.score = tr.match_score;
        res.epistemic_tag = tr.target_epistemic_tag;

        if (!tr.match_status.empty() && tr.match_status != "NOT_EXPORTED") {
            res.status = tr.match_status;
        } else if (tr.target == "UNRESOLVED") {
            res.status = (tr.pc >= 120 && tr.pc <= 127 && tr.msb <= 0)
                             ? "UNSUPPORTED"
                             : "UNRESOLVED_LOW_CONFIDENCE";
        } else if (tr.target.find("(user map)") != std::string::npos) {
            res.status = "USER_OVERRIDE";
        } else {
            res.status = "APPROX";
        }

        const std::string cleanName = manifest_detail::sanitizeOneLine(tr.name);
        const std::string yamStr = std::to_string(tr.msb) + "/" + std::to_string(tr.lsb) + "/" + std::to_string(tr.pc);
        if (res.status == "USER_OVERRIDE") {
            res.reason = "Explicit user --sound-map override";
            res.suggested_soundmap_line = res.track_key + " " + std::to_string(res.target_msb) + " " +
                                          std::to_string(res.target_lsb) + " " + std::to_string(res.target_pc) +
                                          "  # user override";
        } else if (res.status == "EXACT" || res.status == "APPROX") {
            res.reason = (res.status == "EXACT") ? "High-confidence auto match (verify by ear on Pa5X)"
                                                 : "Approximate family/signal auto match (verify by ear on Pa5X)";
            res.suggested_soundmap_line = res.track_key + " " + std::to_string(res.target_msb) + " " +
                                          std::to_string(res.target_lsb) + " " + std::to_string(res.target_pc) +
                                          "  # " + res.status + " score=" + std::to_string(res.score) +
                                          " " + manifest_detail::sanitizeOneLine(res.epistemic_tag);
        } else if (res.status == "UNSUPPORTED") {
            res.reason = "Unsupported GM SFX / non-musical program (fail-closed, zero silent substitution)";
            res.suggested_soundmap_line = res.track_key + " unresolved  # UNSUPPORTED Yamaha " + yamStr +
                                          " '" + cleanName + "' — replace with <msb> <lsb> <pc> on Pa5X";
        } else {
            res.reason = "Unknown/proprietary bank or low-confidence timbre (fail-closed, zero silent substitution)";
            res.suggested_soundmap_line = res.track_key + " unresolved  # UNRESOLVED Yamaha " + yamStr +
                                          " '" + cleanName + "' — replace with <msb> <lsb> <pc> on Pa5X";
        }
        out.push_back(std::move(res));
    }
    return out;
}

inline ConversionManifest buildConversionManifest(
    const std::string& input_filename,
    uint64_t input_size_bytes,
    const std::string& input_sha256,
    const ConversionOptionsRecord& options,
    const Report& rep,
    const Style& style,
    const std::vector<std::string>& reader_warnings,
    uint32_t reader_malformed_sysex,
    const std::vector<ArtifactEntry>& artifacts) {
    ConversionManifest m{};
    m.input_filename = input_filename;
    m.input_size_bytes = input_size_bytes;
    m.input_sha256 = input_sha256;
    m.ok = true;
    m.options = options;
    m.hardware_verified = false;
    m.l3_status = "NOT_TESTED";
    m.output_format = "PA5X_MAPPED_SMF_IMPORT_ARTIFACT";
    m.native_sty_binary_verified = false;
    m.artifacts = artifacts;
    m.track_resolutions = buildTrackSoundResolutions(rep);

    m.elements_exported = static_cast<uint32_t>(style.elements.size());
    m.cv_splits_created = rep.cv_splits_created;
    m.same_pitch_overlaps_fixed = rep.same_pitch_overlaps_fixed;
    m.zero_length_notes_fixed = rep.zero_length_notes_fixed;
    m.anticipations_preserved = rep.anticipations_preserved;
    m.loop_sustain_resets = rep.sustain_resets_added;
    m.loop_pitchbend_resets = rep.pitchbend_resets_added;
    m.loop_cc11_resets = rep.expression_resets_added;
    m.proprietary_ccs_stripped = rep.proprietary_ccs_stripped;
    m.megavoice_tracks_sanitized = rep.megavoice_tracks_sanitized;
    m.megavoice_fx_stripped = rep.megavoice_fx_notes_stripped;
    m.megavoice_vel_reshaped = rep.megavoice_vel_reshaped;
    m.xg_sysex_params_translated = rep.xg_sysex_params_translated;
    m.xg_sysex_seen = rep.xg_sysex_seen;
    m.xg_sysex_recognized = rep.xg_sysex_recognized;
    m.xg_sysex_malformed = std::max(rep.xg_sysex_malformed, reader_malformed_sysex);
    m.rhythm_tracks_swapped = rep.rhythm_tracks_swapped;
    m.rubato_elements_baked = rep.rubato_elements_baked;
    m.warnings = rep.warnings;
    m.reader_warnings = reader_warnings;
    m.quality_diagnostics = rep.quality_diagnostics;
    return m;
}

inline std::string formatConversionManifestJson(const ConversionManifest& m) {
    using namespace manifest_detail;
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": " << jsonString(m.schema) << ",\n";
    out << "  \"ok\": " << (m.ok ? "true" : "false") << ",\n";
    if (!m.error.empty()) {
        out << "  \"error\": " << jsonString(m.error) << ",\n";
    }
    out << "  \"provenance\": {\n";
    out << "    \"hardwareVerified\": " << (m.hardware_verified ? "true" : "false") << ",\n";
    out << "    \"l3Status\": " << jsonString(m.l3_status) << ",\n";
    out << "    \"outputFormat\": " << jsonString(m.output_format) << ",\n";
    out << "    \"nativeStyBinaryVerified\": " << (m.native_sty_binary_verified ? "true" : "false") << "\n";
    out << "  },\n";
    out << "  \"input\": {\n";
    out << "    \"filename\": " << jsonString(m.input_filename) << ",\n";
    out << "    \"sizeBytes\": " << m.input_size_bytes << ",\n";
    out << "    \"sha256\": " << jsonString(m.input_sha256) << "\n";
    out << "  },\n";
    out << "  \"options\": {\n";
    out << "    \"mode\": " << jsonString(m.options.mode) << ",\n";
    out << "    \"ppqn\": " << m.options.ppqn << ",\n";
    out << "    \"break_policy\": " << jsonString(m.options.break_policy) << ",\n";
    out << "    \"auto_sound_map\": " << (m.options.auto_sound_map ? "true" : "false") << ",\n";
    out << "    \"prefer_target_user_sounds\": " << (m.options.prefer_target_user_sounds ? "true" : "false") << ",\n";
    out << "    \"bake_rubato\": " << (m.options.bake_rubato ? "true" : "false") << ",\n";
    out << "    \"target_kst_used\": " << (m.options.target_kst_used ? "true" : "false") << ",\n";
    out << "    \"manual_soundmap_used\": " << (m.options.manual_soundmap_used ? "true" : "false") << "\n";
    out << "  },\n";

    out << "  \"artifacts\": [";
    if (!m.artifacts.empty()) {
        out << "\n";
        for (size_t i = 0; i < m.artifacts.size(); ++i) {
            const auto& a = m.artifacts[i];
            out << "    {\n";
            out << "      \"role\": " << jsonString(a.role) << ",\n";
            out << "      \"relativePath\": " << jsonString(a.relative_path) << ",\n";
            out << "      \"sizeBytes\": " << a.size_bytes << ",\n";
            out << "      \"sha256\": " << jsonString(a.sha256) << "\n";
            out << "    }" << (i + 1 < m.artifacts.size() ? "," : "") << "\n";
        }
        out << "  ";
    }
    out << "],\n";

    out << "  \"soundResolution\": [";
    if (!m.track_resolutions.empty()) {
        out << "\n";
        for (size_t i = 0; i < m.track_resolutions.size(); ++i) {
            const auto& t = m.track_resolutions[i];
            out << "    {\n";
            out << "      \"srcCh\": " << (static_cast<int>(t.src_ch) + 1) << ",\n";
            out << "      \"korgCh\": " << (t.korg_ch <= 15u ? (static_cast<int>(t.korg_ch) + 1) : 0) << ",\n";
            out << "      \"trackKey\": " << jsonString(t.track_key) << ",\n";
            out << "      \"sourceName\": " << jsonString(t.source_name) << ",\n";
            out << "      \"family\": " << jsonString(t.family) << ",\n";
            out << "      \"yamahaVoice\": { \"msb\": " << t.yamaha_msb << ", \"lsb\": " << t.yamaha_lsb
                << ", \"pc\": " << t.yamaha_pc << " },\n";
            out << "      \"status\": " << jsonString(t.status) << ",\n";
            out << "      \"targetVoice\": { \"msb\": " << t.target_msb << ", \"lsb\": " << t.target_lsb
                << ", \"pc\": " << t.target_pc << " },\n";
            out << "      \"targetName\": " << jsonString(t.target_name) << ",\n";
            out << "      \"score\": " << static_cast<int>(t.score) << ",\n";
            out << "      \"epistemicTag\": " << jsonString(t.epistemic_tag) << ",\n";
            out << "      \"reason\": " << jsonString(t.reason) << ",\n";
            out << "      \"suggestedSoundmapLine\": " << jsonString(t.suggested_soundmap_line) << "\n";
            out << "    }" << (i + 1 < m.track_resolutions.size() ? "," : "") << "\n";
        }
        out << "  ";
    }
    out << "],\n";

    out << "  \"qualitySummary\": {\n";
    out << "    \"elementsExported\": " << m.elements_exported << ",\n";
    out << "    \"cvSplitsCreated\": " << m.cv_splits_created << ",\n";
    out << "    \"samePitchOverlapsFixed\": " << m.same_pitch_overlaps_fixed << ",\n";
    out << "    \"zeroLengthNotesFixed\": " << m.zero_length_notes_fixed << ",\n";
    out << "    \"anticipationsPreserved\": " << m.anticipations_preserved << ",\n";
    out << "    \"loopSustainResets\": " << m.loop_sustain_resets << ",\n";
    out << "    \"loopPitchBendResets\": " << m.loop_pitchbend_resets << ",\n";
    out << "    \"loopCc11Resets\": " << m.loop_cc11_resets << ",\n";
    out << "    \"proprietaryCcsStripped\": " << m.proprietary_ccs_stripped << ",\n";
    out << "    \"megavoiceTracksSanitized\": " << m.megavoice_tracks_sanitized << ",\n";
    out << "    \"megavoiceFxStripped\": " << m.megavoice_fx_stripped << ",\n";
    out << "    \"megavoiceVelReshaped\": " << m.megavoice_vel_reshaped << ",\n";
    out << "    \"xgSysexParamsTranslated\": " << m.xg_sysex_params_translated << ",\n";
    out << "    \"xgSysexSeen\": " << m.xg_sysex_seen << ",\n";
    out << "    \"xgSysexRecognized\": " << m.xg_sysex_recognized << ",\n";
    out << "    \"xgSysexMalformed\": " << m.xg_sysex_malformed << ",\n";
    out << "    \"rhythmTracksSwapped\": " << (m.rhythm_tracks_swapped ? "true" : "false") << ",\n";
    out << "    \"rubatoElementsBaked\": " << m.rubato_elements_baked << "\n";
    out << "  },\n";

    out << "  \"warnings\": ";
    writeJsonStringArray(out, m.warnings, "  ");
    out << ",\n";
    out << "  \"readerWarnings\": ";
    writeJsonStringArray(out, m.reader_warnings, "  ");
    out << ",\n";
    out << "  \"qualityDiagnostics\": ";
    writeJsonStringArray(out, m.quality_diagnostics, "  ");
    out << "\n";
    out << "}\n";
    return out.str();
}

inline std::string formatUnresolvedSoundsMd(const ConversionManifest& m) {
    using namespace manifest_detail;
    uint32_t user_cnt = 0, exact_cnt = 0, approx_cnt = 0, unres_cnt = 0, unsup_cnt = 0;
    for (const auto& t : m.track_resolutions) {
        if (t.status == "USER_OVERRIDE") ++user_cnt;
        else if (t.status == "EXACT") ++exact_cnt;
        else if (t.status == "APPROX") ++approx_cnt;
        else if (t.status == "UNSUPPORTED") ++unsup_cnt;
        else ++unres_cnt;
    }

    std::ostringstream out;
    out << "# Unresolved & Sound Resolution Report — " << mdCellEscape(m.input_filename) << "\n\n";
    out << "> **Epistemic Status:** `hardwareVerified: false` | `l3Status: " << mdCellEscape(m.l3_status)
        << "` | `outputFormat: " << mdCellEscape(m.output_format)
        << "` | `nativeStyBinaryVerified: false`\n\n";
    out << "- **Summary:** "
        << "user_override=" << user_cnt
        << ", exact=" << exact_cnt
        << ", approx=" << approx_cnt
        << ", unresolved_low_confidence=" << unres_cnt
        << ", unsupported=" << unsup_cnt
        << " (total exported tracks: " << m.track_resolutions.size() << ")\n";
    if (!m.input_sha256.empty()) {
        out << "- **Input SHA-256:** `" << m.input_sha256 << "` (" << m.input_size_bytes << " bytes)\n";
    }
    out << "\n";

    out << "## Per-Track Sound Resolution\n\n";
    if (m.track_resolutions.empty()) {
        out << "_No exported tracks recorded._\n\n";
    } else {
        out << "| Korg Track | Src Ch | Source Name | Family | Yamaha MSB/LSB/PC | Status | Target Bank/PC | Target Sound | Score |\n";
        out << "|---|---:|---|---|---|---|---|---|---:|\n";
        for (const auto& t : m.track_resolutions) {
            const std::string yv = std::to_string(t.yamaha_msb) + "/" + std::to_string(t.yamaha_lsb) + "/" + std::to_string(t.yamaha_pc);
            const std::string tv = (t.target_msb >= 0 && t.target_lsb >= 0 && t.target_pc >= 0)
                                       ? (std::to_string(t.target_msb) + "/" + std::to_string(t.target_lsb) + "/" + std::to_string(t.target_pc))
                                       : "UNRESOLVED";
            const std::string tname = t.target_name.empty() ? "-" : mdCellEscape(t.target_name);
            out << "| `" << mdCellEscape(t.track_key) << "`"
                << " | " << (static_cast<int>(t.src_ch) + 1)
                << " | " << mdCellEscape(t.source_name)
                << " | " << mdCellEscape(t.family)
                << " | `" << yv << "`"
                << " | `" << mdCellEscape(t.status) << "`"
                << " | `" << tv << "`"
                << " | " << tname
                << " | " << static_cast<int>(t.score)
                << " |\n";
        }
        out << "\n";
    }

    out << "## Actionable `.soundmap` Override Template\n\n";
    if (unres_cnt == 0 && unsup_cnt == 0) {
        out << "All exported tracks resolved cleanly (`0` unresolved, `0` unsupported). "
               "Confirm factory Bank/PC selections by ear on Pa5X (`K8`/`K9` unverified offline).\n\n";
    } else {
        out << "Tracks marked `UNRESOLVED_LOW_CONFIDENCE` or `UNSUPPORTED` were intentionally left without Bank/PC "
               "(fail-closed: zero silent substitution of unknown proprietary banks or GM SFX).\n\n";
        out << "Save the template below (e.g. `overrides.soundmap`), replace `unresolved` with your verified Pa5X "
               "`<msb> <lsb> <pc>` numbers, and rerun with `--sound-map overrides.soundmap`:\n\n";
        out << "```text\n";
        out << "# Manual Pa5X overrides for " << sanitizeOneLine(m.input_filename) << "\n";
        out << "# Syntax: <track> <msb 0-127> <lsb 0-127> <pc 0-127>\n";
        out << "# Optional per-element syntax: <section>.<track> <msb> <lsb> <pc> (e.g. v2.drum 120 0 40)\n";
        for (const auto& t : m.track_resolutions) {
            if (t.status == "UNRESOLVED_LOW_CONFIDENCE" || t.status == "UNSUPPORTED") {
                out << t.suggested_soundmap_line << "\n";
            }
        }
        out << "```\n\n";
    }
    return out.str();
}

inline std::string formatBatchManifestJson(const BatchManifest& b) {
    using namespace manifest_detail;
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": " << jsonString(b.schema) << ",\n";
    out << "  \"provenance\": {\n";
    out << "    \"hardwareVerified\": " << (b.hardware_verified ? "true" : "false") << ",\n";
    out << "    \"l3Status\": " << jsonString(b.l3_status) << ",\n";
    out << "    \"outputFormat\": " << jsonString(b.output_format) << ",\n";
    out << "    \"nativeStyBinaryVerified\": " << (b.native_sty_binary_verified ? "true" : "false") << "\n";
    out << "  },\n";
    out << "  \"options\": {\n";
    out << "    \"mode\": " << jsonString(b.options.mode) << ",\n";
    out << "    \"ppqn\": " << b.options.ppqn << ",\n";
    out << "    \"break_policy\": " << jsonString(b.options.break_policy) << ",\n";
    out << "    \"auto_sound_map\": " << (b.options.auto_sound_map ? "true" : "false") << ",\n";
    out << "    \"prefer_target_user_sounds\": " << (b.options.prefer_target_user_sounds ? "true" : "false") << ",\n";
    out << "    \"bake_rubato\": " << (b.options.bake_rubato ? "true" : "false") << ",\n";
    out << "    \"target_kst_used\": " << (b.options.target_kst_used ? "true" : "false") << ",\n";
    out << "    \"manual_soundmap_used\": " << (b.options.manual_soundmap_used ? "true" : "false") << "\n";
    out << "  },\n";
    out << "  \"totals\": {\n";
    out << "    \"totalFiles\": " << b.total_files << ",\n";
    out << "    \"succeeded\": " << b.succeeded << ",\n";
    out << "    \"failed\": " << b.failed << ",\n";
    out << "    \"totalUnresolvedTracks\": " << b.total_unresolved_tracks << "\n";
    out << "  },\n";
    out << "  \"items\": [";
    if (!b.items.empty()) {
        out << "\n";
        for (size_t i = 0; i < b.items.size(); ++i) {
            const auto& it = b.items[i];
            out << "    {\n";
            out << "      \"inputRelativePath\": " << jsonString(it.input_relative_path) << ",\n";
            out << "      \"outputSubdir\": " << jsonString(it.output_subdir) << ",\n";
            out << "      \"inputSizeBytes\": " << it.input_size_bytes << ",\n";
            out << "      \"inputSha256\": " << jsonString(it.input_sha256) << ",\n";
            out << "      \"ok\": " << (it.ok ? "true" : "false") << ",\n";
            out << "      \"error\": " << jsonString(it.error) << ",\n";
            out << "      \"elementsExported\": " << it.elements_exported << ",\n";
            out << "      \"resolvedTracks\": " << it.resolved_tracks << ",\n";
            out << "      \"unresolvedTracks\": " << it.unresolved_tracks << ",\n";
            out << "      \"unsupportedTracks\": " << it.unsupported_tracks << ",\n";
            out << "      \"warningCount\": " << it.warning_count << ",\n";
            out << "      \"manifestRelativePath\": " << jsonString(it.manifest_relative_path) << "\n";
            out << "    }" << (i + 1 < b.items.size() ? "," : "") << "\n";
        }
        out << "  ";
    }
    out << "]\n";
    out << "}\n";
    return out.str();
}

inline std::string formatBatchSummaryMd(const BatchManifest& b) {
    using namespace manifest_detail;
    std::ostringstream out;
    out << "# Batch Style Conversion Summary\n\n";
    out << "> **Epistemic Status:** `hardwareVerified: false` | `l3Status: " << mdCellEscape(b.l3_status)
        << "` | `outputFormat: " << mdCellEscape(b.output_format)
        << "` | `nativeStyBinaryVerified: false`\n\n";
    out << "- **Total Styles Scanned:** " << b.total_files << "\n";
    out << "- **Succeeded:** " << b.succeeded << "\n";
    out << "- **Failed:** " << b.failed << "\n";
    out << "- **Total Unresolved/Unsupported Tracks:** " << b.total_unresolved_tracks << "\n";
    out << "- **Options:** `mode=" << mdCellEscape(b.options.mode)
        << "`, `ppqn=" << b.options.ppqn
        << "`, `break_policy=" << mdCellEscape(b.options.break_policy)
        << "`, `auto_sound_map=" << (b.options.auto_sound_map ? "true" : "false")
        << "`, `bake_rubato=" << (b.options.bake_rubato ? "true" : "false") << "`\n\n";

    out << "| Input Style | Status | Output Subdir | Elements | Resolved | Unresolved | Unsupported | Warnings | Notes / Error |\n";
    out << "|---|---|---|---:|---:|---:|---:|---:|---|\n";
    for (const auto& it : b.items) {
        out << "| `" << mdCellEscape(it.input_relative_path) << "`"
            << " | " << (it.ok ? "OK" : "FAILED")
            << " | `" << mdCellEscape(it.output_subdir) << "`"
            << " | " << it.elements_exported
            << " | " << it.resolved_tracks
            << " | " << it.unresolved_tracks
            << " | " << it.unsupported_tracks
            << " | " << it.warning_count
            << " | " << (it.ok ? "-" : mdCellEscape(it.error))
            << " |\n";
    }
    out << "\n";
    return out.str();
}

} // namespace ai_arranger::exporters::korg

#endif // AI_ARRANGER_EXPORTERS_KORG_CONVERSION_MANIFEST_H
