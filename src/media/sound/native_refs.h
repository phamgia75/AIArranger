#ifndef AI_ARRANGER_MEDIA_SOUND_NATIVE_REFS_H
#define AI_ARRANGER_MEDIA_SOUND_NATIVE_REFS_H

// Read-only adapter & integration: Pa-native KORF containers + OC31/OC32 Sound payloads (media/panative/oc31.h) +
// Pa5X raw RAM.KMP (media/panative/pa5x_kmp.h) -> the existing Resource Graph (media/sound/resource_graph.h).
// Research basis: owner research SOUND_STRUCTURE_FINDINGS.md and KMP_MULTISAMPLE_ANALYSIS.md (one owner's Pa5X backup;
// OFFLINE RESEARCH VALIDATED there only).
//
// Boundaries (STRICTLY READ-ONLY):
//  * Read-only: never writes, modifies, or deletes any KST/SET file; no OC31 encoder; no Native Sound KST export.
//  * Both low-level (caller supplies already-decoded Sound bodies + KmpIndex) and end-to-end read-only integration
//    (decodeOc31() + parsePa5xKmp() + PCM inventory -> Sound -> Oscillator -> Multisample -> Zone -> Sample -> PCM,
//    plus DrumSample -> L/R Sample -> PCM) are supported.
//  * No layout is built in. A RecordLayout comes from fitLayout() over the caller's own data, or from the caller; a kind
//    without a layout, or an item whose size does not fit it, is reported UNANALYSED, never guessed.
//  * What is a fact: a slot holds the 8-byte ID of a KMP object (Confirmed only where the KMP index supplies the chain).
//    What is not: that a record is an oscillator and slot 0/1 is High/Low. Those edges are Confidence::Inferred.
//  * Unreferenced is never concluded. The graph is not declared complete or scanned, so deleteCheck() can never say
//    Unreferenced for a resource built here; this file only lists dependencies, shared use and gaps.
//  * Native KST reading stays unavailable in the GUI product: nothing here is wired into apps/macos/.

#include "media/panative/oc31.h"
#include "media/panative/pa5x_kmp.h"
#include "media/panative/panative_reader.h"
#include "media/sound/resource_graph.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace ai_arranger::media::graph {

using Id8 = std::array<uint8_t, 8>;
constexpr uint32_t kNoZoneSample = 0xFFFFFFFFu;
static_assert(panative::kNoIndex == kNoZoneSample, "Pa5xKmp kNoIndex must match kNoZoneSample");

