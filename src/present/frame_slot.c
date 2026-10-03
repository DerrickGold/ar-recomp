/* The sole FrameSlot producer. FrameSlot_Capture runs immediately after
 * RtlDrawPpuFrame, snapshots live game state, and hands presentation an
 * isolated value copy. */
#include <string.h>
#include <stdlib.h>

#include "dev/host_dev_tools.h"
#include "host/host_ppu_output.h"
#include "host/host_display.h"
#include "host/host_input.h"
#include "sim/menu/sim_menu_capture.h"
#include "present/display_geometry.h"
#include "host/host_clock.h"
#include "present/present.h"
#include "replacements/hd_replacements.h"
#include "snesrecomp/game/types.h"
#include "app/settings.h"
#include "diorama/diorama_capture.h"
#include "diorama/diorama.h"
#include "sim/sim3d/sim3d.h"
#include "sim/sim_frame_capture.h"
#include "action/action_effect_capture.h"
#include "action/action_bg_tuner.h"
#include "actraiser_game.h"
#include "actraiser/actraiser_localization_runtime.h"
#include "sim/menu/sim_menu_localization.h"
#include "constants.h"
#include "actraiser/actraiser_rtl.h"
#include "snesrecomp/game_runtime.h"
#include "snesrecomp/game/runtime.h" /* g_ram */
#include "present/frame_timing.h"
#include "snesrecomp/runner.h"

typedef struct FramePpuView {
  const SnesRunnerApi *api;
  SrRunnerHandle *runner;
  SrPpuFrameSnapshot state;
  SrPpuStateSnapshot live_state;
} FramePpuView;

static bool FramePpuView_Capture(
    FramePpuView *view, SrPpuSurfaceSnapshot *surfaces) {
  memset(view, 0, sizeof(*view));
  memset(surfaces, 0, sizeof(*surfaces));
  view->api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  view->runner = RtlGameRunner();
  view->state.struct_size = sizeof(view->state);
  view->live_state.struct_size = sizeof(view->live_state);
  surfaces->struct_size = sizeof(*surfaces);
  return view->api && view->runner &&
      view->api->struct_size >= SNES_RUNNER_API_PPU_SURFACE_SIZE &&
      (view->api->capabilities &
       (SR_RUNNER_CAP_PPU_STATE |
        SR_RUNNER_CAP_PPU_FRAME_STATE |
        SR_RUNNER_CAP_PPU_SURFACE_VIEWS |
        SR_RUNNER_CAP_BORROWED_BYTE_SPANS |
        SR_RUNNER_CAP_BORROWED_U16_SPANS)) ==
          (SR_RUNNER_CAP_PPU_STATE |
           SR_RUNNER_CAP_PPU_FRAME_STATE |
           SR_RUNNER_CAP_PPU_SURFACE_VIEWS |
           SR_RUNNER_CAP_BORROWED_BYTE_SPANS |
           SR_RUNNER_CAP_BORROWED_U16_SPANS) &&
      view->api->query_ppu_frame_state(view->runner, &view->state) ==
          SR_RESULT_OK &&
      view->api->query_ppu_state(view->runner, &view->live_state) ==
          SR_RESULT_OK &&
      view->api->query_ppu_surfaces(view->runner, surfaces) == SR_RESULT_OK &&
      view->state.overlay_count == SR_PPU_OVERLAY_SOURCE_COUNT &&
      surfaces->overlay_count == SR_PPU_OVERLAY_SOURCE_COUNT &&
      surfaces->band_count == SR_PPU_SURFACE_BAND_COUNT &&
      surfaces->lifetime_generation == view->state.lifetime_generation &&
      view->api->ppu_surface_snapshot_is_valid(view->runner, surfaces) != 0u;
}

/* Captures are per PRESENTED frame, not per emulated tick: gameplay can batch
 * catch-up ticks into one capture below 60Hz present rates. Host pause/menu
 * redraws do not run an emulated tick and therefore produce a zero delta;
 * ActRaiser's native pause is different—it continues running emulated vblanks
 * and is intentionally filtered only by the action-effect gameplay clock.
 * Reactive-camera statistics follow this emulator-frame delta, not capture
 * call count, so their EMA remains anchored to the fixed 60.0988Hz rate. */
/* The first frame and a counter reset each contribute one tick. A host-paused
 * redraw contributes zero; catch-up batches are bounded for presentation. */
