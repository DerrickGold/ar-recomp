/* Presentation-aware replacement for the action camera at $02:B091.
 *
 * The ROM clamps BG1's camera for a 256x225 viewport. Wide presentation used
 * to leave that camera untouched and shorten whichever synthetic margin met a
 * finite world edge. That exposed the playfield cutoff inside the wider canvas.
 * This replacement keeps the complete requested playfield view inside the live
 * $2E/$30 room dimensions whenever it fits. The original clamp remains the
 * fallback for a room smaller than that view, every authentic/raw mode, and
 * authored scenes whose canonical background plan has no finite BG1 canvas.
 * BG2 parallax and the player-relative tail still run through the recompiled
 * ROM helpers, after the corrected BG1 camera has become canonical and the
 * requested delta has been reconciled with the motion that actually fit. */

#include "action/action_camera_bounds.h"
#include "actraiser_action_bg.h"
#include "actraiser_game.h"
#include "cpu_state.h"
#include "diorama/diorama.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>

extern bool g_ws_active;
extern int g_ws_extra;
extern struct Ppu *g_ppu;

extern RecompReturn bank_02_B9D5_M0X0(CpuState *cpu);
extern RecompReturn bank_02_BA0B_M0X0(CpuState *cpu);
extern RecompReturn bank_00_A1B0_M0X0(CpuState *cpu);

enum {
  kActionCameraDeltaX = 0x007C,
  kActionCameraDeltaY = 0x007E,
  kActionCameraParallaxMask = 0x008E,
  kActionCameraStripRequests = 0x0093,
  kActionCameraHorizontalStrip = 0x0080,
  kActionCameraVerticalStrip = 0x0040,
  kActionBg2HorizontalStrip = 0x0020,
  kActionBg2VerticalStrip = 0x0010,
  kActionVerticalViewportHeight = 0x00E1,
};

typedef RecompReturn (*ActionCameraRomHelper)(CpuState *cpu);

static inline uint16_t ActionCamera_Read16(CpuState *cpu, uint16_t offset) {
  const uint16_t address = (uint16_t)(cpu->D + offset);
  return (uint16_t)(g_ram[address] | (g_ram[(uint16_t)(address + 1)] << 8));
}

static inline void ActionCamera_Write16(
    CpuState *cpu, uint16_t offset, uint16_t value) {
  const uint16_t address = (uint16_t)(cpu->D + offset);
  g_ram[address] = (uint8_t)value;
  g_ram[(uint16_t)(address + 1)] = (uint8_t)(value >> 8);
}

static bool ActionCamera_ResolvePlayfield(ActionBgLayerPlan *playfield) {
  if (playfield) *playfield = (ActionBgLayerPlan){ 0 };
  if (!g_ws_active || !g_settings.ws_action ||
      g_settings.display_mode == kDisplayMode_43 ||
      g_settings.display_mode == kDisplayMode_WideRaw ||
      !ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup]) ||
      !ActRaiserActionBg_HleEnabled() || !playfield)
    return false;

  /* Consume the same canonical classification as the renderer. In
   * particular, Death Heim's hub/final authored scenes set no finite canvas;
   * treating every action group as a streamed playfield would shift those
   * cameras even though their presentation intentionally wraps/clamps. The
   * tuner cannot alter roles, sources, or canvas ownership. The camera uses
   * the baked canonical extent; a session draft remains presentation-only
   * until its exported values are transcribed into that catalogue. */
  ActionBgPlan plan;
  ActionBgPresentationPolicy presentation;
  if (!ActRaiserActionBg_BuildPlan(
          g_ram, kActRaiserWramSize, g_ppu,
          g_settings.ws_bg2_padding, &plan, &presentation) ||
      ActionBgPlan_CanvasOwner(&plan) != 0)
    return false;
  *playfield = plan.layer[0];
  return true;
}

static int ActionCamera_LimitMargin(
    int available, ActionBgExtentMode mode, uint16_t fixed) {
  if (available <= 0) return 0;
  return mode == kActionBgExtent_Fixed && (int)fixed < available
      ? (int)fixed : available;
}