struct KmpIndex {                       // filled from parsePa5xKmp() via kmpIndexFromPa5x(), or directly by caller
    struct SampleRow { Id8 id{}; std::string name; std::optional<SampleInfo> info; };   // no info -> the Sample node is absent (dangling)
    struct Multi { Id8 id{}; std::string name; std::vector<uint32_t> zoneSamples; };    // sample-table index per zone, or kNoZoneSample
    struct Drum { Id8 id{}; std::string name; uint32_t leftSample = kNoZoneSample; uint32_t rightSample = kNoZoneSample; };
    std::vector<SampleRow> samples;
    std::vector<Multi> multisamples;
    std::set<Id8> drumIds;              // drum-sample IDs: known objects, never treated as multisample IDs
    std::vector<Drum> drums;            // optional KMP drum records (L/R sample indices -> DrumSample -> Sample edges)
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

enum class Gap { NoLayoutForKind, SizeDoesNotFit, NoKnownReference, TooManyRecords, NumberOutOfRange, DecodeFailed };
inline const char* gapName(Gap g) {
    switch (g) {
        case Gap::NoLayoutForKind:  return "NoLayoutForKind";
        case Gap::SizeDoesNotFit:   return "SizeDoesNotFit";
        case Gap::NoKnownReference: return "NoKnownReference";
        case Gap::TooManyRecords:   return "TooManyRecords";
        case Gap::NumberOutOfRange: return "NumberOutOfRange";
        case Gap::DecodeFailed:     return "DecodeFailed";
    }
    return "Unknown";
}
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
inline std::string hexId(const Id8& v) {
    static const char* d = "0123456789abcdef";
    std::string o;
    o.reserve(16);
    for (uint8_t b : v) { o += d[b >> 4]; o += d[b & 0x0Fu]; }
    return o;
}
inline std::string upperAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
inline bool endsWithUpper(const std::string& s, const char* upperSuffix) {
    const size_t n = std::strlen(upperSuffix);
    if (s.size() <= n) return false;
    const std::string tail = upperAscii(s.substr(s.size() - n));
    return tail == upperSuffix;
}
inline std::string bankFromLabel(const std::string& label) {
    const size_t slash = label.find_last_of("/\\");
    std::string leaf = slash == std::string::npos ? label : label.substr(slash + 1);
    const size_t dot = leaf.rfind('.');
    if (dot != std::string::npos && dot > 0) leaf.resize(dot);
    return upperAscii(std::move(leaf));
}
inline bool validSampleInfo(const SampleInfo& s) {
    return s.channels >= 1 && s.channels <= 2 && (s.bits == 8 || s.bits == 16) &&
           s.rate > 0 && s.frames > 0 && s.frames <= Graph::kMaxFrames && s.loopFrames <= s.frames;
}
struct ScopedFd {
    int fd = -1;
    explicit ScopedFd(int f = -1) : fd(f) {}
    ~ScopedFd() { if (fd >= 0) ::close(fd); }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
};
inline bool listDirEntriesAt(int dirFd, std::vector<std::string>& names, std::string& err) {
    names.clear();
    const int dupFd = ::dup(dirFd);
    if (dupFd < 0) { err = "cannot duplicate directory descriptor"; return false; }
    DIR* dir = ::fdopendir(dupFd);
    if (!dir) { ::close(dupFd); err = "cannot open directory stream"; return false; }
    for (;;) {
        errno = 0;
        const struct dirent* ent = ::readdir(dir);
        if (!ent) {
            const int saved = errno;
            ::closedir(dir);
            if (saved != 0) { names.clear(); err = "directory read failed"; return false; }
            break;
        }
        const std::string n(ent->d_name);
        if (n == "." || n == "..") continue;
        names.push_back(n);
    }
    std::sort(names.begin(), names.end());
    return true;
}
// Reads one regular file below an open directory descriptor (O_NOFOLLOW + O_NONBLOCK + fstat S_ISREG).
inline bool readRegularAt(int dirFd, const char* name, size_t maxFileBytes, size_t remainingBudget,
                          std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    struct stat lst{};
    if (::fstatat(dirFd, name, &lst, AT_SYMLINK_NOFOLLOW) != 0) { err = "cannot stat entry"; return false; }
    if (!S_ISREG(lst.st_mode)) { err = "not a regular file (symlinks and special files are refused)"; return false; }
    const ScopedFd f(::openat(dirFd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (f.fd < 0) { err = "cannot open regular file (symlinks are not followed)"; return false; }
    struct stat st{};
    if (::fstat(f.fd, &st) != 0 || !S_ISREG(st.st_mode)) { err = "not a regular file"; return false; }
    if (st.st_size < 0) { err = "invalid file size"; return false; }
    const uint64_t fsz = static_cast<uint64_t>(st.st_size);
    if (fsz > maxFileBytes) { err = "file larger than the reader's limit"; return false; }
    if (fsz > remainingBudget) { err = "total read size exceeds the reader's limit"; return false; }
    out.resize(static_cast<size_t>(fsz));
    size_t got = 0;
    while (got < out.size()) {
        const ssize_t r = ::read(f.fd, out.data() + got, out.size() - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            out.clear();
            err = "read failed";
            return false;
        }
        if (r == 0) { out.clear(); err = "file shrank while being read"; return false; }
        got += static_cast<size_t>(r);
    }
    uint8_t extra = 0;
    for (;;) {
        const ssize_t r = ::read(f.fd, &extra, 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            out.clear();
            err = "read failed";
            return false;
        }
        if (r > 0) { out.clear(); err = "file grew while being read"; return false; }
        break;
    }
    return true;
}
}  // namespace native_detail

// Derive a layout for one Sound kind from the data (the research method): items holding only multisample IDs; base = modal
// offset of the first ID; record = largest r <= min(size-base) dividing every (size-base) (limit: items that all hold >= 2
// records give a multiple of the true size); pairOffset = most common non-zero (offset-base) % r. Needs >= minItems items.
inline std::optional<RecordLayout> fitLayout(const std::vector<const DecodedSound*>& sameKind, const KmpIndex& k, size_t minItems = 8) {
    std::set<Id8> multi, other;
    for (const auto& m : k.multisamples) multi.insert(m.id);
    for (const auto& s : k.samples) other.insert(s.id);
    other.insert(k.drumIds.begin(), k.drumIds.end());
    for (const auto& d : k.drums) other.insert(d.id);
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

// Builds a NEW graph with Sound, Oscillator, Multisample, DrumSample and Sample nodes plus edges; all-or-nothing on graph errors.
// It never adds to a caller's graph, so scan/completeness declarations the caller made elsewhere cannot leak onto these
// resources. Combining with other evidence is Graph::merge()'s job and that caller's responsibility.
// Identity of KMP objects is positional inside this KST (scope "KST"), never the 8-byte ID or the name.
inline std::optional<Graph> buildNativeGraph(const KmpIndex& k, const std::vector<DecodedSound>& sounds, const LayoutTable& layouts,
                                             NativeRefReport& rep, std::string* err) {
    Graph t; NativeRefReport r;
    const std::string kScope = "KST";
    constexpr size_t kMaxObjects = 999999999;   // ResourceId numbers are at most 9 digits
    if (k.samples.size() > kMaxObjects || k.multisamples.size() > kMaxObjects || k.drums.size() > kMaxObjects) {
        if (err) *err = "KMP index too large";
        return std::nullopt;
    }
    std::map<Id8, size_t> msIndex, drum, smp;
    for (size_t i = 0; i < k.multisamples.size(); ++i) if (!msIndex.emplace(k.multisamples[i].id, i).second) { if (err) *err = "duplicate multisample ID in KMP index"; return std::nullopt; }
    for (size_t i = 0; i < k.samples.size(); ++i) smp.emplace(k.samples[i].id, i);
    for (size_t i = 0; i < k.drums.size(); ++i) if (!drum.emplace(k.drums[i].id, i).second) { if (err) *err = "duplicate drum ID in KMP index"; return std::nullopt; }
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
    for (size_t i = 0; i < k.drums.size(); ++i) {
        const ResourceId dm{false, kScope, Kind::DrumSample, static_cast<uint32_t>(i)};
        if (!t.addNode({dm, k.drums[i].name, std::nullopt}, err)) return std::nullopt;
        if (k.drums[i].leftSample != kNoZoneSample &&
            !t.addRef({dm, sid(k.drums[i].leftSample), Via::DrumSampleOf, "L", Source::Package, Confidence::Confirmed, "kmp"}, err))
            return std::nullopt;
        if (k.drums[i].rightSample != kNoZoneSample &&
            !t.addRef({dm, sid(k.drums[i].rightSample), Via::DrumSampleOf, "R", Source::Package, Confidence::Confirmed, "kmp"}, err))
            return std::nullopt;
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

// ── Read-only Native-KST Integration (OC31/OC32 + Pa5X RAM.KMP + PCM -> Resource Graph) ──────────────────────────────

inline bool isOc31Magic(const uint8_t* p, size_t n) {
    return p != nullptr && n >= 4 && (std::memcmp(p, "OC31", 4) == 0 || std::memcmp(p, "OC32", 4) == 0);
}

enum class SoundPayloadMode { AutoDetectOc31, RequireOc31 };

struct PcmEntry {
    Id8 id{};
    std::string name;
    std::string file;                   // relative path or container label (e.g. "PCM/ram01.pcm")
    uint32_t payloadBytes = 0;          // item payload byte count in the KORF .pcm container
    std::optional<SampleInfo> info;     // optional explicit PCM facts; derived conservatively when absent
};

struct PcmInventory {
    std::map<Id8, PcmEntry> byId;
    size_t containersParsed = 0;
    size_t samplesTotal = 0;
    size_t samplesWithoutId = 0;        // e.g. Pa700 PCM items lacking directory field 4 (8-byte sample ID)
    std::vector<std::string> anomalies;
};

// Parses one KORF .pcm container in memory and stages its sample IDs into `inv` (all-or-nothing per container).
inline bool addPcmContainerBytes(const uint8_t* data, size_t size, const std::string& label,
                                 PcmInventory& inv, std::string* err = nullptr) {
    if (err) err->clear();
    if (!data || size == 0) { if (err) *err = label + ": empty PCM container"; return false; }
    if (size > panative::kMaxFileBytes) { if (err) *err = label + ": PCM container larger than the reader's limit"; return false; }
    const panative::Container c = panative::parse(data, size);
    if (!c.ok) { if (err) *err = label + ": invalid PCM KORF container: " + c.error; return false; }

    std::map<Id8, PcmEntry> staged;
    std::vector<std::string> stagedAnomalies;
    size_t withoutId = 0;
    for (const panative::Item& it : c.items) {
        if (it.sampleId.empty()) {
            ++withoutId;
            stagedAnomalies.push_back(label + ": sample '" + panative::printable(it.name) + "' lacks 8-byte PCM sample ID");
            continue;
        }
        if (it.sampleId.size() != 8) {
            if (err) *err = label + ": invalid PCM sample ID length";
            return false;
        }
        const Id8 id = native_detail::id8(reinterpret_cast<const uint8_t*>(it.sampleId.data()));
        if (inv.byId.count(id) || staged.count(id)) {
            if (err) *err = label + ": duplicate PCM sample ID " + native_detail::hexId(id);
            return false;
        }
        staged.emplace(id, PcmEntry{id, it.name, label, it.size, std::nullopt});
    }
    inv.byId.insert(staged.begin(), staged.end());
    inv.anomalies.insert(inv.anomalies.end(), stagedAnomalies.begin(), stagedAnomalies.end());
    ++inv.containersParsed;
    inv.samplesTotal += c.items.size();
    inv.samplesWithoutId += withoutId;
    return true;
}

struct MissingPcmSample {
    uint32_t sampleIndex = 0;
    Id8 id{};
    std::string name;
    std::string reason;
};

struct KmpBuildDiagnostics {
    bool zonesContiguous = false;
    size_t samplesTotal = 0;
    size_t samplesWithPcm = 0;
    size_t drumsTotal = 0;
    size_t zonesTotal = 0;
    size_t multisamplesTotal = 0;
    std::vector<MissingPcmSample> missingPcm;
};

// Extracts and parses the Pa5X raw RAM.KMP item (chunk 0x11080018, version 0x0103) from a KORF container. Fail-closed.
inline bool parseKmpContainerBytes(const uint8_t* data, size_t size, panative::Pa5xKmp& out, std::string* err = nullptr) {
    out = panative::Pa5xKmp{};
    if (err) err->clear();
    auto fail = [&](const std::string& m) { out = panative::Pa5xKmp{}; if (err) *err = m; return false; };
    if (!data || size == 0) return fail("empty KMP container");
    if (size > panative::kMaxFileBytes) return fail("KMP container larger than the reader's limit");
    const panative::Container c = panative::parse(data, size);
    if (!c.ok) return fail("invalid KMP KORF container: " + c.error);
    if (c.items.empty()) return fail("KMP container holds no items");

    const panative::Item* kmpItem = nullptr;
    for (const panative::Item& it : c.items) {
        if (static_cast<size_t>(it.offset) + 8 > size || static_cast<size_t>(it.size) > size - static_cast<size_t>(it.offset) - 8)
            return fail("KMP item payload runs past the container");
        const uint8_t* body = data + static_cast<size_t>(it.offset) + 8;
        if (isOc31Magic(body, it.size))
            return fail("KMP item is OC31/OC32-encoded (only raw Pa5X RAM.KMP 0x11080018 version 0x0103 is supported)");
        if (it.chunkId == panative::kPa5xKmpChunkId) {
            if (kmpItem != nullptr) return fail("KMP container holds multiple RAM.KMP items (0x11080018)");
            kmpItem = &it;
        }
    }
    if (!kmpItem) return fail("KMP container holds no Pa5X RAM.KMP item (chunk 0x11080018)");
    return panative::parsePa5xKmp(data + static_cast<size_t>(kmpItem->offset) + 8, kmpItem->size, out, err);
}

// Converts a parsed Pa5xKmp + PcmInventory into a KmpIndex (all-or-nothing, fail-closed on structural errors).
inline bool kmpIndexFromPa5x(const panative::Pa5xKmp& kmp, const PcmInventory& pcm, KmpIndex& out,
                             KmpBuildDiagnostics* diag = nullptr, std::string* err = nullptr) {
    out = KmpIndex{};
    if (diag) *diag = KmpBuildDiagnostics{};
    if (err) err->clear();
    auto fail = [&](const std::string& m) {
        out = KmpIndex{};
        if (diag) *diag = KmpBuildDiagnostics{};
        if (err) *err = m;
        return false;
    };
    constexpr size_t kMaxObjects = 999999999;
    if (kmp.samples.size() > kMaxObjects || kmp.multisamples.size() > kMaxObjects ||
        kmp.drums.size() > kMaxObjects || kmp.zones.size() > kMaxObjects)
        return fail("Pa5X KMP index too large");

    std::set<Id8> sampleIds, multiIds, drumIds;
    for (size_t i = 0; i < kmp.samples.size(); ++i)
        if (!sampleIds.insert(kmp.samples[i].id).second) return fail("duplicate sample ID in Pa5X KMP");
    for (size_t i = 0; i < kmp.multisamples.size(); ++i)
        if (!multiIds.insert(kmp.multisamples[i].id).second) return fail("duplicate multisample ID in Pa5X KMP");
    for (size_t i = 0; i < kmp.drums.size(); ++i)
        if (!drumIds.insert(kmp.drums[i].id).second) return fail("duplicate drum ID in Pa5X KMP");

    for (size_t i = 0; i < kmp.zones.size(); ++i)
        if (kmp.zones[i].sample != panative::kNoIndex && kmp.zones[i].sample >= kmp.samples.size())
            return fail("zone " + std::to_string(i) + ": sample index out of range");
    for (size_t i = 0; i < kmp.multisamples.size(); ++i) {
        const auto& m = kmp.multisamples[i];
        if (m.firstZone > kmp.zones.size() || m.zoneCount > kmp.zones.size() - m.firstZone)
            return fail("multisample " + std::to_string(i) + ": zones run past the zone table");
    }
    for (size_t i = 0; i < kmp.drums.size(); ++i) {
        const auto& d = kmp.drums[i];
        if (d.left >= kmp.samples.size()) return fail("drum record " + std::to_string(i) + ": left sample index out of range");
        if (d.right != panative::kNoIndex && d.right >= kmp.samples.size())
            return fail("drum record " + std::to_string(i) + ": right sample index out of range");
    }

    KmpIndex tmp;
    KmpBuildDiagnostics d;
    d.zonesContiguous = kmp.zonesContiguous;
    d.samplesTotal = kmp.samples.size();
    d.drumsTotal = kmp.drums.size();
    d.zonesTotal = kmp.zones.size();
    d.multisamplesTotal = kmp.multisamples.size();

    tmp.samples.reserve(kmp.samples.size());
    for (size_t i = 0; i < kmp.samples.size(); ++i) {
        const auto& s = kmp.samples[i];
        std::optional<SampleInfo> info;
        const auto it = pcm.byId.find(s.id);
        if (it == pcm.byId.end()) {
            d.missingPcm.push_back({static_cast<uint32_t>(i), s.id, s.name, "no matching PCM entry"});
        } else {
            const PcmEntry& pe = it->second;
            if (pe.info.has_value()) {
                SampleInfo si = *pe.info;
                if (si.file.empty() && !pe.file.empty()) si.file = pe.file;
                if (!native_detail::validSampleInfo(si))
                    return fail("sample " + std::to_string(i) + " (" + panative::printable(s.name) + "): invalid PCM SampleInfo");
                info = std::move(si);
                ++d.samplesWithPcm;
            } else if (s.rate == 0) {
                d.missingPcm.push_back({static_cast<uint32_t>(i), s.id, s.name, "KMP sample rate is 0"});
            } else if (pe.payloadBytes == 0) {
                d.missingPcm.push_back({static_cast<uint32_t>(i), s.id, s.name, "PCM item payload is empty"});
            } else {
                uint64_t frames = static_cast<uint64_t>(pe.payloadBytes) / 2u;
                if (frames == 0) frames = 1;
                if (frames > Graph::kMaxFrames) frames = Graph::kMaxFrames;
                info = SampleInfo{1, s.rate, 16, frames, 0, "", pe.file};
                ++d.samplesWithPcm;
            }
        }
        tmp.samples.push_back({s.id, s.name, std::move(info)});
    }

    tmp.multisamples.reserve(kmp.multisamples.size());
    for (size_t i = 0; i < kmp.multisamples.size(); ++i)
        tmp.multisamples.push_back({kmp.multisamples[i].id, kmp.multisamples[i].name, kmp.zoneSamples(i)});

    tmp.drums.reserve(kmp.drums.size());
    for (const auto& dr : kmp.drums) {
        tmp.drumIds.insert(dr.id);
        tmp.drums.push_back({dr.id, dr.name, dr.left, dr.right});
    }

    out = std::move(tmp);
    if (diag) *diag = std::move(d);
    return true;
}

struct SoundDecodeFailure {
    std::string source;
    std::string bank;
    uint32_t number = 0;
    uint32_t kind = 0;
    std::string name;
    std::string reason;
};

struct SoundExtractResult {
    bool containerOk = false;
    std::string containerError;
    size_t itemsTotal = 0;
    size_t oc31Decoded = 0;
    size_t rawLoaded = 0;
    std::vector<DecodedSound> sounds;
    std::vector<SoundDecodeFailure> failures;
};

// Decodes one Sound item payload (OC31/OC32 via panative::decodeOc31(), or raw bytes in AutoDetectOc31 mode). Fail-closed.
inline bool decodeSoundPayload(const uint8_t* payload, size_t size, std::vector<uint8_t>& bodyOut, bool& wasOc31,
                               SoundPayloadMode mode = SoundPayloadMode::AutoDetectOc31, std::string* err = nullptr) {
    bodyOut.clear();
    wasOc31 = false;
    if (err) err->clear();
    if (!payload && size > 0) { if (err) *err = "null sound payload pointer"; return false; }
    if (size > panative::kMaxFileBytes) { if (err) *err = "sound payload larger than the reader's limit"; return false; }
    if (isOc31Magic(payload, size)) {
        wasOc31 = true;
        return panative::decodeOc31(payload, size, bodyOut, err);
    }
    if (mode == SoundPayloadMode::RequireOc31) {
        if (err) *err = "not an OC31/OC32 payload";
        return false;
    }
    if (size > 0) bodyOut.assign(payload, payload + size);
    return true;
}

// Parses one KORF SOUND/*.pcg container in memory and decodes each item payload into DecodedSound (read-only, fail-closed).
inline SoundExtractResult extractSoundsFromPcgBytes(const uint8_t* data, size_t size, const std::string& bankScope,
                                                    const std::string& sourceLabel,
                                                    SoundPayloadMode mode = SoundPayloadMode::AutoDetectOc31) {
    SoundExtractResult r;
    if (!ResourceId::validScope(bankScope)) { r.containerError = sourceLabel + ": invalid bank scope '" + bankScope + "'"; return r; }
    if (!data || size == 0) { r.containerError = sourceLabel + ": empty Sound container"; return r; }
    if (size > panative::kMaxFileBytes) { r.containerError = sourceLabel + ": Sound container larger than the reader's limit"; return r; }
    const panative::Container c = panative::parse(data, size);
    if (!c.ok) { r.containerError = sourceLabel + ": invalid Sound KORF container: " + c.error; return r; }

    r.containerOk = true;
    r.itemsTotal = c.items.size();
    std::set<uint32_t> seenSlots;
    for (const panative::Item& it : c.items) {
        if (static_cast<size_t>(it.offset) + 8 > size || static_cast<size_t>(it.size) > size - static_cast<size_t>(it.offset) - 8) {
            r.containerOk = false;
            r.containerError = sourceLabel + ": Sound item payload runs past the container";
            r.sounds.clear();
            return r;
        }
        if (!seenSlots.insert(it.slot).second) {
            r.failures.push_back({sourceLabel, bankScope, it.slot, it.chunkId, it.name, "duplicate Sound slot in bank " + bankScope});
            continue;
        }
        std::vector<uint8_t> body;
        bool wasOc31 = false;
        std::string derr;
        if (!decodeSoundPayload(data + static_cast<size_t>(it.offset) + 8, it.size, body, wasOc31, mode, &derr)) {
            r.failures.push_back({sourceLabel, bankScope, it.slot, it.chunkId, it.name, derr});
            continue;
        }
        ++(wasOc31 ? r.oc31Decoded : r.rawLoaded);
        r.sounds.push_back({bankScope, it.slot, it.chunkId, it.name, std::move(body)});
    }
    return r;
}

struct RawNamedBlob {
    std::string label;
    std::string bank;                   // optional bank scope for Sound containers; derived from label stem if empty
    std::vector<uint8_t> bytes;
};

struct NativeKstInput {
    std::optional<RawNamedBlob> kmpContainer;
    std::vector<RawNamedBlob> pcmContainers;
    std::optional<PcmInventory> extraPcm;
    std::vector<RawNamedBlob> soundContainers;
};

struct NativeKstOptions {
    LayoutTable layouts;                // caller-supplied RecordLayouts by Sound chunk kind
    bool allowFitLayout = true;         // fit layouts for remaining Sound kinds via fitLayout()
    size_t fitMinItems = 8;
    SoundPayloadMode soundMode = SoundPayloadMode::AutoDetectOc31;
    size_t maxTotalBytes = 256u << 20;  // 256 MiB total read cap for folder reads
};

struct NativeKstReport {
    bool kmpPresent = false;
    bool kmpParsed = false;
    std::string kmpError;
    KmpBuildDiagnostics kmp;
    size_t pcmContainers = 0;
    size_t pcmSamples = 0;
    size_t pcmSamplesWithoutId = 0;
    size_t soundContainers = 0;
    size_t soundsDecodedOc31 = 0;
    size_t soundsLoadedRaw = 0;
    LayoutTable usedLayouts;
    NativeRefReport refs;
    std::vector<SoundDecodeFailure> decodeFailures;
    std::vector<std::string> containerErrors;
    std::vector<std::string> skippedPaths;
    std::vector<std::string> diagnostics;

    bool ok() const { return kmpPresent && kmpParsed && containerErrors.empty() && decodeFailures.empty(); }
    bool coverageComplete() const {
        return ok() && skippedPaths.empty() && pcmSamplesWithoutId == 0 && kmp.missingPcm.empty() && refs.coverageComplete();
    }
};

struct NativeKstResult {
    bool ok = false;                    // true when a fresh Graph was built (still check report.ok()/coverageComplete() for anomalies)
    std::optional<Graph> graph;         // never declared complete/scanned: deleteCheck() never says Unreferenced
    KmpIndex kmpIndex;
    NativeKstReport report;
};

// End-to-end read-only Native-KST Resource Graph resolution in memory:
//   SOUND/*.pcg (decodeOc31) + MULTISMP/ram.kmp (parsePa5xKmp) + PCM/*.pcm -> Sound -> Oscillator -> Multisample -> Zone -> Sample -> PCM
//   and DrumSample -> L/R Sample -> PCM. Strictly fail-closed on missing/invalid RAM.KMP and corrupt inputs.
inline NativeKstResult resolveNativeKst(const NativeKstInput& in, const NativeKstOptions& opts = {}) {
    NativeKstResult res;

    // 1. PCM inventory
    PcmInventory pcm;
    if (in.extraPcm) pcm = *in.extraPcm;
    for (const RawNamedBlob& blob : in.pcmContainers) {
        std::string perr;
        if (!addPcmContainerBytes(blob.bytes.data(), blob.bytes.size(), blob.label, pcm, &perr)) {
            res.report.containerErrors.push_back(perr);
            res.report.diagnostics.push_back("pcm error: " + perr);
        }
    }
    res.report.pcmContainers = pcm.containersParsed;
    res.report.pcmSamples = pcm.byId.size() + pcm.samplesWithoutId;
    res.report.pcmSamplesWithoutId = pcm.samplesWithoutId;
    for (const std::string& a : pcm.anomalies) res.report.diagnostics.push_back("pcm: " + a);

    // 2. Sound containers (KORF parse + OC31/OC32 decode)
    std::vector<DecodedSound> sounds;
    std::set<std::pair<std::string, uint32_t>> seenSoundKeys;
    for (const RawNamedBlob& blob : in.soundContainers) {
        const std::string bank = blob.bank.empty() ? native_detail::bankFromLabel(blob.label) : blob.bank;
        SoundExtractResult ex = extractSoundsFromPcgBytes(blob.bytes.data(), blob.bytes.size(), bank, blob.label, opts.soundMode);
        if (!ex.containerOk) {
            res.report.containerErrors.push_back(ex.containerError);
            res.report.diagnostics.push_back("sound container error: " + ex.containerError);
            continue;
        }
        ++res.report.soundContainers;
        res.report.soundsDecodedOc31 += ex.oc31Decoded;
        res.report.soundsLoadedRaw += ex.rawLoaded;
        for (const SoundDecodeFailure& f : ex.failures) {
            res.report.decodeFailures.push_back(f);
            res.report.diagnostics.push_back("sound decode failed: " + f.bank + "/" + std::to_string(f.number) +
                                             " (" + panative::printable(f.name) + ") in " + f.source + ": " + f.reason);
        }
        for (DecodedSound& s : ex.sounds) {
            if (!seenSoundKeys.emplace(s.bank, s.number).second) {
                res.report.containerErrors.push_back(blob.label + ": duplicate Sound identity " + s.bank + "/" + std::to_string(s.number));
                res.report.decodeFailures.push_back({blob.label, s.bank, s.number, s.kind, s.name, "duplicate Sound identity across containers"});
                res.report.diagnostics.push_back("duplicate sound: " + s.bank + "/" + std::to_string(s.number) + " in " + blob.label);
                continue;
            }
            sounds.push_back(std::move(s));
        }
    }

    // 3. Pa5X RAM.KMP -> KmpIndex
    if (!in.kmpContainer.has_value()) {
        res.report.kmpPresent = false;
        res.report.kmpParsed = false;
        res.report.kmpError = "RAM.KMP is absent";
        res.report.diagnostics.push_back("unresolved: RAM.KMP is absent");
        return res;
    }
    res.report.kmpPresent = true;
    panative::Pa5xKmp parsedKmp;
    std::string kerr;
    if (!parseKmpContainerBytes(in.kmpContainer->bytes.data(), in.kmpContainer->bytes.size(), parsedKmp, &kerr)) {
        res.report.kmpParsed = false;
        res.report.kmpError = kerr;
        res.report.diagnostics.push_back("kmp error: " + kerr);
        return res;
    }
    if (!kmpIndexFromPa5x(parsedKmp, pcm, res.kmpIndex, &res.report.kmp, &kerr)) {
        res.report.kmpParsed = false;
        res.report.kmpError = kerr;
        res.report.diagnostics.push_back("kmp index error: " + kerr);
        return res;
    }
    res.report.kmpParsed = true;
    for (const MissingPcmSample& mp : res.report.kmp.missingPcm) {
        res.report.diagnostics.push_back("missing pcm: KMP sample " + std::to_string(mp.sampleIndex) +
                                         " (" + panative::printable(mp.name) + ", id " + native_detail::hexId(mp.id) + "): " + mp.reason);
    }

    // 4. Layout resolution (caller-supplied + optional data-fitted layouts)
    res.report.usedLayouts = opts.layouts;
    if (opts.allowFitLayout) {
        std::map<uint32_t, std::vector<const DecodedSound*>> byKind;
        for (const DecodedSound& s : sounds) byKind[s.kind].push_back(&s);
        for (const auto& [kind, items] : byKind) {
            if (res.report.usedLayouts.count(kind)) continue;
            if (auto fitted = fitLayout(items, res.kmpIndex, opts.fitMinItems))
                res.report.usedLayouts.emplace(kind, *fitted);
        }
    }

    // 5. Build fresh Resource Graph
    std::string gerr;
    auto g = buildNativeGraph(res.kmpIndex, sounds, res.report.usedLayouts, res.report.refs, &gerr);
    if (!g) {
        res.report.containerErrors.push_back("graph build failed: " + gerr);
        res.report.diagnostics.push_back("graph build failed: " + gerr);
        return res;
    }

    // 6. Record decode-failed Sounds as unanalysed nodes (Gap::DecodeFailed)
    for (const SoundDecodeFailure& f : res.report.decodeFailures) {
        if (!ResourceId::validScope(f.bank)) continue;
        const ResourceId sid{false, f.bank, Kind::Sound, f.number};
        if (g->find(sid)) continue;
        std::string addErr;
        if (g->addNode({sid, f.name, std::nullopt}, &addErr)) {
            ++res.report.refs.sounds;
            res.report.refs.unanalysed.push_back({sid, Gap::DecodeFailed});
        }
    }

    // 7. Consolidate diagnostics
    for (const Unanalysed& u : res.report.refs.unanalysed) {
        if (u.why == Gap::DecodeFailed) continue;
        res.report.diagnostics.push_back("unanalysed: " + u.sound.str() + " (" + gapName(u.why) + ")");
    }
    for (const UnresolvedSlot& u : res.report.refs.unresolved) {
        res.report.diagnostics.push_back("unresolved slot: " + u.sound.str() + " record " + std::to_string(u.record) +
                                         " slot " + std::to_string(u.slot) + (u.wrongKind ? " (wrong object kind)" : " (unknown ID)"));
    }
    for (const std::string& iss : g->issues()) res.report.diagnostics.push_back("graph issue: " + iss);

    res.graph = std::move(*g);
    res.ok = true;
    return res;
}

// Read-only disk folder entry point: opens the KST folder and its MULTISMP/, PCM/, SOUND/ subdirectories with O_NOFOLLOW,
// refuses symlinks/FIFOs/special files, bounds individual and total read sizes, then runs resolveNativeKst().
inline NativeKstResult resolveNativeKstFolder(const std::filesystem::path& kstDir, const NativeKstOptions& opts = {}) {
    NativeKstResult earlyFail;
    const native_detail::ScopedFd rootFd(::open(kstDir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (rootFd.fd < 0) {
        const std::string msg = "cannot open KST folder (must be a real directory, symlinks are not followed): " + kstDir.string();
        earlyFail.report.containerErrors.push_back(msg);
        earlyFail.report.diagnostics.push_back(msg);
        return earlyFail;
    }

    std::vector<std::string> topEntries;
    std::string lerr;
    if (!native_detail::listDirEntriesAt(rootFd.fd, topEntries, lerr)) {
        const std::string msg = "cannot list KST folder " + kstDir.string() + ": " + lerr;
        earlyFail.report.containerErrors.push_back(msg);
        earlyFail.report.diagnostics.push_back(msg);
        return earlyFail;
    }

    NativeKstInput in;
    std::vector<RawNamedBlob> kmpBlobs;
    std::set<std::string> seenSoundBanks;
    std::vector<std::string> folderErrors, skipped, folderDiags;
    size_t remainingBudget = opts.maxTotalBytes;

    for (const std::string& top : topEntries) {
        struct stat st{};
        if (::fstatat(rootFd.fd, top.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            folderErrors.push_back("cannot stat entry " + top);
            continue;
        }
        const std::string topUpper = native_detail::upperAscii(top);
        const bool wantDir = topUpper == "MULTISMP" || topUpper == "PCM" || topUpper == "SOUND";
        if (!S_ISDIR(st.st_mode)) {
            if (S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode)) {
                skipped.push_back(top);
                folderDiags.push_back("skipped non-regular/symlink entry: " + top);
                if (wantDir) folderErrors.push_back("refusing non-directory/symlink at " + top);
            }
            continue;
        }
        if (!wantDir) continue;

        const native_detail::ScopedFd subFd(::openat(rootFd.fd, top.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (subFd.fd < 0) {
            folderErrors.push_back("cannot open folder " + top + " (symlinks are not followed)");
            continue;
        }
        std::vector<std::string> leaves;
        std::string serr;
        if (!native_detail::listDirEntriesAt(subFd.fd, leaves, serr)) {
            folderErrors.push_back("cannot list folder " + top + ": " + serr);
            continue;
        }
        for (const std::string& leaf : leaves) {
            const std::string rel = top + "/" + leaf;
            struct stat lst{};
            if (::fstatat(subFd.fd, leaf.c_str(), &lst, AT_SYMLINK_NOFOLLOW) != 0) {
                folderErrors.push_back("cannot stat " + rel);
                continue;
            }
            if (!S_ISREG(lst.st_mode)) {
                if (S_ISLNK(lst.st_mode) || !S_ISDIR(lst.st_mode)) {
                    skipped.push_back(rel);
                    folderErrors.push_back("refusing non-regular file or symlink: " + rel);
                    folderDiags.push_back("skipped non-regular/symlink entry: " + rel);
                }
                continue;
            }
            const bool isKmp = topUpper == "MULTISMP" && native_detail::endsWithUpper(leaf, ".KMP");
            const bool isPcm = topUpper == "PCM" && native_detail::endsWithUpper(leaf, ".PCM");
            const bool isPcg = topUpper == "SOUND" && native_detail::endsWithUpper(leaf, ".PCG");
            if (!isKmp && !isPcm && !isPcg) continue;

            std::vector<uint8_t> bytes;
            std::string rerr;
            if (!native_detail::readRegularAt(subFd.fd, leaf.c_str(), panative::kMaxFileBytes, remainingBudget, bytes, rerr)) {
                folderErrors.push_back(rel + ": " + rerr);
                folderDiags.push_back("read error: " + rel + ": " + rerr);
                continue;
            }
            remainingBudget -= bytes.size();
            if (isKmp) {
                kmpBlobs.push_back({rel, "", std::move(bytes)});
            } else if (isPcm) {
                in.pcmContainers.push_back({rel, "", std::move(bytes)});
            } else {
                const std::string bank = native_detail::bankFromLabel(rel);
                if (!seenSoundBanks.insert(bank).second) {
                    folderErrors.push_back("duplicate Sound bank '" + bank + "' (" + rel + "); refusing to guess");
                    folderDiags.push_back("duplicate Sound bank: " + bank + " (" + rel + ")");
                    continue;
                }
                in.soundContainers.push_back({rel, bank, std::move(bytes)});
            }
        }
    }

    if (kmpBlobs.size() == 1) {
        in.kmpContainer = std::move(kmpBlobs[0]);
    } else if (kmpBlobs.size() > 1) {
        folderErrors.push_back("multiple .kmp files in MULTISMP/ (" + std::to_string(kmpBlobs.size()) + "); refusing to guess");
        folderDiags.push_back("ambiguous KMP folder: multiple .kmp files in MULTISMP/");
    }

    NativeKstResult res = resolveNativeKst(in, opts);
    res.report.skippedPaths.insert(res.report.skippedPaths.begin(), skipped.begin(), skipped.end());
    res.report.containerErrors.insert(res.report.containerErrors.begin(), folderErrors.begin(), folderErrors.end());
    res.report.diagnostics.insert(res.report.diagnostics.begin(), folderDiags.begin(), folderDiags.end());
    return res;
}

}  // namespace ai_arranger::media::graph
#endif
