// E5 Pa-native container reader. READ-ONLY / EXPERIMENTAL. No writer exists.
//
// Korg publishes no layout for Pa-native files (.STY .PRF .PAD .PCG .KMP .PCM .SBD .SBL .GBL ...).
// Everything below is OBSERVED in one owner Pa700-family SET (142 files, 2026-11-13; research notes:
// pa-studio-research/interop/PA_NATIVE_CONTAINER_NOTES.md). Only rules that held in 142/142 files are checked;
// any other input is rejected, never guessed at. Item payloads are opaque (most start "OC31", meaning unknown).
//
//   file   = chunk 01 00 01 14, size = file size - 8, holding level-1 chunks back to back
//   chunk  = u32 id (BE) + u32 size (BE) + size bytes
//   level 1: header 02000018 (12 bytes, "KORF" at file offset 0x17)
//            directory 05 vv xx 18 (vv 01 = TLV records, vv 00 = 24-byte records)
//            N item chunks (opaque)
//            index FE000018: "KBEG", u32 offsets of the directory and of every item chunk, "KEND", u32 N+1
//   directory record (vv 01): u16 size (bytes after itself), u16 field count, fields {u16 type, u16 len, data},
//            then 4 opaque bytes. Field 2 = name, 0 = {kind, bank, slot}, 1 = item chunk id bytes 1..2.
//   directory record (vv 00): name[18], kind, bank, slot, item chunk id bytes 1..2, one 0 byte.
#ifndef AI_ARRANGER_MEDIA_PANATIVE_READER_H
#define AI_ARRANGER_MEDIA_PANATIVE_READER_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace ai_arranger::media::panative {

constexpr size_t kMaxFileBytes = 64u << 20;   // largest observed file 2.4 MB; anything far larger is not read

struct Item {
    uint32_t chunkId = 0, offset = 0, size = 0;   // item chunk: id, file offset of its header, payload bytes
    uint8_t kind = 0, bank = 0, slot = 0;          // directory values; meaning UNKNOWN beyond "slot is 0-based"
    std::string name;                               // raw bytes up to the first NUL (Latin-1 as observed)
};

struct Container {
    bool wrapper = false;          // starts with 01 00 01 14 (the observed Pa-native magic)
    bool ok = false;               // every observed rule held
    bool ioError = false;          // readFile could not read the file (not a regular file, unreadable)
    std::string error;
    int directoryFormat = -1;      // 1 = TLV records, 0 = 24-byte records
    uint32_t directoryId = 0;
    std::vector<Item> items;
};

namespace detail {
inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline std::string cstr(const uint8_t* p, size_t n) {
    size_t k = 0;
    while (k < n && p[k]) ++k;
    return std::string(reinterpret_cast<const char*>(p), k);
}
struct Chunk { uint32_t id, offset, size; };
}  // namespace detail

