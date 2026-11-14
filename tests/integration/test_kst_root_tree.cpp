// Pa5X Root-tree container (.STG .KSC .PDC): parser, byte-exact writer, and the KST-wide owner check (W1 + W2).
// Every fixture is generated here from the observed layout (media/panative/root_tree.h); no owner bytes.
// The optional owner check runs only when AIARR_KST_OWNER_COPY points to a local copy of an owner KST folder;
// it reads the copy only and prints counts only.

#include "media/kst_inventory.h"
#include "media/panative/panative_reader.h"
#include "media/panative/root_tree.h"
#include "../support/panative_fixture.h"
#include <cstdio>
#include <cstdlib>

using namespace ai_arranger::media;
using panative::roottree::Node;
using panative::roottree::Tree;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

static void setBe32(std::string& s, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) s[at + i] = char(v >> (8 * (3 - i))); }

// Independent byte builder (does not use the writer under test).
static std::string node(const std::string& tag, const std::string& name, const std::string& payload) {
    std::string s = tag + '\0' + name + '\0';
    be32(s, uint32_t(payload.size()));
    return s + payload;
}
static std::string leaf(const std::string& name, const std::string& b) { return node("Leaf", name, b); }
static std::string branch(const std::string& name, const std::string& kids) { return node("Branch", name, kids); }
static std::string root(const std::string& kids) { return node("Root", "\x01", kids); }

