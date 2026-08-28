#ifndef DIORAMA_FRAME_GENERATION_H
#define DIORAMA_FRAME_GENERATION_H

#include <stddef.h>
#include <stdint.h>

#include "diorama_planes.h"
#include "render/render_device.h"

typedef struct FrameSlot FrameSlot;

/* Capture the native action planes for one completed emulation tick and build
 * a reusable motion field for every continuous previous/current pair.
 * `changed_plane_mask` is the subset whose synchronized upload changed; an
 * unchanged plane with a valid retained endpoint bypasses its CPU copy,
 * private upload, motion analysis, and synthesis. No live game state is read:
 * the FrameSlot and pixel pointers are the complete input. */
void DioramaFrameGeneration_Capture(
    ArRenderDevice *device, const FrameSlot *slot,
    const uint8_t *const pixels[kDioramaPlane_Count],
    const size_t pitch_bytes[kDioramaPlane_Count],
    uint32_t changed_plane_mask);

/* Resolve the plane textures for one host present. `current_textures` are the
 * exact 60 Hz endpoints uploaded by Diorama_Upload. Valid generated planes are
 * rendered into private targets and substituted in `resolved_textures`; every
 * unsupported/discontinuous plane remains the exact current texture. */
uint32_t DioramaFrameGeneration_Prepare(
    ArRenderDevice *device, const FrameSlot *slot, float alpha,
    const ArRenderTexture current_textures[kDioramaPlane_Count],
    uint32_t current_plane_mask,
    ArRenderTexture resolved_textures[kDioramaPlane_Count]);

/* Drop endpoint history and backend resources. Reset is safe after a render
 * reset event; Shutdown is also used during orderly teardown. */
void DioramaFrameGeneration_Reset(void);
void DioramaFrameGeneration_Shutdown(void);

#endif  /* DIORAMA_FRAME_GENERATION_H */
