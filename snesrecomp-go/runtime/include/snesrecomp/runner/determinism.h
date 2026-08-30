/**
 * @file determinism.h
 * @brief Canonical runner-owned semantic-state digests.
 * @ingroup sr_runner_determinism
 */
#pragma once

#include "snesrecomp/runner/base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SR_DETERMINISM_SHA256_SIZE 32u
#define SR_DETERMINISM_SEMANTIC_SCHEMA_VERSION 2u

/** Requests the runner-owned hardware digest at an emulation-thread safe
 * point. `flags` is reserved and must be zero. Game-authored HLE/native
 * extension state is deliberately outside this schema. */
typedef struct SrSemanticDigestRequest {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    uint64_t reserved;
} SrSemanticDigestRequest;

#define SR_SEMANTIC_DIGEST_REQUEST_V2_SIZE                              \
    ((uint32_t)(offsetof(SrSemanticDigestRequest, reserved) +            \
                sizeof(((SrSemanticDigestRequest *)0)->reserved)))

/** Canonical digest of current runner-owned emulated hardware state. The
 * schema is independent from save-state traversal and excludes output
 * surfaces, PCM delivery queues, host mixing policy, diagnostics, and
 * game-authored extension save data. */
typedef struct SrSemanticDigestResult {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    uint64_t frame_counter;
    uint32_t schema_version;
    uint32_t reserved;
    uint8_t sha256[SR_DETERMINISM_SHA256_SIZE];
} SrSemanticDigestResult;

#define SR_SEMANTIC_DIGEST_RESULT_V2_SIZE                               \
    ((uint32_t)(offsetof(SrSemanticDigestResult, sha256) +               \
                sizeof(((SrSemanticDigestResult *)0)->sha256)))

#ifdef __cplusplus
}
#endif
