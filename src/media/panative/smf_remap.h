// M2e-1: Portable in-memory SMF Bank/PC Remapper & Pa5X Root-Tree Round-Trip Verifier.
//
// Transforms Standard MIDI Files (Format 0 & Format 1) and Pa5X Root-tree containers
// (.STG / .KSC / .PDC) strictly in memory with zero JUCE/GUI dependency and zero disk I/O.
//
// Invariants (1:1 parity with kst_closure.h::selects() and root_tree.h):
//  * Strict fail-closed SMF 1.0 parsing: MThd (6 <= hl <= n - 8, format in {0, 1},
//    format == 0 => ntrks == 1), MTrk chunks only, exact track count, zero trailing bytes,
//    End-of-Track (FF 2F 00) exactly at end of every track, all channel data bytes < 0x80,
//    Meta/SysEx cancel running status, max kMaxSelectEvents (4096) select events.
//  * Bounded O(selects) memory in Pass 1: validates every byte of every MTrk without storing
//    non-select events; Pass 2 bulk-copies untouched byte spans and rewrites only affected
//    select event spans.
//  * Cross-track (CC0, CC32, PC) bank state in Format 1 merged by (tick, track, orderInTrack).
//  * Simultaneous rule application: rules map original (MSB, LSB, PC) -> target (MSB, LSB, PC)
//    in a single pass (A -> B and B -> A swap cleanly without double-remapping).
//  * Rule validation: rejects any rule with MSB/LSB/PC > 127 (INVALID_REMAP_RULE). Valid
//    identity rules (src == dst, all fields <= 127) are ignored as explicit no-ops so callers
//    never trigger spurious rewrites.
//  * No-op preservation: when 0 Program Change events match any effective rule, returns the
//    exact original SMF / Root-tree bytes byte-for-byte untouched (modified == false).
//  * Safe rewrite: preserves Meta, Korg SysEx (F0 42 ...), Notes, other CCs, MThd bytes, and
//    XML leaves byte-for-byte; restores (CC0, CC32) before later PCs on shared or cross-track
//    banks; expands running status safely whenever inserted bytes change output status.
//  * Mandatory self-verification (verifyRoundTrip = true): re-parses emitted SMF via
//    closure::selects() and re-parses emitted Root-tree via roottree::parse() +
//    closure::leafSelects(), verifying 1:1 select parity and byte-identical non-.mid leaves.
#ifndef AI_ARRANGER_MEDIA_PANATIVE_SMF_REMAP_H
#define AI_ARRANGER_MEDIA_PANATIVE_SMF_REMAP_H

#include "media/panative/kst_closure.h"
#include "media/panative/panative_reader.h"
#include "media/panative/root_tree.h"

#include <algorithm>
#include <array>
#include <compare>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ai_arranger::media::panative::smf_remap {

struct MidiTriple {
    uint8_t msb = 0;
    uint8_t lsb = 0;
    uint8_t pc = 0;

    constexpr bool valid() const noexcept {
        return msb <= 127u && lsb <= 127u && pc <= 127u;
    }

    constexpr auto operator<=>(const MidiTriple&) const noexcept = default;
    constexpr bool operator==(const MidiTriple&) const noexcept = default;
};

struct RemapOptions {
    bool strictUnresolved = true;
    bool verifyRoundTrip = true;
};

struct SmfRemapResult {
    bool ok = false;
    std::string error;
    bool modified = false;
    size_t remappedEvents = 0;
    size_t unchangedEvents = 0;
    std::vector<uint8_t> bytes;
};

struct RootTreeRemapResult {
    bool ok = false;
    std::string error;
    std::string failedLeafPath;
    bool modified = false;
    size_t inspectedMidiLeaves = 0;
    size_t modifiedMidiLeaves = 0;
    size_t remappedEvents = 0;
    std::vector<uint8_t> bytes;
};

namespace detail {

inline bool validateRules(const std::map<MidiTriple, MidiTriple>& rules,
                          std::map<MidiTriple, MidiTriple>& effectiveRules,
                          std::string& error) {
    effectiveRules.clear();
    for (const auto& [src, dst] : rules) {
        if (!src.valid() || !dst.valid()) {
            error = "INVALID_REMAP_RULE: MIDI MSB/LSB/PC values must be in 0..127";
            return false;
        }
        if (src == dst) continue;  // Valid identity rule is ignored as a no-op.
        effectiveRules.emplace(src, dst);
    }
    return true;
}

inline bool readVlq(const uint8_t* p, size_t& q, size_t end, uint32_t& out) {
    out = 0;
    for (int i = 0; i < 4; ++i) {
        if (q >= end) return false;
        const uint8_t b = p[q++];
        out = (out << 7) | uint32_t(b & 0x7Fu);
        if ((b & 0x80u) == 0u) return true;
    }
    return false;
}

inline void appendBe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t((v >> 24) & 0xFFu));
    out.push_back(uint8_t((v >> 16) & 0xFFu));
    out.push_back(uint8_t((v >> 8) & 0xFFu));
    out.push_back(uint8_t(v & 0xFFu));
}

