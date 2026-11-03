// SFF1 reader — malformed / hostile input hardening.
//
// .sty files are user-supplied ("import your existing style library"), so the
// importer must reject or bound anything a crafted/corrupt file can express,
// never crash or terminate. Each case below was a confirmed defect:
//
//  1. Nested CASM "CSEG" sub-chunks recursed without a depth limit: 16 bytes per
//     level, ~40k levels (640 KB) overflowed an 8 MB stack (ASan stack-overflow).
//  2. Tiny 10-byte "SInt" chunks repeated to the 10 MB cap produced ~1M sections
//     with 12 fabricated tracks each (+2.2 GB RSS, 219x) — bad_alloc inside the
//     noexcept parseBuffer => std::terminate.
//  3. A zero-size chunk called memcpy(nullptr, ..., 0) (UBSan: null argument).
//  5. Division 1 + time signature 1/16 gave a 0-tick bar; the mapper's
//     unmarked-section length divided by it (SIGFPE on import). Now an import
//     error (Issue #6.1).
//
// Run under -fsanitize=address,undefined in CI (portable-core-linux job).

#include "importers/sff1/sff1_reader.h"
#include "importers/sff1/sff1_mapper.h"
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

using namespace ai_arranger::importers::sff1;

static int failures = 0;
static int passes = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

static void putId(std::vector<uint8_t>& b, const char* id) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(id[i]));
}
static void putBE32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v >> 24)); b.push_back(static_cast<uint8_t>(v >> 16));
    b.push_back(static_cast<uint8_t>(v >> 8));  b.push_back(static_cast<uint8_t>(v));
}
static void putBE32At(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    b[at] = static_cast<uint8_t>(v >> 24); b[at + 1] = static_cast<uint8_t>(v >> 16);
    b[at + 2] = static_cast<uint8_t>(v >> 8); b[at + 3] = static_cast<uint8_t>(v);
}

static bool hasWarningContaining(const ParseResult& r, const std::string& needle) {
    for (const auto& w : r.warnings)
        if (w.find(needle) != std::string::npos) return true;
    return false;
}

int main() {
    std::printf("Test: SFF1 reader malformed-input hardening\n");
    Sff1Reader reader;

    // ── 1. Deeply nested CSEG must not recurse without bound ────────────
    {
        constexpr int kLevels = 50000;   // ~800 KB, well under the 10 MB file cap
        std::vector<uint8_t> buf;
        putId(buf, "CASM");
        const size_t casmSizeAt = buf.size();
        putBE32(buf, 0);
        // Level k: "CSEG"<size> marker (skipped) + "CSEG"<size> sub-chunk (recursed).
        std::vector<size_t> sizeAts;
        for (int i = 0; i < kLevels; ++i) {
            putId(buf, "CSEG"); sizeAts.push_back(buf.size()); putBE32(buf, 0);
            putId(buf, "CSEG"); sizeAts.push_back(buf.size()); putBE32(buf, 0);
        }
        for (size_t at : sizeAts)
            putBE32At(buf, at, static_cast<uint32_t>(buf.size() - (at + 4)));
        putBE32At(buf, casmSizeAt, static_cast<uint32_t>(buf.size() - (casmSizeAt + 4)));

        const ParseResult r = reader.parseBuffer(buf, "nested_cseg.sty");
        TEST("nested CSEG x50k: returns (no stack overflow)", r.total_chunks == 1);
        TEST("nested CSEG x50k: depth limit reported as a warning",
             hasWarningContaining(r, "CASM nesting"));
    }

    // ── 2. Chunk-count amplification is bounded ─────────────────────────
    {
        const uint8_t rec[] = {'S', 'I', 'n', 't', 0, 0, 0, 2, 0x0C, 0};
        std::vector<uint8_t> buf;
        buf.reserve(2 * 1024 * 1024);
        while (buf.size() + sizeof(rec) <= 2 * 1024 * 1024)
            buf.insert(buf.end(), rec, rec + sizeof(rec));

        const ParseResult r = reader.parseBuffer(buf, "sint_flood.sty");
        TEST("SInt flood: chunk count capped (<= 1024)", r.total_chunks <= 1024);
        TEST("SInt flood: sections bounded by the chunk cap", r.sections.size() <= 1024);
        TEST("SInt flood: cap reported as a warning",
             hasWarningContaining(r, "chunk limit"));
    }

    // ── 3. Zero-size chunk: no memcpy(nullptr) (UBSan) and still parsed ──
    {
        std::vector<uint8_t> buf;
        putId(buf, "MTrk"); putBE32(buf, 0);
        putId(buf, "MThd"); putBE32(buf, 6);
        const uint8_t hdr[] = {0, 0, 0, 1, 0x01, 0xE0};
        buf.insert(buf.end(), hdr, hdr + sizeof(hdr));
        const ParseResult r = reader.parseBuffer(buf, "zero_size.sty");
        TEST("zero-size chunk: both chunks read", r.total_chunks == 2);
        TEST("zero-size chunk: following MThd still parsed (480 PPQN)",
             !r.sections.empty() && r.sections.back().resolution == 480);
    }

    // ── 4. Non-printable chunk id is rejected without corrupting position ─
    {
        std::vector<uint8_t> buf;
        putId(buf, "MThd"); putBE32(buf, 6);
        const uint8_t hdr[] = {0, 0, 0, 1, 0x01, 0xE0};
        buf.insert(buf.end(), hdr, hdr + sizeof(hdr));
        const uint8_t junk[] = {0x00, 0x01, 0x02, 0x03, 0, 0, 0, 0};
        buf.insert(buf.end(), junk, junk + sizeof(junk));
        const ParseResult r = reader.parseBuffer(buf, "junk_tail.sty");
        TEST("junk after valid chunk: valid chunk kept, parse succeeds",
             r.success && r.total_chunks == 1);
    }

    // ── 5. A meter/resolution whose bar is < 1 tick must not divide by zero ─
    {
        std::vector<uint8_t> buf;
        putId(buf, "MThd"); putBE32(buf, 6);
        const uint8_t hdr[] = {0, 0, 0, 1, 0x00, 0x01};          // division = 1 PPQN
        buf.insert(buf.end(), hdr, hdr + sizeof(hdr));
        const uint8_t trk[] = {
            0x00, 0xFF, 0x58, 0x04, 0x01, 0x04, 0x18, 0x08,       // time signature 1/16
            0x00, 0x90, 60, 100,                                  // NoteOn
            0x05, 0x80, 60, 0,                                    // NoteOff
            0x00, 0xFF, 0x2F, 0x00};                              // End of Track
        putId(buf, "MTrk"); putBE32(buf, sizeof(trk));
        buf.insert(buf.end(), trk, trk + sizeof(trk));
        const ParseResult r = reader.parseBuffer(buf, "zero_bar.sty");
        TEST("1 PPQN + 1/16 meter: reader accepts it", r.success);
        Sff1ToUasfMapper mapper;
        const auto mr = mapper.map(r);                            // SIGFPE before d85831e
        // d85831e made this import "succeed" as a style that never sounds (the
        // sequencer ignores a 0-tick bar); Issue #6.1 rejects it instead.
        TEST("1 PPQN + 1/16 meter: import does not crash and is rejected (0-tick bar)",
             !mr.success && mr.error.find("0 ticks") != std::string::npos);
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
