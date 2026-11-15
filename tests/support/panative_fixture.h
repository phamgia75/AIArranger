// Synthetic Pa-native container files for tests, built from the observed rules only (media/panative/panative_reader.h).
// No owner bytes. Shared by test_panative_reader and test_library_model.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

inline void be32(std::string& s, uint32_t v) { for (int i = 3; i >= 0; --i) s += char(v >> (8 * i)); }
inline void be16(std::string& s, uint16_t v) { s += char(v >> 8); s += char(v); }

struct Synth { uint32_t id; uint8_t kind, bank, slot; std::string name, payload; };

// A file that follows only the observed container rules (see panative_reader.h).
inline std::string build(const std::vector<Synth>& items, int format) {
    std::string dir;
    for (const auto& it : items) {
        const uint16_t sub = uint16_t(it.id >> 8);
        if (format == 0) {
            std::string n = it.name; n.resize(18, '\0');
            dir += n; dir += char(it.kind); dir += char(it.bank); dir += char(it.slot); be16(dir, sub); dir += '\0';
        } else {
            std::string r;
            be16(r, 3);
            std::string n = it.name; n.resize(28, '\0');
            be16(r, 2); be16(r, 28); r += n;
            be16(r, 0); be16(r, 3); r += char(it.kind); r += char(it.bank); r += char(it.slot);
            be16(r, 1); be16(r, 2); be16(r, sub);
            r += "\x12\x34\x56\x78";                 // 4 opaque trailing bytes
            be16(dir, uint16_t(r.size())); dir += r;
        }
    }
    std::string body;
    be32(body, 0x02000018); be32(body, 12); body += std::string("\x0b\0\0\0\0\0\0KORF\0", 12);
    be32(body, format ? 0x05010018 : 0x05000018); be32(body, uint32_t(dir.size())); body += dir;
    std::vector<uint32_t> offs{0x1C};
    for (const auto& it : items) {
        offs.push_back(uint32_t(8 + body.size()));
        be32(body, it.id); be32(body, uint32_t(it.payload.size())); body += it.payload;
    }
    std::string idx = "KBEG";
    for (uint32_t o : offs) be32(idx, o);
    idx += "KEND"; be32(idx, uint32_t(offs.size()));
    be32(body, 0xFE000018); be32(body, uint32_t(idx.size())); body += idx;
    std::string f;
    be32(f, 0x01000114); be32(f, uint32_t(body.size()));
    return f + body;
}

