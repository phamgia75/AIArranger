// Regression and untrusted-input hardening tests for read-only decodeOc31() (media/panative/oc31.h).
// All test vectors are 100% synthetic (no owner library bytes, names, or proprietary samples).

#include "media/panative/oc31.h"
#include "media/panative/panative_reader.h"   // also verifies panative_reader.h exposes decodeOc31()
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace ai_arranger::media::panative;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

static uint8_t xorBytes(const std::string& s) {
    uint8_t x = 0;
    for (char c : s) x ^= uint8_t(c);
    return x;
}

static std::string frame(const char magic[4], uint32_t want, const std::string& ops, uint8_t x) {
    std::string out(magic, 4);
    out += char(want & 0xFFu);
    out += char((want >> 8) & 0xFFu);
    out += char((want >> 16) & 0xFFu);
    out += char((want >> 24) & 0xFFu);
    out += ops;
    out += char(x);
    return out;
}

static std::string frameValid(const char magic[4], const std::string& ops, const std::string& expected) {
    return frame(magic, uint32_t(expected.size()), ops + std::string("\x01\x00\x00", 3), xorBytes(expected));
}

static bool decodeStr(const std::string& in, std::string& decoded, std::string* err) {
    std::vector<uint8_t> out = {0xAA, 0xBB};   // pre-populate to verify output is always cleared
    const bool ok = decodeOc31(reinterpret_cast<const uint8_t*>(in.data()), in.size(), out, err);
    if (!ok) {
        if (!out.empty()) {
            std::fprintf(stderr, "    invariant violated: failed decode left non-empty output (%zu bytes)\n", out.size());
            ++failures;
        }
        decoded.clear();
        return false;
    }
    decoded.assign(reinterpret_cast<const char*>(out.data()), out.size());
    return true;
}

static void appendCopy(std::string& exp, size_t d, size_t k) {
    const size_t from = exp.size() - d;
    for (size_t i = 0; i < k; ++i) exp += exp[from + i];
}

