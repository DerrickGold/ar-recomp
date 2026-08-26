#include "runner_next.h"

/* Keep synchronized with the source boundary in runner.cmake. */
static const SrRunnerDescriptor k_runner = {
    SR_RUNNER_ABI_VERSION,
    "next",
    0u,
};

const SrRunnerDescriptor *sr_runner_descriptor(void) {
    return &k_runner;
}
