// W3-W5: Pa5X object builders (schema-checked against observed structure names), dependency closure, KST assembler.
// Every fixture is generated here; no owner bytes. The optional owner closure check runs only when AIARR_KST_OWNER_COPY
// points to a local copy of an owner KST folder; it reads the copy only and prints counts only.

#include "media/panative/kst_assemble.h"
#include "media/panative/kst_builders.h"
#include "media/panative/kst_closure.h"
#include "media/panative/kst_demo.h"
#include "../support/kst_observed_names.h"
#include "../support/panative_fixture.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace ai_arranger::media::panative;
using roottree::Node;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

// --- schema check: every node / XML element / attribute we write must be an observed structure name ---------------
static std::string role(const std::string& n) {
    static const std::set<std::string> fix = {"Main.ssn", "RTTracks.scn", "SongStyle1.scn", "Lower.plc", "Upper1.plc", "Upper2.plc", "Upper3.plc",
                                              "Main.mid", "MFX.mid", "ClipDescription.xml", "SceneDescription.xml", "SessionDescription.xml", "StageDescription.xml"};
    static const std::set<std::string> el = {"Intro1.scn", "Intro2.scn", "Intro3.scn", "CountIn.scn", "Var1.scn", "Var2.scn", "Var3.scn", "Var4.scn",
                                             "Fill1.scn", "Fill2.scn", "Fill3.scn", "Fill4.scn", "Break.scn", "End1.scn", "End2.scn", "End3.scn"};
    if (fix.count(n)) return n;
    if (el.count(n)) return "<E>.scn";
    const auto d = n.rfind('.');
    return "*" + (d == std::string::npos ? std::string() : n.substr(d));
}
// Element paths and attribute names of one XML document we wrote (simple well-formed output only).
static bool xmlNames(const std::string& x, std::vector<std::string>& out) {
    std::vector<std::string> stack;
    for (size_t i = x.find('<'); i != std::string::npos; i = x.find('<', i + 1)) {
        if (x.compare(i, 2, "<?") == 0) continue;
        const size_t e = x.find('>', i);
        if (e == std::string::npos) return false;
        if (x[i + 1] == '/') { if (stack.empty()) return false; stack.pop_back(); continue; }
        std::string body = x.substr(i + 1, e - i - 1);
        const bool self = !body.empty() && body.back() == '/';
        if (self) body.pop_back();
        const size_t sp = body.find(' ');
        const std::string tag = body.substr(0, sp);
        std::string path;
        for (const auto& s : stack) path += s + "/";
        path += tag;
        out.push_back(path);
        for (size_t q = sp; q != std::string::npos && q < body.size();) {
            const size_t eq = body.find('=', q);
            if (eq == std::string::npos) break;
            size_t a = body.find_first_not_of(' ', q);
            out.push_back(path + "@" + body.substr(a, eq - a));
            const size_t v1 = body.find('"', eq), v2 = body.find('"', v1 + 1);
            if (v1 == std::string::npos || v2 == std::string::npos) return false;
            q = v2 + 1;
        }
        if (!self) stack.push_back(tag);
    }
    return stack.empty();
}
static void schema(const std::string& kind, const Node& n, const std::string& path, std::vector<std::string>& bad) {
    for (const auto& c : n.children) {
        const std::string rp = path + "/" + role(c.name);
        const std::string key = kind + " " + (c.kind == Node::Leaf ? "L" : "B") + rp;
        if (!kst_observed::nodes().count(key)) bad.push_back("node " + key);
        if (c.kind != Node::Leaf) { schema(kind, c, rp, bad); continue; }
        if (c.name.size() > 4 && c.name.compare(c.name.size() - 4, 4, ".xml") == 0) {
            std::vector<std::string> names;
            if (!xmlNames(c.payload, names)) { bad.push_back("malformed xml " + rp); continue; }
            for (const auto& nm : names) {
                const bool attr = nm.find('@') != std::string::npos;
                if (!(attr ? kst_observed::attributes() : kst_observed::elements()).count(kind + " " + rp + " " + nm)) bad.push_back(kind + " " + rp + " " + nm);
            }
        }
    }
}
static std::vector<std::string> schemaBad(const std::string& kind, const Node& root) { std::vector<std::string> b; schema(kind, root, "", b); return b; }

