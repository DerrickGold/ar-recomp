/**
 * @file determinism.h
 * @brief Canonical semantic-state and completed-presentation digests.
 * @ingroup sr_runner_determinism
 */
#pragma once

#include "snesrecomp/runner/base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SR_DETERMINISM_SHA256_SIZE 32u
#define SR_DETERMINISM_SEMANTIC_SCHEMA_VERSION 1u
#define SR_DETERMINISM_PRESENTATION_SCHEMA_VERSION 1u

#define SR_DETERMINISM_DIGEST_SEMANTIC UINT32_C(0x00000001)
#define SR_DETERMINISM_DIGEST_PRESENTATION UINT32_C(0x00000002)
#define SR_DETERMINISM_DIGEST_FLAGS_SUPPORTED                            \
    (SR_DETERMINISM_DIGEST_SEMANTIC |                                   \
     SR_DETERMINISM_DIGEST_PRESENTATION)

typedef uint32_t SrDeterminismPixelFormat;
enum {
    /** Canonical byte stream is one little-endian 0x00RRGGBB word per pixel. */
    SR_DETERMINISM_PIXEL_XRGB8888_LE = 1u
};

typedef struct SrDeterminismDigestRequest {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    uint64_t reserved;
} SrDeterminismDigestRequest;

#define SR_DETERMINISM_DIGEST_REQUEST_V2_SIZE                           \
    ((uint32_t)(offsetof(SrDeterminismDigestRequest, reserved) +         \
                sizeof(((SrDeterminismDigestRequest *)0)->reserved)))

/** Hashes and the exact schema/geometry that produced them. A requested
 * component sets its matching valid flag only when it was produced. Semantic
 * state is current at the call safe point. Presentation is retrospective and
 * remains unavailable until runner-owned scanout with
 * `SR_PPU_SCANOUT_CAPTURE_PRESENTATION_DIGEST` has completed successfully.
 * `SR_RESULT_OK` means every requested component is valid. */
typedef struct SrDeterminismDigestResult {
    uint32_t struct_size;
    uint32_t valid_flags;
    uint64_t lifetime_generation;
    uint64_t frame_counter;
    uint64_t presentation_frame_counter;
    uint32_t semantic_schema_version;
    uint32_t presentation_schema_version;
    uint32_t presentation_width_pixels;
    uint32_t presentation_height_pixels;
    uint16_t presentation_margin_left_pixels;
    uint16_t presentation_margin_right_pixels;
    uint16_t presentation_margin_top_pixels;
    uint16_t presentation_margin_bottom_pixels;
    SrDeterminismPixelFormat presentation_pixel_format;
    uint32_t reserved;
    uint8_t semantic_sha256[SR_DETERMINISM_SHA256_SIZE];
    uint8_t presentation_sha256[SR_DETERMINISM_SHA256_SIZE];
} SrDeterminismDigestResult;

#define SR_DETERMINISM_DIGEST_RESULT_V2_SIZE                            \
    ((uint32_t)(offsetof(SrDeterminismDigestResult, presentation_sha256) + \
                sizeof(((SrDeterminismDigestResult *)0)                 \
                           ->presentation_sha256)))

#ifdef __cplusplus
}
#endif