inline bool selectsEqual(const std::vector<closure::Select>& a,
                         const std::vector<closure::Select>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].channel != b[i].channel || a[i].msb != b[i].msb ||
            a[i].lsb != b[i].lsb || a[i].program != b[i].program) {
            return false;
        }
    }
    return true;
}

inline bool isMidiLeafName(const std::string& name) {
    return name.size() > 4 && closure::upper(name.substr(name.size() - 4)) == ".MID";
}

inline bool isLowercaseMidiLeafName(const std::string& name) {
    return name.size() > 4 && name.compare(name.size() - 4, 4, ".mid") == 0;
}

struct TrackSpan {
    size_t bodyStart = 0;
    size_t bodyEnd = 0;
    size_t selBegin = 0;
    size_t selEnd = 0;
    size_t matchedCount = 0;
};

// O(selects) metadata stored per CC0/CC32/PC event (bounded by closure::kMaxSelectEvents = 4096).
struct SelectEvent {
    uint64_t tick = 0;
    unsigned track = 0;
    size_t trackEventSeq = 0;
    size_t deltaStart = 0;
    size_t deltaEnd = 0;
    size_t evEnd = 0;

    uint8_t status = 0;
    uint8_t ch0 = 0;   // 0..15
    int cc = -1;       // 0, 32, or -1 (PC)
    uint8_t value = 0;

    // Immediate next event on the same track, if it omitted its status byte via running status.
    bool nextReliesOnRunningStatus = false;
    size_t nextMsgStart = 0;
    uint8_t nextStatus = 0;

    // Resolved bank & rewrite plan
    int resolvedMsb = -1;
    int resolvedLsb = -1;
    bool matched = false;
    MidiTriple target{};

    bool inPlacePcOnly = false;
    bool inPlaceWithBank = false;
    bool restoreMsbAfter = false;
    bool restoreLsbAfter = false;

    bool overwrittenInPlace = false;
    uint8_t newCcVal = 0;
};

