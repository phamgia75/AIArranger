// aiarranger-style: Style Studio editing core from the command line (UASF styles).
//
//   aiarranger-style inspect <style.uasf>
//   aiarranger-style edit <in.uasf> --out <out.uasf> --op <op> [op options]
//   aiarranger-style export-smf <in.uasf> --out <out.mid> [--ppqn N]
//   aiarranger-style cv-copy <in.mid> --out <out.mid> --from v1cv1 --to v1cv2
//
// Ops (section/track/index are 0-based; ticks at the section's resolution):
//   quantize    --section S [--track T] --grid 1/16[t] [--strength 0-100] [--from A --to B]
//   transpose   [--section S] [--track T] --semitones N        (drum/percussion tracks skipped)
//   velocity    [--section S] [--track T] (--set N | --offset N | --scale PCT)
//   tempo       --bpm N
//   meter       --section S --num N --den D
//   insert-note --section S --track T --tick N --pitch P --vel V --length L
//   delete      --section S --track T --index I[,I...]
//   move        --section S --track T --index I[,I...] --delta D
//   copy-paste  --section S --track T --from A --to B --dest-section S2 --dest-track T2 --at N
//   import-smf  --section S --track T --smf FILE [--smf-track N] [--channel 1-16]
//
// --out must not exist (nothing is overwritten); it is written via a temp file.
// Exit: 0 = done, 1 = usage or input error, 2 = edit / export rejected (no file written).

#include "engine/uasf/deserializer.h"
#include "engine/uasf/serializer.h"
#include "exporters/korg/genos_to_korg.h"
#include "style_edit/style_editor.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>

using namespace ai_arranger;
using namespace ai_arranger::style_edit;
namespace fs = std::filesystem;

namespace {

int usage() {
    std::cerr << "usage: aiarranger-style inspect <style.uasf>\n"
                 "       aiarranger-style edit <in.uasf> --out <out.uasf> --op <op> [options]\n"
                 "       aiarranger-style export-smf <in.uasf> --out <out.mid> [--ppqn N]\n"
                 "       aiarranger-style cv-copy <in.mid> --out <out.mid> --from <marker> --to <marker>\n"
                 "ops: quantize transpose velocity tempo meter insert-note delete move copy-paste import-smf\n";
    return 1;
}

struct Args {
    std::map<std::string, std::string> kv;
    bool bad = false;
    bool has(const std::string& k) const { return kv.count(k) != 0; }
    long num(const std::string& k, long def = 0) {
        const auto it = kv.find(k);
        if (it == kv.end()) return def;
        try {
            size_t used = 0;
            const long v = std::stol(it->second, &used);
            if (used == it->second.size()) return v;
        } catch (...) {}
        std::cerr << "--" << k << ": not a number: " << it->second << "\n";
        bad = true;
        return def;
    }
    std::vector<size_t> list(const std::string& k) {
        std::vector<size_t> out;
        std::stringstream ss(kv.count(k) ? kv[k] : "");
        for (std::string part; std::getline(ss, part, ',');) {
            try { out.push_back(std::stoul(part)); } catch (...) { bad = true; }
        }
        if (out.empty()) bad = true;
        return out;
    }
};

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::cerr << "cannot open " << path << "\n"; return false; }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool loadStyle(const std::string& path, uasf::StyleDefinition& out) {
    uasf::UasfDeserializer d;
    auto r = d.deserializeFromFile(path);
    if (!r.success) { std::cerr << path << ": " << r.error << "\n"; return false; }
    out = std::move(r.style);
    return true;
}

// Never overwrites: refuses an existing path, writes a sibling temp file, renames.
bool writeNew(const std::string& path, const std::vector<uint8_t>& bytes) {
    if (fs::exists(path)) { std::cerr << path << " exists; refusing to overwrite\n"; return false; }
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!f) { std::cerr << "cannot write " << tmp << "\n"; fs::remove(tmp); return false; }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) { std::cerr << "cannot rename to " << path << ": " << ec.message() << "\n"; fs::remove(tmp); return false; }
    return true;
}

const char* roleName(uasf::TrackRole r) {
    static const char* names[] = {"Accompaniment", "Chord", "Bass", "Drum", "Percussion",
                                  "Phrase1", "Phrase2", "Pad", "Melody", "Articulation"};
    const auto i = static_cast<size_t>(r);
    return i < std::size(names) ? names[i] : "?";
}

