#ifndef AI_ARRANGER_MEDIA_SOUND_KORG_KMP_WRITER_H
#define AI_ARRANGER_MEDIA_SOUND_KORG_KMP_WRITER_H

// EXPERIMENTAL, verification level L1 (software only): an Instrument written as Korg-format multisample files, .KMP +
// .KSF, from the layout in the KRONOS Parameter Guide En 3 p.1158-1159 "Korg format sample files" (printed pages; PDF
// pp.1172-1173), the same documentation korg_kmp_source.h reads. Not run on a KRONOS or a Pa5X: no compatibility is
// claimed. Before the folder is published it is read back with korg_kmp_source.h and must give the same zones and audio.
//
// One velocity layer = one multisample (velocity is not in a KMP; it lives in the Program/Sound). Per layer:
//   mono:   <stem>.KMP                 multisample name = the layer name
//   stereo: <stem>_L.KMP + <stem>_R.KMP, multisample names "<layer> -L" / "<layer> -R": the KRONOS stereo naming
//           (p.836, p.1161) inside, the Pa5X Import KMP pairing by _L/_R file name (MAN5 p.1206, 1209) outside.
// KSF names MSnnnn[L|R].KSF fit the 12-byte RLP1 file name field ("including period and extension", 8.3). Keys
// outside every zone become SKIPPEDSAMPL regions (p.1159), since each region starts at the previous top key + 1.
// Each KSF holds the whole sample (Start 0); End = last frame, or the last looped frame (loop end inclusive, the
// reader's convention: inferred, docs/research/KRONOS_KMP_KSF_IMPORT.md A3), so audio after a loop is kept but not played.

#include "media/sound/korg_kmp_source.h"
#include "media/sound/package_writer.h"
#include <set>

namespace ai_arranger::media::sound {

namespace kmpw {
using Bytes = std::vector<uint8_t>;
inline void be32(Bytes& b, uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s)); }
inline void str(Bytes& b, const std::string& s, size_t n) { for (size_t i = 0; i < n; ++i) b.push_back(i < s.size() ? uint8_t(s[i]) : 0); }
inline void chunk(Bytes& b, const char* id, const Bytes& body) { str(b, id, 4); be32(b, uint32_t(body.size())); b.insert(b.end(), body.begin(), body.end()); }

// side: 0 mono, else 0/1 = left/right channel of a stereo sample.
inline Bytes ksf(const Sample& s, int side, uint32_t number) {
    const uint32_t n = uint32_t(s.frames()), end = s.looped ? s.loopEnd - 1 : n - 1;
    Bytes b, smp, smd, sno, nm;
    str(smp, s.name, 16);
    be32(smp, 0); be32(smp, 0); be32(smp, s.looped ? s.loopStart : 0); be32(smp, end);   // Start, 2nd Start, Loop Start, End
    chunk(b, "SMP1", smp);
    be32(smd, s.rate);
    smd.push_back(uint8_t((s.looped ? 0 : 0x80) | 0x20));   // Loop Off bit 7, Not Use 2nd Start bit 5, uncompressed, 0 dB
    smd.push_back(0);                                       // loop tune
    smd.push_back(1);                                       // channels: 1 (documented); stereo = an -L/-R pair
    smd.push_back(16);
    be32(smd, n);
    for (uint32_t f = 0; f < n; ++f) { const uint16_t v = uint16_t(s.pcm[size_t(f) * s.channels + side]); smd.push_back(uint8_t(v >> 8)); smd.push_back(uint8_t(v)); }
    chunk(b, "SMD1", smd);
    be32(sno, number);
    chunk(b, "SNO1", sno);
    str(nm, s.name, 24);
    chunk(b, "NAME", nm);
    return b;
}

struct Region { uint8_t root, top; int8_t tune; std::string ksf; };
inline Bytes kmp(const std::string& name, const std::vector<Region>& rs, uint32_t number) {
    Bytes b, msp, nm, rlp1, rlp2(4, 0), mno;
    str(msp, name, 16);
    msp.push_back(uint8_t(rs.size()));
    msp.push_back(0x80);   // Not Use 2nd Start
    chunk(b, "MSP1", msp);
    str(nm, name, 24);
    chunk(b, "NAME", nm);
    for (const auto& r : rs) {
        rlp1.push_back(r.root);   // MSB 0 = Transpose
        rlp1.push_back(r.top);
        rlp1.push_back(uint8_t(r.tune));
        rlp1.push_back(0);        // level: written 0 (its unit is not documented for the Pa5X), see the report
        rlp1.push_back(64);       // pan, "currently unused"
        rlp1.push_back(0);        // filter cutoff, "currently unused"
        str(rlp1, r.ksf, 12);
    }
    chunk(b, "RLP1", rlp1);
    chunk(b, "RLP2", rlp2);
    be32(mno, number);
    chunk(b, "MNO1", mno);
    return b;
}
}  // namespace kmpw

struct KorgFile { std::string name; std::vector<uint8_t> bytes; };