inline SmfRemapResult remapSmfValidated(const uint8_t* data, size_t size,
                                        const std::map<MidiTriple, MidiTriple>& effectiveRules,
                                        RemapOptions opts,
                                        std::vector<closure::Select>* outExpectedSelects) {
    SmfRemapResult res;
    auto fail = [&](std::string e) {
        res.ok = false;
        res.modified = false;
        res.remappedEvents = 0;
        res.unchangedEvents = 0;
        res.bytes.clear();
        res.error = std::move(e);
        return res;
    };

    if (size > panative::kMaxFileBytes) {
        return fail("SIZE_LIMIT_EXCEEDED: SMF exceeds kMaxFileBytes");
    }
    if (data == nullptr || size < 14 || std::memcmp(data, "MThd", 4) != 0) {
        return fail("MALFORMED_MIDI_LEAF: not a Standard MIDI File");
    }
    const uint32_t hl = panative::detail::be32(data + 4);
    if (hl < 6 || hl > size - 8) {
        return fail("MALFORMED_MIDI_LEAF: header length is not 6..file size");
    }
    const unsigned format = (unsigned(data[8]) << 8) | data[9];
    const unsigned declared = (unsigned(data[10]) << 8) | data[11];
    if (format > 1) {
        return fail("MALFORMED_MIDI_LEAF: SMF format " + std::to_string(format) + " (only 0 and 1 are supported)");
    }
    if (format == 0 && declared != 1) {
        return fail("MALFORMED_MIDI_LEAF: format 0 must have exactly one track");
    }

    // Pass 1: strict full validation matching closure::selects() 1:1 while storing only O(selects) metadata.
    const size_t maxPossibleTracks = (size - 8u - size_t(hl)) / 8u;
    std::vector<TrackSpan> tracks;
    tracks.reserve(std::min<size_t>(declared, maxPossibleTracks));
    std::vector<SelectEvent> selectEvents;

    unsigned found = 0;
    size_t o = 8 + size_t(hl);
    while (size - o >= 8) {
        const uint32_t len = panative::detail::be32(data + o + 4);
        if (std::memcmp(data + o, "MTrk", 4) != 0) {
            return fail("MALFORMED_MIDI_LEAF: chunk other than MTrk");
        }
        o += 8;
        if (len > size - o) {
            return fail("MALFORMED_MIDI_LEAF: chunk runs past the file");
        }
        const size_t bodyStart = o;
        const size_t end = o + size_t(len);
        const unsigned track = found++;
        const size_t selBegin = selectEvents.size();

        bool endOfTrack = false;
        uint8_t status = 0;
        uint64_t tick = 0;
        size_t trackEventSeq = 0;
        bool prevWasSelect = false;
        size_t prevSelectIdx = 0;

        while (o < end) {
            if (endOfTrack) {
                return fail("MALFORMED_MIDI_LEAF: event after End of Track");
            }
            const size_t deltaStart = o;
            uint32_t delta = 0;
            if (!readVlq(data, o, end, delta) || o >= end) {
                return fail("MALFORMED_MIDI_LEAF: event runs past its track");
            }
            const size_t deltaEnd = o;
            tick += delta;
            const uint8_t b = data[o];

            if (b == 0xFFu) {
                if (end - o < 2) {
                    return fail("MALFORMED_MIDI_LEAF: meta event runs past its track");
                }
                const uint8_t type = data[o + 1];
                o += 2;
                uint32_t metaLen = 0;
                if (!readVlq(data, o, end, metaLen) || metaLen > end - o) {
                    return fail("MALFORMED_MIDI_LEAF: meta event runs past its track");
                }
                endOfTrack = (type == 0x2Fu && metaLen == 0u);
                o += size_t(metaLen);
                status = 0;
                prevWasSelect = false;
                ++trackEventSeq;
                continue;
            }
            if (b == 0xF0u || b == 0xF7u) {
                ++o;
                uint32_t syxLen = 0;
                if (!readVlq(data, o, end, syxLen) || syxLen > end - o) {
                    return fail("MALFORMED_MIDI_LEAF: SysEx runs past its track");
                }
                o += size_t(syxLen);
                status = 0;
                prevWasSelect = false;
                ++trackEventSeq;
                continue;
            }

            bool hadExplicitStatus = false;
            if ((b & 0x80u) != 0u) {
                status = b;
                ++o;
                hadExplicitStatus = true;
            }
            if (status < 0x80u || status >= 0xF0u) {
                return fail("MALFORMED_MIDI_LEAF: data byte without a running status");
            }
            const uint8_t hi = uint8_t(status & 0xF0u);
            const uint8_t ch0 = uint8_t(status & 0x0Fu);
            const uint8_t k = (hi == 0xC0u || hi == 0xD0u) ? 1u : 2u;
            if (size_t(k) > end - o) {
                return fail("MALFORMED_MIDI_LEAF: channel message runs past its track");
            }
            for (uint8_t d = 0; d < k; ++d) {
                if ((data[o + size_t(d)] & 0x80u) != 0u) {
                    return fail("MALFORMED_MIDI_LEAF: channel message data byte with the high bit set");
                }
            }
            const uint8_t d0 = data[o];
            const uint8_t d1 = (k == 2u) ? data[o + 1] : 0u;
            o += size_t(k);

            if (prevWasSelect) {
                if (!hadExplicitStatus) {
                    selectEvents[prevSelectIdx].nextReliesOnRunningStatus = true;
                    selectEvents[prevSelectIdx].nextMsgStart = deltaEnd;
                    selectEvents[prevSelectIdx].nextStatus = status;
                }
                prevWasSelect = false;
            }

            const bool isBankCc = (hi == 0xB0u && (d0 == 0u || d0 == 32u));
            const bool isPc = (hi == 0xC0u);
            if (isBankCc || isPc) {
                if (selectEvents.size() >= closure::kMaxSelectEvents) {
                    return fail("EVENT_LIMIT_EXCEEDED: more than " + std::to_string(closure::kMaxSelectEvents) +
                                " bank-select/Program Change events");
                }
                SelectEvent se;
                se.tick = tick;
                se.track = track;
                se.trackEventSeq = trackEventSeq;
                se.deltaStart = deltaStart;
                se.deltaEnd = deltaEnd;
                se.evEnd = o;
                se.status = status;
                se.ch0 = ch0;
                se.cc = isBankCc ? int(d0) : -1;
                se.value = isBankCc ? d1 : d0;
                prevSelectIdx = selectEvents.size();
                prevWasSelect = true;
                selectEvents.push_back(se);
            }
            ++trackEventSeq;
        }
        if (!endOfTrack) {
            return fail("MALFORMED_MIDI_LEAF: track does not end with End of Track");
        }
        tracks.push_back(TrackSpan{bodyStart, end, selBegin, selectEvents.size(), 0u});
    }
    if (o != size) {
        return fail("MALFORMED_MIDI_LEAF: trailing bytes after the last track");
    }
    if (found != declared) {
        return fail("MALFORMED_MIDI_LEAF: header declares " + std::to_string(declared) +
                    " tracks, file has " + std::to_string(found));
    }

    // Build global (tick, track, orderInTrack) timeline identically to closure::selects().
    std::vector<size_t> timeline(selectEvents.size());
    for (size_t i = 0; i < timeline.size(); ++i) timeline[i] = i;
    std::stable_sort(timeline.begin(), timeline.end(), [&](size_t ia, size_t ib) {
        const auto& a = selectEvents[ia];
        const auto& b = selectEvents[ib];
        return a.tick != b.tick ? a.tick < b.tick : a.track < b.track;
    });

    std::array<std::array<int, 2>, 16> bank{};
    for (auto& chBank : bank) chBank = {-1, -1};

    std::vector<closure::Select> expectedSelects;
    size_t matchedCount = 0;
    size_t unchangedCount = 0;

    for (size_t sIdx : timeline) {
        auto& ev = selectEvents[sIdx];
        if (ev.cc == 0) {
            bank[ev.ch0][0] = int(ev.value);
            continue;
        }
        if (ev.cc == 32) {
            bank[ev.ch0][1] = int(ev.value);
            continue;
        }
        ev.resolvedMsb = bank[ev.ch0][0];
        ev.resolvedLsb = bank[ev.ch0][1];
        if (ev.resolvedMsb < 0 || ev.resolvedLsb < 0) {
            if (opts.strictUnresolved) {
                return fail("UNRESOLVED_MIDI_BANK_SELECT: Program Change on channel " +
                            std::to_string(int(ev.ch0) + 1) + " has unset CC0/CC32");
            }
            ++unchangedCount;
            expectedSelects.push_back(closure::Select{int(ev.ch0) + 1, ev.resolvedMsb, ev.resolvedLsb, int(ev.value)});
            continue;
        }
        const MidiTriple src{uint8_t(ev.resolvedMsb), uint8_t(ev.resolvedLsb), ev.value};
        const auto it = effectiveRules.find(src);
        if (it != effectiveRules.end()) {
            ev.matched = true;
            ev.target = it->second;
            ++matchedCount;
            ++tracks[ev.track].matchedCount;
            expectedSelects.push_back(closure::Select{
                int(ev.ch0) + 1,
                int(it->second.msb),
                int(it->second.lsb),
                int(it->second.pc)
            });
        } else {
            ++unchangedCount;
            expectedSelects.push_back(closure::Select{int(ev.ch0) + 1, ev.resolvedMsb, ev.resolvedLsb, int(ev.value)});
        }
    }

    // No-op preservation: return exact original bytes untouched when 0 PCs matched.
    if (matchedCount == 0) {
        res.bytes.assign(data, data + size);
        res.modified = false;
        res.remappedEvents = 0;
        res.unchangedEvents = unchangedCount;
        if (opts.verifyRoundTrip) {
            std::vector<closure::Select> postSelects;
            std::string verifyErr;
            if (!closure::selects(res.bytes.data(), res.bytes.size(), postSelects, verifyErr)) {
                return fail("ROUND_TRIP_MISMATCH: closure::selects() rejected unmodified SMF: " + verifyErr);
            }
            if (!selectsEqual(postSelects, expectedSelects)) {
                return fail("ROUND_TRIP_MISMATCH: unmodified SMF select timeline mismatch");
            }
        }
        if (outExpectedSelects != nullptr) *outExpectedSelects = std::move(expectedSelects);
        res.ok = true;
        return res;
    }

    // Plan per-PC rewrite strategy (PC-only in-place, Bank+PC in-place, or General with bank restore).
    for (size_t tPos = 0; tPos < timeline.size(); ++tPos) {
        const size_t sIdx = timeline[tPos];
        auto& ev = selectEvents[sIdx];
        if (ev.cc != -1 || !ev.matched) continue;

        const bool msbChanged = (int(ev.target.msb) != ev.resolvedMsb);
        const bool lsbChanged = (int(ev.target.lsb) != ev.resolvedLsb);
        if (!msbChanged && !lsbChanged) {
            ev.inPlacePcOnly = true;
            continue;
        }

        // Check independently whether any later PC on channel ch0 relies on CC0 / CC32
        // before the next original CC0 / CC32 on channel ch0 across ALL tracks.
        bool sawCc0 = false;
        bool sawCc32 = false;
        bool laterPcBeforeNextCc0 = false;
        bool laterPcBeforeNextCc32 = false;
        for (size_t j = tPos + 1; j < timeline.size(); ++j) {
            const auto& later = selectEvents[timeline[j]];
            if (later.ch0 != ev.ch0) continue;
            if (later.cc == 0) {
                sawCc0 = true;
            } else if (later.cc == 32) {
                sawCc32 = true;
            } else {
                if (!sawCc0) laterPcBeforeNextCc0 = true;
                if (!sawCc32) laterPcBeforeNextCc32 = true;
            }
            if (sawCc0 && sawCc32) break;
        }

        const bool needRestoreMsb = msbChanged && laterPcBeforeNextCc0;
        const bool needRestoreLsb = lsbChanged && laterPcBeforeNextCc32;

        // Fast-path in-place bank+PC rewrite when dedicated CC0 & CC32 immediately precede
        // this PC on the same track at the exact same tick and no later PC depends on them.
        const auto& trk = tracks[ev.track];
        if (!needRestoreMsb && !needRestoreLsb && sIdx >= trk.selBegin + 2u) {
            auto& prev2 = selectEvents[sIdx - 2u];
            auto& prev1 = selectEvents[sIdx - 1u];
            const uint8_t ccStatus = uint8_t(0xB0u | ev.ch0);
            const bool immediatelyAdjacent =
                (prev2.trackEventSeq + 2u == ev.trackEventSeq &&
                 prev1.trackEventSeq + 1u == ev.trackEventSeq);
            const bool sameTick = (prev2.tick == ev.tick && prev1.tick == ev.tick);
            const bool bothCcOnCh = (prev2.status == ccStatus && prev1.status == ccStatus &&
                                     !prev2.overwrittenInPlace && !prev1.overwrittenInPlace);
            const bool pairCc0Cc32 = (prev2.cc == 0 && prev1.cc == 32);
            const bool pairCc32Cc0 = (prev2.cc == 32 && prev1.cc == 0);
            if (immediatelyAdjacent && sameTick && bothCcOnCh && (pairCc0Cc32 || pairCc32Cc0)) {
                ev.inPlaceWithBank = true;
                prev2.overwrittenInPlace = true;
                prev1.overwrittenInPlace = true;
                if (pairCc0Cc32) {
                    prev2.newCcVal = ev.target.msb;
                    prev1.newCcVal = ev.target.lsb;
                } else {
                    prev2.newCcVal = ev.target.lsb;
                    prev1.newCcVal = ev.target.msb;
                }
                continue;
            }
        }

        ev.restoreMsbAfter = needRestoreMsb;
        ev.restoreLsbAfter = needRestoreLsb;
    }

    // Pass 2: copy untouched byte spans verbatim and rewrite only affected select event spans.
    std::vector<uint8_t> out;
    out.reserve(std::min<size_t>(panative::kMaxFileBytes, size + matchedCount * 18u));
    out.insert(out.end(), data, data + 8 + size_t(hl));

    size_t postSelectEvents = 0;
    for (const auto& trk : tracks) {
        const size_t origTrackLen = trk.bodyEnd - trk.bodyStart;
        if (trk.matchedCount == 0) {
            const size_t trackSelects = trk.selEnd - trk.selBegin;
            if (postSelectEvents + trackSelects > closure::kMaxSelectEvents) {
                return fail("EVENT_LIMIT_EXCEEDED: remapped SMF exceeds " +
                            std::to_string(closure::kMaxSelectEvents) + " bank-select/Program Change events");
            }
            postSelectEvents += trackSelects;
            if (origTrackLen > panative::kMaxFileBytes ||
                out.size() + 8u + origTrackLen > panative::kMaxFileBytes) {
                return fail("SIZE_LIMIT_EXCEEDED: remapped SMF exceeds kMaxFileBytes");
            }
            out.push_back('M');
            out.push_back('T');
            out.push_back('r');
            out.push_back('k');
            appendBe32(out, uint32_t(origTrackLen));
            out.insert(out.end(), data + trk.bodyStart, data + trk.bodyEnd);
            continue;
        }

        std::vector<uint8_t> trackOut;
        trackOut.reserve(std::min<size_t>(panative::kMaxFileBytes, origTrackLen + trk.matchedCount * 18u));

        size_t cursor = trk.bodyStart;
        uint8_t pendingBrokenStatus = 0;
        size_t pendingExpandAt = 0;

        for (size_t sIdx = trk.selBegin; sIdx < trk.selEnd; ++sIdx) {
            const auto& ev = selectEvents[sIdx];
            const bool isPendingEvent = (pendingBrokenStatus != 0u && ev.deltaEnd == pendingExpandAt);

            if (pendingBrokenStatus != 0u && !isPendingEvent) {
                // The immediately following running-status event was a non-select event; expand its status.
                trackOut.insert(trackOut.end(), data + cursor, data + pendingExpandAt);
                trackOut.push_back(pendingBrokenStatus);
                cursor = pendingExpandAt;
                pendingBrokenStatus = 0u;
            }

            if (ev.matched && !ev.inPlacePcOnly && !ev.inPlaceWithBank) {
                const uint8_t ccStatus = uint8_t(0xB0u | ev.ch0);
                const uint8_t pcStatus = uint8_t(0xC0u | ev.ch0);
                const size_t addedSelects =
                    3u + (ev.restoreMsbAfter ? 1u : 0u) + (ev.restoreLsbAfter ? 1u : 0u);
                if (postSelectEvents + addedSelects > closure::kMaxSelectEvents) {
                    return fail("EVENT_LIMIT_EXCEEDED: remapped SMF exceeds " +
                                std::to_string(closure::kMaxSelectEvents) + " bank-select/Program Change events");
                }
                postSelectEvents += addedSelects;
                pendingBrokenStatus = 0u;

                // Copy untouched span + this PC's original delta VLQ.
                trackOut.insert(trackOut.end(), data + cursor, data + ev.deltaEnd);

                // Explicit CC0 = dstMsb (inherits original PC delta time)
                trackOut.push_back(ccStatus);
                trackOut.push_back(0u);
                trackOut.push_back(ev.target.msb);

                // Explicit CC32 = dstLsb (delta 0)
                trackOut.push_back(0u);
                trackOut.push_back(ccStatus);
                trackOut.push_back(32u);
                trackOut.push_back(ev.target.lsb);

                // Explicit PC = dstPc (delta 0)
                trackOut.push_back(0u);
                trackOut.push_back(pcStatus);
                trackOut.push_back(ev.target.pc);

                uint8_t endedStatus = pcStatus;
                if (ev.restoreMsbAfter) {
                    trackOut.push_back(0u);
                    trackOut.push_back(ccStatus);
                    trackOut.push_back(0u);
                    trackOut.push_back(uint8_t(ev.resolvedMsb));
                    endedStatus = ccStatus;
                }
                if (ev.restoreLsbAfter) {
                    trackOut.push_back(0u);
                    trackOut.push_back(ccStatus);
                    trackOut.push_back(32u);
                    trackOut.push_back(uint8_t(ev.resolvedLsb));
                    endedStatus = ccStatus;
                }

                cursor = ev.evEnd;
                if (endedStatus != ev.status && ev.nextReliesOnRunningStatus) {
                    pendingBrokenStatus = ev.nextStatus;
                    pendingExpandAt = ev.nextMsgStart;
                }
                continue;
            }

            if (++postSelectEvents > closure::kMaxSelectEvents) {
                return fail("EVENT_LIMIT_EXCEEDED: remapped SMF exceeds " +
                            std::to_string(closure::kMaxSelectEvents) + " bank-select/Program Change events");
            }

            if (ev.matched) {
                // PC-only in-place or Bank+PC in-place.
                if (isPendingEvent) {
                    trackOut.insert(trackOut.end(), data + cursor, data + ev.deltaEnd);
                    trackOut.push_back(ev.status);
                    trackOut.push_back(ev.target.pc);
                    pendingBrokenStatus = 0u;
                } else {
                    trackOut.insert(trackOut.end(), data + cursor, data + ev.evEnd - 1u);
                    trackOut.push_back(ev.target.pc);
                }
                cursor = ev.evEnd;
            } else if (ev.overwrittenInPlace) {
                // Dedicated CC0/CC32 rewritten in-place (preserves delta, status, and controller number).
                trackOut.insert(trackOut.end(), data + cursor, data + ev.evEnd - 1u);
                trackOut.push_back(ev.newCcVal);
                cursor = ev.evEnd;
            } else if (isPendingEvent) {
                // Untouched select event whose running status was broken by the previous rewrite.
                trackOut.insert(trackOut.end(), data + cursor, data + ev.deltaEnd);
                trackOut.push_back(pendingBrokenStatus);
                trackOut.insert(trackOut.end(), data + ev.deltaEnd, data + ev.evEnd);
                cursor = ev.evEnd;
                pendingBrokenStatus = 0u;
            }
            // Otherwise: completely untouched select event; copied as part of the next untouched span.
        }

        if (pendingBrokenStatus != 0u) {
            trackOut.insert(trackOut.end(), data + cursor, data + pendingExpandAt);
            trackOut.push_back(pendingBrokenStatus);
            cursor = pendingExpandAt;
        }
        trackOut.insert(trackOut.end(), data + cursor, data + trk.bodyEnd);

        if (trackOut.size() > panative::kMaxFileBytes ||
            out.size() + 8u + trackOut.size() > panative::kMaxFileBytes) {
            return fail("SIZE_LIMIT_EXCEEDED: remapped SMF exceeds kMaxFileBytes");
        }
        out.push_back('M');
        out.push_back('T');
        out.push_back('r');
        out.push_back('k');
        appendBe32(out, uint32_t(trackOut.size()));
        out.insert(out.end(), trackOut.begin(), trackOut.end());
    }

    if (opts.verifyRoundTrip) {
        std::vector<closure::Select> postSelects;
        std::string verifyErr;
        if (!closure::selects(out.data(), out.size(), postSelects, verifyErr)) {
            return fail("ROUND_TRIP_MISMATCH: closure::selects() rejected remapped SMF: " + verifyErr);
        }
        if (!selectsEqual(postSelects, expectedSelects)) {
            return fail("ROUND_TRIP_MISMATCH: remapped SMF selects do not match expected post-remap timeline");
        }
    }

    if (outExpectedSelects != nullptr) *outExpectedSelects = std::move(expectedSelects);
    res.ok = true;
    res.modified = true;
    res.remappedEvents = matchedCount;
    res.unchangedEvents = unchangedCount;
    res.bytes = std::move(out);
    return res;
}