int inspect(const std::string& path) {
    uasf::StyleDefinition s;
    if (!loadStyle(path, s)) return 1;
    std::cout << "Style: " << s.name << "\nTempo: " << s.tempo_bpm << " BPM\nResolution: " << s.resolution
              << " ticks/quarter\nSections: " << s.sections.size() << "\n";
    for (size_t i = 0; i < s.sections.size(); ++i) {
        const auto& sec = s.sections[i];
        const char* marker = exporters::korg::korgMarkerFor(sec.type);
        std::cout << "[" << i << "] " << sec.name << " (Korg " << (marker ? marker : "-") << ") " << sec.bars
                  << " bars " << sec.beats_per_bar << "/" << sec.beat_note << " res " << sec.resolution << " len "
                  << sectionLengthTicks(sec) << " ticks\n";
        for (size_t t = 0; t < sec.tracks.size(); ++t) {
            const auto& tr = sec.tracks[t];
            size_t notes = 0;
            for (const auto& e : tr.events) notes += e.type == uasf::MidiEventType::NoteOn && e.data2 > 0;
            std::cout << "    [" << t << "] ch " << int(tr.midi_channel) + 1 << " " << roleName(tr.role)
                      << (tr.is_drum ? " drum" : "") << " '" << tr.name << "' " << tr.events.size() << " events, "
                      << notes << " notes\n";
        }
    }
    uasf::UasfValidator v;
    const auto res = v.validate(s);
    for (const auto& i : res.issues)
        std::cout << (i.severity == uasf::ValidationIssue::ERROR ? "ERROR" : i.severity == uasf::ValidationIssue::WARNING ? "WARNING" : "INFO")
                  << ": " << i.message << (i.location.empty() ? "" : " (" + i.location + ")") << "\n";
    std::cout << "Validator: " << (res.valid ? "PASS" : "FAIL") << "\n";
    return 0;
}

Selection selection(Args& a) {
    Selection s;
    if (a.has("section")) s.section = static_cast<size_t>(a.num("section"));
    if (a.has("track")) s.track = static_cast<size_t>(a.num("track"));
    if (a.has("from")) s.from = static_cast<uint64_t>(a.num("from"));
    if (a.has("to")) s.to = static_cast<uint64_t>(a.num("to"));
    return s;
}