static void ActionCamera_RequestStrip(
    CpuState *cpu, uint16_t old_camera, uint16_t new_camera,
    uint16_t request) {
  if (!((old_camera ^ new_camera) & 0x0010)) return;
  ActionCamera_Write16(
      cpu, kActionCameraStripRequests,
      (uint16_t)(ActionCamera_Read16(cpu, kActionCameraStripRequests) |
                 request));
}

static RecompReturn ActionCamera_CallRts(
    CpuState *cpu, uint16_t return_address, ActionCameraRomHelper helper) {
  const uint16_t call_stack = cpu->S;
  cpu_write8(cpu, 0x00, cpu->S, (uint8_t)(return_address >> 8));
  cpu->S--;
  cpu_write8(cpu, 0x00, cpu->S, (uint8_t)return_address);
  cpu->S--;
  cpu->host_return_valid = 1;
  const RecompReturn result = helper(cpu);
  cpu->S = call_stack;
  return result;
}

static RecompReturn ActionCamera_CallPlayerTail(CpuState *cpu) {
  const uint16_t call_stack = cpu->S;
  const uint8_t saved_pb = cpu->PB;
  cpu_write8(cpu, 0x00, cpu->S, saved_pb);
  cpu->S--;
  cpu_write8(cpu, 0x00, cpu->S, 0xB1);
  cpu->S--;
  cpu_write8(cpu, 0x00, cpu->S, 0x24);
  cpu->S--;
  cpu->PB = 0x00;
  cpu->host_return_valid = 1;
  const RecompReturn result = bank_00_A1B0_M0X0(cpu);
  cpu->PB = saved_pb;
  cpu->S = call_stack;
  return result;
}

static bool ActionCamera_DebugEnabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *value = getenv("AR_WS_ACTION_CAMDBG");
    enabled = value && value[0] && value[0] != '0';
  }
  return enabled != 0;
}

