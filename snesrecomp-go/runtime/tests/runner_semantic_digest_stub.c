#include "runner_internal.h"

#include <string.h>

/* Focused runner compositions that omit CPU/APU hardware install this domain
 * stub instead of fabricating private save-state symbols. */
SrResult sr_runner_query_semantic_digest(
        SrRunnerHandle *runner, const SrSemanticDigestRequest *request,
        SrSemanticDigestResult *out_result) {
    (void)runner;
    if (request == NULL || out_result == NULL ||
        request->struct_size < SR_SEMANTIC_DIGEST_REQUEST_V2_SIZE ||
        out_result->struct_size < SR_SEMANTIC_DIGEST_RESULT_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_result, 0, SR_SEMANTIC_DIGEST_RESULT_V2_SIZE);
    out_result->struct_size = SR_SEMANTIC_DIGEST_RESULT_V2_SIZE;
    return SR_RESULT_UNAVAILABLE;
}
