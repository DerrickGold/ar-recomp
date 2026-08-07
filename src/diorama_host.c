#include "diorama.h"

#include <string.h>

#include "actraiser_game.h"
#include "actraiser_rtl.h"
#include "host_display.h"
#include "host_input.h"
#include "settings.h"
#include "snes/ppu.h"

extern uint8 g_ram[0x20000];
extern Ppu *g_ppu;
extern bool g_diorama_frame_active;
extern uint8_t g_pixels[];
extern uint8_t g_hud_bg_pixels[];
extern uint8_t g_hud_obj_pixels[];

/* The single diorama gate: capture and rendering both call this definition,
 * so mode, renderer capability, and map-group policy cannot drift apart. */
bool Diorama_IsActiveThisFrame(void) {
  return g_settings.diorama_mode && Diorama_NewPpuCapable() &&
         ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup]);
}

/* The live room for the layer editor: the ($18,$19) pair the draw loop keys its
 * override lookup on (the DioramaLayerOrder_Resolve call in diorama.c), reported
 * only while a diorama room is actually running.
 *
 * The gate is Diorama_IsActiveThisFrame rather than just the map group, so the
 * editor reports a room exactly when the renderer would apply its overrides --
 * with the diorama off there is nothing on screen to compare, and offering to
 * author a room whose settings would not be used is worse than saying so. */
bool Diorama_LiveRoom(uint8_t *out_group, uint8_t *out_map) {
  if (!Diorama_IsActiveThisFrame()) return false;
  if (out_group) *out_group = g_ram[kActRaiserWram_MapGroup];
  if (out_map) *out_map = g_ram[kActRaiserWram_CurrentMap];
  return true;
}

/* The render margin widens while diorama mode is armed. Re-resolve geometry,
 * clear buffers at their new pitch, and rebind the PPU output surfaces. */
void Diorama_OnModeChanged(void) {
  if (!g_settings.diorama_mode) g_diorama_frame_active = false;
  if (!g_ppu) return;

  HostDisplay_ResolveVideoGeometry(false);
  memset(g_pixels, 0,
         kPpuSurfaceWidth * 4 * kHostDisplayFramebufferHeight);
  memset(g_hud_bg_pixels, 0,
         kPpuSurfaceWidth * 4 * kHostDisplayFramebufferHeight);
  memset(g_hud_obj_pixels, 0,
         kPpuSurfaceWidth * 4 * kHostDisplayFramebufferHeight);
  ActRaiser_RebindPpuOutputSurfaces();
  HostInput_RequestPausedRedraw();
  HostDisplay_InvalidatePresentHistory();
}
