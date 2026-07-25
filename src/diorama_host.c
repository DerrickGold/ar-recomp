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

/* The render margin widens while diorama mode is armed. Re-resolve geometry,
 * clear buffers at their new pitch, and rebind the PPU output surfaces. */
void Diorama_OnModeChanged(void) {
  if (!g_settings.diorama_mode) g_diorama_frame_active = false;
  if (!g_ppu) return;

  HostDisplay_ResolveVideoGeometry(false);
  memset(g_pixels, 0,
         kPpuBufWidth * 4 * kHostDisplayFramebufferHeight);
  memset(g_hud_bg_pixels, 0,
         kPpuBufWidth * 4 * kHostDisplayFramebufferHeight);
  memset(g_hud_obj_pixels, 0,
         kPpuBufWidth * 4 * kHostDisplayFramebufferHeight);
  ActRaiser_RebindPpuOutputSurfaces();
  HostInput_RequestPausedRedraw();
  HostDisplay_InvalidatePresentHistory();
}
