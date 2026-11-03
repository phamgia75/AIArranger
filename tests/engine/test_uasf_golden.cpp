// UASF golden files — the backward-compatibility gate required by ADR-015 and
// docs/architecture/UASF_MIGRATION_POLICY.md ("Reader must successfully load and
// validate all golden files"; "CI must pass all existing golden files"). Until
// now no test loaded them. Every committed .uasf (golden + corpus conversions)
// must deserialize, validate, and survive a serialize -> deserialize round trip
// with the same (tick, event) content per track.

#include "engine/uasf/deserializer.h"
#include "engine/uasf/serializer.h"
#include "engine/uasf/validator.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

using namespace ai_arranger::uasf;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

using Key = std::tuple<uint64_t, int, int, int, int>;
static std::vector<Key> keys(const TrackDefinition& t) {
    std::vector<Key> k;
    for (const auto& e : t.events)
        k.emplace_back(e.tick, static_cast<int>(e.type), e.channel, e.data1, e.data2);
    std::sort(k.begin(), k.end());
    return k;
}

int main() {
    std::printf("Test: UASF golden files load, validate and round-trip\n");
    const std::string golden = std::string(REPO_DIR) + "/tests/golden/uasf/";
    const std::string corpus = std::string(CORPUS_DIR) + "/";
    const std::vector<std::string> files = {
        golden + "gate-3-demo-v1.uasf",
        corpus + "POP_ACOUSTIC_2.uasf",
        corpus + "CLASSIC_6_8.uasf",
    };

    for (const auto& path : files) {
        const std::string name = path.substr(path.find_last_of('/') + 1);
        UasfDeserializer des;
        const auto r = des.deserializeFromFile(path);
        TEST((name + ": loads").c_str(), r.success);
        if (!r.success) { std::fprintf(stderr, "    %s\n", r.error.c_str()); continue; }

        UasfValidator val;
        const auto v = val.validate(r.style);
        TEST((name + ": validates").c_str(), v.valid);

        UasfSerializer ser;
        UasfDeserializer des2;
        const auto again = des2.deserialize(ser.serialize(r.style).data);
        bool same = again.success && again.style.sections.size() == r.style.sections.size();
        for (size_t s = 0; same && s < r.style.sections.size(); ++s) {
            const auto& a = r.style.sections[s].tracks;
            const auto& b = again.style.sections[s].tracks;
            same = a.size() == b.size();
            for (size_t t = 0; same && t < a.size(); ++t) same = keys(a[t]) == keys(b[t]);
        }
        TEST((name + ": re-serializes to identical content").c_str(), same);
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