// The files for in, or a refusal. *notes receives what is not carried.
inline std::string korgFiles(const Instrument& in, std::vector<KorgFile>* out, std::string* notes) {
    out->clear();
    if (const auto e = validate(in); !e.empty()) return "invalid: " + e.front();
    std::set<std::string> names;
    std::map<std::pair<int, int>, std::string> ksfOf;   // (sample, side) -> file name
    std::map<int, uint32_t> number;                     // sample -> nnnn of MSnnnn[L|R].KSF and its SNO1
    size_t levels = 0, tails = 0;
    for (size_t l = 0; l < in.layers.size(); ++l) {
        const Layer& L = in.layers[l];
        const std::string what = "layer '" + L.name + "'";
        // The name must come back unchanged from the reader (korg::layerName, korg::side).
        if (L.name != korg::layerName(L.name)) return what + ": a multisample name cannot start or end with a space or contain a comma";
        if (korg::side(L.name).second) return what + ": a mono multisample name cannot end in -L or -R (the KRONOS stereo naming, p.1161)";
        if (!names.insert(L.name).second) return what + ": two layers have the same name";
        const int ch = in.samples[size_t(L.zones.front().sample)].channels;
        std::vector<Zone> zs = L.zones;
        std::sort(zs.begin(), zs.end(), [](const Zone& a, const Zone& b) { return a.lo < b.lo; });
        std::vector<std::vector<kmpw::Region>> rs(ch == 2 ? 2 : 1);
        int key = 0;   // first key not covered yet
        for (const Zone& z : zs) {
            const Sample& s = in.samples[size_t(z.sample)];
            if (s.channels != ch) return what + ": mixes mono and stereo samples (a stereo multisample is an -L/-R pair of mono ones, p.1161)";
            if (z.tuneCents < -99 || z.tuneCents > 99) return what + ": zone tune " + std::to_string(z.tuneCents) + " cents is outside -99..+99 (p.1158)";
            levels += z.levelCentibels != 0;
            for (auto& r : rs) {
                if (z.lo > key) r.push_back({60, uint8_t(z.lo - 1), 0, "SKIPPEDSAMPL"});
                const int side = &r - rs.data();
                auto& f = ksfOf[{z.sample, ch == 2 ? side : -1}];
                if (f.empty()) {
                    const uint32_t k = number.emplace(z.sample, uint32_t(number.size())).first->second;
                    if (k > 9999) return "more than 10000 samples (the MSnnnn.KSF naming)";
                    char nm[16];
                    std::snprintf(nm, sizeof nm, "MS%04u%s.KSF", unsigned(k), ch == 2 ? (side ? "R" : "L") : "");
                    f = nm;
                    out->push_back({f, kmpw::ksf(s, ch == 2 ? side : 0, k)});
                    tails += s.looped && s.loopEnd < s.frames();
                }
                r.push_back({z.root, z.hi, z.tuneCents, f});
            }
            key = z.hi + 1;
        }
        std::string stem;   // 4 letters/digits of the name + the layer number: 6 characters, 8.3 with _L/_R
        for (char c : L.name) if (stem.size() < 4 && std::isalnum(static_cast<unsigned char>(c))) stem += char(std::toupper(static_cast<unsigned char>(c)));
        if (stem.empty()) stem = "MS";
        char num[4];
        std::snprintf(num, sizeof num, "%02zu", l);
        stem += num;
        for (auto& r : rs) {
            if (key <= 127) r.push_back({60, 127, 0, "SKIPPEDSAMPL"});
            const int side = &r - rs.data();
            const std::string name = ch == 2 ? L.name + (side ? " -R" : " -L") : L.name;
            out->push_back({stem + (ch == 2 ? (side ? "_R" : "_L") : "") + ".KMP", kmpw::kmp(name, r, uint32_t(l))});
        }
    }
    const auto line = [&](size_t n, const std::string& s) { if (n) *notes += "  " + std::to_string(n) + " " + s + "\n"; };
    line(levels, "zone(s) have a level offset: NOT carried (RLP1 Level unit not documented for the Pa5X; written 0)");
    line(tails, "sample(s) have audio after the loop end: kept in the KSF, but End = loop end, so it does not play");
    return "";
}

// Expected after a read back: zones as written (skipped regions dropped), each sample cut at End.
inline std::string korgReadBackDiffers(const Instrument& in, const Instrument& back) {
    if (back.layers.size() != in.layers.size()) return "layer count";
    for (size_t l = 0; l < in.layers.size(); ++l) {
        const Layer &A = in.layers[l], &B = back.layers[l];
        std::vector<Zone> zs = A.zones;
        std::sort(zs.begin(), zs.end(), [](const Zone& a, const Zone& b) { return a.lo < b.lo; });
        if (A.name != B.name || A.velLo != B.velLo || A.velHi != B.velHi || zs.size() != B.zones.size()) return "layer " + A.name;
        for (size_t z = 0; z < zs.size(); ++z) {
            const Zone &x = zs[z], &y = B.zones[z];
            const Sample &s = in.samples[size_t(x.sample)], &t = back.samples[size_t(y.sample)];
            const size_t keep = (s.looped ? s.loopEnd : s.frames()) * s.channels;
            if (x.lo != y.lo || x.hi != y.hi || x.root != y.root || x.tuneCents != y.tuneCents || s.rate != t.rate || s.channels != t.channels ||
                s.looped != t.looped || (s.looped && (s.loopStart != t.loopStart || s.loopEnd != t.loopEnd)) ||
                !std::equal(s.pcm.begin(), s.pcm.begin() + std::ptrdiff_t(keep), t.pcm.begin(), t.pcm.end()))
                return "layer " + A.name + " zone " + std::to_string(z + 1);
        }
    }
    return "";
}