// A minimal SMF of the observed shape: format 0, 1 track, 384 PPQ, CC0/CC32/PC on channel 1, end of track.
static std::string smf(uint8_t msb, uint8_t lsb, uint8_t pc) {
    std::string t = {0, char(0xB0), 0, char(msb), 0, char(0xB0), 32, char(lsb), 0, char(0xC0), char(pc), 0, char(0xFF), 0x2F, 0};
    std::string s = "MThd";
    be32(s, 6); be16(s, 0); be16(s, 1); be16(s, 0x0180);
    s += "MTrk"; be32(s, uint32_t(t.size()));
    return s + t;
}
static std::string xml(const std::string& body) { return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" + body + "\n"; }
static std::string clip(const std::string& type) { return xml("<CLIP type=\"" + type + "\" version=\"0\" release=\"4\"/>"); }

static Tree P(const std::string& s) { return panative::roottree::parse(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
static bool roundTrips(const std::string& s) {
    const auto t = P(s);
    std::string out, err;
    return t.ok && panative::roottree::write(t.root, out, err) && out == s;
}
static std::string W(const Node& n, std::string* err = nullptr) {
    std::string out = "unchanged", e;
    if (!panative::roottree::write(n, out, e)) out = "REFUSED";
    if (err) *err = e;
    return out;
}

int main() {
    std::printf("Test: Pa5X Root-tree container (parser + byte-exact writer)\n");

    // Generated fixtures, one per observed shape (KST_BINARY_STRUCTURE_FINDINGS.md §2.1). Names are invented.
    const std::string pdc = root(leaf("Main.mid", smf(121, 3, 10)) + leaf("ClipDescription.xml", clip("PadClip")));
    const std::string kscFlat = root(leaf("Lower.mid", smf(121, 0, 1)) + leaf("Upper1.mid", smf(121, 64, 5)) +
                                     leaf("Upper2.mid", smf(121, 0, 2)) + leaf("Upper3.mid", smf(120, 0, 0)) +
                                     leaf("MFX.mid", smf(0, 0, 0)) + leaf("ClipDescription.xml", clip("KeyboardSetClip")));
    std::string parts;
    for (const char* p : {"Lower", "Upper1", "Upper2", "Upper3"})
        parts += branch(std::string(p) + ".plc", leaf("Main.mid", smf(121, 1, 7)) + leaf("ClipDescription.xml", clip("PlainClip")));
    const std::string kscBody = branch("Main.ssn", branch("RTTracks.scn", parts + leaf("SceneDescription.xml", xml("<SCENE/>"))) +
                                                   leaf("SessionDescription.xml", xml("<SESSION/>"))) +
                                leaf("MFX.mid", smf(0, 0, 0)) + leaf("ClipDescription.xml", clip("KeyboardSetClip"));
    const std::string kscSession = root(kscBody);
    std::string scenes;
    for (const char* e : {"Var1", "Var2", "Fill1", "Intro1", "End1"})
        scenes += branch(std::string(e) + ".scn", branch("Drum.pdc", leaf("Main.mid", smf(120, 0, 0)) + leaf("ClipDescription.xml", clip("TrackClip"))) +
                                                  branch("Chord.csc", leaf("Main.mid", smf(0, 0, 0))) + leaf("SceneDescription.xml", xml("<SCENE/>")));
    std::string ksa;
    for (const char* k : {"ks1", "ks2", "ks3", "ks4"}) ksa += branch(k, branch("Generated Set.ksc", kscBody));
    const std::string stg = root(branch("Main.ssn", branch("SongStyle1.scn",
        branch("Generated Style.stc", branch("Main.ssn", scenes + leaf("SessionDescription.xml", xml("<SESSION/>"))) + leaf("MFX.mid", smf(0, 0, 0))) +
        branch("Generated Style.ksa", ksa))) + leaf("StageDescription.xml", xml("<STAGE version=\"1\" release=\"2\"/>")));

    const auto tp = P(pdc), tf = P(kscFlat), ts = P(kscSession), tg = P(stg);
    TEST("PDC shape parses: Root -> Main.mid, ClipDescription.xml",
         tp.ok && tp.root.kind == Node::Root && tp.root.children.size() == 2 && tp.root.children[0].name == "Main.mid" &&
         tp.root.children[0].kind == Node::Leaf && tp.root.children[0].payload == smf(121, 3, 10) && tp.root.children[1].payload == clip("PadClip"));
    TEST("flat KSC shape parses: 6 leaves", tf.ok && tf.root.children.size() == 6 && tf.root.children[5].name == "ClipDescription.xml");
    TEST("session KSC shape parses: Main.ssn/RTTracks.scn/Lower.plc/Main.mid",
         ts.ok && ts.root.children.size() == 3 && ts.root.children[0].kind == Node::Branch &&
         ts.root.children[0].children[0].children[0].name == "Lower.plc" &&
         ts.root.children[0].children[0].children[0].children[0].payload == smf(121, 1, 7));
    TEST("STG shape parses: stage, style core, 5 scenes, 4 embedded KBD Sets",
         tg.ok && tg.root.children.size() == 2 && tg.root.children[0].children[0].children.size() == 2 &&
         tg.root.children[0].children[0].children[0].children[0].children.size() == 6 &&
         tg.root.children[0].children[0].children[1].children.size() == 4 &&
         tg.root.children[0].children[0].children[1].children[3].children[0].name == "Generated Set.ksc");
    TEST("W2: write(parse(x)) == x for the PDC, both KSC shapes and the STG",
         roundTrips(pdc) && roundTrips(kscFlat) && roundTrips(kscSession) && roundTrips(stg));
    const std::string odd = root(branch("", "") + leaf("", "") + leaf(std::string("N\xE9\xFF\x01 .mid", 8), std::string("\0\0Root\0", 7)) +
                                 branch("deep", branch("d2", branch("d3", leaf("x", "y")))));
    TEST("empty Branch, empty Leaf, empty and non-ASCII names, NUL-laden leaf bytes parse and round-trip",
         P(odd).ok && P(odd).root.children.size() == 4 && P(odd).root.children[2].payload.size() == 7 && roundTrips(odd));
    TEST("Root with no children is accepted and round-trips", roundTrips(root("")));

    // Writer builds from a tree, not only from parsed bytes.
    Node b; b.kind = Node::Root; b.name = "\x01";
    Node l; l.kind = Node::Leaf; l.name = "Main.mid"; l.payload = smf(121, 3, 10);
    Node c; c.kind = Node::Leaf; c.name = "ClipDescription.xml"; c.payload = clip("PadClip");
    b.children = {l, c};
    TEST("writer output for a built tree equals the independently built bytes", W(b) == pdc);
    auto edited = P(pdc);
    edited.root.children[0].payload = smf(121, 64, 0);
    TEST("an edited leaf is written with its new length and parses back",
         P(W(edited.root)).ok && P(W(edited.root)).root.children[0].payload == smf(121, 64, 0) && W(edited.root) != pdc);

    // Parser refusals (each a corruption of a valid generated file).
    std::string bad = pdc; bad.pop_back();
    TEST("truncated file refused", !P(bad).ok && P(bad).error.find("Root length") != std::string::npos);
    TEST("trailing byte refused", !P(pdc + "x").ok);
    TEST("empty / short input refused", !P("").ok && !P(std::string("Root\0\x01\0", 7)).ok);
    TEST("top tag other than Root refused", !P(node("Branch", "\x01", "")).ok && !P(node("Rooq", "\x01", "")).ok);
    TEST("Root name other than 0x01 refused", !P(node("Root", "\x02", "")).ok && !P(node("Root", "", "")).ok);
    TEST("Root below the top level refused", !P(root(node("Root", "\x01", ""))).ok);
    TEST("unknown inner tag refused", !P(root(node("Twig", "x", ""))).ok);
    {
        std::string over = root(leaf("Main.mid", "abc") + leaf("x", "y"));
        setBe32(over, 11 + 14, 1000);   // first leaf claims more than its parent holds
        TEST("child running past its parent refused", !P(over).ok && P(over).error.find("past its parent") != std::string::npos);
        std::string inner = root(branch("b", leaf("a", "12345")) + leaf("z", "q"));
        setBe32(inner, 11 + 9, 3);      // branch payload ends inside its child leaf
        TEST("branch length ending inside a child refused", !P(inner).ok);
    }
    TEST("unterminated tag / name and missing length refused",
         !P(root("Leaf")).ok && !P(root(std::string("Leaf\0abc", 8))).ok && !P(root(std::string("Leaf\0a\0\0\0", 9))).ok);
    {
        std::string deep = leaf("x", "y");
        for (int i = 0; i < 40; ++i) deep = branch("b", deep);
        std::string ok = leaf("x", "y");
        for (int i = 0; i < 30; ++i) ok = branch("b", ok);
        TEST("nesting deeper than the limit refused, 30 levels accepted", !P(root(deep)).ok && P(root(ok)).ok);
    }
    {
        // Review finding: a 60 MB file of empty leaves took 1.24 GB RSS (6.3 M nodes); observed maximum is 513 nodes.
        std::string many, few;
        for (int i = 0; i < 70000; ++i) many += leaf("", "");
        for (int i = 0; i < 1000; ++i) few += leaf("", "");
        // Codex r1 C3: the writer must refuse what the parser refuses.
        Node wide; wide.kind = Node::Root; wide.name = "\x01";
        wide.children.resize(65537);
        Node deepN; deepN.kind = Node::Leaf; deepN.name = "x";
        for (int i = 0; i < 32; ++i) { Node b2; b2.kind = Node::Branch; b2.name = "b"; b2.children.push_back(deepN); deepN = b2; }
        Node d32 = deepN.children[0];   // 31 branches + leaf at depth 32
        Node r33; r33.kind = Node::Root; r33.name = "\x01"; r33.children.push_back(deepN);
        Node r32; r32.kind = Node::Root; r32.name = "\x01"; r32.children.push_back(d32);
        TEST("writer refuses more than kMaxNodes nodes and nesting past kMaxDepth; depth 32 written and parsed",
             W(wide) == "REFUSED" && W(r33) == "REFUSED" && W(r32) != "REFUSED" && P(W(r32)).ok && !P(W(r33) == "REFUSED" ? "" : W(r33)).ok);
        // Codex r3 R3-3: the parser descends into every Branch's (possibly empty) child list at depth + 1, so an empty
        // Branch at depth 32 is refused there; the writer must refuse it too. Leaf boundary: depth 32 ok, 33 refused.
        auto chain = [](Node inner, int wraps) {
            for (int i = 0; i < wraps; ++i) { Node b2; b2.kind = Node::Branch; b2.name = "b"; b2.children.push_back(inner); inner = b2; }
            Node r; r.kind = Node::Root; r.name = "\x01"; r.children.push_back(inner); return r;
        };
        Node emptyB; emptyB.kind = Node::Branch; emptyB.name = "e";
        const Node e32 = chain(emptyB, 31), e31 = chain(emptyB, 30);
        TEST("writer: an empty Branch at depth 32 refused (parser refuses it), at depth 31 written and parsed",
             W(e32) == "REFUSED" && P(W(e31)).ok);
        TEST("writer: leaf boundary unchanged (leaf at depth 32 written and parsed, 33 refused)",
             W(r32) != "REFUSED" && P(W(r32)).ok && W(r33) == "REFUSED");
        TEST("more nodes than the node limit refused (memory bound), 1000 accepted",
             !P(root(many)).ok && P(root(many)).error.find("nodes") != std::string::npos && P(root(few)).ok);
    }

    // The two KST containers tell each other apart.
    const std::string korf = build({{0x12020010, 6, 0, 0, "Generated Sample", "raw"}}, 1);
    TEST("a KORF file is not a Root tree; a Root tree is not a KORF file",
         !P(korf).ok && !panative::roottree::looksLikeRootTree(reinterpret_cast<const uint8_t*>(korf.data()), korf.size()) &&
         panative::parse(reinterpret_cast<const uint8_t*>(korf.data()), korf.size()).ok &&
         !panative::parse(reinterpret_cast<const uint8_t*>(pdc.data()), pdc.size()).wrapper);

    // Writer refusals: anything parse() could not have produced.
    std::string err;
    Node r2 = b; r2.name = "\x02";
    TEST("writer refuses a Root not named 0x01", W(r2, &err) == "REFUSED" && err.find("0x01") != std::string::npos);
    Node r3 = b; r3.kind = Node::Branch;
    TEST("writer refuses a top node that is not Root", W(r3) == "REFUSED");
    Node r4 = b; r4.children.push_back(b);
    TEST("writer refuses Root below the top level", W(r4) == "REFUSED");
    Node r5 = b; r5.children[0].name = std::string("a\0b", 3);
    TEST("writer refuses a name with NUL", W(r5) == "REFUSED");
    Node r6 = b; r6.children[0].children.push_back(c);
    TEST("writer refuses a Leaf with children", W(r6) == "REFUSED");
    Node r7 = b; r7.payload = "x";
    TEST("writer refuses Root/Branch leaf bytes", W(r7) == "REFUSED");
    {
        std::string out = "keep", e;
        TEST("refused write leaves the output untouched", !panative::roottree::write(r5, out, e) && out == "keep");
    }

    // Optional: owner KST copy (never in CI). Read-only; counts only.
    if (const char* owner = std::getenv("AIARR_KST_OWNER_COPY")) {
        detail::Listing ls;
        std::string lerr;
        size_t trees = 0, treeOk = 0, same = 0, korfFiles = 0, korfOk = 0, korfItems = 0, json = 0, other = 0, unreadable = 0;
        if (detail::listFiles(owner, ls, lerr))
            for (const auto& [rel, e] : ls.files) {
                std::string bytes, rerr;
                if (!panative::roottree::readBytes(e.abs, bytes, rerr)) { ++unreadable; continue; }
                const auto* p = reinterpret_cast<const uint8_t*>(bytes.data());
                if (panative::roottree::looksLikeRootTree(p, bytes.size())) {
                    ++trees;
                    const auto t = panative::roottree::parse(p, bytes.size());
                    treeOk += t.ok;
                    std::string out, werr;
                    same += t.ok && panative::roottree::write(t.root, out, werr) && out == bytes;
                } else if (const auto k = panative::parse(p, bytes.size()); k.wrapper) {
                    ++korfFiles; korfOk += k.ok; korfItems += k.items.size();
                } else if (e.abs.filename() == "metadir.json") ++json;
                else ++other;
            }
        std::printf("  owner KST: %zu files; Root trees %zu/%zu parse, %zu/%zu byte-identical after write; "
                    "KORF %zu/%zu valid (%zu items); metadir.json %zu; other %zu; unreadable %zu\n",
                    ls.files.size(), treeOk, trees, same, trees, korfOk, korfFiles, korfItems, json, other, unreadable);
        TEST("owner KST: every Root tree parses", trees > 0 && treeOk == trees);
        TEST("owner KST: every Root tree is byte-identical after parse -> write", same == trees);
        TEST("owner KST: every KORF file is a valid container", korfFiles > 0 && korfOk == korfFiles);
        TEST("owner KST: nothing else besides metadir.json, nothing unreadable", other == 0 && unreadable == 0);
    } else std::printf("  SKIP: owner KST check (set AIARR_KST_OWNER_COPY to a local copy)\n");

    std::printf("%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
