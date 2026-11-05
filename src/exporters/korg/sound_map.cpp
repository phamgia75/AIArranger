#include "exporters/korg/sound_map.h"

#include <sstream>

namespace ai_arranger::exporters::korg {

namespace {
constexpr struct { const char* key; uint8_t ch; } kTracks[] = {
    {"bass", kBassCh}, {"drum", kDrumCh}, {"perc", kPercCh}, {"acc1", 11},
    {"acc2", 12}, {"acc3", 13}, {"acc4", 14}, {"acc5", 15}};

// Whole token must be a decimal 0..127 ("12abc", "-1", "128" are rejected).
bool byte7(const std::string& s, uint8_t& out) {
    if (s.empty() || s.size() > 3) return false;
    int v = 0;
    for (char c : s) { if (c < '0' || c > '9') return false; v = v * 10 + (c - '0'); }
    if (v > 127) return false;
    out = static_cast<uint8_t>(v);
    return true;
}
} // namespace

const char* soundMapKey(uint8_t ch) noexcept {
    for (const auto& t : kTracks) if (t.ch == ch) return t.key;
    return nullptr;
}

SoundMapResult parseSoundMap(std::istream& in, const std::string& name) {
    SoundMapResult r;
    std::set<uint8_t> seen;
    std::string line;
    for (int n = 1; std::getline(in, line); ++n) {
        if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
        std::istringstream ss(line);
        std::vector<std::string> tok;
        for (std::string t; ss >> t;) tok.push_back(t);
        if (tok.empty()) continue;
        auto fail = [&](const std::string& m) { r.error = name + ":" + std::to_string(n) + ": " + m; return r; };

        uint8_t ch = 0xFF;
        for (const auto& t : kTracks) if (tok[0] == t.key) ch = t.ch;
        if (ch == 0xFF) return fail("unknown track '" + tok[0] + "' (bass, drum, perc, acc1..acc5)");
        if (!seen.insert(ch).second) return fail("track '" + tok[0] + "' listed twice");
        if (tok.size() == 2 && tok[1] == "unresolved") { r.unresolved.insert(ch); continue; }
        SoundRef s;
        if (tok.size() != 4 || !byte7(tok[1], s.msb) || !byte7(tok[2], s.lsb) || !byte7(tok[3], s.pc))
            return fail("expected '<track> <msb 0-127> <lsb 0-127> <pc 0-127>' or '<track> unresolved'");
        r.sounds[ch] = s;
    }
    if (in.bad()) { r.error = name + ": read error"; return r; }
    r.ok = true;
    return r;
}

std::string soundMapTemplate(const Report& r, const std::string& styleName) {
    std::string o;
    o += "# Pa5X sound map for " + styleName + "\n";
    o += "# Replace 'unresolved' with '<msb> <lsb> <pc>' read from the Pa5X Sound\n";
    o += "# Select window (or its MIDI implementation chart). Leave 'unresolved'\n";
    o += "# until the number is verified on the instrument. Never copy the Yamaha\n";
    o += "# numbers below: they select different sounds on Pa5X.\n";
    for (const auto& t : kTracks) {
        o += "\n";
        bool any = false;
        for (const auto& row : r.tracks) {
            if (row.korg_ch != t.ch) continue;
            any = true;
            o += "# Genos ch " + std::to_string(row.src_ch + 1) + " '" + row.name + "'  family: " + row.family +
                 "  Yamaha MSB/LSB/PC " + std::to_string(row.msb) + "/" + std::to_string(row.lsb) + "/" +
                 std::to_string(row.pc) + (row.megavoice ? "  MEGAVOICE CANDIDATE: " + row.megavoice_why : "") + "\n";
        }
        if (!any) o += "# no Genos part plays this track\n";
        o += std::string(t.key) + " unresolved\n";
    }
    return o;
}

} // namespace ai_arranger::exporters::korg
