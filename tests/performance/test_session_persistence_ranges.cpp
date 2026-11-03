// Session JSON: out-of-range numbers are rejected, never wrapped into range.
//
// deserialize() narrowed each parsed long with a C cast before isValid() ran, so
// "tempo_bpm": 4294967416 became 120 and "variation": 256 became 0 — a corrupt
// or hand-edited session file loaded as a different, valid-looking session.

#include "session/session_persistence.h"
#include <cstdio>
#include <string>

using namespace ai_arranger::session;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

static std::string with(const std::string& json, const std::string& key, const std::string& val) {
    const std::string k = "\"" + key + "\": ";
    const size_t p = json.find(k);
    if (p == std::string::npos) return json;
    const size_t start = p + k.size();
    const size_t end = json.find_first_of(",\n}", start);
    return json.substr(0, start) + val + json.substr(end);
}

int main() {
    std::printf("Test: session JSON numeric range handling\n");
    PerformerSession base;
    const std::string good = SessionPersistence::serialize(base);
    PerformerSession out;
    TEST("baseline round-trips", SessionPersistence::deserialize(good, out) && out.tempo_bpm == 120);

    TEST("tempo 4294967416 (wraps to 120 as uint32) rejected",
         !SessionPersistence::deserialize(with(good, "tempo_bpm", "4294967416"), out));
    TEST("variation 256 (wraps to 0 as uint8) rejected",
         !SessionPersistence::deserialize(with(good, "variation", "256"), out));
    TEST("split_point 316 (wraps to 60 as uint8) rejected",
         !SessionPersistence::deserialize(with(good, "split_point", "316"), out));
    TEST("negative tempo rejected",
         !SessionPersistence::deserialize(with(good, "tempo_bpm", "-5"), out));
    TEST("version 65537 (wraps to 1 as uint16) rejected",
         !SessionPersistence::deserialize(with(good, "version", "65537"), out));
    TEST("in-range edit still accepted (tempo 96)",
         SessionPersistence::deserialize(with(good, "tempo_bpm", "96"), out) && out.tempo_bpm == 96);

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
