// Pa5X USB-MIDI port profile: exact names identify the ports, in the right
// direction only; anything else mentioning Pa5X is a candidate, never a match.

#include "midi/pa5x_profile.h"
#include <cstdio>

using namespace ai_arranger::midi::pa5x;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

int main() {
    std::printf("Test: Pa5X port profile\n");
    TEST("destination 'Pa5X SOUND' -> Sound", classify("Pa5X SOUND", false) == PortMatch::Sound);
    TEST("source 'Pa5X KEYBOARD' -> Keyboard", classify("Pa5X KEYBOARD", true) == PortMatch::Keyboard);
    TEST("'Pa5X SOUND' as a source is only a candidate", classify("Pa5X SOUND", true) == PortMatch::Candidate);
    TEST("'Pa5X KEYBOARD' as a destination is only a candidate", classify("Pa5X KEYBOARD", false) == PortMatch::Candidate);
    TEST("class-compliant style name -> candidate", classify("PA5X Port 1", false) == PortMatch::Candidate);
    TEST("lowercase exact name is not a match", classify("pa5x sound", false) == PortMatch::Candidate);
    TEST("unrelated device -> None", classify("FLOW 8", false) == PortMatch::None);
    TEST("Pa4X -> None", classify("Pa4X SOUND", false) == PortMatch::None);
    TEST("empty -> None", classify("", true) == PortMatch::None);
    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
