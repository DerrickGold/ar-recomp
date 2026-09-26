#ifndef AR_PRESENTATION_TEXTURES_H
#define AR_PRESENTATION_TEXTURES_H
/* PresentationTextures: the GPU textures each frame's host surfaces are
 * uploaded into and the present stages sample: the base framebuffer, the
 * authentic comparison frame, the HUD BG/OBJ planes, the SIM semantic OBJ
 * atlas, the SIM 3D capture family and the diorama planes. Created once the
 * render device exists and destroyed just before it; in between the handles
 * only change across a render device reset. They are presentation resources,
 * not live game state, so the D6 boundary does not fence them off.
 * Phase: host (main thread). */

#include <stdbool.h>

#include "diorama/diorama_planes.h"
#include "render/render_types.h"
#include "sim/sim3d/sim3d_planes.h"

extern ArRenderTexture g_texture;
extern ArRenderTexture g_authentic_texture;
extern ArRenderTexture g_hud_bg_texture;
extern ArRenderTexture g_hud_obj_texture;
extern ArRenderTexture g_diorama_textures[kDioramaPlane_Count];
extern ArRenderTexture g_sim_obj_atlas_texture;
extern ArRenderTexture g_sim3d_layer_textures[kSim3DPlane_Count];
extern ArRenderTexture g_sim3d_flat_texture;
/* Whether the SIM 3D capture family and the object-billboard atlas exist.
 * Settings, input and the frame draw consult them before offering those
 * paths. */
extern bool g_sim3d_textures_ready;
extern bool g_sim3d_billboard_renderer_ready;

/* Creates every texture. Dies when an enabled SIM 3D feature cannot get the
 * textures it needs. */
void PresentationTextures_Create(void);
/* After SDL_EVENT_RENDER_DEVICE_RESET: re-creates the textures whose content
 * is not re-uploaded every frame (the zero-filled diorama planes). */
void PresentationTextures_HandleDeviceReset(void);
void PresentationTextures_Destroy(void);

/* Apply live settings here, beside the subsystem they configure. */
struct SettingDesc;
bool PresentationTextures_ValidateSetting(const struct SettingDesc *desc);

#endif  /* AR_PRESENTATION_TEXTURES_H */
