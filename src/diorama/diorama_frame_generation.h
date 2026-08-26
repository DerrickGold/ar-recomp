#ifndef DIORAMA_FRAME_GENERATION_H
#define DIORAMA_FRAME_GENERATION_H

#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdint.h>

#include "diorama_planes.h"

typedef struct FrameSlot FrameSlot;

/* Capture the native action planes for one completed emulation tick and build
 * a reusable motion field for every continuous previous/current pair.
 * `changed_plane_mask` is the subset whose synchronized upload changed; an
 * unchanged plane with a valid retained endpoint bypasses its CPU copy,
 * private upload, motion analysis, and synthesis. No live game state is read:
 * the FrameSlot and pixel pointers are the complete input. */
void DioramaFrameGeneration_Capture(
    SDL_Renderer *renderer, const FrameSlot *slot,
    const uint8_t *const pixels[kDioramaPlane_Count],
    const size_t pitch_bytes[kDioramaPlane_Count],
    uint32_t changed_plane_mask);

/* Resolve the plane textures for one host present. `current_textures` are the
 * exact 60 Hz endpoints uploaded by Diorama_Upload. Valid generated planes are
 * rendered into private targets and substituted in `resolved_textures`; every
 * unsupported/discontinuous plane remains the exact current texture. */
uint32_t DioramaFrameGeneration_Prepare(
    SDL_Renderer *renderer, const FrameSlot *slot, float alpha,
    SDL_Texture *const current_textures[kDioramaPlane_Count],
    uint32_t current_plane_mask,
    SDL_Texture *resolved_textures[kDioramaPlane_Count]);

/* Drop endpoint history and renderer resources. Reset is safe after either SDL
 * render reset event; Shutdown is also used during orderly teardown. */
void DioramaFrameGeneration_Reset(void);
void DioramaFrameGeneration_Shutdown(void);

#endif  /* DIORAMA_FRAME_GENERATION_H */
