#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SR_RUNNER_ABI_VERSION 1u

typedef struct SrRunnerDescriptor {
    uint32_t abi_version;
    const char *variant;
    /* Must remain zero for the independently licensed runner. */
    uint32_t legacy_source_count;
} SrRunnerDescriptor;

/* Describes the selected replacement-runner boundary. */
const SrRunnerDescriptor *sr_runner_descriptor(void);

#ifdef __cplusplus
}
#endif