inline std::string joinLeafPath(const std::string& prefix, const std::string& name) {
    return prefix.empty() ? name : (prefix + "/" + name);
}

inline bool remapTreeNodes(roottree::Node& node,
                           const std::string& prefix,
                           const std::map<MidiTriple, MidiTriple>& effectiveRules,
                           RemapOptions opts,
                           RootTreeRemapResult& res,
                           std::vector<std::vector<closure::Select>>& expectedLeafSelects) {
    for (auto& child : node.children) {
        const std::string path = joinLeafPath(prefix, child.name);
        if (child.kind == roottree::Node::Leaf) {
            if (!isMidiLeafName(child.name)) continue;
            ++res.inspectedMidiLeaves;
            if (!isLowercaseMidiLeafName(child.name)) {
                res.failedLeafPath = path;
                res.error = "MALFORMED_MIDI_LEAF: MIDI leaf name does not end in lower-case .mid";
                return false;
            }
            std::vector<closure::Select> leafExpected;
            auto smfRes = remapSmfValidated(
                reinterpret_cast<const uint8_t*>(child.payload.data()),
                child.payload.size(),
                effectiveRules,
                opts,
                &leafExpected);
            if (!smfRes.ok) {
                res.failedLeafPath = path;
                res.error = std::move(smfRes.error);
                return false;
            }
            expectedLeafSelects.push_back(std::move(leafExpected));
            if (smfRes.modified) {
                ++res.modifiedMidiLeaves;
                res.remappedEvents += smfRes.remappedEvents;
                child.payload.assign(reinterpret_cast<const char*>(smfRes.bytes.data()), smfRes.bytes.size());
            }
        } else {
            if (!remapTreeNodes(child, path, effectiveRules, opts, res, expectedLeafSelects)) {
                return false;
            }
        }
    }
    return true;
}