static int CaptureElapsedTicks(int current_tick) {
  static int last_emulated_tick = -1;
  int elapsed = 1;
  if (last_emulated_tick >= 0) {
    elapsed = current_tick - last_emulated_tick;
    if (elapsed < 0)
      elapsed = 1;
    if (elapsed > kFrameTimingMaximumElapsedTicks)
      elapsed = kFrameTimingMaximumElapsedTicks;
  }
  last_emulated_tick = current_tick;
  return elapsed;
}

/* The sole FrameSlot writer.
 * Reads the coherent ABI PPU view, g_settings, g_snes_width/height,
 * g_scene_inspector_presentation: legitimate here (this
 * runs on the game thread, immediately after RtlDrawPpuFrame() returns,
 * before the game thread touches any of this state again). present.c must
 * never do this; it only reads the FrameSlot this produces. */
void FrameSlot_Capture(FrameSlot *dst, const SimFrameData *annotated_sim) {
  memset(dst, 0, sizeof(*dst));
  /* The slot is already zeroed; stamping the header is all that is left. */
  ArLocalizationFrame_InitCleared(&dst->localization);
  FramePpuView ppu_view;
  const bool have_ppu_view =
      FramePpuView_Capture(&ppu_view, &dst->ppu_surfaces);
  dst->background_packet = ActRaiser_LiveBackgroundPacket();
  SimMenu_CaptureFrame(&dst->sim_menu,
      have_ppu_view ? ppu_view.api : NULL, ppu_view.runner);

  const int elapsed_ticks = CaptureElapsedTicks(snes_frame_counter);
  /* A capture can follow multiple catch-up ticks. Presentation interpolation
   * and camera smoothing both use this elapsed period; paused redraws use zero.
   */
  dst->capture_ticks = (uint8_t)elapsed_ticks;

  ActionEffectCapture_CaptureFrame(dst);
  dst->magic_cycle_armed = g_settings.cheat_magic_cycle;
  dst->magic_cycle_selected =
      g_settings.cheat_magic_cycle ? ActRaiser_SelectedMagic() : 0;

  /* Produced frames already include the completed canvas. Paused redraws
   * refresh metadata only, retaining that canvas and its generation. */
  if (annotated_sim)
    dst->sim = *annotated_sim;
  else
    SimFrameCapture_RefreshMetadata(&dst->sim);
  Sim3D_CaptureOutputSurfaceViews(&dst->sim3d_output_surfaces);

  dst->snes_width = g_snes_width;
  dst->snes_height = g_snes_height;
  dst->display_mode = g_settings.display_mode;
  dst->pixel_aspect = g_active_pixel_aspect;
  dst->ws_active = g_ws_active;
  dst->ws_extra = g_ws_extra;
  dst->ignore_aspect_ratio = Settings_IgnoreAspectRatio();
  dst->visible_x0 = Settings_VisibleX0();
  dst->visible_width = Settings_VisibleWidth();
  /* Latched, not read from g_ppu, for the same reason extra_left_cur is. */
  ActRaiser_LiveVerticalMargins(
      &dst->ws_extra_top, &dst->ws_extra_bottom);
  ActRaiser_LiveDioramaVerticalExtent(
      &dst->diorama_world_y0, &dst->diorama_world_height);
  ActRaiser_LiveDioramaHorizontalExtent(
      &dst->diorama_world_x0, &dst->diorama_world_width);
  ActRaiser_LiveDioramaFraming(&dst->diorama_framing_x, &dst->diorama_framing_y);
  dst->diorama_bg_apron_mask = ActRaiser_DioramaBgApronMask();
  dst->diorama_bg2_source_bounds.valid = ActRaiser_DioramaBg2SourceBounds(
      &dst->diorama_bg2_source_bounds.x0, &dst->diorama_bg2_source_bounds.x1);
  /* The PPU authors the native camera as its own centred 256-pixel pass; this
   * crop no longer translates a completed enhanced scanout. */
  dst->authentic_x0 = g_ws_extra;
  dst->authentic_y0 = dst->ws_extra_top;
  dst->authentic_frame_serial = HostPpuOutput_AuthenticFrameSerial();
  dst->obj_apron = SR_PPU_OBJ_APRON;
  /* Density-corrected here, at the D6 producer, so present.c consumes a value
   * already expressed in PHYSICAL output pixels (0 = auto passes through). */
  dst->hud_scale_percent =
      Settings_ScalePercentToOutput(g_settings.hud_scale_percent);
  dst->show_fps = g_settings.show_fps;
  dst->performance_overlay = g_settings.performance_overlay;

  DioramaCapture_CaptureFrame(dst, have_ppu_view ? &ppu_view.state : NULL);

  /* Pair timestamp and feature gates for presentation-time frame generation. */
  dst->timestamp_ns = HostClock_Nanoseconds();
  /* Explicit offline image-comparison mode only: readbacks/file IO may take
   * longer than the live pair timeout. Keep successive emulated endpoints
   * exactly one tick apart without changing the game or host pacing clocks. */
  static int capture_tick_clock = -1;
  if (capture_tick_clock < 0) {
    const char *headless = getenv("AR_HEADLESS");
    const char *clock = getenv("AR_FRAME_CAPTURE_TICK_CLOCK");
    capture_tick_clock = headless && !strcmp(headless, "1") &&
        clock && !strcmp(clock, "1");
  }
  if (capture_tick_clock)
    dst->timestamp_ns = ((uint64_t)snes_frame_counter + 1u) * 1000000000u / 60u;
  dst->turbo_active = HostInput_IsTurbo();
  dst->interp_setting_enabled = g_settings.gpu_interp_enabled;
  dst->diorama_hud_flat = g_settings.diorama_hud_flat;
  /* Manual controls are main-thread owned and can change during production.
   * The host fills them before presentation; only game motion travels through
   * the queue. Never sample presentation state here. */
  DioramaCamera_CaptureMotion(&dst->diorama_camera, elapsed_ticks);

  Sim3DCamera_CaptureFrame(&dst->sim_camera, elapsed_ticks);
  /* Stable game-authored camera coordinates used by action effect projection.
   * Read before HDMA mutates the PPU scroll registers. */
  dst->bg1_camera_x = (int16_t)ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraX);
  dst->bg1_camera_y = (int16_t)ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraY);
  dst->bg2_camera_x = (int16_t)ActRaiser_ReadWram16(kActRaiserWram_Bg2CameraX);
  dst->bg2_camera_y = (int16_t)ActRaiser_ReadWram16(kActRaiserWram_Bg2CameraY);
  const uint16_t bg2_ratio =
      ActRaiser_ReadWram16(kActRaiserWram_Bg2VerticalNumerator);
  dst->bg2_vertical_ratio = ((bg2_ratio & 15u) << 4) | ((bg2_ratio >> 8) & 15u);

  if (have_ppu_view) {
    const SrPpuFrameSnapshot *ppu_frame = &ppu_view.state;
    dst->hud_split_height = ppu_frame->hud_split_height;
    dst->hud_left_end = ppu_frame->hud_left_end;
    dst->hud_right_start = ppu_frame->hud_right_start;
    dst->hud_player_row_y = ppu_frame->hud_player_row_y;
    dst->hud_left_only_y = ppu_frame->hud_left_only_y;
    dst->extra_left_right = ppu_frame->margin_budget;
    /* Fix B: from the latch, NOT g_ppu — see the field comment in present.h and
     * the latch in ActRaiserDrawPpuFrame. */
    {
      int live_left = 0, live_right = 0;
      ActRaiser_LiveMargins(&live_left, &live_right);
      dst->extra_left_cur = (uint8_t)live_left;
      dst->extra_right_cur = (uint8_t)live_right;
      ActRaiser_LiveActionBgPlan(&dst->action_bg_plan,
                                 &dst->bg_capture_pad_to_budget);
      ActRaiser_LiveDioramaSkybox(&dst->diorama_skybox_surface,
                                &dst->diorama_skybox_world_x,
                                &dst->diorama_skybox_periodic);
      dst->action_bg_extent_guides = ActionBgTuner_GuidesEnabled();
    }
    dst->inidisp = ppu_frame->display_control;
    dst->bg_mode = ppu_frame->bg_mode;
    const SrPpuBackgroundState *bg3 = &ppu_view.live_state.backgrounds[2];
    dst->bg3_state_valid = bg3->tilemap_width_tiles != 0u &&
        bg3->tilemap_height_tiles != 0u;
    dst->bg3_hscroll = bg3->h_scroll;
    dst->bg3_vscroll = bg3->v_scroll;
    dst->bg3_tilemap_base_words = bg3->tilemap_base_word;
    dst->bg3_tilemap_width_tiles = bg3->tilemap_width_tiles;
    dst->bg3_tilemap_height_tiles = bg3->tilemap_height_tiles;

    _Static_assert(kFrameSlotOverlaySourceCount == SR_PPU_OVERLAY_SOURCE_COUNT,
                   "FrameSlot overlay source count must match the PPU's");
    _Static_assert(kFrameSlotOverlaySourceCount ==
                       SR_PPU_OVERLAY_SOURCE_COUNT,
                   "FrameSlot overlay source count must match the ABI's");
    _Static_assert(kFrameSlotOverlay_Bg3 == SR_PPU_OVERLAY_BG3 &&
                   kFrameSlotOverlay_Obj == SR_PPU_OVERLAY_OBJ,
                   "present.h's mirrored overlay source order must match ppu.h");
    _Static_assert(kFrameSlotOverlayFlag_RemoveFromGame ==
                       SR_PPU_OVERLAY_REMOVE_FROM_GAME,
                   "present.h's mirrored overlay flag must match the ABI");
    _Static_assert(kFrameSlotOverlayFlag_MarkFullAddSubscreen ==
                       SR_PPU_OVERLAY_MARK_FULL_ADD_SUBSCREEN,
                   "present.h's mirrored full-add flag must match the ABI");
    dst->action_bg1_mask_valid =
        !dst->diorama_active &&
        (ppu_frame->overlays[SR_PPU_OVERLAY_BG1].flags &
         (SR_PPU_OVERLAY_MARK_OWNING_SCREEN_WINNER |
          SR_PPU_OVERLAY_MARK_VISIBLE_MAIN_WINNER)) != 0u &&
        (ppu_frame->overlays[SR_PPU_OVERLAY_BG1].content_band_mask & 1u) != 0u;
    dst->action_bg2_mask_valid =
        !dst->diorama_active &&
        (ppu_frame->overlays[SR_PPU_OVERLAY_BG2].flags &
         SR_PPU_OVERLAY_MARK_MAIN_SCREEN_WINNER) != 0u &&
        (ppu_frame->overlays[SR_PPU_OVERLAY_BG2].content_band_mask & 1u) != 0u;
    dst->diorama_bg_transparent_fill_argb[0] =
        ppu_frame->overlays[SR_PPU_OVERLAY_BG1].transparent_fill_argb;
    dst->diorama_bg_transparent_fill_argb[1] =
        ppu_frame->overlays[SR_PPU_OVERLAY_BG2].transparent_fill_argb;
    dst->diorama_bg_transparent_fill_configured[0] =
        ppu_frame->overlays[SR_PPU_OVERLAY_BG1]
            .transparent_fill_configured != 0u;
    dst->diorama_bg_transparent_fill_configured[1] =
        ppu_frame->overlays[SR_PPU_OVERLAY_BG2]
            .transparent_fill_configured != 0u;
    /* These mirrors are load-bearing allocation contracts shared by capture,
     * frame generation, and the compositor. */
    _Static_assert(kFrameSlotLayerTextureWidth == SR_PPU_SURFACE_MAX_WIDTH,
                   "present.h's layer texture width must match the ABI");
    _Static_assert(kFrameSlotLayerTextureHeight == SR_PPU_SURFACE_MAX_HEIGHT,
                   "present.h's layer texture height must match the ABI");
    _Static_assert(kFrameSlotAuthenticWidth == kActRaiserAuthenticWidth,
                   "present.h's mirrored authentic width must match "
                   "actraiser_game.h");
    _Static_assert(kFrameSlotAuthenticHeight == kActRaiserAuthenticHeight,
                   "present.h's mirrored authentic height must match "
                   "actraiser_game.h");
    for (int i = 0; i < kFrameSlotOverlaySourceCount; i++) {
      const SrPpuOverlayState *src = &ppu_frame->overlays[i];
      FrameSlotOverlayCapture *d = &dst->overlay_captures[i];
      d->x0 = src->x0;
      d->x1 = src->x1;
      d->y0 = src->y0;
      d->y1 = src->y1;
      d->flags = (uint8_t)src->flags;
      d->oamFirst = src->oam_first;
      d->oamCount = src->oam_count;
    }

    ActRaiser_HudObjIconRange(&dst->hud_icon_first, &dst->hud_icon_count,
                              &dst->hud_icon_rows);
    if (dst->hud_icon_count)
      ActRaiser_HudObjSurfaceView(&dst->hud_obj_surface);

    /* Only needed when an OBJ overlay/HUD icon is active this frame (§2.8
     * cost note). */
    if (ppu_frame->overlays[SR_PPU_OVERLAY_OBJ].oam_count ||
        dst->hud_icon_count) {
      SrBorrowedU16Span oam = {sizeof(oam), 0u, NULL, 0u, 0u};
      SrBorrowedSpan high_oam = {
          sizeof(high_oam), 0u, NULL, 0u, 0u};
      _Static_assert(sizeof(dst->oam) ==
                         SR_PPU_OAM_WORD_COUNT * sizeof(uint16_t),
                     "oam size (D18)");
      _Static_assert(sizeof(dst->high_oam) == SR_PPU_HIGH_OAM_BYTE_COUNT,
                     "high OAM size (D18)");
      if (ppu_view.api->borrow_u16_memory(
              ppu_view.runner, SR_MEMORY_OAM, &oam) == SR_RESULT_OK &&
          ppu_view.api->borrow_memory(
              ppu_view.runner, SR_MEMORY_HIGH_OAM, &high_oam) ==
              SR_RESULT_OK &&
          oam.element_count >= SR_PPU_OAM_WORD_COUNT &&
          high_oam.byte_size >= SR_PPU_HIGH_OAM_BYTE_COUNT &&
          oam.lifetime_generation == ppu_frame->lifetime_generation &&
          high_oam.lifetime_generation == ppu_frame->lifetime_generation) {
        memcpy(dst->oam, oam.data, sizeof(dst->oam));
        memcpy(dst->high_oam, high_oam.data, sizeof(dst->high_oam));
        dst->oam_valid = true;
      }
    }

    dst->m7_active = ppu_frame->mode7_override_active != 0u;
  }

  if (have_ppu_view && dst->bg3_state_valid) {
    SrBorrowedU16Span vram = {sizeof(vram), 0u, NULL, 0u, 0u};
    if (ppu_view.api->borrow_u16_memory(
            ppu_view.runner, SR_MEMORY_VRAM, &vram) == SR_RESULT_OK &&
        vram.data && vram.lifetime_generation ==
            ppu_view.state.lifetime_generation) {
      SrBorrowedU16Span cgram = {sizeof(cgram), 0u, NULL, 0u, 0u};
      if (ppu_view.api->borrow_u16_memory(
              ppu_view.runner, SR_MEMORY_CGRAM, &cgram) != SR_RESULT_OK ||
          cgram.lifetime_generation != ppu_view.state.lifetime_generation) {
        cgram.data = NULL;
        cgram.element_count = 0;
      }
      /* Identity is A=D=1.0 with no shear in 8.8 fixed point; anything else
       * means the layer is rotated or scaled away from the flat presentation
       * a screen-space replacement can stand in for. */
      const int16_t *m7 = ppu_view.live_state.mode7_matrix;
      const bool mode7_transformed = dst->bg_mode == 7 &&
          !(m7[0] == 256 && m7[1] == 0 && m7[2] == 0 && m7[3] == 256);
      ActRaiserLocalizationRuntime_CaptureFrame(
          &dst->localization, dst->bg3_tilemap_base_words,
          ppu_view.live_state.backgrounds[2].tile_base_word,
          vram.data, vram.element_count, cgram.data, cgram.element_count,
          mode7_transformed);
      if (dst->sim_menu.valid)
        SimMenuLocalization_CaptureLabels(
            &dst->sim_menu.label_frame, &dst->localization, &dst->sim_menu.model,
            cgram.data, cgram.element_count);
      if (dst->sim_menu.help.active)
        SimMenuLocalization_AppendHelp(
            &dst->localization,&dst->sim_menu.help,cgram.data,cgram.element_count);
      if (dst->sim.view == kSimView_WorldNavigation &&
          dst->sim.world_navigation_scene.composition.valid &&
          !dst->sim.world_navigation_scene.composition.empty_animation) {
        ActRaiserLocalizationRuntime_AppendWorldNavigationLabel(
            &dst->localization,
            dst->sim.world_navigation_scene.composition.label_location,
            dst->sim.world_navigation_scene.composition.label.visible,
            cgram.data, cgram.element_count);
      }
    }
  }

  HdReplacements_CaptureFrame(dst);

  dst->scene_inspector_enabled = g_settings.scene_inspector;
  dst->inspector_selection = *HostDevTools_InspectorPresentation();
}