static const Node* find(const Node& n, const std::string& path) {
    const Node* cur = &n;
    size_t s = 0;
    while (cur && s <= path.size()) {
        const size_t e = path.find('/', s);
        const std::string part = path.substr(s, e == std::string::npos ? std::string::npos : e - s);
        const Node* next = nullptr;
        for (const auto& c : cur->children) if (c.name == part) next = &c;
        cur = next;
        if (e == std::string::npos) break;
        s = e + 1;
    }
    return cur;
}
static std::vector<closure::Select> sel(const Node* leaf) {
    std::vector<closure::Select> s;
    std::string e;
    if (!leaf || !closure::selects(reinterpret_cast<const uint8_t*>(leaf->payload.data()), leaf->payload.size(), s, e)) s.push_back({-1, -1, -1, -1});
    return s;
}
static bool roundTrip(const Node& t) {
    std::string b, e, b2;
    if (!roottree::write(t, b, e)) return false;
    const auto p = roottree::parse(reinterpret_cast<const uint8_t*>(b.data()), b.size());
    return p.ok && roottree::write(p.root, b2, e) && b2 == b;
}
static void put(const fs::path& p, const std::string& bytes) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << bytes; }
static std::string smfWith(const std::string& trk) {
    std::string s = "MThd";
    be32(s, 6); be16(s, 0); be16(s, 1); be16(s, 384);
    s += "MTrk"; be32(s, uint32_t(trk.size()));
    return s + trk;
}
static std::string smfTracks(uint16_t format, const std::vector<std::string>& trks) {
    std::string s = "MThd";
    be32(s, 6); be16(s, format); be16(s, uint16_t(trks.size())); be16(s, 384);
    for (const auto& t : trks) { s += "MTrk"; be32(s, uint32_t(t.size())); s += t; }
    return s;
}
static Node pdcRef(uint8_t msb, uint8_t lsb, uint8_t pc) { return build::padClipTree({msb, lsb, pc}, {{0, 96, 60, 100}}); }

