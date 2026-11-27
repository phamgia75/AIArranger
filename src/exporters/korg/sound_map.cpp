#include "exporters/korg/sound_map.h"

#include <sstream>
#include <string_view>

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

std::string sanitizeSoundMapComment(std::string_view s) {
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

uint8_t lookupTrackChannel(const std::string& key) noexcept {
    for (const auto& t : kTracks) {
        if (key == t.key) return t.ch;
    }
    return 0xFF;
}
} // namespace

const char* soundMapKey(uint8_t ch) noexcept {
    for (const auto& t : kTracks) if (t.ch == ch) return t.key;
    return nullptr;
}

const char* soundMapSectionKey(uasf::SectionType section) noexcept {
    using T = uasf::SectionType;
    switch (section) {
        case T::Main1:   return "v1";
        case T::Main2:   return "v2";
        case T::Main3:   return "v3";
        case T::Main4:   return "v4";
        case T::Intro1:  return "i1";
        case T::Intro2:  return "i2";
        case T::Intro3:  return "i3";
        case T::Fill1:   return "f1";
        case T::Fill2:   return "f2";
        case T::Fill3:   return "f3";
        case T::Fill4:   return "f4";
        case T::Ending1: return "e1";
        case T::Ending2: return "e2";
        case T::Ending3: return "e3";
        case T::Break:   return "br";
        default:         return nullptr;
    }
}

bool parseSoundMapSectionKey(const std::string& token, uasf::SectionType& out) noexcept {
    using T = uasf::SectionType;
    static constexpr struct { const char* key; T sec; } kSections[] = {
        {"v1", T::Main1},   {"v1cv1", T::Main1},   {"main_a", T::Main1},
        {"v2", T::Main2},   {"v2cv1", T::Main2},   {"main_b", T::Main2},
        {"v3", T::Main3},   {"v3cv1", T::Main3},   {"main_c", T::Main3},
        {"v4", T::Main4},   {"v4cv1", T::Main4},   {"main_d", T::Main4},
        {"i1", T::Intro1},  {"i1cv1", T::Intro1},  {"intro_a", T::Intro1},
        {"i2", T::Intro2},  {"i2cv1", T::Intro2},  {"intro_b", T::Intro2},
        {"i3", T::Intro3},  {"i3cv1", T::Intro3},  {"intro_c", T::Intro3},
        {"f1", T::Fill1},   {"f1cv1", T::Fill1},   {"fill_aa", T::Fill1},
        {"f2", T::Fill2},   {"f2cv1", T::Fill2},   {"fill_bb", T::Fill2},
        {"f3", T::Fill3},   {"f3cv1", T::Fill3},   {"fill_cc", T::Fill3},
        {"f4", T::Fill4},   {"f4cv1", T::Fill4},   {"fill_dd", T::Fill4},
        {"e1", T::Ending1}, {"e1cv1", T::Ending1}, {"ending_a", T::Ending1},
        {"e2", T::Ending2}, {"e2cv1", T::Ending2}, {"ending_b", T::Ending2},
        {"e3", T::Ending3}, {"e3cv1", T::Ending3}, {"ending_c", T::Ending3},
        {"br", T::Break},   {"break", T::Break},
    };
    for (const auto& s : kSections) {
        if (token == s.key) {
            out = s.sec;
            return true;
        }
    }
    return false;
}

SoundMapResult parseSoundMap(std::istream& in, const std::string& name) {
    SoundMapResult r;
    std::set<uint8_t> seen;
    std::set<std::pair<uasf::SectionType, uint8_t>> seenElem;
    std::string line;
    for (int n = 1; std::getline(in, line); ++n) {
        auto fail = [&](const std::string& m) { r.error = name + ":" + std::to_string(n) + ": " + m; return r; };

        if (line.find('\0') != std::string::npos) {
            return fail("embedded NUL byte rejected");
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.find('\r') != std::string::npos) {
            return fail("embedded CR character rejected");
        }
        if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
        for (char c : line) {
            const unsigned char uc = static_cast<unsigned char>(c);
            if ((uc < 0x20u && c != '\t') || uc == 0x7Fu) {
                return fail("control character rejected");
            }
        }

        std::istringstream ss(line);
        std::vector<std::string> tok;
        for (std::string t; ss >> t;) tok.push_back(t);
        if (tok.empty()) continue;

        const size_t dot = tok[0].find('.');
        if (dot != std::string::npos) {
            if (tok[0].find('.', dot + 1) != std::string::npos) {
                return fail("invalid per-element key '" + tok[0] + "' (expected '<section>.<track>')");
            }
            const std::string secTok = tok[0].substr(0, dot);
            const std::string trkTok = tok[0].substr(dot + 1);
            uasf::SectionType sec{};
            if (!parseSoundMapSectionKey(secTok, sec)) {
                return fail("unknown section '" + secTok +
                            "' (v1..v4, i1..i3, f1..f4, e1..e3, br, main_a..main_d, intro_a..intro_c, fill_aa..fill_dd, ending_a..ending_c, break)");
            }
            const uint8_t ch = lookupTrackChannel(trkTok);
            if (ch == 0xFF) {
                return fail("unknown track '" + trkTok + "' (bass, drum, perc, acc1..acc5)");
            }
            if (!seenElem.insert({sec, ch}).second) {
                return fail("element track '" + tok[0] + "' listed twice");
            }
            if (tok.size() == 2 && tok[1] == "unresolved") {
                r.element_unresolved.insert({sec, ch});
                continue;
            }
            SoundRef s;
            if (tok.size() != 4 || !byte7(tok[1], s.msb) || !byte7(tok[2], s.lsb) || !byte7(tok[3], s.pc)) {
                return fail("expected '<section>.<track> <msb 0-127> <lsb 0-127> <pc 0-127>' or '<section>.<track> unresolved'");
            }
            r.element_sounds[{sec, ch}] = s;
            continue;
        }

        const uint8_t ch = lookupTrackChannel(tok[0]);
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
    o += "# Pa5X sound map for " + sanitizeSoundMapComment(styleName) + "\n";
    o += "# Replace 'unresolved' with '<msb> <lsb> <pc>' read from the Pa5X Sound\n";
    o += "# Select window (or its MIDI implementation chart). Optional per-element\n";
    o += "# overrides use '<section>.<track> <msb> <lsb> <pc>' (e.g. v2.drum 120 0 40).\n";
    o += "# Leave 'unresolved' until the number is verified on the instrument. Never\n";
    o += "# copy the Yamaha numbers below: they select different sounds on Pa5X.\n";
    for (const auto& t : kTracks) {
        o += "\n";
        bool any = false;
        for (const auto& row : r.tracks) {
            if (row.korg_ch != t.ch) continue;
            any = true;
            o += "# Genos ch " + std::to_string(row.src_ch + 1) + " '" + sanitizeSoundMapComment(row.name) +
                 "'  family: " + sanitizeSoundMapComment(row.family) +
                 "  Yamaha MSB/LSB/PC " + std::to_string(row.msb) + "/" + std::to_string(row.lsb) + "/" +
                 std::to_string(row.pc) +
                 (row.megavoice ? "  MEGAVOICE CANDIDATE: " + sanitizeSoundMapComment(row.megavoice_why) : "") + "\n";
        }
        if (!any) o += "# no Genos part plays this track\n";
        o += std::string(t.key) + " unresolved\n";
    }
    return o;
}

} // namespace ai_arranger::exporters::korg
