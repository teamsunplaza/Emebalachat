// Standalone host for the Plan-B (REQ-B009) §12.2 crash-isolation pin
// (TestTranslatePoolCrashIsolationReqB009). See the PLACEMENT NOTE at the top
// of translate_pool_test.inc for why this runs in its OWN process (the
// run_tests process cannot create the SECOND concurrent named-pipe client; a
// fresh process with no prior pipe state can).
//
// This main provides the minimal TEST_CHECK harness + the std:: facilities
// the .inc needs, includes the SAME translate_pool_test.inc the default suite
// uses, and runs ONLY the crash-isolation pin. Exit 0 on PASS, 1 on FAIL.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

static int g_failed_count = 0;

#define TEST_CHECK(cond, msg)                                                  \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++g_failed_count;                                                  \
            std::cout << "[FAIL] Line " << __LINE__ << ": " << (msg)           \
                      << " (" #cond ")" << std::endl;                          \
        }                                                                      \
    } while (0)

#include "../src/host_v2_translate_pool.hpp"
#include "../src/host_v2_worker_manager.hpp"
#include "translate_pool_test.inc"

int main() {
    TestTranslatePoolCrashIsolationReqB009();
    // The pin runs in its own short-lived process; the WorkerManager and its
    // handles are intentionally leaked (not destroyed) so the test never
    // fail-fasts in ~WorkerManager's GracefulStop pipe reads (the pipes are
    // fake-launcher stubs, not real workers — leaking them is harmless here,
    // and the OS reclaims all handles at process exit). The pin's ASSERTIONS
    // are the product signal; a clean process exit is the pass/fail carrier.
    std::cout << "========================================" << std::endl;
    std::cout << "Total Checks: (see above)" << std::endl;
    std::cout << "Failures:     " << g_failed_count << std::endl;
    std::cout << "========================================" << std::endl;
    if (g_failed_count) {
        std::cout << ">>> TEST FAILURES DETECTED! <<<" << std::endl;
        return 1;
    }
    std::cout << "ALL CHECKS PASSED" << std::endl;
    return 0;
}
