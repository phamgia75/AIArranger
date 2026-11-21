#ifndef AI_ARRANGER_MEDIA_SOUND_NATIVE_REFS_H
#define AI_ARRANGER_MEDIA_SOUND_NATIVE_REFS_H

// Read-only adapter: references found in DECODED Pa5X Sound bodies -> the existing Resource Graph (resource_graph.h).
// Research basis: owner research SOUND_STRUCTURE_FINDINGS.md (one owner's Pa5X backup; OFFLINE RESEARCH VALIDATED there only).
//
// Boundaries:
//  * Decoder-independent: the caller supplies already decoded bodies. No OC31 code lives here (decodeOc31 is B2's, #50).
//  * No layout is built in. A RecordLayout comes from fitLayout() over the caller's own data, or from the caller; a kind
//    without a layout, or an item whose size does not fit it, is reported UNANALYSED, never guessed.
//  * What is a fact: a slot holds the 8-byte ID of a KMP object (Confirmed only where the KMP index supplies the chain).
//    What is not: that a record is an oscillator and slot 0/1 is High/Low. Those edges are Confidence::Inferred.
//  * Unreferenced is never concluded. The graph is not declared complete or scanned, so deleteCheck() can never say
//    Unreferenced for a resource built here; this file only lists dependencies, shared use and gaps.
// Native KST reading stays unavailable in the product: nothing here is wired into the GUI.

#include "media/sound/resource_graph.h"
#include <algorithm>
#include <array>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <vector>

