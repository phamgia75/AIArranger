#ifndef AI_ARRANGER_MEDIA_PANATIVE_OC31_H
#define AI_ARRANGER_MEDIA_PANATIVE_OC31_H

// Read-only OC31/OC32 payload decoder (B2, extracted from research/oc31-known-plaintext f4b1425).
// EXPERIMENTAL / OFFLINE RESEARCH ONLY. No encoder exists.
//
// Format (observed and verified on 1 804/1 804 items of one owner Pa700-family SET; no universal-format claim):
//   bytes 0..3   : magic "OC31" or "OC32" (same codec)
//   bytes 4..7   : u32 LE decoded size (must be <= kMaxOc31DecodedBytes)
//   bytes 8..n-2 : LZ77 op stream (LZO1Z-like op classes and distance packing with OC31 length biases),
//                  terminated by the 3-byte end marker 01 00 00
//   byte  n-1    : XOR of all decoded bytes
//
// Untrusted-input guarantees:
//   - Never reads outside [p, p + n); null pointer and short/oversized inputs fail closed.
//   - All bounds checks use overflow-free subtraction against remaining input and remaining output.
//   - Initial output reservation is capped so a tiny malformed payload cannot trigger a huge allocation.
//   - Unverified ops (t < 16 after literals), zero-length 16-bit matches, trailing bytes after 01 00 00,
//     decoded-size mismatches, and XOR mismatches all fail closed with out.empty() and a non-empty *err.
//   - Caller must not alias `out` with `[p, p + n)`.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ai_arranger::media::panative {

constexpr size_t kMaxOc31DecodedBytes = 64u << 20;   // 64 MiB; matches panative::kMaxFileBytes

inline bool decodeOc31(const uint8_t* p, size_t n, std::vector<uint8_t>& out, std::string* err = nullptr) {
    out.clear();
    if (err) err->clear();
    auto fail = [&](const char* e) {
        out.clear();
        if (err) *err = e;
        return false;
    };

    if (!p || n < 4 || (std::memcmp(p, "OC31", 4) != 0 && std::memcmp(p, "OC32", 4) != 0))
        return fail("not an OC31/OC32 payload");
    if (n < 12) return fail("truncated OC31/OC32 payload");

    const uint32_t want32 = uint32_t(p[4]) | (uint32_t(p[5]) << 8) | (uint32_t(p[6]) << 16) | (uint32_t(p[7]) << 24);
    if (size_t(want32) > kMaxOc31DecodedBytes) return fail("decoded size above the reader's limit");
    const size_t want = size_t(want32);

    const uint8_t* s = p + 8;
    const size_t sn = n - 9;                                // bytes 8..n-2; byte n-1 is the XOR checksum
    size_t ip = 0;
    int state = 0;                                          // 0: after match without literals, 1-3: match literals, 4: after literal run

    // Cap initial reservation so a small malformed input claiming a large decoded size
    // cannot force a multi-megabyte allocation before failing.
    constexpr size_t kInitialReserveCap = 1u << 20;         // 1 MiB
    const size_t reserveHint = sn <= kInitialReserveCap / 4 ? std::max<size_t>(sn * 4, 64) : kInitialReserveCap;
    out.reserve(std::min<size_t>(want, reserveHint));

    auto lit = [&](size_t k) {
        if (k > sn - ip || k > want - out.size()) return false;
        out.insert(out.end(), s + ip, s + ip + k);
        ip += k;
        return true;
    };

    if (ip >= sn) return fail("empty op stream");
    unsigned t = s[ip++];
    for (;;) {
        if (t == 1 && sn - ip >= 2 && s[ip] == 0 && s[ip + 1] == 0) {
            ip += 2;
            break;
        }
        size_t k = 0, d = 0;
        unsigned sb = 0;
        if (state == 0 && t < 16) {
            if (t == 1 && sn - ip >= 2 && s[ip] == 0) {     // 01 00 yy (yy in 1..3): yy literals, no copy
                const unsigned y = s[ip + 1];
                ip += 2;
                if (y == 0 || (y >> 2) != 0 || !lit(y)) return fail("bad 01 00 op");
                state = int(y);
            } else {
                if (t == 0) {
                    if (ip >= sn) return fail("truncated");
                    k = size_t(18) + size_t(s[ip++]);
                } else {
                    k = size_t(t) + 2;
                }
                if (!lit(k)) return fail("literal run past the input or the decoded size");
                state = 4;
            }
            if (ip >= sn) return fail("truncated");
            t = s[ip++];
            continue;
        }
        if (t >= 64) {                                      // M2: length 3..8, distance 1..2048
            if (ip >= sn) return fail("truncated");
            sb = s[ip++];
            k = size_t(t >> 5) + 1;
            d = size_t(1) + (size_t(t & 0x1Fu) << 6) + size_t(sb >> 2);
        } else if (t >= 16) {                               // M3 (32..63, dist 1..16384) or M4 (16..31, dist 16385..49152)
            const unsigned L = t >= 32 ? (t & 31u) : (t & 7u);
            if (L == 0) {
                if (ip >= sn) return fail("truncated");
                k = size_t(t >= 32 ? 34u : 10u) + size_t(s[ip++]);
            } else if (L == 1) {
                if (sn - ip < 2) return fail("truncated");
                k = (size_t(s[ip]) << 8) | size_t(s[ip + 1]);
                ip += 2;
                if (k == 0) return fail("zero-length match");
            } else {
                k = size_t(L) + 2;
            }
            if (sn - ip < 2) return fail("truncated");
            sb = s[ip + 1];
            d = (size_t(s[ip]) << 6) + size_t(sb >> 2) + (t >= 32 ? size_t(1) : size_t(16385) + (size_t(t & 8u) << 11));
            ip += 2;
        } else {
            return fail("unverified op");                   // t < 16 after literals: never observed
        }
        if (d == 0 || d > out.size() || k > want - out.size()) return fail("match outside the decoded data");
        const size_t from = out.size() - d;
        for (size_t i = 0; i < k; ++i) {
            const uint8_t b = out[from + i];                // copy by value before push_back (safe on reallocation)
            out.push_back(b);
        }
        state = int(sb & 3u);
        if (state && !lit(size_t(state))) return fail("literal past the input or the decoded size");
        if (ip >= sn) return fail("truncated");
        t = s[ip++];
    }

    if (out.size() != want) return fail("decoded size mismatch");
    if (ip != sn) return fail("trailing bytes after end marker");
    uint8_t x = 0;
    for (uint8_t b : out) x ^= b;
    if (x != p[n - 1]) return fail("XOR checksum mismatch");
    return true;
}

}  // namespace ai_arranger::media::panative

#endif
