#ifndef DIORAMA_ROM_SKYBOX_RESOURCE_H
#define DIORAMA_ROM_SKYBOX_RESOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "render/render_device.h"

/* Persistent, backend-neutral texture cache for authored ROM skyboxes. The
 * decoded pixels remain game-owned; only opaque texture handles cross the
 * rendering boundary. */
bool DioramaRomSkyboxResource_Init(const uint8_t *rom_data, size_t rom_size);

/* Returns an invalid handle when the source cannot be decoded or uploaded.
 * That is an optional omission and callers may use captured BG2 instead.
 * `state_restore_failed` distinguishes a lost scoped-target restore, after
 * which the current frame must stop because subsequent draw ownership is
 * unknown. */
ArRenderTexture DioramaRomSkyboxResource_Resolve(
    ArRenderDevice *device, int source,
    bool transparent_fill_configured, uint32_t transparent_fill_argb,
    bool *state_restore_failed);

/* Drop device-owned handles while retaining decoded source pixels. */
void DioramaRomSkyboxResource_Reset(ArRenderDevice *device);

#endif /* DIORAMA_ROM_SKYBOX_RESOURCE_H */