// Parses an in-memory file. Never reads outside [p, p+n); every length is checked before use.
inline Container parse(const uint8_t* p, size_t n) {
    using namespace detail;
    Container c;
    auto fail = [&](std::string e) { c.error = std::move(e); c.items.clear(); return c; };
    if (n < 8 || be32(p) != 0x01000114u) return fail("not a Pa-native wrapper (bytes 0-3 are not 01 00 01 14)");
    c.wrapper = true;
    if (n > kMaxFileBytes) return fail("file larger than the reader's limit");
    if (be32(p + 4) != n - 8) return fail("wrapper length does not match the file size (truncated or padded)");

    std::vector<Chunk> l1;
    for (size_t off = 8; off < n;) {
        if (n - off < 8) return fail("trailing bytes shorter than a chunk header at " + std::to_string(off));
        const uint32_t sz = be32(p + off + 4);
        if (sz > n - off - 8) return fail("chunk at " + std::to_string(off) + " runs past the end of the file");
        l1.push_back({be32(p + off), uint32_t(off), sz});
        off += 8 + size_t(sz);
    }
    if (l1.size() < 3) return fail("fewer than 3 level-1 chunks (header, directory, index)");
    const Chunk& h = l1.front();
    if (h.id != 0x02000018u || h.size != 12 || h.offset != 8 || std::string(reinterpret_cast<const char*>(p + 0x17), 4) != "KORF")
        return fail("header chunk 02000018 with KORF at 0x17 not found");
    const Chunk& d = l1[1];
    if ((d.id >> 24) != 0x05 || (d.id & 0xFF) != 0x18 || ((d.id >> 16) & 0xFF) > 1) return fail("directory chunk 05 0x xx 18 not found");
    c.directoryId = d.id;
    c.directoryFormat = int((d.id >> 16) & 0xFF);
    const Chunk& x = l1.back();
    if (x.id != 0xFE000018u) return fail("index chunk FE000018 is not last");
    const size_t items = l1.size() - 3;

    // Index: KBEG, offsets of directory + items, KEND, count.
    const uint8_t* q = p + x.offset + 8;
    if (x.size != 16 + 4 * items || std::string(reinterpret_cast<const char*>(q), 4) != "KBEG" ||
        std::string(reinterpret_cast<const char*>(q + x.size - 8), 4) != "KEND" || be32(q + x.size - 4) != items + 1)
        return fail("index chunk does not match the chunk list");
    for (size_t i = 0; i <= items; ++i)
        if (be32(q + 4 + 4 * i) != l1[1 + i].offset) return fail("index offset " + std::to_string(i) + " does not match the chunk list");

    const uint8_t* r = p + d.offset + 8;
    const size_t rn = d.size;
    size_t pos = 0;
    for (size_t i = 0; i < items; ++i) {
        const Chunk& ic = l1[2 + i];
        Item it{ic.id, ic.offset, ic.size};
        uint16_t sub = 0;
        if (c.directoryFormat == 0) {
            if (rn - pos < 24) return fail("directory shorter than its item count");
            const uint8_t* e = r + pos;
            it.name = cstr(e, 18);
            it.kind = e[18]; it.bank = e[19]; it.slot = e[20];
            sub = be16(e + 21);
            pos += 24;
        } else {
            if (rn - pos < 4) return fail("directory shorter than its item count");
            const size_t end = pos + 2 + be16(r + pos);
            if (be16(r + pos) < 6 || end > rn) return fail("directory record " + std::to_string(i) + " runs past the directory");
            const unsigned nf = be16(r + pos + 2);
            size_t f = pos + 4;
            bool seen0 = false, seen1 = false, seen2 = false;
            for (unsigned k = 0; k < nf; ++k) {
                if (end - f < 4) return fail("directory record " + std::to_string(i) + ": field header past the record");
                const uint16_t t = be16(r + f), len = be16(r + f + 2);
                if (len > end - f - 4) return fail("directory record " + std::to_string(i) + ": field past the record");
                const uint8_t* v = r + f + 4;
                if (t == 2) { it.name = cstr(v, len); seen2 = true; }
                else if (t == 0 && len == 3) { it.kind = v[0]; it.bank = v[1]; it.slot = v[2]; seen0 = true; }
                else if (t == 1 && len == 2) { sub = be16(v); seen1 = true; }
                f += 4 + size_t(len);
            }
            if (!seen0 || !seen1 || !seen2 || end - f != 4) return fail("directory record " + std::to_string(i) + " lacks the observed fields");
            pos = end;
        }
        if (sub != ((ic.id >> 8) & 0xFFFF)) return fail("directory record " + std::to_string(i) + " does not match its item chunk id");
        c.items.push_back(std::move(it));
    }
    if (pos != rn) return fail("directory has bytes after its last record");
    c.ok = true;
    return c;
}

// Reads one regular file (symlinks not followed) of at most kMaxFileBytes and parses it.
inline Container readFile(const std::filesystem::path& f) {
    Container c;
    c.ioError = true;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(f, ec))) { c.error = "not a regular file (symlinks are not followed)"; return c; }
    const auto size = std::filesystem::file_size(f, ec);
    if (ec) { c.error = "cannot stat: " + ec.message(); return c; }
    std::ifstream in(f, std::ios::binary);
    std::vector<uint8_t> b(size_t(std::min<uintmax_t>(size, kMaxFileBytes + 1)));
    if (!in || !in.read(reinterpret_cast<char*>(b.data()), std::streamsize(b.size()))) { c.error = "cannot read"; return c; }
    return parse(b.data(), b.size());
}

// Item names are user text: anything outside printable ASCII is shown as \xNN.
inline std::string printable(const std::string& s) {
    std::string o;
    for (unsigned char ch : s) {
        if (ch >= 0x20 && ch < 0x7F && ch != '\\') o += char(ch);
        else { char b[8]; std::snprintf(b, sizeof b, "\\x%02X", ch); o += b; }
    }
    return o;
}

inline std::string format(const Container& c, const std::string& label) {
    std::string o = label + ": ";
    if (c.ioError) return o + "ERROR: " + c.error + "\n";
    if (!c.wrapper) return o + "no Pa-native wrapper (" + c.error + ")\n";
    if (!c.ok) return o + "INVALID Pa-native container: " + c.error + "\n";
    char h[96];
    std::snprintf(h, sizeof h, "valid Pa-native container, directory %08X (format %d), %zu items\n", c.directoryId, c.directoryFormat, c.items.size());
    o += h;
    for (const auto& it : c.items) {
        char line[96];
        std::snprintf(line, sizeof line, "  kind %3u bank %3u slot %3u  chunk %08X %8u B  ", it.kind, it.bank, it.slot, it.chunkId, it.size);
        o += line + printable(it.name) + "\n";
    }
    return o;
}

}  // namespace ai_arranger::media::panative

#endif
