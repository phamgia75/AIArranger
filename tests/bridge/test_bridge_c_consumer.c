/* Gate 4 — C ABI bridge consumed from a pure C translation unit.
 *
 * The product host (Swift via a module map, or any C caller) sees only
 * include/aiarranger/aiarranger_engine.h. This file is compiled as C (C11,
 * -pedantic in CI via the compiler's defaults) so any C++-only construct leaking
 * into the header — references, bool without <stdbool.h>, namespaces, default
 * arguments, enum class — is a compile error, and linking it proves every entry
 * point has C linkage. */

#include "aiarranger/aiarranger_engine.h"

#include <stdio.h>

static int failures = 0;
#define CHECK(name, expr) do { \
    if (!(expr)) { fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { printf("  PASS: %s\n", name); } \
} while (0)

int main(void) {
    AiArrEngine* e;
    AiArrSnapshot snap;
    AiArrCapabilities caps;
    AiArrError err = AIARR_OK;
    int i;

    printf("Test: C ABI bridge from a pure C consumer\n");

    CHECK("contract version is non-zero", aiarr_contract_version() != 0u);

    e = aiarr_engine_create();
    CHECK("create returns a handle", e != NULL);
    if (!e) return 1;

    CHECK("capabilities OK", aiarr_engine_capabilities(e, &caps) == AIARR_OK);
    CHECK("capabilities carry the contract version",
          caps.contractVersion == aiarr_contract_version());
    CHECK("start OK", aiarr_engine_start(e) == AIARR_OK);
    CHECK("lifecycle Running", aiarr_engine_lifecycle_state(e) == AIARR_LIFECYCLE_RUNNING);
    CHECK("tempo OK", aiarr_engine_set_tempo(e, 100u) == AIARR_OK);
    CHECK("variation out of range -> UNSUPPORTED",
          aiarr_engine_set_variation(e, 7) == AIARR_ERR_UNSUPPORTED);
    CHECK("transport start OK", aiarr_engine_transport_start(e) == AIARR_OK);
    for (i = 0; i < 64; ++i) aiarr_engine_tick(e, 256u);
    CHECK("snapshot OK", aiarr_engine_snapshot(e, &snap) == AIARR_OK);
    CHECK("snapshot shows tempo 100", snap.tempoBpm == 100u);
    CHECK("snapshot lifecycle Running", snap.lifecycleState == AIARR_LIFECYCLE_RUNNING);
    CHECK("last_error OK", aiarr_engine_last_error(e, &err) == AIARR_OK && err == AIARR_OK);
    CHECK("NULL out -> NULL_ARGUMENT",
          aiarr_engine_snapshot(e, NULL) == AIARR_ERR_NULL_ARGUMENT);
    CHECK("panic OK", aiarr_engine_panic(e) == AIARR_OK);
    CHECK("stop OK", aiarr_engine_stop(e) == AIARR_OK);
    aiarr_engine_destroy(e);
    aiarr_engine_destroy(NULL);

    CHECK("NULL handle -> NULL_ARGUMENT", aiarr_engine_start(NULL) == AIARR_ERR_NULL_ARGUMENT);

    printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
