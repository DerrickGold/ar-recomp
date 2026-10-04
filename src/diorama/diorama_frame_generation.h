#ifndef AR_DIORAMA_FRAME_GENERATION_H
#define AR_DIORAMA_FRAME_GENERATION_H
/* DioramaFrameGeneration: captures the action planes after each emulation
 * tick and builds motion fields between consecutive captures, so frame
 * generation can synthesize in-between frames for the diorama view.
 * Phase: present (FrameSlot and already-uploaded textures only).
 * Tests: tests/diorama_frame_generation_test.c */

#include <stddef.h>
#include <stdint.h>

#include "diorama_planes.h"
#include "diorama_skybox_source.h"
#include "render/render_device.h"

typedef struct FrameSlot FrameSlot;
enum { kDioramaFrameGenerationSkybox = kDioramaPlane_Count };

/* Capture the native action planes for one completed emulation tick and build
 * a reusable motion field for every continuous previous/current pair.
 * `changed_plane_mask` is the subset whose synchronized upload changed; an
 * unchanged plane with a valid retained endpoint bypasses its CPU copy,
 * endpoint refresh, motion analysis, and synthesis. `source_textures` are the
 * compositor textures Diorama_Upload has already filled this presentation;
 * endpoints are copied from them on the GPU rather than uploaded again. No live game state is read:
 * the FrameSlot and pixel pointers are the complete input. */
void DioramaFrameGeneration_Capture(
    ArRenderDevice *device, const FrameSlot *slot,
    const ArRenderTexture source_textures[kDioramaPlane_Count],
    const uint8_t *const pixels[kDioramaPlane_Count],
    const size_t pitch_bytes[kDioramaPlane_Count],
    uint32_t changed_plane_mask);

/* The independent finite-world skybox uses the same endpoint/motion pipeline
 * but is not a gameplay plane or part of its priority mask. */
void DioramaFrameGeneration_CaptureWithSkybox(
    ArRenderDevice *device, const FrameSlot *slot,
    const ArRenderTexture source_textures[kDioramaPlane_Count],
    const uint8_t *const pixels[kDioramaPlane_Count],
    const size_t pitch_bytes[kDioramaPlane_Count],
    uint32_t changed_plane_mask, ArRenderTexture skybox_texture,
    bool skybox_changed);

/* Join optional native preparation before changing/destroying any captured
 * source texture. Waits for CPU submission only, never GPU completion. Prepare
 * and Reset also join before consuming or invalidating a pending endpoint. */
void DioramaFrameGeneration_FinishCapture(void);

/* Resolve the plane textures for one host present. `current_textures` are the
 * exact 60 Hz endpoints uploaded by Diorama_Upload. Valid generated planes are
 * rendered into private targets and substituted in `resolved_textures`; every
 * unsupported/discontinuous plane remains the exact current texture. */
uint32_t DioramaFrameGeneration_Prepare(
    ArRenderDevice *device, const FrameSlot *slot, float alpha,
    const ArRenderTexture current_textures[kDioramaPlane_Count],
    uint32_t current_plane_mask,
    ArRenderTexture resolved_textures[kDioramaPlane_Count]);

uint32_t DioramaFrameGeneration_PrepareWithSkybox(
    ArRenderDevice *device, const FrameSlot *slot, float alpha,
    const ArRenderTexture current_textures[kDioramaPlane_Count],
    uint32_t current_plane_mask,
    ArRenderTexture resolved_textures[kDioramaPlane_Count],
    ArRenderTexture skybox_texture, ArRenderTexture *resolved_skybox);

/* Current-capture coordinates to the last successfully prepared texture.
 * Uniform background motion, including kDioramaFrameGenerationSkybox, has one
 * offset; ungenerated/OBJ planes and reset state return zero. Query immediately
 * after Prepare on the presenter. */
/* Diagnostic mask of backgrounds synthesized by the optional compute path.
 * Resident projection reports candidate planes; motion rejection is resolved
 * on GPU by returning the current endpoint, without downloading confidence. */
uint32_t DioramaFrameGeneration_GpuPlaneMask(void);
/* All successfully synthesized planes from the most recent Prepare, including
 * the CPU-analysis path. Diagnostics must not count a skipped pair as parity. */
uint32_t DioramaFrameGeneration_GeneratedPlaneMask(void);
/* Presenter-thread diagnostic of the projection fence wait in the last
 * Prepare. Dormant unless explicitly enabled; never performs an extra wait. */
void DioramaFrameGeneration_EnableWaitTrace(bool enabled);
uint64_t DioramaFrameGeneration_LastWaitNs(void);

ArRenderPointF DioramaFrameGeneration_PlaneOffset(int plane);

/* Resident source-projection path. The presenter selects the shared action
 * policy before Capture; unavailable backends keep the reference path.
 * Active is latched per capture, so its consumers and metadata policy agree. */
typedef struct ActionEffectSourceBatch ActionEffectSourceBatch;
typedef struct ActionMoonlightOcclusion ActionMoonlightOcclusion;
typedef struct DioramaProjection DioramaProjection;
void DioramaFrameGeneration_AllowSourceProjection(bool allowed);
/* Capability check before dropping CPU background copies. Presenter only. */
bool DioramaFrameGeneration_UsesGpuAnalysis(ArRenderDevice *, const FrameSlot *);
bool DioramaFrameGeneration_SourceProjectionActive(void);
bool DioramaFrameGeneration_SourceProjectionFailed(void);
/* Exceptional failure latch, excluding a deliberately selected CPU policy. */
bool DioramaFrameGeneration_SourceProjectionFallback(void);
void DioramaFrameGeneration_RecoverSourceProjection(ArRenderDevice *);
unsigned DioramaFrameGeneration_MetadataReadbackCount(void);
bool DioramaFrameGeneration_DrawSource(ArRenderDevice *, const ActionEffectSourceBatch *,
    const DioramaProjection *, const ActionMoonlightOcclusion *, ArRenderBlendMode, float);
bool DioramaFrameGeneration_DrawSkybox(ArRenderDevice *, ArRenderTexture,
    const DioramaSkyboxSourceDraw *);

/* Drop endpoint history and backend resources. Reset is safe after a render
 * reset event; Shutdown is also used during orderly teardown. */
void DioramaFrameGeneration_Reset(void);
void DioramaFrameGeneration_Shutdown(void);

#endif  /* AR_DIORAMA_FRAME_GENERATION_H */
