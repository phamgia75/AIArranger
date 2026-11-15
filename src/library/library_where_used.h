// Where-used for one library item (Style, Sound, Pad, Keyboard Set, ...), from the resource graph's reference model
// (media/sound/resource_graph.h: which kinds may refer to which, referrerVias / edgeAllowed) over the open libraries.
// A reference is only ever listed when it was READ from decoded data. No library payload is decoded today (Pa-native
// items are opaque, most start "OC31", E5; KST file content is UNKNOWN, D14), so every possible referrer is reported as
// UNKNOWN with its reason and a count of the open items that could hold it: nothing is inferred from names or banks.
// CPU work over loaded snapshots: no file is touched.
#ifndef AI_ARRANGER_LIBRARY_LIBRARY_WHERE_USED_H
#define AI_ARRANGER_LIBRARY_LIBRARY_WHERE_USED_H

#include "library/library_model.h"
#include "media/sound/resource_graph.h"

namespace ai_arranger::library {

namespace rg = media::graph;

// The library file type that holds a resource-graph kind on its own ("" = only inside another resource's payload).
inline std::string typeOfKind(rg::Kind k) {
    switch (k) {
        case rg::Kind::Sound: return "Sound";
        case rg::Kind::Multisample: return "Multisample";
        case rg::Kind::Sample: return "Sample";
        case rg::Kind::KeyboardSet: return "Keyboard Set";
        case rg::Kind::Style: return "Style";
        case rg::Kind::Pad: return "Pad";
        case rg::Kind::SongBookEntry: return "SongBook";
        case rg::Kind::MidiSong: return "MIDI Song";
        default: return "";
    }
}

inline std::string undecodedWhy(Format f) {
    return f == Format::PaSet ? "Pa-native item payloads are opaque (most start \"OC31\", E5)"
           : f == Format::Pa5xKst ? "KST file content is not decoded (container UNKNOWN, D14)"
                                  : "file content is not decoded";
}

struct WhereUsed {
    std::string verdict;              // what can be said about references to the item
    std::vector<std::string> lines;   // per kind of reference: what was checked and why it stays UNKNOWN
};

// `other` (optional): the other open library, whose items may refer to this one too.
inline WhereUsed whereUsed(const Library& L, const Item& it, const Library* other = nullptr) {
    WhereUsed w{"references UNKNOWN: no reference to it is read from these files", {}};
    std::optional<rg::Kind> kind;
    for (int k = 0; k <= int(rg::Kind::MidiSong); ++k) if (typeOfKind(rg::Kind(k)) == it.type) kind = rg::Kind(k);
    if (!kind) { w.lines.push_back("a " + it.type + " is not a resource in the resource graph: what refers to it is not modelled (UNKNOWN)"); return w; }
    const auto vias = rg::referrerVias(*kind);
    if (vias.empty()) w.lines.push_back(std::string("what refers to a ") + rg::kindName(*kind) + " is not modelled in the resource graph (UNKNOWN)");
    for (const auto v : vias)
        for (int f = 0; f <= int(rg::Kind::MidiSong); ++f) {
            if (!rg::edgeAllowed(rg::Kind(f), v, *kind)) continue;
            const std::string t = typeOfKind(rg::Kind(f));
            std::string s = std::string(rg::viaName(v)) + " from " + rg::kindName(rg::Kind(f)) + ": ";
            if (t.empty()) { w.lines.push_back(s + "held inside other resources' payloads, which are not decoded (UNKNOWN)"); continue; }
            std::vector<std::string> parts;
            for (const Library* x : {&L, other}) {
                if (!x) continue;
                size_t n = 0;
                for (const auto& o : x->items) n += o.type == t;
                if (n) parts.push_back(std::to_string(n) + " " + t + " item(s) in " + x->name + ", whose " + undecodedWhy(x->format));
            }
            if (parts.empty()) s += "no " + t + " item in the open libraries; referrers elsewhere (keyboard memory, other libraries) are not visible";
            for (size_t i = 0; i < parts.size(); ++i) s += (i ? "; " : "") + parts[i];
            w.lines.push_back(s + (parts.empty() ? " (UNKNOWN)" : ": whether any of them refers to it is UNKNOWN"));
        }
    return w;
}

inline std::string whereUsedText(const Library& L, const Item& it, const Library* other = nullptr) {
    const auto w = whereUsed(L, it, other);
    std::string o = "Where used: " + it.type + " \"" + trimmed(it.name) + "\" (" + it.bank + (it.slot >= 0 ? " slot " + std::to_string(it.slot + 1) : "") +
                    ")\n  " + w.verdict + "\n  Known references: none (only references read from documented or observed data are listed)\n";
    for (const auto& l : w.lines) o += "  " + l + "\n";
    return o + "  Not proven unused: deleting or replacing it may break what refers to it.\n";
}

}  // namespace ai_arranger::library

#endif
