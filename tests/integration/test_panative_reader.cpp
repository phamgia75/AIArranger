// E5 Pa-native container reader (READ-ONLY / EXPERIMENTAL) and kst-inventory SET layout detection.
// Every input is synthetic and built here from the observed rules only (no owner bytes). The optional owner
// check runs only when AIARR_PANATIVE_OWNER_SET points to a local copy of an owner SET folder.

#include "media/kst_inventory.h"
#include "media/panative/panative_reader.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace ai_arranger::media;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

static void be32(std::string& s, uint32_t v) { for (int i = 3; i >= 0; --i) s += char(v >> (8 * i)); }
static void be16(std::string& s, uint16_t v) { s += char(v >> 8); s += char(v); }
static void setBe32(std::string& s, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) s[at + i] = char(v >> (8 * (3 - i))); }

struct Synth { uint32_t id; uint8_t kind, bank, slot; std::string name, payload; };

// A file that follows only the observed container rules (see panative_reader.h).
static std::string build(const std::vector<Synth>& items, int format) {
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

static panative::Container P(const std::string& s) { return panative::parse(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
static void put(const fs::path& p, const std::string& bytes) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << bytes; }

int main() {
    std::printf("Test: Pa-native container reader (read-only, experimental)\n");
    const std::vector<Synth> items = {
        {0x06000030, 2, 17, 5, "Synthetic Style A", "OC31payload"},
        {0x09020130, 7, 17, 5, "", "x"},
        {0x12000018, 6, 0, 47, std::string("N\xE9\x01", 3), ""},   // non-ASCII bytes and an empty payload
    };
    const std::string v1 = build(items, 1), v0 = build(items, 0);
    const auto c1 = P(v1), c0 = P(v0);
    TEST("TLV directory parses", c1.ok && c1.directoryFormat == 1 && c1.items.size() == 3);
    TEST("24-byte directory parses", c0.ok && c0.directoryFormat == 0 && c0.items.size() == 3);
    TEST("names, kind/bank/slot and chunk ids come back",
         c1.items[0].name == "Synthetic Style A" && c1.items[0].kind == 2 && c1.items[0].bank == 17 && c1.items[0].slot == 5 &&
         c1.items[1].chunkId == 0x09020130 && c1.items[2].slot == 47 && c1.items[0].size == 11 &&
         c0.items[0].name == c1.items[0].name && c0.items[2].name == c1.items[2].name);
    TEST("names are printed escaped", panative::format(c1, "f").find("N\\xE9\\x01") != std::string::npos);
    TEST("an empty container is valid", P(build({}, 1)).ok && P(build({}, 0)).ok);

    // Every truncation fails cleanly; so does any padding (the wrapper length is never trusted).
    bool allTruncFail = true;
    for (size_t n = 0; n < v1.size(); ++n) allTruncFail &= !panative::parse(reinterpret_cast<const uint8_t*>(v1.data()), n).ok;
    TEST("every truncated prefix is rejected", allTruncFail);
    TEST("trailing bytes are rejected", !P(v1 + "x").ok);
    std::string big = v1; setBe32(big, 4, 0xFFFFFFF0u);
    TEST("oversized wrapper length is rejected", !P(big).ok && P(big).wrapper);
    std::string grown = v1 + std::string(8, '\0'); setBe32(grown, 4, uint32_t(grown.size() - 8));
    TEST("a consistent wrapper around a stray chunk breaks the index check", !P(grown).ok);
    TEST("no wrapper: not reported as a Pa-native file", !P("RIFF....WAVE").wrapper && !P("").wrapper);

    // Targeted corruptions.
    std::string s = v1; setBe32(s, 0x1C + 8 + 2 + 2, 0);   // record 0's name field header -> type 0, length 0
    TEST("broken directory record is rejected", !P(s).ok);
    s = v1; s[0x17] = 'X';
    TEST("missing KORF is rejected", !P(s).ok);
    s = v1; setBe32(s, v1.size() - 4, 9);
    TEST("wrong KEND count is rejected", !P(s).ok);
    s = v1; setBe32(s, c1.items[0].offset, 0x06010030);
    TEST("item chunk id not matching its directory record is rejected", !P(s).ok);
    {   // directory with 1 record but 2 item chunks: rebuild index for 2 items
        std::string a = build(items, 1), b = build({items[0]}, 1);
        // copy b's directory into a (same offsets after it only if sizes match, so expect failure either way)
        const uint32_t da = 0x1C, sa = 8 + uint32_t((uint8_t(a[da + 4]) << 24) | (uint8_t(a[da + 5]) << 16) | (uint8_t(a[da + 6]) << 8) | uint8_t(a[da + 7]));
        const uint32_t sb = 8 + uint32_t((uint8_t(b[da + 4]) << 24) | (uint8_t(b[da + 5]) << 16) | (uint8_t(b[da + 6]) << 8) | uint8_t(b[da + 7]));
        std::string m = a.substr(0, da) + b.substr(da, sb) + a.substr(da + sa);
        setBe32(m, 4, uint32_t(m.size() - 8));
        TEST("directory/item count mismatch is rejected", !P(m).ok);
    }

    // Fuzz: every single-byte change and random garbage behind the magic must not crash or over-read.
    bool noCrash = true;
    for (size_t i = 0; i < v1.size(); ++i)
        for (int bit = 0; bit < 8; ++bit) {
            std::string m = v1; m[i] ^= char(1 << bit);
            const auto c = P(m);
            noCrash &= c.ok || !c.error.empty();
        }
    uint32_t rng = 12345;
    for (int k = 0; k < 20000; ++k) {
        std::string g = "\x01\x00\x01\x14";
        g.resize(4 + rng % 300);
        for (size_t i = 4; i < g.size(); ++i) { rng = rng * 1103515245u + 12345u; g[i] = char(rng >> 16); }
        if (g.size() >= 8) setBe32(g, 4, uint32_t(g.size() - 8));
        const auto c = P(g);
        noCrash &= c.ok || !c.error.empty();
    }
    TEST("bit flips and garbage: every input yields ok or an error", noCrash);

    const fs::path root = fs::temp_directory_path() / ("aiarr_panative_" + std::to_string(::getpid()));
    fs::remove_all(root);
    put(root / "F.STY", v1);
    TEST("readFile reads a regular file", panative::readFile(root / "F.STY").ok);
    TEST("readFile: missing file and a folder are I/O errors", panative::readFile(root / "nope").ioError && panative::readFile(root).ioError);
    std::error_code ec;
    fs::create_symlink(root / "F.STY", root / "L.STY", ec);
    TEST("readFile does not follow symlinks", ec || !panative::readFile(root / "L.STY").ok);

    // kst-inventory layout detection (names only; markers from FORMAT_MATRIX / MAN4 PDF p.975).
    const fs::path p7 = root / "owner_set_copy";
    put(p7 / "GLOBAL/LIMITER.AUD", "a"); put(p7 / "STYLE/USER01.STY", "s"); put(p7 / "SONGBOOK/SONGDB.SBD/BANK01.SBD", "b");
    put(p7 / "MULTISMP/RAM.KMP", "k"); put(p7 / "PCM/RAM01.PCM", "pp");
    const Inventory i7 = scan(p7);
    TEST("Pa700/Pa1000 markers -> Pa700-family SET", i7.ok && i7.layout == kLayoutPa700);
    TEST("SET types from the Pa4X table, not '(not a documented Pa5X type)'",
         i7.by_type.count("Style bank (48 per file, MAN4 p.975)") && !i7.by_type.count("(not a documented Pa5X type)") &&
         i7.by_type.count("GLOBAL audio preset (Pa700-family, not in the Pa4X table)") && i7.by_type.at("Sample").bytes == 2 &&
         i7.by_type.at("Multisample").files == 1);
    TEST("report names the layout and the SET table", format(i7, "x").find("Layout: Pa700/Pa1000-family SET") != std::string::npos &&
         format(i7, "x").find("By documented SET file type") != std::string::npos);
    const fs::path p4 = root / "MY.SET";
    put(p4 / "SOUND/USERDK.PCG", "d"); put(p4 / "KEYBOARDSET/USER01.PRF", "k");
    TEST("Pa4X manual layout -> Pa4X-family SET", scan(p4).layout == kLayoutPa4x);
    TEST(".SET suffix alone -> Pa4X-family SET", [&] { put(root / "E.SET/X.TXT", ""); return scan(root / "E.SET").layout == kLayoutPa4x; }());
    TEST(".KST suffix -> Pa5X KST even with .STY inside",
         [&] { put(root / "K.KST/STYLE/A.STY", "s"); return scan(root / "K.KST").layout == kLayoutKst; }());
    TEST(".STG without suffix -> Pa5X KST", [&] { put(root / "plain/STYLE/A.STG", "s"); return scan(root / "plain").layout == kLayoutKst; }());
    TEST("no markers -> unknown", [&] { put(root / "misc/A.BIN", "s"); return scan(root / "misc").layout == kLayoutUnknown; }());
    fs::remove_all(root);

    // Optional: owner data (never in CI). Every file must be a valid container; the folder must read as Pa700-family.
    if (const char* owner = std::getenv("AIARR_PANATIVE_OWNER_SET")) {
        detail::Listing l;
        std::string err;
        size_t valid = 0, total = 0, items = 0;
        if (detail::listFiles(owner, l, err))
            for (const auto& [rel, e] : l.files) { const auto c = panative::readFile(e.abs); ++total; valid += c.ok; items += c.items.size(); }
        std::printf("  owner SET: %zu/%zu valid containers, %zu items\n", valid, total, items);
        TEST("owner SET: every file is a valid container", total > 0 && valid == total);
        TEST("owner SET: layout is Pa700/Pa1000-family", scan(owner).layout == kLayoutPa700);
    } else std::printf("  SKIP: owner SET check (set AIARR_PANATIVE_OWNER_SET to a local copy)\n");

    std::printf("%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
