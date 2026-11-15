// Issue #27: the Genos acceptance corpus (user-owned Yamaha styles and their .uasf conversions) is private and
// is not in this repository. A test that uses it is registered twice (CMakeLists.txt): `test_x` runs its public
// checks, on in-test data or the CC0 styles in fixtures/synthetic, and `test_x_corpus` (argv[1] == "--corpus")
// runs the checks that need the corpus. Without a corpus that entry prints CORPUS UNAVAILABLE and exits 77,
// which CTest reports as Skipped, never as a pass.
#pragma once
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace genos_corpus {

inline constexpr int kUnavailable = 77;

inline bool available() {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(CORPUS_DIR, ec))
        if (e.path().extension() == ".sty") return true;
    return false;
}

// A test with no public checks (registered only as test_x_corpus): returns 77 without a corpus, else -1.
inline int require(const char* test) {
    if (available()) return -1;
    std::printf("CORPUS UNAVAILABLE: %s needs the private Genos corpus "
                "(AIARR_GENOS_CORPUS_DIR=\"%s\" holds no .sty)\n", test, CORPUS_DIR);
    return kUnavailable;
}

// First thing in main() of a test with public checks: sets corpusRun (the corpus entry runs the public checks
// too, plus its corpus checks); returns the exit code when the corpus run cannot happen, else -1.
inline int gate(int argc, char** argv, const char* test, bool& corpusRun) {
    corpusRun = argc > 1 && std::strcmp(argv[1], "--corpus") == 0;
    if (corpusRun) return require(test);
    std::printf("  mode: public (no corpus data)\n");
    return -1;
}

#ifdef REPO_DIR
// A CC0 style from fixtures/synthetic (README there), e.g. synthetic("SYNTH_WALTZ_68").
inline std::string synthetic(const char* name) {
    return std::string(REPO_DIR) + "/fixtures/synthetic/" + name + ".sty";
}
#endif

} // namespace genos_corpus
