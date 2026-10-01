#ifndef AR_ACTION_DECORATION_PASS_H
#define AR_ACTION_DECORATION_PASS_H
#include "action_effects.h"
#include "render/render_types.h"
#include "diorama/diorama_planes.h"
/* Attachment is draw order, not the source camera. For example, shoreline
 * mist uses BG1 coordinates but belongs after BG2-high. Flat composition uses
 * winner masks; Diorama instead relies on later foreground/actor planes. */
typedef struct ActionDecorationPass {
  uint8_t layer;
  int attachment;
  int mask_plane; /* -1: Diorama-only atmosphere, no flat winner mask. */
  ArRenderBlendMode blend;
  bool diorama_lighting;
  bool finite_plane_only;
  const char *label;
} ActionDecorationPass;

/* Order is also the established flat composition order. */
static const ActionDecorationPass kDecorationPasses[] = {
  {kActionEffectRenderLayer_Bg1Light, SR_PPU_OVERLAY_BG1, SR_PPU_OVERLAY_BG1,
   kArRenderBlendMode_Light, true, false, "scenery receiver illumination"},
  {kActionEffectRenderLayer_Bg1Plane, SR_PPU_OVERLAY_BG1, SR_PPU_OVERLAY_BG1,
   kArRenderBlendMode_Add, true, false, "BG1-local decoration"},
  {kActionEffectRenderLayer_Bg1HighPlane, kDioramaPlane_Bg1Hi, SR_PPU_OVERLAY_BG1,
   kArRenderBlendMode_Add, true, false, "BG1-high lava decoration"},
  {kActionEffectRenderLayer_Bg2Plane, SR_PPU_OVERLAY_BG2, SR_PPU_OVERLAY_BG2,
   kArRenderBlendMode_Add, true, false, "BG2-local decoration"},
  {kActionEffectRenderLayer_Bg2HighPlane, kDioramaPlane_Bg2Hi, SR_PPU_OVERLAY_BG2,
   kArRenderBlendMode_Add, true, false, "BG2-high water decoration"},
  {kActionEffectRenderLayer_Bg2Alpha, SR_PPU_OVERLAY_BG2, SR_PPU_OVERLAY_BG2,
   kArRenderBlendMode_Alpha, true, false, "BG2 alpha atmosphere"},
  {kActionEffectRenderLayer_Bg2HighAlpha, kDioramaPlane_Bg2Hi, SR_PPU_OVERLAY_BG2,
   kArRenderBlendMode_Alpha, false, false, "BG2-high lake mist"},
  {kActionEffectRenderLayer_Bg1Mist, SR_PPU_OVERLAY_BG1, SR_PPU_OVERLAY_BG1,
   kArRenderBlendMode_Alpha, false, false, "temple ground mist"},
  {kActionEffectRenderLayer_Atmosphere, SR_PPU_OVERLAY_BG2, -1,
   kArRenderBlendMode_Alpha, true, true, "waterfall bottom atmosphere"},
};


#endif
