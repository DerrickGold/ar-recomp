#ifndef AR_DIORAMA_ASTRAL_RIFT_H
#define AR_DIORAMA_ASTRAL_RIFT_H

#include "diorama.h"

enum { kDioramaAstralFloorWorldY = 208 };

/* A presentation-only replacement for the two native nebula planes in 07:08.
 * The host owns this frame for the duration of Composite. No gameplay, wall
 * clock, SDL or live settings are read by the shared native/WASM renderer. */
typedef struct DioramaAstralRiftFrame {
  ArRenderDevice *device;
  float yaw, pitch;
  uint16_t tick; /* Pausable action visual clock, including its natural wrap. */
  uint8_t inidisp; /* Native master brightness/forced blank. */
  /* Composite publishes this before invoking the backdrop. The floor shares
   * the player's exact OBJ projection rather than guessing a screen row. */
  const DioramaProjection *projection;
  float floor_capture_x, floor_capture_y;
} DioramaAstralRiftFrame;

/* Decode/upload immutable artwork at boot or editor scene entry. Draws
 * only rebuild bounded geometry; no readback or texture upload per frame. */
bool DioramaAstralRift_Prepare(ArRenderDevice *device);
void DioramaAstralRift_Reset(ArRenderDevice *device);

/* Presentation may omit these native planes (and the native skybox) only
 * after all replacement art is ready. Zero preserves native fallback. This
 * does not change the PPU capture, native color math, or object planes. */
uint32_t DioramaAstralRift_ReplacedPlanes(ArRenderDevice *device, bool enabled,
    unsigned map_group, unsigned map_number, unsigned layer_section);

/* On success, overrides only this draw's copied options and installs the
 * background callback. Ineligible scenes/resource failures stay authentic.
 * Call only from a Diorama presentation path, never flat mode. */
bool DioramaAstralRift_Attach(bool enabled, DioramaScene *scene,
    DioramaRenderOptions *options, DioramaAstralRiftFrame *frame);

#endif