// Writes a new folder dest (under dest.INCOMPLETE, renamed once complete and read back): the KMP/KSF files, REPORT.txt,
// PACKAGE.sha256. Never writes into an existing folder. Returns the report, or "" with *error set.
inline std::string writeKorgFolder(const Instrument& in, const std::filesystem::path& destGiven, const std::string& notes, std::string* error) {
    namespace fs = std::filesystem;
    auto fail = [&](std::string m) { if (error) *error = std::move(m); return std::string(); };
    std::error_code ec;
    const fs::path dest = folderPath(destGiven), out = incompletePath(dest);
    if (fs::exists(dest, ec) || ec) return fail("refusing: " + dest.string() + " already exists");
    if (fs::exists(out, ec) || ec) return fail("refusing: " + out.string() + " already exists (an earlier write that did not finish)");
    std::vector<KorgFile> kf;
    std::string lost;
    if (const auto e = korgFiles(in, &kf, &lost); !e.empty()) return fail("refusing: " + e);
    fs::create_directories(dest.parent_path(), ec);
    if (!fs::create_directory(out, ec)) return fail("cannot create " + out.string());
    pkgio::FileSet files{out};
    for (const auto& f : kf) files.save(f.name, f.bytes);

    std::string layers, splits, rep =
        "AIArranger Korg-format multisample folder (.KMP + .KSF)\n"
        "EXPERIMENTAL. Verification level L1 only: written from the KRONOS Parameter Guide En 3 p.1158-1159 and read back\n"
        "by AIArranger's own KMP/KSF reader. Not tested on a KRONOS or a Pa5X (L3 needs an owner hardware test). It is\n"
        "not labelled Pa5X-compatible.\n\n";
    rep += "Instrument: " + in.name + "\n";
    for (size_t l = 0; l < in.layers.size(); ++l) {
        const Layer& L = in.layers[l];
        rep += "Layer " + std::to_string(l) + "  velocity " + std::to_string(L.velLo) + "-" + std::to_string(L.velHi) + "  " + L.name + "\n";
        layers += (l ? "," : "") + L.name;
        if (l) splits += (l > 1 ? "," : "") + std::to_string(L.velLo);
    }
    rep += "Files:\n";
    for (const auto& f : kf) rep += "  " + f.name + "  " + std::to_string(f.bytes.size()) + " bytes\n";
    rep += "\nNot in these files: velocity layers (a KMP is one key-split multisample; layers belong to the Program/Sound), "
           "envelopes, filters, effects. Read back with:\n  aiarranger-sound kronos --in <this folder> --name '" + in.name + "'" +
           (in.layers.size() > 1 ? " --layers '" + layers + "' --splits " + splits : "") + " --out DIR\n";
    if (!lost.empty()) rep += "Not carried:\n" + lost;
    rep += "\nPa5X (owner test, not done): Sample Edit > Import, format KMP (MAN5 p.1206-1209) takes one multisample;\n"
           "  stereo = the _L/_R pair. Whether the Pa5X accepts KRONOS-generation KMP/KSF is UNKNOWN (MAN5 p.722 names\n"
           "  Trinity/Triton).\n";
    if (!notes.empty()) rep += "\n" + notes;
    files.save("REPORT.txt", rep);
    files.saveManifest();
    if (!files.ok) return fail("write failed; the unfinished folder stays at " + out.string() + " (delete it)");
    if (const auto why = files.checkFiles(); !why.empty())
        return fail("the written folder does not check (" + why + "); the unfinished folder stays at " + out.string() + " (delete it)");
    std::vector<std::string> names;
    std::vector<uint8_t> vs;
    for (size_t l = 0; l < in.layers.size(); ++l) { names.push_back(in.layers[l].name); if (l) vs.push_back(in.layers[l].velLo); }
    Instrument back;
    const auto re = buildFromKorgFolder(out, in.name, names, vs, &back);
    const auto diff = re.empty() ? korgReadBackDiffers(in, back) : re;
    if (!diff.empty()) return fail("the written folder does not read back as written (" + diff + "); the unfinished folder stays at " + out.string() + " (delete it)");
    if (!publishNewFolder(out, dest)) return fail("refusing: " + dest.string() + " appeared while writing; the folder stays at " + out.string());
    return rep;
}

} // namespace ai_arranger::media::sound

#endif // AI_ARRANGER_MEDIA_SOUND_KORG_KMP_WRITER_H