int main() {
    std::printf("Test: OC31/OC32 read-only decoder (synthetic vectors + adversarial hardening)\n");
    std::string dec, err;

    // 1. Reference vector from B2 round 1: literal run (02 -> 4), M2 + 1 trailing literal, M3 (len 8, dist 8), end, XOR.
    const std::string b2Round1("OC31\x10\0\0\0\x02" "abcd" "\x40\x0d" "Z" "\x26\x00\x1c\x01\x00\x00\x00", 23);
    err = "stale error";
    TEST("B2 round-1 vector: literal run, M2 + literal, M3, end marker, XOR",
         decodeStr(b2Round1, dec, &err) && dec == "abcdabcZabcdabcZ" && err.empty());

    // 2. Reference vector from B2 round 2: 0x21 (M3 16-bit BE length) + all M4 forms (L=2, L=0, L=1, high dist t&8).
    {
        std::string ops("\x06" "ABCDEFGH", 9), exp = "ABCDEFGH";
        ops += std::string("\x21\x40\x10\x00\x1c", 5); appendCopy(exp, 8, 16400);         // M3 16-bit BE len 0x4010, dist 8
        ops += std::string("\x12\x00\x08", 3); appendCopy(exp, 16387, 4);                  // M4 L=2: len 4, dist 16385+2
        ops += std::string("\x10\x00\x00\x01" "z", 5); appendCopy(exp, 16385, 10); exp += 'z';  // M4 L=0: len 10+0, 1 lit
        ops += std::string("\x11\x00\x05\x00\x04", 5); appendCopy(exp, 16386, 5);          // M4 L=1: len 0x0005, dist 16386
        ops += std::string("\x21\x40\x00\x00\x1c", 5); appendCopy(exp, 8, 16384);          // grow past 32 768
        ops += std::string("\x1b\x00\x0c", 3); appendCopy(exp, 32772, 5);                  // M4 L=3, t&8: dist 16385+16384+3
        const std::string big = frameValid("OC31", ops, exp);
        TEST("B2 round-2 vector: 0x21 16-bit length and all M4 forms",
             decodeStr(big, dec, &err) && dec == exp && err.empty());

        // Same payload with OC32 magic must decode identically.
        std::string big32 = big;
        big32[3] = '2';
        TEST("OC32 magic uses the same codec as OC31",
             decodeStr(big32, dec, &err) && dec == exp && err.empty());
    }

    // 3. Zero decoded size (valid minimal 12-byte frame) and nullptr err parameter.
    {
        const std::string empty31 = frameValid("OC31", "", "");
        const std::string empty32 = frameValid("OC32", "", "");
        TEST("zero decoded size (OC31 and OC32)",
             empty31.size() == 12 &&
             decodeStr(empty31, dec, &err) && dec.empty() && err.empty() &&
             decodeStr(empty32, dec, &err) && dec.empty() && err.empty());

        std::vector<uint8_t> out = {1, 2, 3};
        TEST("err == nullptr works on valid and invalid inputs",
             decodeOc31(reinterpret_cast<const uint8_t*>(empty31.data()), empty31.size(), out, nullptr) && out.empty() &&
             !decodeOc31(reinterpret_cast<const uint8_t*>("BAD!"), 4, out, nullptr) && out.empty());
    }

    // 4. Literal runs: t == 0 (18 + e, min 18 and max 273), t == 1 (3 bytes, non-zero first byte), t == 15 (17 bytes).
    {
        const std::string lit18 = "0123456789ABCDEFGH";
        TEST("t=0 long literal run min (e=0 -> 18 bytes)",
             decodeStr(frameValid("OC31", std::string("\x00\x00", 2) + lit18, lit18), dec, &err) && dec == lit18);

        std::string lit273;
        for (int i = 0; i < 273; ++i) lit273 += char((i * 37 + 11) & 0xFF);
        TEST("t=0 long literal run max (e=255 -> 273 bytes)",
             decodeStr(frameValid("OC31", std::string("\x00\xFF", 2) + lit273, lit273), dec, &err) && dec == lit273);

        TEST("t=1 short literal run (3 bytes starting with non-zero byte)",
             decodeStr(frameValid("OC31", std::string("\x01" "XYZ", 4), "XYZ"), dec, &err) && dec == "XYZ");

        const std::string lit17 = "0123456789abcdefg";
        TEST("t=15 literal run (17 bytes)",
             decodeStr(frameValid("OC31", std::string("\x0F", 1) + lit17, lit17), dec, &err) && dec == lit17);
    }

    // 5. 01 00 yy literal escape (yy = 1, 2, 3) including leading NUL byte and state==0 after match.
    {
        TEST("01 00 01 (1 literal, NUL byte)",
             decodeStr(frameValid("OC31", std::string("\x01\x00\x01\x00", 4), std::string("\x00", 1)), dec, &err) &&
             dec == std::string("\x00", 1));
        TEST("01 00 02 (2 literals)",
             decodeStr(frameValid("OC31", std::string("\x01\x00\x02\x00\x7F", 5), std::string("\x00\x7F", 2)), dec, &err) &&
             dec == std::string("\x00\x7F", 2));
        TEST("01 00 03 (3 literals starting with NUL) followed by M2 then 01 00 01",
             [&] {
                 std::string ops("\x01\x00\x03\x00\xAA\x55", 6);
                 std::string exp("\x00\xAA\x55", 3);
                 ops += std::string("\x40\x08", 2); appendCopy(exp, 3, 3);   // M2 len 3, dist 3, 0 trailing lits -> state=0
                 ops += std::string("\x01\x00\x01!", 4); exp += '!';         // 01 00 01 in state=0
                 return decodeStr(frameValid("OC31", ops, exp), dec, &err) && dec == exp;
             }());
    }

    // 6. M2 boundaries (min len 3/dist 1 overlapping RLE, max len 8/dist 2048 + 3 trailing literals).
    {
        std::string ops, exp;
        // Build 2048 distinct-pattern bytes: 18-byte seed + M3 16-bit copy of 2030 bytes.
        std::string seed;
        for (int i = 0; i < 18; ++i) seed += char('a' + i);
        ops += std::string("\x00\x00", 2) + seed; exp += seed;
        ops += std::string("\x21\x07\xEE\x00\x44", 5); appendCopy(exp, 18, 2030);   // dist = 1 + 17 = 18, now size = 2048
        ops += std::string("\xFF\xFF" "123", 5); appendCopy(exp, 2048, 8); exp += "123";  // M2 max: t=0xFF len=8, dist=2048, 3 lits
        ops += std::string("\xE0\x00", 2); appendCopy(exp, 1, 8);                   // M2 overlapping RLE: t=0xE0 len=8, dist=1
        TEST("M2 boundaries: max len=8/dist=2048 + 3 lits and overlapping dist=1 RLE",
             decodeStr(frameValid("OC31", ops, exp), dec, &err) && dec == exp);
    }

    // 7. M3 and M4 maximum-distance / maximum-length boundaries (M3 dist=16384, M3 L=0 e=255 -> 289, M4 dist=49152, M4 L=0 e=255 -> 265, L=1 0xFFFF).
    {
        std::string ops, exp;
        const std::string seed = "0123456789ABCDEF";   // 16 bytes -> t=14 (0x0E)
        ops += std::string("\x0E", 1) + seed; exp += seed;
        // Grow to 16384 bytes using M3 L=1 (len = 16368 = 0x3FF0, dist = 16 -> x=0, sb=15<<2=0x3C)
        ops += std::string("\x21\x3F\xF0\x00\x3C", 5); appendCopy(exp, 16, 16368);
        // M3 max distance 16384 (x=0xFF, sb=0xFC -> 1 + (255<<6) + 63 = 16384), L=0 e=255 -> len 289
        ops += std::string("\x20\xFF\xFF\xFC", 4); appendCopy(exp, 16384, 289);
        // Grow past 49152 using M3 L=1 max 16-bit length 0xFFFF? Wait: grow by 32479 (0x7EDF) to hit exact 49152!
        ops += std::string("\x21\x7E\xDF\x00\x3C", 5); appendCopy(exp, 16, 32479);
        // exp.size() is now 16384 + 289 + 32479 = 49152
        // M4 max distance 49152 (t&8 set -> 16385 + 16384 + (255<<6) + 63 = 49152), L=7 (t=0x1F) -> len 9
        ops += std::string("\x1F\xFF\xFC", 3); appendCopy(exp, 49152, 9);
        // M4 L=0 e=255 -> len 265, dist = 16385 (x=0, sb=0)
        ops += std::string("\x10\xFF\x00\x00", 4); appendCopy(exp, 16385, 265);
        // M4 L=1 max 16-bit length 0xFFFF (65535), dist = 16385
        ops += std::string("\x11\xFF\xFF\x00\x00", 5); appendCopy(exp, 16385, 65535);
        TEST("M3/M4 extreme boundaries (M3 dist=16384/len=289, M4 dist=49152/len=265/len=65535)",
             decodeStr(frameValid("OC31", ops, exp), dec, &err) && dec == exp);
    }

    // 8. Exact kMaxOc31DecodedBytes (64 MiB) boundary accepted, and kMaxOc31DecodedBytes + 1 rejected.
    {
        // Build a compact RLE stream decoding to exact 64 MiB: 3 literal bytes + 1024 * 65535 + 1021 bytes = 67,108,864.
        std::string ops("\x01" "AAA", 4);
        size_t total = 3;
        while (total + 65535 <= kMaxOc31DecodedBytes) {
            ops += std::string("\x21\xFF\xFF\x00\x00", 5);   // M3 len=65535, dist=1
            total += 65535;
        }
        const size_t rem = kMaxOc31DecodedBytes - total;     // 1021 bytes
        ops += char(0x21);
        ops += char((rem >> 8) & 0xFFu);
        ops += char(rem & 0xFFu);
        ops += std::string("\x00\x00", 2);
        // XOR of 67,108,864 copies of 'A' (even count) is 0x00.
        const std::string maxValid = frame("OC31", uint32_t(kMaxOc31DecodedBytes), ops + std::string("\x01\x00\x00", 3), 0x00);
        std::vector<uint8_t> out;
        const bool okMax = decodeOc31(reinterpret_cast<const uint8_t*>(maxValid.data()), maxValid.size(), out, &err);
        TEST("exact kMaxOc31DecodedBytes (64 MiB) decodes cleanly",
             okMax && out.size() == kMaxOc31DecodedBytes && out.front() == 'A' && out.back() == 'A' && err.empty());
        out.clear();
        out.shrink_to_fit();

        const std::string over1 = frame("OC31", uint32_t(kMaxOc31DecodedBytes + 1), std::string("\x01\x00\x00", 3), 0x00);
        TEST("kMaxOc31DecodedBytes + 1 is rejected before decoding",
             !decodeStr(over1, dec, &err) && !err.empty());

        const std::string overMax32 = frame("OC31", 0xFFFFFFFFu, std::string("\x01\x00\x00", 3), 0x00);
        TEST("uint32 max decoded size (0xFFFFFFFF) is rejected",
             !decodeStr(overMax32, dec, &err) && !err.empty());

        // Tiny malformed payload claiming 64 MiB must fail quickly without allocating 64 MiB.
        const std::string fakeMax = frame("OC31", uint32_t(kMaxOc31DecodedBytes), std::string("\x01\x00\x00", 3), 0x00);
        TEST("tiny malformed payload claiming 64 MiB fails closed",
             !decodeStr(fakeMax, dec, &err) && err.find("size") != std::string::npos);
    }

    // 9. Null pointer, short, and foreign inputs.
    {
        std::vector<uint8_t> out = {0x42};
        TEST("nullptr with len=0 and len=16 rejected without crash",
             !decodeOc31(nullptr, 0, out, &err) && out.empty() && !err.empty() &&
             !decodeOc31(nullptr, 16, out, &err) && out.empty() && !err.empty());
        TEST("empty, short (<12 B), and foreign magic inputs rejected",
             !decodeStr("", dec, &err) &&
             !decodeStr("OC31", dec, &err) &&
             !decodeStr(std::string("OC31\0\0\0\0\x01\0\0", 11), dec, &err) &&
             !decodeStr(std::string("RIFF\x10\0\0\0abcd\x01\0\0\0", 16), dec, &err) &&
             !decodeStr(std::string("OC30\0\0\0\0\x01\0\0\0", 12), dec, &err) &&
             !decodeStr(std::string("OC33\0\0\0\0\x01\0\0\0", 12), dec, &err));
    }

    // 10. Every truncated prefix of a valid stream fails cleanly.
    {
        bool allTruncFail = true;
        for (size_t len = 0; len < b2Round1.size(); ++len) {
            std::vector<uint8_t> out = {0x99};
            err.clear();
            const bool ok = decodeOc31(reinterpret_cast<const uint8_t*>(b2Round1.data()), len, out, &err);
            allTruncFail &= (!ok && out.empty() && !err.empty());
        }
        TEST("every truncated prefix (0..n-1) fails closed", allTruncFail);
    }

    // 11. Size mismatch (underrun / overrun), trailing bytes after end marker, early mid-stream 01 00 00, bad XOR.
    {
        std::string badXor = b2Round1;
        badXor.back() ^= 0x01;
        TEST("wrong XOR checksum rejected", !decodeStr(badXor, dec, &err) && err.find("XOR") != std::string::npos);

        const std::string emptyBadXor = frame("OC31", 0, std::string("\x01\x00\x00", 3), 0x01);
        TEST("zero-size payload with non-zero XOR rejected", !decodeStr(emptyBadXor, dec, &err) && err.find("XOR") != std::string::npos);

        std::string underrun = b2Round1;
        underrun[4] = '\x11';   // header claims 17, stream outputs 16
        TEST("decoded size underrun rejected", !decodeStr(underrun, dec, &err) && !err.empty());

        std::string overrunLit = frame("OC31", 3, std::string("\x02" "abcd" "\x01\x00\x00", 8), xorBytes("abc"));
        TEST("literal run overrun past declared decoded size rejected", !decodeStr(overrunLit, dec, &err) && !err.empty());

        std::string overrunMatch = frame("OC31", 5, std::string("\x01" "abc" "\x40\x08\x01\x00\x00", 9), 0);
        TEST("match overrun past declared decoded size rejected", !decodeStr(overrunMatch, dec, &err) && !err.empty());

        std::string overrunTrail = frame("OC31", 6, std::string("\x01" "abc" "\x40\x09" "Z" "\x01\x00\x00", 10), 0);
        TEST("trailing-literal overrun past declared decoded size rejected", !decodeStr(overrunTrail, dec, &err) && !err.empty());

        // Trailing bytes between end marker 01 00 00 and XOR byte.
        std::string trailing = b2Round1;
        trailing.insert(trailing.size() - 1, std::string("\x00", 1));
        TEST("trailing byte after end marker rejected", !decodeStr(trailing, dec, &err) && err.find("trailing") != std::string::npos);

        // Early 01 00 00 mid-stream when state == 0 must NOT act as a NOP.
        const std::string earlyEnd = frame("OC31", 4, std::string("\x01\x00\x00\x02" "abcd" "\x01\x00\x00", 11), xorBytes("abcd"));
        TEST("early mid-stream 01 00 00 rejected (never treated as NOP)", !decodeStr(earlyEnd, dec, &err) && !err.empty());

        // Missing end marker right after reaching want.
        const std::string noEnd = frame("OC31", 4, std::string("\x02" "abcd", 5), xorBytes("abcd"));
        TEST("stream missing 01 00 00 end marker rejected", !decodeStr(noEnd, dec, &err) && !err.empty());
    }

    // 12. Invalid / unverified ops, bad 01 00 yy, zero-length 16-bit matches, OOB match distances.
    {
        // Unverified op: t < 16 after match that carried 1 literal (state == 1).
        std::string m1 = b2Round1;
        m1[16] = '\x02';
        TEST("unverified op (t < 16 after match trailing literal) rejected",
             !decodeStr(m1, dec, &err) && err == "unverified op");

        // Unverified op: t < 16 after a literal run (state == 4).
        const std::string litThenLit = frame("OC31", 8, std::string("\x02" "abcd" "\x02" "efgh" "\x01\x00\x00", 13), xorBytes("abcdefgh"));
        TEST("unverified op (t < 16 after literal run) rejected",
             !decodeStr(litThenLit, dec, &err) && err == "unverified op");

        // Bad 01 00 yy with yy >= 4.
        const std::string bad0100 = frame("OC31", 4, std::string("\x01\x00\x04" "abcd" "\x01\x00\x00", 10), xorBytes("abcd"));
        TEST("bad 01 00 yy (yy >= 4) rejected", !decodeStr(bad0100, dec, &err) && !err.empty());

        // Zero-length 16-bit BE match (M3 0x21 and M4 0x11 with length 0x0000).
        const std::string zeroM3 = frame("OC31", 4, std::string("\x02" "abcd" "\x21\x00\x00\x00\x00\x01\x00\x00", 13), xorBytes("abcd"));
        TEST("M3 16-bit zero-length match rejected", !decodeStr(zeroM3, dec, &err) && err.find("zero-length") != std::string::npos);

        const std::string zeroM4 = frame("OC31", 4, std::string("\x02" "abcd" "\x11\x00\x00\x00\x00\x01\x00\x00", 13), xorBytes("abcd"));
        TEST("M4 16-bit zero-length match rejected", !decodeStr(zeroM4, dec, &err) && err.find("zero-length") != std::string::npos);

        // Match as first op when output is empty (M2, M3, M4).
        TEST("match on empty output rejected (M2/M3/M4)",
             !decodeStr(frame("OC31", 4, std::string("\x40\x00\x01\x00\x00", 5), 0), dec, &err) &&
             !decodeStr(frame("OC31", 4, std::string("\x22\x00\x00\x01\x00\x00", 6), 0), dec, &err) &&
             !decodeStr(frame("OC31", 4, std::string("\x12\x00\x00\x01\x00\x00", 6), 0), dec, &err));

        // Match distance == out.size() + 1 (off-by-one past beginning of output).
        const std::string oobM2 = frame("OC31", 7, std::string("\x02" "abcd" "\x40\x10\x01\x00\x00", 10), 0);   // d = 1 + 4 = 5 > 4
        const std::string oobM3 = frame("OC31", 8, std::string("\x02" "abcd" "\x22\x00\x10\x01\x00\x00", 11), 0); // d = 1 + 4 = 5 > 4
        const std::string oobM4 = frame("OC31", 8, std::string("\x02" "abcd" "\x12\x00\x00\x01\x00\x00", 11), 0); // d = 16385 > 4
        TEST("off-by-one OOB match distances rejected (M2/M3/M4)",
             !decodeStr(oobM2, dec, &err) && !decodeStr(oobM3, dec, &err) && !decodeStr(oobM4, dec, &err));
    }

    // 13. Truncation inside multi-byte ops (t=0, M2, M3 L=0/L=1/dist, M4 L=0/L=1/dist, trailing literals).
    {
        const std::vector<std::string> badOps = {
            std::string("\x00", 1),                          // t=0 missing extension byte
            std::string("\x01\x00", 2),                      // 01 00 missing yy
            std::string("\x01\x00\x03" "ab", 5),             // 01 00 03 missing 3rd literal
            std::string("\x02" "abcd" "\x40", 6),            // M2 missing sb
            std::string("\x02" "abcd" "\x40\x03" "x", 8),    // M2 missing 2 of 3 trailing lits
            std::string("\x02" "abcd" "\x20", 6),            // M3 L=0 missing ext
            std::string("\x02" "abcd" "\x20\x00\x00", 8),    // M3 L=0 missing 2nd dist byte
            std::string("\x02" "abcd" "\x21\x00", 7),        // M3 L=1 missing 2nd len byte
            std::string("\x02" "abcd" "\x21\x00\x04\x00", 9),// M3 L=1 missing 2nd dist byte
            std::string("\x02" "abcd" "\x10", 6),            // M4 L=0 missing ext
            std::string("\x02" "abcd" "\x11\x00", 7),        // M4 L=1 missing 2nd len byte
            std::string("\x02" "abcd" "\x12\x00", 7),        // M4 missing 2nd dist byte
        };
        bool allRejected = true;
        for (const auto& op : badOps) {
            const std::string f = frame("OC31", 100, op, 0);
            allRejected &= (!decodeStr(f, dec, &err) && !err.empty());
        }
        TEST("truncation inside every multi-byte op fails closed", allRejected);
    }

    // 14. Deterministic bit-flip and PRNG fuzzing (no crash, no UB, fail-closed invariants).
    {
        bool invariantsOk = true;
        for (size_t i = 0; i < b2Round1.size(); ++i) {
            for (int bit = 0; bit < 8; ++bit) {
                std::string m = b2Round1;
                m[i] ^= char(1 << bit);
                std::vector<uint8_t> out = {0x5A};
                err.clear();
                const bool ok = decodeOc31(reinterpret_cast<const uint8_t*>(m.data()), m.size(), out, &err);
                if (ok) {
                    const uint32_t w = uint32_t(uint8_t(m[4])) | (uint32_t(uint8_t(m[5])) << 8) |
                                       (uint32_t(uint8_t(m[6])) << 16) | (uint32_t(uint8_t(m[7])) << 24);
                    uint8_t x = 0;
                    for (uint8_t b : out) x ^= b;
                    invariantsOk &= (out.size() == w && x == uint8_t(m.back()) && err.empty());
                } else {
                    invariantsOk &= (out.empty() && !err.empty());
                }
            }
        }

        uint32_t rng = 0x5EED1234u;
        auto nextByte = [&]() -> uint8_t {
            rng = rng * 1664525u + 1013904223u;
            return uint8_t(rng >> 24);
        };
        for (int k = 0; k < 20000; ++k) {
            const char* magic = (k & 1) ? "OC31" : "OC32";
            const size_t bodyLen = size_t(nextByte() % 96);
            std::string payload(magic, 4);
            uint32_t want = 0;
            switch (k % 5) {
                case 0: want = 0; break;
                case 1: want = nextByte() % 64; break;
                case 2: want = uint32_t(nextByte()) | (uint32_t(nextByte()) << 8); break;
                case 3: want = uint32_t(kMaxOc31DecodedBytes) - (nextByte() % 16); break;
                case 4: want = 0xFFFFFFFFu - (nextByte() % 16); break;
            }
            payload += char(want & 0xFFu);
            payload += char((want >> 8) & 0xFFu);
            payload += char((want >> 16) & 0xFFu);
            payload += char((want >> 24) & 0xFFu);
            for (size_t j = 0; j < bodyLen; ++j) payload += char(nextByte());

            std::vector<uint8_t> out = {0xEE};
            err.clear();
            const bool ok = decodeOc31(reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), out, &err);
            if (ok) {
                uint8_t x = 0;
                for (uint8_t b : out) x ^= b;
                invariantsOk &= (payload.size() >= 12 && out.size() == want && x == uint8_t(payload.back()) && err.empty());
            } else {
                invariantsOk &= (out.empty() && !err.empty());
            }
        }
        TEST("bit flips + 20,000 deterministic PRNG fuzz payloads uphold all invariants", invariantsOk);
    }

    std::printf("%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
