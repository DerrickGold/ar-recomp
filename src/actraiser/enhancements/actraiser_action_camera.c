/* Presentation-aware replacement for the action camera at $02:B091.
 *
 * The ROM clamps BG1's camera for a 256x225 viewport. Wide horizontal
 * presentation used to leave that camera untouched and shorten whichever
 * synthetic margin met a finite world edge. That exposed the playfield cutoff
 * inside the wider canvas, so this replacement fits the horizontal view inside
 * the live $2E room width whenever it can.
 *
 * Diorama's vertical extension is deliberately different: it captures however
 * many real rows exist around the native $24 camera instead of moving gameplay
 * state to manufacture equal top/bottom margins. Keeping the ROM's vertical
 * camera range preserves object activation/collision and makes toggling
 * Diorama presentation-neutral. BG2 parallax and the player-relative tail
 * still run through the recompiled ROM helpers, after the corrected BG1 camera
 * has become canonical and the requested delta has been reconciled with the
 * motion that actually fit. */

#include "action/action_camera_bounds.h"
#include "actraiser/actraiser_action_bg.h"
#include "actraiser/enhancements/actraiser_camera_internal.h"
#include "actraiser_game.h"
#include "present/display_geometry.h"
#include "snesrecomp/game/cpu.h"
#include "app/settings.h"
#include "diorama/diorama.h"
#include "diorama/diorama_camera_policy.h"
#include "diorama/diorama_layer_manifest.h"
#include "render/presentation_layout.h"

#include <stdio.h>
#include <stdlib.h>

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

static bool ActionCamera_ResolvePlayfield(ActionBgPlan *plan) {
  if (plan) *plan = (ActionBgPlan){ 0 };
  const bool automatic = g_settings.extended_aspect == kScreenAspect_Auto;
  if (!g_ws_active || (!automatic && (!g_settings.ws_action ||
      g_settings.display_mode == kDisplayMode_43 ||
      g_settings.display_mode == kDisplayMode_WideRaw)) ||
      !ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup]) ||
      !ActRaiserActionBg_HleEnabled() || !plan)
    return false;

  /* Consume the same canonical classification as the renderer. In
   * particular, Death Heim's hub/final authored scenes set no finite canvas;
   * treating every action group as a streamed playfield would shift those
   * cameras even though their presentation intentionally wraps/clamps. The
   * saved policies cannot alter roles, sources, or canvas ownership. Apply
   * their edge/extent choices just as rendering does. Transient diagnostic
   * tuner overrides do not become gameplay policy. */
  ActionBgPresentationPolicy presentation;
  if (!ActRaiserActionBg_BuildCurrentPlan(
          g_ram, kActRaiserWramSize,
          g_settings.ws_bg2_padding, plan, &presentation) ||
      ActionBgPlan_CanvasOwner(plan) != 0)
    return false;
  const DioramaRoomOverride *room = ActRaiser_CurrentVirtualLayerRoom();
  /* Atomic rejection leaves the canonical plan intact, matching frame-plan
   * rendering when an edited policy is invalid for the current topology. */
  if (room) DioramaBgPolicy_Apply(room->bg_policy, plan);
  return true;
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

/* Keyed by room and width so a stale range never outlives its room. */
static struct {
  bool valid;
  uint8_t group, map;
  uint16_t width, minimum, maximum;
} s_travel;

bool ActRaiser_ActionCameraTravel(uint16_t *minimum, uint16_t *maximum) {
  if (!s_travel.valid || s_travel.group != g_ram[kActRaiserWram_MapGroup] ||
      s_travel.map != g_ram[kActRaiserWram_CurrentMap] ||
      s_travel.width != ActRaiser_ReadWram16(kActRaiserWram_Bg1Width))
    return false;
  if (minimum) *minimum = s_travel.minimum;
  if (maximum) *maximum = s_travel.maximum;
  return true;
}

static bool ActionCamera_DebugEnabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *value = getenv("AR_WS_ACTION_CAMDBG");
    enabled = value && value[0] && value[0] != '0';
  }
  return enabled != 0;
}

/* The producer uses the saved dynamic-camera baseline, never the presenter's
 * transient orbit/hit animation. A camera response must not feed back into
 * gameplay travel. Capture and presentation share the projection calculation;
 * the drawable is latched by the host rather than queried from this thread. */
