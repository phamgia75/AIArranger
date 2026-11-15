// aiarranger-kit: the Pa5X Import Kit (roadmap E9, src/jobs/import_kit.h).
//   build --in PATH [--in PATH ...] --out DIR
//       AIArranger outputs (Sound or Drum Kit package folders, KMP/KSF folders, Style .mid files; only read) gathered into
//       the new folder DIR for a USB stick, with KIT.md, manifest.json and PACKAGE.sha256. Runs as an E6 export job:
//       DIR.INCOMPLETE, every member re-read, one no-replace rename, DIR.export.md/.json beside it.
//   verify DIR
//       Re-checks a kit (for example the copy on the stick): PACKAGE.sha256 and every member through its reader.
// Best possible label: STRUCTURALLY VALID — NOT VERIFIED ON A PA5X. Exit 0 = built / verified, 1 = refused or failed.

#include "jobs/import_kit.h"
#include <cstdio>

using namespace ai_arranger::jobs;

static int usage() {
    std::fprintf(stderr, "usage: aiarranger-kit build --in PATH [--in PATH ...] --out DIR\n"
                         "       aiarranger-kit verify DIR\n");
    return 1;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string cmd = argv[1];
    if (cmd == "verify" && argc == 3) {
        std::string text;
        const auto e = kit::verifyKit(argv[2], nullptr, &text);
        std::printf("%s\n", e.empty() ? text.c_str() : ("NOT verified: " + e).c_str());
        return e.empty() ? 0 : 1;
    }
    if (cmd != "build") return usage();
    std::vector<fs::path> in;
    fs::path out;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--in") in.push_back(argv[i + 1]);
        else if (a == "--out" && out.empty()) out = argv[i + 1];
        else return usage();
    }
    if (argc % 2 || in.empty() || out.empty()) return usage();
    JobState st;
    st.id = 1;
    runExport(kit::importKit(in, out), st, [] { return false; }, [](const JobState&) {});
    if (st.phase != Phase::Done) {
        std::fprintf(stderr, "Kit NOT written (%s): %s\n", phaseName(st.stoppedAt), st.reason.c_str());
        for (const auto& n : st.missing) std::fprintf(stderr, "  %s\n", n.c_str());
        return 1;
    }
    std::printf("Written: %s\n%s\n", st.dest.c_str(), kit::kLabel);
    for (const auto& n : st.notes) std::printf("  %s\n", n.c_str());
    std::printf("Report: %s\n", st.reportPath.c_str());
    return 0;
}