inline bool verifyRoundTripNodes(const roottree::Node& orig,
                                 const roottree::Node& reparsed,
                                 const std::string& prefix,
                                 const std::vector<std::vector<closure::Select>>& expectedLeafSelects,
                                 size_t& midiLeafCursor,
                                 std::string& failedLeafPath,
                                 std::string& error) {
    if (orig.kind != reparsed.kind || orig.name != reparsed.name ||
        orig.children.size() != reparsed.children.size()) {
        failedLeafPath = prefix;
        error = "ROUND_TRIP_MISMATCH: Root-tree structure changed after re-serialization";
        return false;
    }
    for (size_t i = 0; i < orig.children.size(); ++i) {
        const auto& oc = orig.children[i];
        const auto& rc = reparsed.children[i];
        const std::string path = joinLeafPath(prefix, oc.name);
        if (oc.kind != rc.kind || oc.name != rc.name) {
            failedLeafPath = path;
            error = "ROUND_TRIP_MISMATCH: Root-tree node kind/name mismatch";
            return false;
        }
        if (oc.kind == roottree::Node::Leaf) {
            if (isMidiLeafName(oc.name)) {
                if (midiLeafCursor >= expectedLeafSelects.size()) {
                    failedLeafPath = path;
                    error = "ROUND_TRIP_MISMATCH: unexpected MIDI leaf count after re-parse";
                    return false;
                }
                std::vector<closure::Select> postSel;
                std::string selErr;
                if (!closure::leafSelects(rc, postSel, selErr)) {
                    failedLeafPath = path;
                    error = "ROUND_TRIP_MISMATCH: closure::leafSelects() failed after re-parse: " + selErr;
                    return false;
                }
                if (!selectsEqual(postSel, expectedLeafSelects[midiLeafCursor])) {
                    failedLeafPath = path;
                    error = "ROUND_TRIP_MISMATCH: MIDI leaf selects differ after Root-tree round-trip";
                    return false;
                }
                ++midiLeafCursor;
            } else {
                if (oc.payload != rc.payload) {
                    failedLeafPath = path;
                    error = "ROUND_TRIP_MISMATCH: non-MIDI leaf bytes mutated during Root-tree round-trip";
                    return false;
                }
            }
        } else {
            if (!verifyRoundTripNodes(oc, rc, path, expectedLeafSelects, midiLeafCursor, failedLeafPath, error)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace detail

inline SmfRemapResult remapSmf(const uint8_t* data, size_t size,
                               const std::map<MidiTriple, MidiTriple>& rules,
                               RemapOptions opts = {}) {
    std::map<MidiTriple, MidiTriple> effectiveRules;
    std::string ruleErr;
    if (!detail::validateRules(rules, effectiveRules, ruleErr)) {
        SmfRemapResult res;
        res.ok = false;
        res.error = std::move(ruleErr);
        return res;
    }
    return detail::remapSmfValidated(data, size, effectiveRules, opts, nullptr);
}

inline RootTreeRemapResult remapRootTree(const uint8_t* data, size_t size,
                                         const std::map<MidiTriple, MidiTriple>& rules,
                                         RemapOptions opts = {}) {
    RootTreeRemapResult res;
    auto fail = [&](std::string e, std::string leafPath = {}) {
        res.ok = false;
        res.modified = false;
        res.modifiedMidiLeaves = 0;
        res.remappedEvents = 0;
        res.bytes.clear();
        res.error = std::move(e);
        if (!leafPath.empty()) res.failedLeafPath = std::move(leafPath);
        return res;
    };

    std::map<MidiTriple, MidiTriple> effectiveRules;
    std::string ruleErr;
    if (!detail::validateRules(rules, effectiveRules, ruleErr)) {
        return fail(std::move(ruleErr));
    }
    if (size > panative::kMaxFileBytes) {
        return fail("SIZE_LIMIT_EXCEEDED: Root tree exceeds kMaxFileBytes");
    }
    if (data == nullptr && size > 0) {
        return fail("MALFORMED_ROOT_TREE: null buffer with non-zero size");
    }

    const auto origTree = roottree::parse(data, size);
    if (!origTree.ok) {
        return fail("MALFORMED_ROOT_TREE: " + origTree.error);
    }

    roottree::Node workingRoot = origTree.root;
    std::vector<std::vector<closure::Select>> expectedLeafSelects;
    if (!detail::remapTreeNodes(workingRoot, "", effectiveRules, opts, res, expectedLeafSelects)) {
        res.ok = false;
        res.modified = false;
        res.modifiedMidiLeaves = 0;
        res.remappedEvents = 0;
        res.bytes.clear();
        return res;
    }

    if (res.modifiedMidiLeaves == 0) {
        res.modified = false;
        res.bytes.assign(data, data + size);
        res.ok = true;
        return res;
    }

    std::string serialized;
    std::string writeErr;
    if (!roottree::write(workingRoot, serialized, writeErr)) {
        const std::string tag = (writeErr.find("larger than the reader's limit") != std::string::npos)
                                    ? "SIZE_LIMIT_EXCEEDED: "
                                    : "MALFORMED_ROOT_TREE: ";
        return fail(tag + writeErr);
    }

    if (opts.verifyRoundTrip) {
        const auto reparsed = roottree::parse(
            reinterpret_cast<const uint8_t*>(serialized.data()), serialized.size());
        if (!reparsed.ok) {
            return fail("ROUND_TRIP_MISMATCH: roottree::parse() rejected re-serialized tree: " + reparsed.error);
        }
        size_t midiLeafCursor = 0;
        std::string failLeaf;
        std::string verifyErr;
        if (!detail::verifyRoundTripNodes(
                origTree.root, reparsed.root, "", expectedLeafSelects, midiLeafCursor, failLeaf, verifyErr)) {
            return fail(std::move(verifyErr), std::move(failLeaf));
        }
        if (midiLeafCursor != expectedLeafSelects.size()) {
            return fail("ROUND_TRIP_MISMATCH: MIDI leaf count mismatch after Root-tree round-trip");
        }
    }

    res.modified = true;
    res.bytes.assign(
        reinterpret_cast<const uint8_t*>(serialized.data()),
        reinterpret_cast<const uint8_t*>(serialized.data()) + serialized.size());
    res.ok = true;
    return res;
}

}  // namespace ai_arranger::media::panative::smf_remap

#endif