static bool ActionCamera_DioramaMargins(const ActionBgPlan *plan,
    const DioramaRoomOverride *room, int native_width,
    int x, int y, int *before, int *after) {
  const ActionBgLayerPlan *playfield = &plan->layer[0];
  if (g_settings.diorama_camera_mode != kDioramaCam_Dynamic ||
      !DioramaCameraPolicy_FiniteLayer(playfield)) return false;
  const bool edits = DioramaCameraPolicy_HasEdits(room, 0);
  const bool bg_apron = ActRaiser_DioramaNeedsBgApron(room, 0, edits,
      DioramaCameraPolicy_WorldApron(playfield));
  const DioramaCapture capture = DioramaCameraPolicy_Capture(plan, room, x, y,
      g_ws_extra, g_settings.diorama_vertical_extend, bg_apron ? 1 : 0);
  const bool crt = g_settings.pixel_aspect == kPixelAspect_Crt43;
  const int visible_width = Settings_VisibleWidth();
  const int visible_height = 224 + (g_settings.extended_aspect == kScreenAspect_Auto
      ? 2 * g_actraiser_display_geometry->auto_vertical_budget : 0);
  int output_width = g_actraiser_display_geometry->output_width;
  int output_height = g_actraiser_display_geometry->output_height;
  if (output_width <= 0 || output_height <= 0) {
    output_width = visible_width * (crt ? 7 : 1);
    output_height = visible_height * (crt ? 6 : 1);
  }
  const bool stretch = Settings_IgnoreAspectRatio();
  const DioramaView view = {.camera = {
      .tilt_x = (float)g_settings.diorama_dyncam_baseline_tilt_x_mrad / 1000.0f,
      .tilt_y = (float)g_settings.diorama_dyncam_baseline_tilt_y_mrad / 1000.0f,
      .distance = (float)g_settings.diorama_dyncam_baseline_distance_x100 / 100.0f},
    .distance_scale = 1, .camera_framing_weight = 1, .pixel_aspect = g_settings.pixel_aspect,
    .ignore_aspect_ratio = stretch, .visible_width = visible_width, .visible_height = visible_height,
    .viewport = ArPresentationLayout_ResolveViewport(output_width, output_height,
        stretch, crt, visible_width, visible_height)};
  const DioramaRenderOptions options = {.layers = DioramaLayerManifest_Table(),
    .visible_planes = g_settings.diorama_layer_bg1 ? (1u << SR_PPU_OVERLAY_BG1) | (1u << kDioramaPlane_Bg1Hi) : 0,
    .skybox = g_settings.diorama_skybox};
  const DioramaScene scene = {.render = &options,
    .map_group = g_ram[kActRaiserWram_MapGroup], .map_number = g_ram[kActRaiserWram_CurrentMap],
    .layer_section = room ? room->section : kDioramaLayerSection_Room};
  int minimum, maximum;
  if (!Diorama_ResolveCaptureCameraRange(&capture, &view, &scene, x, &minimum, &maximum)) return false;
  *before = minimum > 0 ? minimum : 0;
  *after = native_width - 256 - maximum;
  if (*after < 0) *after = 0;
  return true;
}

RecompReturn ActRaiser_UpdateActionCamera(CpuState *cpu) {
  const uint8_t saved_p = cpu->P;
  cpu_write8(cpu, 0x00, cpu->S, saved_p);
  cpu->S--;
  cpu->P = (uint8_t)(cpu->P & ~0x20);
  cpu_p_to_mirrors(cpu);

  ActionBgPlan plan;
  const bool wide = ActionCamera_ResolvePlayfield(&plan);
  const DioramaRoomOverride *room = g_settings.diorama_mode ? ActRaiser_CurrentVirtualLayerRoom() : NULL;
  int horizontal_before, horizontal_after;
  DioramaCameraPolicy_HorizontalMargins(&plan, room, wide ? g_ws_extra : 0,
      &horizontal_before, &horizontal_after);
  const uint16_t native_width = ActionCamera_Read16(
      cpu, kActRaiserWram_Bg1Width);
  const uint16_t native_height = ActionCamera_Read16(
      cpu, kActRaiserWram_Bg1Height);
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
  /* Do not fit the gameplay camera to Diorama's vertical capture budget.
   * ActRaiser_ApplyVerticalMarginPolicy independently resolves the real rows
   * available above and below this native camera, and the compositor already
   * accepts asymmetric captures. The native-only API has no margin arguments,
   * so presentation cannot accidentally push required action objects outside
   * the authentic activation/collision window while still drawing them. */
  const uint16_t camera_y = ActionCameraAxisBounds_UpdateNativeCamera(
      old_y, requested_delta_y, native_height,
      kActRaiserActionCameraViewportHeight, &vertical_bounds);
  if (wide && g_settings.diorama_mode) {
    ActionCamera_DioramaMargins(&plan, room, native_width,
        old_x, camera_y,
        &horizontal_before, &horizontal_after);
  }
  const uint16_t camera_x = ActionCameraAxisBounds_UpdateCamera(
      old_x, requested_delta_x,
      native_width,
      kActRaiserAuthenticWidth, horizontal_before, horizontal_after,
      &horizontal_bounds);
  s_travel.valid = true;
  s_travel.group = g_ram[kActRaiserWram_MapGroup];
  s_travel.map = g_ram[kActRaiserWram_CurrentMap];
  s_travel.width = native_width;
  s_travel.minimum = horizontal_bounds.minimum;
  s_travel.maximum = horizontal_bounds.maximum;
  const int16_t effective_delta_x =
      ActionCameraAxisBounds_EffectiveDelta(
          &horizontal_bounds, old_x, camera_x, requested_delta_x);
  /* Native ROM behavior publishes the request even when its camera clamp
   * blocks some or all motion. Effective-delta reconciliation is exclusively
   * a fitted-presentation correction and therefore remains X-only. */
  const int16_t effective_delta_y = requested_delta_y;
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
              0, 0,
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
