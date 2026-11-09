#ifndef AI_ARRANGER_MEDIA_SOUND_EVIDENCE_IMPORT_H
#define AI_ARRANGER_MEDIA_SOUND_EVIDENCE_IMPORT_H

// Optional importer for owner-authorized resource evidence (docs/research/PA5X_RESOURCE_EVIDENCE_IMPORT.md).
// Input is AIArranger's own neutral text format ("aiarranger-evidence 1"), transcribed from an
// authorized source such as a PA Manager report. It is NOT a PA Manager file format and never reads
// KST binary data. Each file is one evidence source: every reference keeps its provenance and its
// confidence, and an inferred reference is never treated as a confirmed one (resource_graph.h).
//
//   aiarranger-evidence <TAB> 1
//   source   <TAB> label <TAB> authorization <TAB> pa-manager-report|owner-transcription
//   node     <TAB> pa5x:BANK/Kind/n <TAB> name
//   ref      <TAB> from <TAB> Via <TAB> to <TAB> confirmed|inferred <TAB> detail
//   scanned  <TAB> Via <TAB> what was covered
//   end      <TAB> record count (all lines before this one)

#include "media/sound/package_graph.h"

namespace ai_arranger::media::sound {

// Strict and all-or-nothing: on any error `g` is unchanged.
inline bool importEvidence(const std::string& textIn, graph::Graph& g, std::string* label, std::string* err) {
    using namespace graph;
    using namespace pg;
    auto fail = [&](size_t line, const std::string& m) { if (err) *err = "evidence line " + std::to_string(line) + ": " + m; return false; };
    if (textIn.empty() || textIn.back() != '\n' || textIn.size() > (16u << 20)) return fail(0, "empty, truncated (no final newline) or over 16 MiB");
    std::vector<std::string> lines;
    std::stringstream ss(textIn);
    for (std::string l; std::getline(ss, l);) lines.push_back(l);
    if (lines.size() < 3 || lines[0] != "aiarranger-evidence\t1") return fail(1, "not an aiarranger-evidence version 1 file");
    const auto sf = split(lines[1]);
    if (sf.size() != 4 || sf[0] != "source" || !text(sf[1]) || !text(sf[2]) ||
        (sf[3] != "pa-manager-report" && sf[3] != "owner-transcription"))
        return fail(2, "first record must be: source, label, owner authorization, pa-manager-report|owner-transcription");
    const std::string src = sf[1];
    const auto ef = split(lines.back());
    uint64_t count = 0;
    if (ef.size() != 2 || ef[0] != "end" || !num(ef[1], 1u << 24, count) || count != lines.size() - 1)
        return fail(lines.size(), "missing or wrong end record (truncated?)");

    Graph t = g;
    std::vector<std::pair<size_t, Edge>> refs;
    auto reportId = [](const std::string& s, ResourceId& r) { return ResourceId::parse(s, r) && !r.provisional; };
    for (size_t i = 2; i + 1 < lines.size(); ++i) {
        const auto f = split(lines[i]);
        std::string e;
        if (f[0] == "node" && f.size() == 3) {
            ResourceId id;
            if (!reportId(f[1], id)) return fail(i + 1, "evidence describes Pa5X resources only (pa5x:BANK/Kind/n): " + f[1]);
            if (id.kind == Kind::Sample) return fail(i + 1, "sample PCM facts cannot come from a report; reference the sample instead");
            if (!text(f[2])) return fail(i + 1, "bad name");
            if (const Node* n = t.find(id)) {
                if (n->name != f[2]) return fail(i + 1, id.str() + " is named '" + n->name + "' by another source and '" + f[2] + "' here");
            } else if (!t.addNode({id, f[2], {}}, &e)) {
                return fail(i + 1, e);
            }
        } else if (f[0] == "ref" && f.size() == 6) {
            Edge r;
            if (!reportId(f[1], r.from) || !reportId(f[3], r.to) || !parseVia(f[2], r.via) || !text(f[5]) ||
                (f[4] != "confirmed" && f[4] != "inferred"))
                return fail(i + 1, "bad ref record (Pa5X ids, a Via name, confirmed|inferred, detail)");
            r.detail = f[5];
            r.source = Source::PaManagerReport;
            r.confidence = f[4] == "inferred" ? Confidence::Inferred : Confidence::Confirmed;
            r.provenance = src;
            refs.emplace_back(i + 1, r);
        } else if (f[0] == "scanned" && f.size() == 3) {
            Via v;
            if (!parseVia(f[1], v) || !text(f[2])) return fail(i + 1, "bad scanned record");
            // Q19 (open): whether PA Manager's search covers these is unknown, so a report cannot close them.
            if (sf[3] == "pa-manager-report" && (v == Via::SongBookSound || v == Via::ProgramChange))
                return fail(i + 1, std::string(viaName(v)) + " coverage of a PA Manager report is unverified (Q19): it stays unresolved");
            if (v == Via::OscillatorOf || v == Via::OscHigh || v == Via::OscLow || v == Via::Zone || v == Via::DrumKeyLayer || v == Via::DrumSampleOf)
                return fail(i + 1, std::string(viaName(v)) + ": only Pa5X-level referrers (Styles, Keyboard Sets, Pads, SongBook, Program Change) can be declared scanned");
            t.declareScanned(v, src + ": " + f[2]);
        } else {
            return fail(i + 1, "unknown or malformed record");
        }
    }
    for (const auto& [line, r] : refs) {
        std::string e;
        if (!t.addRef(r, &e)) return fail(line, e);
    }
    g = std::move(t);
    if (label) *label = src;
    return true;
}

} // namespace ai_arranger::media::sound

#endif // AI_ARRANGER_MEDIA_SOUND_EVIDENCE_IMPORT_H