namespace ai_arranger::media::graph {

using Id8 = std::array<uint8_t, 8>;
constexpr uint32_t kNoZoneSample = 0xFFFFFFFFu;

struct KmpIndex {                       // filled by whoever parsed the KMP; the adapter does not parse files
    struct SampleRow { Id8 id{}; std::string name; std::optional<SampleInfo> info; };   // no info -> the Sample node is absent (dangling)
    struct Multi { Id8 id{}; std::string name; std::vector<uint32_t> zoneSamples; };    // sample-table index per zone, or kNoZoneSample
    std::vector<SampleRow> samples;
    std::vector<Multi> multisamples;
    std::set<Id8> drumIds;              // drum-sample IDs: known objects, but drum layouts are not modelled here
};

struct DecodedSound {
    std::string bank;                   // e.g. "USER03": scope of the Sound
    uint32_t number = 0;                // position inside the bank
    uint32_t kind = 0;                  // chunk id, e.g. 0x10050038
    std::string name;
    std::vector<uint8_t> body;          // decoded bytes
};

struct RecordLayout { uint32_t base = 0, record = 0, pairOffset = 0; };   // size = base + n * record; slots at +0 and +pairOffset
using LayoutTable = std::map<uint32_t, RecordLayout>;

enum class Gap { NoLayoutForKind, SizeDoesNotFit, NoKnownReference, TooManyRecords, NumberOutOfRange };
struct Unanalysed { ResourceId sound; Gap why; };
struct UnresolvedSlot { ResourceId sound; uint32_t record = 0, slot = 0; bool wrongKind = false; };   // not a multisample ID

struct NativeRefReport {
    size_t sounds = 0, analysed = 0, emptySlots = 0;
    std::vector<Unanalysed> unanalysed;
    std::vector<UnresolvedSlot> unresolved;
    bool coverageComplete() const { return unanalysed.empty() && unresolved.empty(); }   // still no delete proof: see header note
};

namespace native_detail {
inline Id8 id8(const uint8_t* p) { Id8 r; std::copy(p, p + 8, r.begin()); return r; }
inline bool allSame(const Id8& v, uint8_t b) { return std::all_of(v.begin(), v.end(), [b](uint8_t x) { return x == b; }); }
}

// Derive a layout for one Sound kind from the data (the research method): items holding only multisample IDs; base = modal
// offset of the first ID; record = largest r <= min(size-base) dividing every (size-base) (limit: items that all hold >= 2
// records give a multiple of the true size); pairOffset = most common non-zero (offset-base) % r. Needs >= minItems items.
inline std::optional<RecordLayout> fitLayout(const std::vector<const DecodedSound*>& sameKind, const KmpIndex& k, size_t minItems = 8) {
    std::set<Id8> multi, other;
    for (const auto& m : k.multisamples) multi.insert(m.id);
    for (const auto& s : k.samples) other.insert(s.id);
    other.insert(k.drumIds.begin(), k.drumIds.end());
    struct Item { size_t size, first; std::vector<size_t> offs; };
    std::vector<Item> items;
    for (const DecodedSound* s : sameKind) {
        std::vector<size_t> o; bool onlyMulti = true; size_t last = 0; bool any = false;
        for (size_t i = 0; i + 8 <= s->body.size(); ++i) {
            const Id8 v = native_detail::id8(&s->body[i]);
            const bool m = multi.count(v) > 0, x = !m && other.count(v) > 0;
            if ((m || x) && (!any || i >= last + 8)) { any = true; last = i; if (x) onlyMulti = false; else o.push_back(i); }
        }
        if (!any || !onlyMulti || o.empty()) continue;
        items.push_back({s->body.size(), o.front(), std::move(o)});
    }
    if (items.empty() || items.size() < minItems) return std::nullopt;
    std::map<size_t, size_t> cnt; for (const Item& i : items) ++cnt[i.first];
    const size_t base = std::max_element(cnt.begin(), cnt.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
    std::vector<size_t> at; for (const Item& i : items) if (i.first == base && i.size >= base) at.push_back(i.size - base);
    if (at.empty()) return std::nullopt;
    const size_t mn = *std::min_element(at.begin(), at.end());
    size_t rec = 0;
    for (size_t r = 8; r <= mn; ++r) if (std::all_of(at.begin(), at.end(), [r](size_t a) { return a % r == 0; })) rec = r;
    if (!rec || base > 0xFFFFFFFFu) return std::nullopt;
    std::map<size_t, size_t> po;   // only items that define the base vote, so the pair offset belongs to the same layout
    for (const Item& i : items) if (i.first == base && i.size >= base) for (size_t o : i.offs) if ((o - base) % rec) ++po[(o - base) % rec];
    if (po.empty()) return std::nullopt;
    // Fail closed on ambiguity: (1) a tie for the pair offset; (2) no item with exactly one record (then the true record could be a
    // divisor of the fitted size); (3) any ID of a base item outside slot 0 / the pair slot (the fit does not describe the data).
    size_t best = 0, ties = 0, pair = 0;
    for (const auto& [off, c] : po) { if (c > best) { best = c; pair = off; ties = 1; } else if (c == best) ++ties; }
    if (ties != 1 || mn != rec) return std::nullopt;
    for (const Item& i : items) if (i.first == base && i.size >= base) for (size_t o : i.offs) { const size_t m = (o - base) % rec; if (m != 0 && m != pair) return std::nullopt; }
    return RecordLayout{static_cast<uint32_t>(base), static_cast<uint32_t>(rec), static_cast<uint32_t>(pair)};
}

inline ResourceId soundId(const DecodedSound& s) { return {false, s.bank, Kind::Sound, s.number}; }

// Builds a NEW graph with Sound, Oscillator, Multisample and Sample nodes plus edges; all-or-nothing on graph errors.
// It never adds to a caller's graph, so scan/completeness declarations the caller made elsewhere cannot leak onto these
// resources. Combining with other evidence is Graph::merge()'s job and that caller's responsibility.
// Identity of KMP objects is positional inside this KST (scope "KST"), never the 8-byte ID or the name.
inline std::optional<Graph> buildNativeGraph(const KmpIndex& k, const std::vector<DecodedSound>& sounds, const LayoutTable& layouts,
                                             NativeRefReport& rep, std::string* err) {
    Graph t; NativeRefReport r;
    const std::string kScope = "KST";
    constexpr size_t kMaxObjects = 999999999;   // ResourceId numbers are at most 9 digits
    if (k.samples.size() > kMaxObjects || k.multisamples.size() > kMaxObjects) { if (err) *err = "KMP index too large"; return std::nullopt; }
    std::map<Id8, size_t> msIndex, drum, smp;
    for (size_t i = 0; i < k.multisamples.size(); ++i) if (!msIndex.emplace(k.multisamples[i].id, i).second) { if (err) *err = "duplicate multisample ID in KMP index"; return std::nullopt; }
    for (size_t i = 0; i < k.samples.size(); ++i) smp.emplace(k.samples[i].id, i);
    for (const Id8& d : k.drumIds) drum.emplace(d, 0);
    auto sid = [&](size_t i) { return ResourceId{false, kScope, Kind::Sample, static_cast<uint32_t>(i)}; };
    for (size_t i = 0; i < k.samples.size(); ++i)
        if (k.samples[i].info && !t.addNode({sid(i), k.samples[i].name, k.samples[i].info}, err)) return std::nullopt;
    for (size_t i = 0; i < k.multisamples.size(); ++i) {
        const ResourceId m{false, kScope, Kind::Multisample, static_cast<uint32_t>(i)};
        if (!t.addNode({m, k.multisamples[i].name, std::nullopt}, err)) return std::nullopt;
        for (size_t z = 0; z < k.multisamples[i].zoneSamples.size(); ++z) {
            const uint32_t s = k.multisamples[i].zoneSamples[z];
            if (s == kNoZoneSample) continue;
            if (!t.addRef({m, sid(s), Via::Zone, "zone " + std::to_string(z), Source::Package, Confidence::Confirmed, "kmp"}, err)) return std::nullopt;   // s beyond the table: dangling by design
        }
    }
    for (const DecodedSound& s : sounds) {
        ++r.sounds;
        const ResourceId sid_ = soundId(s);
        if (!t.addNode({sid_, s.name, std::nullopt}, err)) return std::nullopt;
        if (s.number > 999999) { r.unanalysed.push_back({sid_, Gap::NumberOutOfRange}); continue; }   // oscillator number = number * 1000 + record must stay below 10^9
        const auto li = layouts.find(s.kind);
        if (li == layouts.end() || li->second.record < 8 || li->second.pairOffset > li->second.record - 8) { r.unanalysed.push_back({sid_, Gap::NoLayoutForKind}); continue; }
        const RecordLayout& L = li->second;
        if (s.body.size() < L.base || (s.body.size() - L.base) % L.record) { r.unanalysed.push_back({sid_, Gap::SizeDoesNotFit}); continue; }
        const size_t n = (s.body.size() - L.base) / L.record;   // size_t throughout: q * record cannot wrap
        if (n > 999) { r.unanalysed.push_back({sid_, Gap::TooManyRecords}); continue; }
        std::vector<std::pair<size_t, std::pair<uint32_t, size_t>>> hits;   // record, slot, multisample index
        std::vector<UnresolvedSlot> bad; size_t empty = 0;
        for (size_t q = 0; q < n; ++q)
            for (uint32_t slot = 0; slot < 2; ++slot) {
                const Id8 v = native_detail::id8(&s.body[L.base + q * L.record + (slot ? L.pairOffset : 0)]);
                if (const auto m = msIndex.find(v); m != msIndex.end()) hits.push_back({q, {slot, m->second}});
                else if (native_detail::allSame(v, 0) || native_detail::allSame(v, 0xFF)) ++empty;
                else bad.push_back({sid_, static_cast<uint32_t>(q), slot, drum.count(v) > 0 || smp.count(v) > 0});
            }
        r.unresolved.insert(r.unresolved.end(), bad.begin(), bad.end());   // kept even when the Sound turns out unanalysed
        if (hits.empty()) { r.unanalysed.push_back({sid_, Gap::NoKnownReference}); continue; }
        ++r.analysed; r.emptySlots += empty;
        std::set<size_t> made;
        for (const auto& [q, sm] : hits) {
            const ResourceId osc{false, s.bank, Kind::Oscillator, s.number * 1000 + static_cast<uint32_t>(q)};
            if (made.insert(q).second) {
                if (!t.addNode({osc, "record " + std::to_string(q), std::nullopt}, err)) return std::nullopt;
                if (!t.addRef({sid_, osc, Via::OscillatorOf, "record " + std::to_string(q) + " (role as oscillator: HYPOTHESIS)", Source::Package, Confidence::Inferred, "native-kst"}, err)) return std::nullopt;
            }
            const ResourceId ms{false, kScope, Kind::Multisample, static_cast<uint32_t>(sm.second)};
            if (!t.addRef({osc, ms, sm.first ? Via::OscLow : Via::OscHigh, "slot " + std::to_string(sm.first) + " (High/Low role: HYPOTHESIS)", Source::Package, Confidence::Inferred, "native-kst"}, err)) return std::nullopt;
        }
    }
    rep = std::move(r);
    return t;
}

// Dependencies of a Sound: everything it reaches, present or dangling, in id order (read-only).
inline std::vector<ResourceId> dependencies(const Graph& g, const ResourceId& sound) {
    auto c = g.closure({sound}); c.erase(sound);
    return {c.begin(), c.end()};
}
// Resources of kind k used by more than one Sound, with the number of distinct Sounds. A count of ANALYSED Sounds only.
inline std::map<ResourceId, size_t> sharedCounts(const Graph& g, Kind k) {
    std::map<ResourceId, size_t> r;
    for (const auto& [id, n] : g.nodes()) {
        if (id.kind != k) continue;
        size_t c = 0;
        for (const ResourceId& u : g.usedByRoots(id)) c += u.kind == Kind::Sound;
        if (c > 1) r[id] = c;
    }
    return r;
}

}  // namespace ai_arranger::media::graph
#endif