RecompReturn ActRaiser_UpdateActionCamera(CpuState *cpu) {
  const uint8_t saved_p = cpu->P;
  cpu_write8(cpu, 0x00, cpu->S, saved_p);
  cpu->S--;
  cpu->P = (uint8_t)(cpu->P & ~0x20);
  cpu_p_to_mirrors(cpu);

  ActionBgLayerPlan playfield;
  const bool wide = ActionCamera_ResolvePlayfield(&playfield);
  const int horizontal_before = wide
      ? ActionCamera_LimitMargin(
            g_ws_extra, playfield.horizontal_extent.mode,
            playfield.horizontal_extent.left)
      : 0;
  const int horizontal_after = wide
      ? ActionCamera_LimitMargin(
            g_ws_extra, playfield.horizontal_extent.mode,
            playfield.horizontal_extent.right)
      : 0;
  const int vertical_available =
      wide && Diorama_IsActiveThisFrame()
          ? g_settings.diorama_vertical_extend
          : 0;
  const int vertical_before = ActionCamera_LimitMargin(
      vertical_available, playfield.vertical_extent.mode,
      playfield.vertical_extent.top);
  const int vertical_after = ActionCamera_LimitMargin(
      vertical_available, playfield.vertical_extent.mode,
      playfield.vertical_extent.bottom);
  const uint16_t old_x = ActionCamera_Read16(
      cpu, kActRaiserWram_Bg1CameraX);
  const uint16_t old_y = ActionCamera_Read16(
      cpu, kActRaiserWram_Bg1CameraY);
  const int16_t requested_delta_x =
      (int16_t)ActionCamera_Read16(cpu, kActionCameraDeltaX);
  const int16_t requested_delta_y =
      (int16_t)ActionCamera_Read16(cpu, kActionCameraDeltaY);
  ActionCameraAxisBounds horizontal_bounds = { 0 };
  ActionCameraAxisBounds vertical_bounds = { 0 };
  const uint16_t camera_x = ActionCameraAxisBounds_UpdateCamera(
      old_x, requested_delta_x,
      ActionCamera_Read16(cpu, kActRaiserWram_Bg1Width),
      kActRaiserAuthenticWidth, horizontal_before, horizontal_after,
      &horizontal_bounds);
  const uint16_t camera_y = ActionCameraAxisBounds_UpdateCamera(
      old_y, requested_delta_y,
      ActionCamera_Read16(cpu, kActRaiserWram_Bg1Height),
      kActionVerticalViewportHeight, vertical_before, vertical_after,
      &vertical_bounds);
  const int16_t effective_delta_x =
      ActionCameraAxisBounds_EffectiveDelta(
          &horizontal_bounds, old_x, camera_x, requested_delta_x);
  const int16_t effective_delta_y =
      ActionCameraAxisBounds_EffectiveDelta(
          &vertical_bounds, old_y, camera_y, requested_delta_y);
  ActionCamera_Write16(
      cpu, kActionCameraDeltaX, (uint16_t)effective_delta_x);
  ActionCamera_Write16(
      cpu, kActionCameraDeltaY, (uint16_t)effective_delta_y);
  ActionCamera_Write16(cpu, kActRaiserWram_Bg1CameraX, camera_x);
  ActionCamera_Write16(cpu, kActRaiserWram_Bg1CameraY, camera_y);
  ActionCamera_RequestStrip(
      cpu, old_x, camera_x, kActionCameraHorizontalStrip);
  ActionCamera_RequestStrip(
      cpu, old_y, camera_y, kActionCameraVerticalStrip);

  cpu->X = kActRaiserBgLayerStateStride;
  if (!(ActionCamera_Read16(cpu, kActionCameraParallaxMask) & 0x0001)) {
    cpu->A = kActionBg2HorizontalStrip;
    const RecompReturn result = ActionCamera_CallRts(
        cpu, 0xB113, bank_02_B9D5_M0X0);
    if (result != RECOMP_RETURN_NORMAL) return result;
  }
  if (!(ActionCamera_Read16(cpu, kActionCameraParallaxMask) & 0x0002)) {
    cpu->A = kActionBg2VerticalStrip;
    const RecompReturn result = ActionCamera_CallRts(
        cpu, 0xB120, bank_02_BA0B_M0X0);
    if (result != RECOMP_RETURN_NORMAL) return result;
  }
  {
    const RecompReturn result = ActionCamera_CallPlayerTail(cpu);
    if (result != RECOMP_RETURN_NORMAL) return result;
  }

  if (wide && ActionCamera_DebugEnabled()) {
    static uint8_t last_group = 0xFF;
    static uint8_t last_map = 0xFF;
    static uint16_t last_x = UINT16_MAX;
    static uint16_t last_y = UINT16_MAX;
    if (camera_x != last_x || camera_y != last_y ||
        g_ram[kActRaiserWram_MapGroup] != last_group ||
        g_ram[kActRaiserWram_CurrentMap] != last_map) {
      fprintf(stderr,
              "[ws-action-camera] room=%02x/%02x camera=%u,%u "
              "bounds=%u..%u,%u..%u margins=%d/%d,%d/%d fit=%d,%d "
              "delta=%d->%d,%d->%d\n",
              g_ram[kActRaiserWram_MapGroup],
              g_ram[kActRaiserWram_CurrentMap],
              camera_x, camera_y,
              horizontal_bounds.minimum, horizontal_bounds.maximum,
              vertical_bounds.minimum, vertical_bounds.maximum,
              horizontal_before, horizontal_after,
              vertical_before, vertical_after,
              horizontal_bounds.includes_requested_margins,
              vertical_bounds.includes_requested_margins,
              requested_delta_x, effective_delta_x,
              requested_delta_y, effective_delta_y);
      last_group = g_ram[kActRaiserWram_MapGroup];
      last_map = g_ram[kActRaiserWram_CurrentMap];
      last_x = camera_x;
      last_y = camera_y;
    }
  }

  cpu->S++;
  cpu->P = cpu_read8(cpu, 0x00, cpu->S);
  cpu_p_to_mirrors(cpu);
  if (cpu->x_flag) {
    cpu->X &= 0x00FF;
    cpu->Y &= 0x00FF;
  }
  cpu->S = (uint16_t)(cpu->S + 3);
  return RECOMP_RETURN_NORMAL;
}