EditResult runOp(StyleEditor& ed, const std::string& op, Args& a) {
    auto bad = [](std::string m) { return EditResult{false, std::move(m)}; };
    const size_t sec = static_cast<size_t>(a.num("section")), trk = static_cast<size_t>(a.num("track"));
    if (a.bad) return bad("bad option value");
    if (op == "quantize") {
        if (!a.has("section")) return bad("quantize needs --section");
        if (sec >= ed.style().sections.size()) return bad("no section " + std::to_string(sec));
        const uint32_t g = gridTicks(ed.style().sections[sec].resolution, a.kv["grid"]);
        if (!g) return bad("--grid must be 1/4, 1/8, 1/16 or 1/32 (t = triplet) at a whole tick count");
        return ed.quantize(selection(a), g, static_cast<uint32_t>(a.num("strength", 100)));
    }
    if (op == "transpose") return ed.transpose(selection(a), static_cast<int>(a.num("semitones")));
    if (op == "velocity") {
        if (a.has("set")) return ed.velocity(selection(a), VelocityMode::Set, static_cast<int>(a.num("set")));
        if (a.has("offset")) return ed.velocity(selection(a), VelocityMode::Offset, static_cast<int>(a.num("offset")));
        if (a.has("scale")) return ed.velocity(selection(a), VelocityMode::ScalePercent, static_cast<int>(a.num("scale")));
        return bad("velocity needs --set, --offset or --scale");
    }
    if (op == "tempo") return ed.setTempo(static_cast<uint32_t>(a.num("bpm")));
    if (op == "meter") return ed.setMeter(sec, static_cast<uint32_t>(a.num("num")), static_cast<uint32_t>(a.num("den")));
    if (op == "insert-note") {
        const long p = a.num("pitch"), v = a.num("vel");
        if (p < 0 || p > 127 || v < 1 || v > 127) return bad("--pitch 0-127 and --vel 1-127 required");
        uasf::MidiEvent e{uasf::MidiEventType::NoteOn, 0, static_cast<uint8_t>(p), static_cast<uint8_t>(v),
                          static_cast<uint64_t>(a.num("tick"))};
        return ed.insertEvent(sec, trk, e, static_cast<uint64_t>(a.num("length")));
    }
    if (op == "delete") { auto idx = a.list("index"); return a.bad ? bad("--index I[,I...]") : ed.deleteEvents(sec, trk, idx); }
    if (op == "move") { auto idx = a.list("index"); return a.bad ? bad("--index I[,I...]") : ed.moveEvents(sec, trk, idx, a.num("delta")); }
    if (op == "copy-paste") {
        std::string err;
        const auto clip = ed.copy(sec, trk, static_cast<uint64_t>(a.num("from")), static_cast<uint64_t>(a.num("to")), &err);
        if (!clip) return bad(err);
        return ed.paste(static_cast<size_t>(a.num("dest-section")), static_cast<size_t>(a.num("dest-track")),
                        static_cast<uint64_t>(a.num("at")), *clip);
    }
    if (op == "import-smf") {
        std::vector<uint8_t> bytes;
        if (!readFile(a.kv["smf"], bytes)) return bad("cannot read --smf");
        const auto smf = readSmf(bytes);
        if (!smf.ok) return bad("SMF: " + smf.error);
        const size_t st = static_cast<size_t>(a.num("smf-track"));
        if (st >= smf.tracks.size()) return bad("SMF has " + std::to_string(smf.tracks.size()) + " tracks");
        std::optional<uint8_t> ch;
        if (a.has("channel")) {
            const long c = a.num("channel");
            if (c < 1 || c > 16) return bad("--channel 1-16");
            ch = static_cast<uint8_t>(c - 1);
        }
        if (sec >= ed.style().sections.size()) return bad("no section " + std::to_string(sec));
        const auto& s = ed.style().sections[sec];
        size_t dropped = 0;
        auto ev = smfTrackToUasf(smf, st, ch, s.resolution, sectionLengthTicks(s), &dropped);
        if (dropped) std::cout << dropped << " event(s) at or past the element end dropped\n";
        return ed.replaceTrackEvents(sec, trk, std::move(ev), "Import SMF track");
    }
    return bad("unknown op '" + op + "'");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string cmd = argv[1], in = argv[2];
    Args a;
    for (int i = 3; i < argc; ++i) {
        std::string k = argv[i];
        if (k.rfind("--", 0) != 0 || i + 1 >= argc) return usage();
        a.kv[k.substr(2)] = argv[++i];
    }
    if (cmd == "inspect") return a.kv.empty() ? inspect(in) : usage();
    const std::string out = a.has("out") ? a.kv["out"] : "";
    if (out.empty()) return usage();

    if (cmd == "cv-copy") {
        std::vector<uint8_t> bytes;
        if (!readFile(in, bytes)) return 1;
        auto p = exporters::korg::parse(bytes);
        if (!p.ok) { std::cerr << in << ": " << p.error << "\n"; return 1; }
        KorgStyleEditor ed(std::move(p.style));
        const auto r = ed.copyChordVariation(a.kv["from"], a.kv["to"]);
        if (!r.ok) { std::cerr << "rejected: " << r.error << "\n"; return 2; }
        const auto w = exporters::korg::write(ed.style());
        if (!w.ok) { std::cerr << "write failed: " << w.error << "\n"; return 2; }
        if (!writeNew(out, w.bytes)) return 1;
        std::cout << "Copied " << a.kv["from"] << " to " << a.kv["to"] << "; wrote " << out << "\n";
        return 0;
    }

    uasf::StyleDefinition style;
    if (!loadStyle(in, style)) return 1;
    if (cmd == "export-smf") {
        const long ppqn = a.num("ppqn", 0);
        if (a.bad || ppqn < 0 || ppqn > 0x7FFF) return usage();
        const auto x = exportPa5xSmf(style, static_cast<uint16_t>(ppqn));
        std::cout << x.report;
        if (!x.ok) { std::cerr << x.error << "\n"; return 2; }
        if (!writeNew(out, x.bytes)) return 1;
        std::cout << "Validator: PASS (" << x.korg.elements.size() << " elements)\nWrote " << out << " ("
                  << x.bytes.size() << " bytes)\n";
        return 0;
    }
    if (cmd != "edit" || !a.has("op")) return usage();

    StyleEditor ed(std::move(style));
    const auto r = runOp(ed, a.kv["op"], a);
    for (const auto& i : r.issues)
        if (i.severity != uasf::ValidationIssue::INFO)
            std::cout << "validator: " << i.message << (i.location.empty() ? "" : " (" + i.location + ")") << "\n";
    if (!r.ok) { std::cerr << "rejected: " << r.error << "\n"; return 2; }
    uasf::UasfSerializer ser;
    const auto bytes = ser.serialize(ed.style());
    if (!bytes.success) { std::cerr << "serialize failed: " << bytes.error << "\n"; return 2; }
    if (!writeNew(out, bytes.data)) return 1;
    std::cout << a.kv["op"] << ": " << r.changed << " changed" << (r.skipped_tracks ? ", " + std::to_string(r.skipped_tracks) + " drum track(s) skipped" : "")
              << "; wrote " << out << "\n";
    return 0;
}
