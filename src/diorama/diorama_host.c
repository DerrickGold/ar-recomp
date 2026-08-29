#include "diorama.h"

#include <string.h>

#include "actraiser_game.h"
#include "actraiser_rtl.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game_runtime.h"
#include "diorama_layer_order.h"
#include "host/host_display.h"
#include "host/host_input.h"
#include "snesrecomp/runner.h"
#include "settings.h"

extern bool g_diorama_frame_active;
extern uint8_t g_pixels[];
extern uint8_t g_hud_bg_pixels[];
extern uint8_t g_hud_obj_pixels[];

/* The single diorama gate: capture and rendering both call this definition, so
 * mode and map-group policy cannot drift apart. The renderer capability that
 * used to be a third term here is gone: the PPU has one path now, so it was
 * always true. */
bool Diorama_IsActiveThisFrame(void) {
  return g_settings.diorama_mode &&
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
static uint8_t s_layer_section_group;
static uint8_t s_layer_section_map;
static uint8_t s_layer_section;

void Diorama_PublishLiveLayerSection(uint8_t map_group, uint8_t map_number,
                                     uint8_t section) {
  s_layer_section_group = map_group;
  s_layer_section_map = map_number;
  s_layer_section = section;
}

bool Diorama_LiveRoom(uint8_t *out_group, uint8_t *out_map,
                      uint8_t *out_section) {
  if (!Diorama_IsActiveThisFrame()) return false;
  const uint8_t group = g_ram[kActRaiserWram_MapGroup];
  const uint8_t map = g_ram[kActRaiserWram_CurrentMap];
  if (out_group) *out_group = group;
  if (out_map) *out_map = map;
  if (out_section) {
    *out_section = s_layer_section_group == group &&
                       s_layer_section_map == map
        ? s_layer_section : kDioramaLayerSection_Room;
  }
  return true;
}

/* The render margin widens while diorama mode is armed. Re-resolve geometry,
 * clear buffers at their new pitch, and rebind the PPU output surfaces. */
void Diorama_OnModeChanged(void) {
  if (!g_settings.diorama_mode) g_diorama_frame_active = false;
  if (!RtlGameRunner()) return;

  HostDisplay_ResolveVideoGeometry(false);
  memset(g_pixels, 0,
         SR_PPU_SURFACE_MAX_WIDTH * 4 * kHostDisplayFramebufferHeight);
  memset(g_hud_bg_pixels, 0,
         SR_PPU_SURFACE_MAX_WIDTH * 4 * kHostDisplayFramebufferHeight);
  memset(g_hud_obj_pixels, 0,
         SR_PPU_SURFACE_MAX_WIDTH * 4 * kHostDisplayFramebufferHeight);
  ActRaiser_RebindPpuOutputSurfaces();
  HostInput_RequestPausedRedraw();
  HostDisplay_InvalidatePresentHistory();
}