int main() {
    std::printf("Test: Pa5X native KST writer W3-W5 (builders, closure, assembler)\n");

    // ---- W3 builders --------------------------------------------------------------------------------------------
    const Node pad = build::padClipTree({121, 0, 5}, {{0, 96, 60, 100}, {384, 96, 64, 90}});
    TEST("Pad: Root -> Main.mid, ClipDescription.xml", pad.children.size() == 2 && pad.children[0].name == "Main.mid" && pad.children[1].name == "ClipDescription.xml");
    const auto& pm = pad.children[0].payload;
    TEST("Pad Main.mid: SMF format 1, 2 tracks, 384 PPQ (observed PadClip v0 r4 form)",
         pm.compare(0, 4, "MThd") == 0 && pm[9] == 1 && pm[11] == 2 && uint8_t(pm[12]) == 0x01 && uint8_t(pm[13]) == 0x80);
    TEST("Pad Main.mid: sound select CC0/CC32/PC on channel 1", sel(&pad.children[0]).size() == 1 && sel(&pad.children[0])[0].channel == 1 &&
         sel(&pad.children[0])[0].msb == 121 && sel(&pad.children[0])[0].lsb == 0 && sel(&pad.children[0])[0].program == 5);
    TEST("Pad Main.mid: no SysEx written (Korg parameter blocks are UNKNOWN, not fabricated)", pm.find('\xF0') == std::string::npos);
    TEST("Pad ClipDescription: PadClip v0 r4, RESOURCE name=Main.mid, observed declaration",
         pad.children[1].payload.find("<CLIP type=\"PadClip\" version=\"0\" release=\"4\">") != std::string::npos &&
         pad.children[1].payload.find("<RESOURCE name=\"Main.mid\" />") != std::string::npos &&
         pad.children[1].payload.rfind("<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\"?>", 0) == 0);
    TEST("Pad: schema check passes (every node/element/attribute observed)", schemaBad("pad", pad).empty());
    TEST("Pad: round-trips through the Root-tree writer and parser", roundTrip(pad));

    const std::array<build::SoundSelect, 4> parts = {build::SoundSelect{121, 0, 48}, build::SoundSelect{121, 0, 0}, build::SoundSelect{121, 0, 52}, build::SoundSelect{121, 0, 89}};
    const Node ksc = build::keyboardSetTree(parts);
    TEST("KBD Set: session form Main.ssn/RTTracks.scn/{Lower,Upper1-3}.plc + MFX.mid + ClipDescription.xml",
         ksc.children.size() == 3 && find(ksc, "Main.ssn/RTTracks.scn/Upper3.plc/Main.mid") && find(ksc, "Main.ssn/SessionDescription.xml") && find(ksc, "MFX.mid"));
    TEST("KBD Set: each part selects its own sound; plc Main.mid is SMF format 0",
         sel(find(ksc, "Main.ssn/RTTracks.scn/Lower.plc/Main.mid"))[0].program == 48 && sel(find(ksc, "Main.ssn/RTTracks.scn/Upper2.plc/Main.mid"))[0].program == 52 &&
         find(ksc, "Main.ssn/RTTracks.scn/Upper3.plc/Main.mid")->payload[9] == 0);
    TEST("KBD Set: scene maps Lower..Upper3 to tracks 1..4 (observed constants)",
         find(ksc, "Main.ssn/RTTracks.scn/SceneDescription.xml")->payload.find("name=\"Upper3.plc\" track=\"4\" type=\"PlainClip\"") != std::string::npos);
    TEST("KBD Set: KeyboardSetClip v0 r4 with RESOURCE Main.ssn + complement MFX.mid",
         ksc.children[2].payload.find("type=\"KeyboardSetClip\" version=\"0\" release=\"4\"") != std::string::npos &&
         ksc.children[2].payload.find("name=\"Main.ssn\" complement=\"MFX.mid\"") != std::string::npos);
    TEST("KBD Set: schema check passes", schemaBad("ksc", ksc).empty());
    TEST("KBD Set: round-trips", roundTrip(ksc));

    const auto kits = demo::kits();
    const Node& sty = kits[2].objects[0].tree;
    TEST("Style: STAGE v1 r2 with Main.ssn -> SongStyle1.scn, PadSet1.scn, KbdSet1-4.scn",
         find(sty, "StageDescription.xml") && find(sty, "Main.ssn/SongStyle1.scn/AIA Test Beat.stc/Main.ssn/End3.scn/Drum.pdc/Main.mid") &&
         find(sty, "Main.ssn/PadSet1.scn/AIA Pad 4.pdc/Main.mid") && find(sty, "Main.ssn/KbdSet4.scn/AIA Set 4.ksc/Main.ssn/RTTracks.scn/Lower.plc/Main.mid") &&
         find(sty, "StageDescription.xml")->payload.find("<STAGE type=\"1\" version=\"1\" release=\"2\">") != std::string::npos);
    {
        const auto& sd = find(sty, "Main.ssn/SongStyle1.scn/AIA Test Beat.stc/Main.ssn/SessionDescription.xml")->payload;
        TEST("Style: 15 elements with the observed role/scenenum pairs (Var1 0/4, CountIn 11/3, End3 12/15)",
             find(sty, "Main.ssn/SongStyle1.scn/AIA Test Beat.stc/Main.ssn")->children.size() == 16 &&
             sd.find("name=\"Var1.scn\" role=\"0\" scenenum=\"4\"") != std::string::npos && sd.find("name=\"CountIn.scn\" role=\"11\" scenenum=\"3\"") != std::string::npos &&
             sd.find("name=\"End3.scn\" role=\"12\" scenenum=\"15\"") != std::string::npos);
    }
    TEST("Style: drum track selects the GM2 drum kit 120/0/0",
         sel(find(sty, "Main.ssn/SongStyle1.scn/AIA Test Beat.stc/Main.ssn/Var1.scn/Drum.pdc/Main.mid"))[0].msb == 120);
    TEST("Style: schema check passes", schemaBad("stg", sty).empty());
    TEST("Style: round-trips", roundTrip(sty));
    {
        Node inv = pad;
        auto& x = inv.children[1].payload;
        x.replace(x.find("<CLIP "), 6, "<CLIP tempo=\"120\" ");
        Node invNode = pad;
        invNode.children.push_back(build::detail::leaf("Extra.mid", ""));
        TEST("schema check catches an invented attribute and an invented node",
             schemaBad("pad", inv).size() == 1 && schemaBad("pad", invNode).size() == 1);
    }
    TEST("UNKNOWN list names the omitted SysEx blocks", !build::unknownOmitted().empty() && build::unknownOmitted()[0].find("SysEx") != std::string::npos);

    // ---- W4 closure -----------------------------------------------------------------------------------------------
    {
        std::vector<closure::Select> s;
        std::string e;
        // running status CC0 then CC32, PC on ch 3; PC on ch 2 without bank; CC0 on ch1 then two PCs
        const std::string t = std::string("\x00\xB2\x00\x79\x00\x20\x40\x00\xC2\x05\x00\xC1\x07\x00\xB0\x00\x78\x00\xC0\x01\x00\xC0\x02\x00\xFF\x2F\x00", 27);
        TEST("selects: running status, per-channel bank, missing bank = -1",
             closure::selects(reinterpret_cast<const uint8_t*>(smfWith(t).data()), smfWith(t).size(), s, e) && s.size() == 4 &&
             s[0].channel == 3 && s[0].msb == 121 && s[0].lsb == 64 && s[0].program == 5 && s[1].channel == 2 && s[1].msb == -1 &&
             s[2].msb == 120 && s[2].lsb == -1 && s[3].program == 2);
        const std::string sx = std::string("\x00\xF0\x05\x42\x7F\x78\x00\xF7\x00\xFF\x03\x02hi\x00\xC0\x03\x00\xFF\x2F\x00", 21);
        s.clear();
        TEST("selects: SysEx and meta events are skipped by length",
             closure::selects(reinterpret_cast<const uint8_t*>(smfWith(sx).data()), smfWith(sx).size(), s, e) && s.size() == 1 && s[0].program == 3);
        std::string tr = smfWith(t); tr.resize(tr.size() - 5);
        s.clear();
        TEST("selects: truncated track refused", !closure::selects(reinterpret_cast<const uint8_t*>(tr.data()), tr.size(), s, e));
        // Codex r1 C2: structure an SMF must have (all 231 087 owner MIDI leaves have it; counts from a private run).
        auto ok = [&](const std::string& m) { std::vector<closure::Select> q; std::string er; return closure::selects(reinterpret_cast<const uint8_t*>(m.data()), m.size(), q, er); };
        const std::string eot = std::string("\x00\xC0\x03\x00\xFF\x2F\x00", 7);
        std::string hdrOnly = smfWith(eot).substr(0, 14), twoDeclared = smfWith(eot), shortHdr = smfWith(eot), alien = smfWith(eot);
        twoDeclared[11] = 2;
        shortHdr[7] = 5;
        alien += "XTRA"; be32(alien, 0);
        TEST("selects: valid SMF accepted; header-only, ntrks mismatch, short header, trailing bytes, alien chunk, no End-of-Track refused",
             ok(smfWith(eot)) && !ok(hdrOnly) && !ok(twoDeclared) && !ok(shortHdr) && !ok(smfWith(eot) + "abc") && !ok(alien) &&
             !ok(smfWith(std::string("\x00\xC0\x03", 3))) && !ok(smfWith(eot + std::string("\x00\xC0\x04", 3))));
        // Codex r2 R2-1: data bytes must be < 0x80; SMF 1.0: meta and SysEx events cancel running status.
        TEST("selects: a data byte with the high bit set is refused (B0 00 FF)", !ok(smfWith(std::string("\x00\xB0\x00\xFF\x00\xC0\x05\x00\xFF\x2F\x00", 11))));
        TEST("selects: running status after a meta or SysEx event is refused",
             !ok(smfWith(std::string("\x00\xC0\x05\x00\xFF\x01\x00\x00\x06\x00\xFF\x2F\x00", 13))) &&
             !ok(smfWith(std::string("\x00\xC0\x05\x00\xF0\x01\xF7\x00\x06\x00\xFF\x2F\x00", 13))));
        TEST("selects: format 2, and format 0 with two tracks, refused (SMF 1.0; only 0 and 1 observed)",
             !ok(smfTracks(2, {eot})) && !ok(smfTracks(0, {eot, eot})) && ok(smfTracks(1, {eot, eot})));
        // Codex r2 R2-2: format-1 tracks share channels; banks resolve across tracks in time order.
        {
            const std::string bankT1 = std::string("\x00\xB0\x00\x79\x00\xB0\x20\x40\x00\xFF\x2F\x00", 12);
            const std::string pcT2at1 = std::string("\x01\xC0\x05\x00\xFF\x2F\x00", 7), pcT1at0 = std::string("\x00\xC0\x05\x00\xFF\x2F\x00", 7);
            const std::string bankT2 = std::string("\x00\xB0\x00\x79\x00\xB0\x20\x40\x00\xFF\x2F\x00", 12);
            std::vector<closure::Select> q;
            std::string er, m = smfTracks(1, {bankT1, pcT2at1});
            TEST("selects: CC0/CC32 in track 1 at tick 0 apply to a PC in track 2 at tick 1",
                 closure::selects(reinterpret_cast<const uint8_t*>(m.data()), m.size(), q, er) && q.size() == 1 && q[0].msb == 121 && q[0].lsb == 64 && q[0].program == 5);
            q.clear(); m = smfTracks(1, {pcT1at0, bankT2});
            TEST("selects: equal ticks: lower track first (PC in track 1 at tick 0 is before the bank set in track 2 at tick 0)",
                 closure::selects(reinterpret_cast<const uint8_t*>(m.data()), m.size(), q, er) && q.size() == 1 && q[0].msb == -1 && q[0].lsb == -1);
        }
        const std::string over = smfWith(std::string("\x00\xF0\x7F\x42", 4));
        {
            // Codex r3 R3-1: bank/PC events are bounded per file (owner maximum 4 per MIDI leaf). Running-status
            // Program Changes "00 nn" are 2 bytes each: kMaxSelectEvents + 1 of them must be refused, kMaxSelectEvents not.
            auto pcs = [](size_t count) {
                std::string t("\x00\xC0\x00", 3);
                for (size_t i = 1; i < count; ++i) t += std::string("\x00\x00", 2);
                return smfWith(t + std::string("\x00\xFF\x2F\x00", 4));
            };
            const std::string atMax = pcs(closure::kMaxSelectEvents), over = pcs(closure::kMaxSelectEvents + 1);
            std::vector<closure::Select> q;
            std::string er;
            const bool okMax = closure::selects(reinterpret_cast<const uint8_t*>(atMax.data()), atMax.size(), q, er) && q.size() == closure::kMaxSelectEvents;
            q.clear();
            const bool refused = !closure::selects(reinterpret_cast<const uint8_t*>(over.data()), over.size(), q, er) && er.find("events") != std::string::npos && q.empty();
            TEST("selects: more than kMaxSelectEvents bank/PC events refused, exactly kMaxSelectEvents accepted", okMax && refused);
        }
        TEST("selects: SysEx length past the track refused", !closure::selects(reinterpret_cast<const uint8_t*>(over.data()), over.size(), s, e));
    }
    char tmpl[] = "/tmp/aiarr_kstw_XXXXXX";
    const fs::path tmp = mkdtemp(tmpl);
    {
        const fs::path k = tmp / "MINI.KST";
        std::string b, e;
        auto putTree = [&](const std::string& rel, const Node& n) { roottree::write(n, b, e); put(k / rel, b); };
        putTree("PAD/B/Resolved.pdc", pdcRef(121, 64, 5));          // USER01.PCG slot 5: present
        putTree("PAD/B/Empty.PDC", pdcRef(121, 64, 6));             // slot 6: empty (upper-case extension also read)
        putTree("PAD/B/Absent.pdc", pdcRef(120, 65, 0));            // USERDK02.PCG: absent
        putTree("KEYBOARDSET/B/Factory.ksc", build::keyboardSetTree(parts));
        put(k / "SOUND/USER01.PCG", ::build({{0x10000038, 4, 9, 5, "Generated Sound", "OC31x"}}, 1));
        const auto c = closure::check(k);
        TEST("closure on a generated mini-KST: 1 resolved, 1 slot empty, 1 file absent, 4 factory",
             c.ok && c.trees == 4 && c.resolved == 1 && c.slotEmpty == 1 && c.fileAbsent == 1 && c.factory == 4 &&
             c.dangling.count("USER01.PCG slot 6") && c.dangling.count("USERDK02.PCG slot 0"));
        {
            Node noBank = pdcRef(121, 64, 5);
            noBank.children[0].payload = smfTracks(1, {std::string("\x00\xFF\x2F\x00", 4), std::string("\x00\xC0\x05\x00\xFF\x2F\x00", 7)});
            const fs::path u = tmp / "UNRES.KST";
            roottree::write(noBank, b, e); put(u / "PAD/B/NoBank.pdc", b);
            const auto cu = closure::check(u);
            TEST("closure: a PC with no bank set is UNRESOLVED, never factory", cu.ok && cu.unresolved == 1 && cu.factory == 0 && cu.resolved == 0);
            const auto ru = assemble::build(tmp, "R6", {{"PAD", "B", "NoBank", noBank}});
            TEST("assemble: an unresolved selection fails the gate before publication",
                 !ru.ok && ru.message.find("unresolved") != std::string::npos && !fs::exists(tmp / "R6.KST"));
        }
        {
            // Codex r3 R3-2: all 231 087 owner MIDI leaves end in lower-case ".mid"; any other case is refused, not skipped.
            Node upperMid = pdcRef(121, 64, 5);
            upperMid.children[0].name = "Main.MID";
            const fs::path um = tmp / "UPMID.KST";
            roottree::write(upperMid, b, e); put(um / "PAD/B/Up.pdc", b);
            const auto cm = closure::check(um);
            TEST("closure: a MIDI leaf named .MID is a problem, not skipped", !cm.ok && cm.problems.size() == 1 && cm.problems[0].find(".mid") != std::string::npos);
            const auto rm = assemble::build(tmp, "R7", {{"PAD", "B", "Up", upperMid}});
            TEST("assemble: a MIDI leaf named .MID refused before any write", !rm.ok && rm.message.find(".mid") != std::string::npos && !fs::exists(tmp / "R7.KST.INCOMPLETE"));
        }
        put(k / "STYLE/B/Broken.stg", "Root\0\x01\0\0\0\0\x05xxxxx");
        TEST("closure reports an unparsable object instead of skipping it", !closure::check(k).ok && closure::check(k).problems.size() == 1);
    }

    // ---- W5 assembler ---------------------------------------------------------------------------------------------
    {
        const auto r = assemble::build(tmp, kits[0].name, kits[0].objects);
        TEST("assemble Pad KST: ok, 1 file, validator 0 errors, closure 0 dangling", r.ok && r.files == 1 && r.validatorErrors == 0 && r.dangling == 0 && r.factoryRefs == 1);
        TEST("assemble: layout PAD/<bank>/<name>.pdc, no other top-level entries, manifest beside the folder",
             fs::is_regular_file(tmp / "AIA_PAD_TEST.KST/PAD/AIA/AIA Pad Test.pdc") && std::distance(fs::directory_iterator(tmp / "AIA_PAD_TEST.KST"), {}) == 1 &&
             fs::is_regular_file(tmp / "AIA_PAD_TEST.KST.manifest.sha256") && ai_arranger::media::verifyBackup(tmp / "AIA_PAD_TEST.KST").empty());
        TEST("assemble: the written object parses back to the same tree bytes",
             roottree::readFile(tmp / "AIA_PAD_TEST.KST/PAD/AIA/AIA Pad Test.pdc").ok && roundTrip(roottree::readFile(tmp / "AIA_PAD_TEST.KST/PAD/AIA/AIA Pad Test.pdc").root));
        TEST("assemble: an existing KST is never overwritten", !assemble::build(tmp, kits[0].name, kits[0].objects).ok);
        TEST("assemble: a mounted volume is refused", assemble::build("/Volumes/AIARR_NOPE", "X", kits[0].objects).message.find("mounted volume") != std::string::npos);
        TEST("assemble: unsafe names refused (separator, '..', FAT character)",
             !assemble::build(tmp, "a/b", kits[0].objects).ok && !assemble::build(tmp, "..", kits[0].objects).ok &&
             !assemble::build(tmp, "N1", {{"PAD", "B", "x:y", kits[0].objects[0].tree}}).ok && !fs::exists(tmp / "N1.KST.INCOMPLETE"));
        TEST("assemble: names differing only in case refused before any write",
             !assemble::build(tmp, "N2", {{"PAD", "B", "P", kits[0].objects[0].tree}, {"PAD", "B", "p", kits[0].objects[0].tree}}).ok && !fs::exists(tmp / "N2.KST.INCOMPLETE"));
        TEST("assemble: unknown type refused", !assemble::build(tmp, "N3", {{"SONGBOOK", "B", "P", kits[0].objects[0].tree}}).ok);
        put(tmp / "src/BAD.PCG", "not korf");
        TEST("assemble: a carried file that is not a valid KORF container is refused",
             !assemble::build(tmp, "N4", kits[0].objects, {{tmp / "src/BAD.PCG", "SOUND/USER01.PCG"}}).ok);
        put(tmp / "src/USER01.PCG", ::build({{0x10000038, 4, 9, 5, "Generated Sound", "OC31x"}}, 1));
        const auto c = assemble::build(tmp, "N5", {{"PAD", "B", "Uses User", pdcRef(121, 64, 5)}}, {{tmp / "src/USER01.PCG", "SOUND/USER01.PCG"}});
        std::string a, bb, e;
        roottree::readBytes(tmp / "src/USER01.PCG", a, e); roottree::readBytes(tmp / "N5.KST/SOUND/USER01.PCG", bb, e);
        TEST("assemble: carried KORF copied byte-for-byte; the user reference resolves", c.ok && c.userRefs == 1 && c.dangling == 0 && a == bb);
        const auto d = assemble::build(tmp, "N6", {{"PAD", "B", "Dangles", pdcRef(121, 64, 6)}});
        TEST("assemble: a dangling user reference fails before publication (no final-named KST or manifest; work kept)",
             !d.ok && d.message.find("dangling") != std::string::npos && !fs::exists(tmp / "N6.KST") && !fs::exists(tmp / "N6.KST.manifest.sha256") &&
             fs::exists(tmp / "N6.KST.INCOMPLETE"));
        // Review findings (Level 1, author-run): each refusal must happen before anything is published.
        put(tmp / "R1.KST.manifest.sha256", "stale");
        TEST("assemble: an existing manifest is refused before any write",
             !assemble::build(tmp, "R1", kits[0].objects).ok && !fs::exists(tmp / "R1.KST") && !fs::exists(tmp / "R1.KST.INCOMPLETE"));
        TEST("assemble: a MIDI leaf the reader cannot parse (tick >= 2^28, 5-byte delta) is refused before any write",
             !assemble::build(tmp, "R2", {{"PAD", "B", "Late", build::padClipTree({121, 0, 0}, {{0x10000000u, 96, 60, 100}})}}).ok &&
             !fs::exists(tmp / "R2.KST") && !fs::exists(tmp / "R2.KST.INCOMPLETE"));
        TEST("assemble: an object whose tree is not its type (a Style as PAD, a Pad as STYLE) is refused",
             !assemble::build(tmp, "R3", {{"PAD", "B", "X", kits[2].objects[0].tree}}).ok &&
             !assemble::build(tmp, "R4", {{"STYLE", "B", "X", kits[0].objects[0].tree}}).ok &&
             !assemble::build(tmp, "R5", {{"KEYBOARDSET", "B", "X", kits[0].objects[0].tree}}).ok && !fs::exists(tmp / "R3.KST"));
        size_t all = 0;
        for (const auto& kit : kits) { const auto k = assemble::build(tmp / "src", kit.name, kit.objects); all += k.ok && k.validatorErrors == 0 && k.dangling == 0; }
        TEST("assemble: the three W6 test KSTs (Pad, KBD Set, Style) build, validate and close", all == 3);
    }
    fs::remove_all(tmp);

    // Optional: owner KST copy (never in CI). Read-only; counts only.
    if (const char* owner = std::getenv("AIARR_KST_OWNER_COPY")) {
        const auto c = closure::check(owner);
        std::printf("  owner KST closure: %zu trees, %zu MIDI leaves; user refs resolved %zu, dangling %zu (slot empty %zu, file absent %zu); "
                    "factory/GM %zu; unresolved %zu; problems %zu\n", c.trees, c.midis, c.resolved, c.danglingCount(), c.slotEmpty, c.fileAbsent, c.factory, c.unresolved, c.problems.size());
        // Not "every object read": the owner copy holds MIDI leaves that break SMF 1.0 (research note W4); the exact
        // number of refused leaves is pinned by AIARR_KST_OWNER_EXPECT instead, so any change is still caught.
        TEST("owner KST closure: the folder was walked", c.error.empty() && c.trees > 0);
        if (const char* want = std::getenv("AIARR_KST_OWNER_EXPECT")) {   // "resolved/dangling/unresolved/problems", e.g. from the research notes
            char got[96];
            std::snprintf(got, sizeof got, "%zu/%zu/%zu/%zu", c.resolved, c.danglingCount(), c.unresolved, c.problems.size());
            TEST("owner KST closure: counts match AIARR_KST_OWNER_EXPECT", std::string(want) == got);
        }
    } else std::printf("  SKIP: owner KST closure check (set AIARR_KST_OWNER_COPY to a local copy)\n");

    std::printf("%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
