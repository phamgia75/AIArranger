#include "session/session_persistence.h"

#include <fstream>
#include <limits>
#include <type_traits>
#include <sstream>
#include <string>

namespace ai_arranger::session {

namespace {

bool readInt(const std::string& j, const std::string& key, long& out) {
    const std::string needle = "\"" + key + "\"";
    size_t p = j.find(needle);
    if (p == std::string::npos) return false;
    p = j.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) ++p;
    size_t start = p;
    if (p < j.size() && (j[p] == '-' || j[p] == '+')) ++p;
    while (p < j.size() && j[p] >= '0' && j[p] <= '9') ++p;
    if (p == start) return false;
    try { out = std::stol(j.substr(start, p - start)); } catch (...) { return false; }
    return true;
}

// Reads "key": "value" (no escape handling needed for our simple values).
bool readStr(const std::string& j, const std::string& key, std::string& out) {
    const std::string needle = "\"" + key + "\"";
    size_t p = j.find(needle);
    if (p == std::string::npos) return false;
    p = j.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    p = j.find('"', p);
    if (p == std::string::npos) return false;
    size_t end = j.find('"', p + 1);
    if (end == std::string::npos) return false;
    out = j.substr(p + 1, end - p - 1);
    return true;
}

std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o;
}

} // namespace

bool SessionPersistence::isValid(const PerformerSession& s) noexcept {
    if (s.version != PerformerSession::kVersion) return false;
    if (s.variation > 3) return false;
    if (s.tempo_bpm < 20 || s.tempo_bpm > 400) return false;
    if (s.split_point > 127) return false;
    if (s.chord_scan_mode > 3) return false;
    if (s.groove_profile > 4) return false;
    if (s.ui_layout > 1) return false;
    return true;
}

std::string SessionPersistence::serialize(const PerformerSession& s) {
    std::ostringstream o;
    o << "{\n"
      << "  \"version\": " << s.version << ",\n"
      << "  \"style_name\": \"" << esc(s.style_name) << "\",\n"
      << "  \"variation\": " << (int)s.variation << ",\n"
      << "  \"tempo_bpm\": " << s.tempo_bpm << ",\n"
      << "  \"split_point\": " << (int)s.split_point << ",\n"
      << "  \"manual_bass\": " << (s.manual_bass ? 1 : 0) << ",\n"
      << "  \"sync_armed\": " << (s.sync_armed ? 1 : 0) << ",\n"
      << "  \"chord_scan_mode\": " << (int)s.chord_scan_mode << ",\n"
      << "  \"groove_profile\": " << (int)s.groove_profile << ",\n"
      << "  \"midi_output_name\": \"" << esc(s.midi_output_name) << "\",\n"
      << "  \"ui_layout\": " << (int)s.ui_layout << ",\n"
      << "  \"theme\": " << (int)s.theme << "\n"
      << "}\n";
    return o.str();
}

bool SessionPersistence::deserialize(const std::string& j, PerformerSession& out) {
    PerformerSession s;
    // Read an integer field and store it only if it fits the destination type;
    // an out-of-range value (e.g. tempo_bpm 4294967416) must be rejected, not
    // wrapped into a valid-looking one before isValid() sees it.
    auto field = [&j](const char* key, auto& dst) {
        long v = 0;
        if (!readInt(j, key, v)) return false;
        using T = std::remove_reference_t<decltype(dst)>;
        if constexpr (std::is_same_v<T, bool>) {
            dst = (v != 0);
        } else {
            if (v < static_cast<long>(std::numeric_limits<T>::min()) ||
                static_cast<unsigned long>(v) > std::numeric_limits<T>::max())
                return false;
            dst = static_cast<T>(v);
        }
        return true;
    };
    if (!field("version", s.version)) return false;
    readStr(j, "style_name", s.style_name);   // optional
    if (!field("variation", s.variation)) return false;
    if (!field("tempo_bpm", s.tempo_bpm)) return false;
    if (!field("split_point", s.split_point)) return false;
    if (!field("manual_bass", s.manual_bass)) return false;
    if (!field("sync_armed", s.sync_armed)) return false;
    if (!field("chord_scan_mode", s.chord_scan_mode)) return false;
    if (!field("groove_profile", s.groove_profile)) return false;
    readStr(j, "midi_output_name", s.midi_output_name); // optional
    if (!field("ui_layout", s.ui_layout)) return false;
    if (!field("theme", s.theme)) return false;

    if (!isValid(s)) return false;
    out = s;
    return true;
}

bool SessionPersistence::save(const PerformerSession& s, const std::string& path) {
    if (!isValid(s)) return false;
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << serialize(s);
    return f.good();
}

bool SessionPersistence::load(const std::string& path, PerformerSession& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf();
    return deserialize(ss.str(), out);
}

} // namespace ai_arranger::session
