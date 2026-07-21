#pragma once
#include <stdbool.h>
#include <SDL3/SDL.h>
#include "diorama_planes.h"

void Diorama_SeedCameraFromSettings(void);
void Diorama_AdjustCamera(float d_yaw, float d_pitch, float d_zoom);
void Diorama_ResetCamera(void);

float Diorama_DragRadPerPx(void);
float Diorama_ZoomStep(void);
bool Diorama_IsDragging(void);
void Diorama_SetDragging(bool dragging);

/* Draw the diorama scene, split into an upload phase (SDL_UpdateTexture only)
 * and a composite phase (SDL_RenderGeometry + camera projection), per M5's
 * buffer-ownership design: the caller can release pixel-buffer writers right
 * after Diorama_Upload returns, without waiting for Composite's vsync-bound
 * present. textures[]/pixels[] are kDioramaPlane_Count-sized, indexed by
 * kDioramaPlane_*; the caller fills [kDioramaPlane_Backdrop] with the
 * residual main framebuffer. Planes with a NULL texture or pixels are
 * skipped. visible_width replaces an internal Settings_VisibleWidth() call
 * (M5 D3 — present-time code must not re-derive live settings state). */
void Diorama_Upload(SDL_Texture *textures[], uint8_t *pixels[],
                    int snes_width, int snes_height);

/* M7 (§6): per-BG-layer UV shift for present-time scroll interpolation.
 * Indexed by SNES BG number (0=BG1..3=BG4); Diorama_Composite maps each
 * layer's plane to its BG internally and applies bg_du/bg_dv[that BG] to
 * the layer's mesh UVs. active=false (or a NULL pointer) disables
 * interpolation entirely for that composite call — e.g. first frame, a
 * BG-mode change between captures, or turbo (§6.4 edge cases). */
typedef struct DioramaScrollDelta {
  bool active;
  float bg_du[4];
  float bg_dv[4];
} DioramaScrollDelta;

bool Diorama_Composite(SDL_Renderer *renderer, int snes_width, int snes_height,
                       int active_pixel_aspect, bool ignore_aspect_ratio,
                       int visible_width, SDL_Texture *textures[],
                       uint8_t *pixels[],
                       const DioramaScrollDelta *scroll_delta);

void Diorama_FlushSettingsIfDirty(void);
