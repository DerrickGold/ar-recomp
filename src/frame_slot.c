/* frame_slot.c — the sole game-thread FrameSlot producer, extracted from
 * main.c (Q3). FrameSlot_Capture runs on the game thread immediately after
 * RtlDrawPpuFrame() returns, reading live g_ppu/g_settings/g_ram/etc.;
 * present.c only ever consumes the FrameSlot this produces. No behavior
 * change — the body is verbatim from main.c. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "frame_slot.h"
#include "present.h"
#include "types.h"
#include "settings.h"
#include "sim3d.h"
#include "sim_render_metadata.h"
#include "sim_town_canvas.h"
#include "actraiser_game.h"
#include "actraiser_rtl.h"
#include "common_rtl.h"      /* g_ram, g_ppu */
#include "snes/ppu.h"        /* PPU_mode, PpuOverlay* */

/* main.c-owned globals with no header declaration, read here. */
extern bool g_ws_active;
extern int g_ws_extra;
extern bool g_diorama_frame_active;

/* B4-vellean (followup doc): self-calibrating velocity normalizer — REVISED
 * TWICE after live measurement (AR_DYNCAM_LOG captures, 2026-07-21):
 *   1st capture: the doc's literal "permanent running max, seeded 256" was
 *   dominated by a too-high floor (ordinary run/walk PlayerVelocityX never
 *   exceeded ~1-2 raw units — the 256 floor never got superseded, yaw lean
 *   read 0.004-0.008 all session) and by a single early outlier on Y (one
 *   big fall — likely the stage-entry drop-in, not a real jump — spiked the
 *   running max once; a monotonic max can only grow, so every ordinary jump
 *   afterward normalized against that outlier and read near-zero).
 *   2nd capture (after switching to a decaying PEAK follower, ~10s
 *   half-life): fixed X (yaw now reaches ±0.5 during normal running), but Y
 *   was STILL dead — one -1.000 spike at the very start (4 frames, matching
 *   the same drop-in event), then near-zero for the rest of a ~56s session
 *   with plenty of real jumps in it. A peak-follower is fundamentally the
 *   wrong shape for this: ONE frame can set the entire session's scale, no
 *   matter how fast it decays, if that one frame is a scripted event (a
 *   drop-in) rather than representative gameplay physics.
 * Fix: normalize against a recent-activity AVERAGE (exponential moving
 * average of |v|, ~0.8s time constant) instead of a peak, scaled by
 * kNormMultiple so "typical recent motion" reads as a fraction of full
 * lean and a burst clearly above that reads as more. A single-frame outlier
 * — however large — only nudges the average by kEmaAlpha of its excess, so
 * it can't singlehandedly desensitize anything; sustained real motion (a
 * multi-frame jump arc, continuous running) dominates the average the way
 * it should. This is the auto-gain-control shape (RMS/average-following),
 * not peak-following, and it's what "self-calibrates near real top speed"
 * actually needs when scripted one-off events share the same WRAM signal as
 * real gameplay motion. */
static float g_diorama_velx_avg = 4.0f;
static float g_diorama_vely_avg = 4.0f;

/* Captures are per PRESENTED frame, not per emulated tick: gameplay batches
 * catch-up ticks into one capture below 60Hz present rates (Frame limit,
 * <60Hz panels), and the paused/menu loop re-captures frozen WRAM at panel
 * rate with zero ticks elapsing. All reactive-camera statistics therefore
 * advance by TICKS, not by call: FrameSlot_Capture measures how many
 * emulated ticks elapsed since the last capture (snes_frame_counter) and
 * the EMA applies once per tick — so kEmaAlpha's time constant is anchored
 * to the fixed 60.0988Hz tick rate, identical on every display and limit
 * setting, and a session parked in the settings menu cannot recalibrate the
 * averages against a frozen pause velocity. */
static int g_capture_ticks;   /* ticks since previous capture; 0 while paused */

static float NormalizeReactiveVelocity(int16_t v, float *avg) {
  static const float kFloor = 4.0f;
  static const float kEmaAlpha = 0.02f;      /* ~0.8s time constant, per-tick */
  static const float kNormMultiple = 3.0f;   /* "full lean" = 3x recent avg */
  float av = fabsf((float)v);
  for (int t = 0; t < g_capture_ticks; t++)
    *avg += (av - *avg) * kEmaAlpha;
  float ref = *avg * kNormMultiple;
  if (ref < kFloor) ref = kFloor;
  float norm = (float)v / ref;
  if (norm > 1.0f) norm = 1.0f;
  if (norm < -1.0f) norm = -1.0f;
  return norm;
}

/* B4-kick (followup doc): rising-edge detection for the three event
 * triggers. Game-thread-only state (FrameSlot_Capture's exclusive caller) —
 * present.c only ever sees the resulting one-shot FrameSlot flags. */
static bool g_diorama_prev_boost;
static int16_t g_diorama_prev_vely;
static uint8_t g_diorama_prev_hp;

/* Sim-town reactive camera. Separate averages from the action-stage pair
 * above: the two modes measure different actors moving at different scales,
 * and sharing an accumulator would make every town entry re-calibrate against
 * whatever the last action stage was doing. */
static float g_sim_velx_avg = 4.0f;
static float g_sim_vely_avg = 4.0f;
static uint8_t g_sim_prev_hp;
static bool g_sim_prev_in_town;

/* Sim world-record planar velocities. The catalogue keeps every world record
 * on one flat map, so these are the whole of the angel's motion -- there is no
 * third axis to read and none is implied by the projection. */
enum {
  kSimRecordVelocityX = 0x1A,
  kSimRecordVelocityY = 0x1C,
};

/* The pose the projection is built from this frame.
 *
 * Free Cam's is the player-owned one the right-drag edits and the reset action
 * restores; Dynamic Cam has its own baseline that the reactive lean works
 * around. Resolved in one place because two Sim3DTuning sites read it, and a
 * camera that differed between them would be a genuinely confusing bug. */
typedef struct SimCameraPose { int pitch_mrad, yaw_mrad, distance_x100; } SimCameraPose;

static SimCameraPose Sim3D_ActivePose(void) {
  if (g_settings.sim3d_camera_mode == kSimCam_Dynamic)
    return (SimCameraPose){
      g_settings.sim3d_dyncam_baseline_tilt_x_mrad,
      g_settings.sim3d_dyncam_baseline_tilt_y_mrad,
      g_settings.sim3d_dyncam_baseline_distance_x100,
    };
  return (SimCameraPose){
    g_settings.sim3d_tilt_x_mrad,
    g_settings.sim3d_tilt_y_mrad,
    g_settings.sim3d_distance_x100,
  };
}

/* #16: the Sim3DTuning snapshot was spelled out identically at both
 * Sim3D_AnnotateFrame sites (FrameSlot_Capture and DrawAndPresentFrame). Build
 * it once here so the two can never drift. Output is byte-identical to the
 * former inline literals (same field list, same sources). */
Sim3DTuning BuildSim3DTuning(void) {
  int sim_margin_left = 0, sim_margin_right = 0;
  ActRaiser_SimSpriteMargins(&sim_margin_left, &sim_margin_right);
  SimCameraPose sim_pose = Sim3D_ActivePose();
  return (Sim3DTuning){
      .pitch_mrad = sim_pose.pitch_mrad,
      .yaw_mrad = sim_pose.yaw_mrad,
      .distance_x100 = sim_pose.distance_x100,
      .height_scale_x100 = g_settings.sim3d_height_scale_x100,
      .shadow_opacity_pct = g_settings.sim3d_shadow_opacity_pct,
      .height_pop_pct = g_settings.sim3d_height_pop_pct,
      .light_azimuth_deg = g_settings.sim3d_light_azimuth_deg,
      .light_elevation_deg = g_settings.sim3d_light_elevation_deg,
      .shadow_softness_pct = g_settings.sim3d_shadow_softness_pct,
      .rim_strength_pct = g_settings.sim3d_rim_strength_pct,
      .underlay_haze_pct = g_settings.sim3d_underlay_haze_pct,
      .cloud_opacity_pct = g_settings.sim3d_cloud_opacity_pct,
      .cloud_falloff_px = g_settings.sim3d_cloud_falloff_px,
      .cloud_inset_px = g_settings.sim3d_cloud_inset_px,
      .cull_lead_px = g_settings.sim3d_cull_lead_px,
      .cull_haze_pct = g_settings.sim3d_cull_haze_pct,
      .cull_dim_pct = g_settings.sim3d_cull_dim_pct,
      .cull_haze_lead_px = g_settings.sim3d_cull_haze_lead_px,
      .cull_corner_px = g_settings.sim3d_cull_corner_px,
      .underlay_defocus_pct = g_settings.sim3d_underlay_defocus_pct,
      .cloud_altitude_px = g_settings.sim3d_cloud_altitude_px,
      .cloud_drift_pct = g_settings.sim3d_cloud_drift_pct,
      .cull_lift_inset = g_settings.sim3d_cull_lift_inset,
      .backdrop_strength_pct = g_settings.sim3d_backdrop_strength_pct,
      .backdrop_horizon_pct = g_settings.sim3d_backdrop_horizon_pct,
      .sprite_margin_left = sim_margin_left,
      .sprite_margin_right = sim_margin_right };
}

static void CaptureSimDynamicCamera(FrameSlot *dst, bool in_town) {
  dst->sim_camera_mode = g_settings.sim3d_camera_mode;
  dst->sim_dyncam_strength = g_settings.sim3d_reactive_strength;

  /* Outside a town there is no angel record to read: the memory holds
   * whatever the action stage left there. Reporting a neutral camera and
   * resetting the edge state means re-entering a town starts level instead of
   * inheriting a lean from a stale read. */
  if (!in_town) {
    dst->sim_dyncam_lean_yaw = 0.0f;
    dst->sim_dyncam_lean_pitch = 0.0f;
    dst->sim_dyncam_event_hit = false;
    g_sim_prev_in_town = false;
    return;
  }

  int16_t vel_x = (int16_t)ActRaiser_ReadWram16(
      kActRaiserWram_SimAngelRecord + kSimRecordVelocityX);
  int16_t vel_y = (int16_t)ActRaiser_ReadWram16(
      kActRaiserWram_SimAngelRecord + kSimRecordVelocityY);
  dst->sim_dyncam_lean_yaw = NormalizeReactiveVelocity(vel_x, &g_sim_velx_avg);
  dst->sim_dyncam_lean_pitch =
      NormalizeReactiveVelocity(vel_y, &g_sim_vely_avg);

  /* Damage taken, on the frame it applies. Same reasoning as the action
   * stage's revision: an HP decrease is the instant damage lands, whereas an
   * invulnerability flag is set later, once hit-stun begins.
   *
   * The first town frame only seeds the previous value. Entering a town with
   * less HP than the last one ended with is not a hit, and without this the
   * camera jolts on arrival. */
  uint8_t hp = g_ram[kActRaiserWram_AngelCurrentHp];
  dst->sim_dyncam_event_hit = g_sim_prev_in_town && hp < g_sim_prev_hp;
  g_sim_prev_hp = hp;
  g_sim_prev_in_town = true;
}

/* #16: DrawAndPresentFrame annotates the canonical SimFrameData once per
 * frame and points this at it around its SubmitFrameToPresent call;
 * FrameSlot_Capture copies it instead of recomputing the identical
 * CaptureFrame+AnnotateFrame (same wram/settings/tuning inputs, same
 * thread, nothing mutates them in between). NULL for every other caller —
 * the AR_SHOT/F2 screenshot capture (DevTools_WriteFramebufferPpm) and the
 * paused/menu redraw submit — which fall back to computing their own. */
static const SimFrameData *s_pending_annotated_sim;

void FrameSlot_SetPendingAnnotatedSim(const SimFrameData *sim) {
  s_pending_annotated_sim = sim;
}

/* M5 (ar-recomp-threading-impl.md Appendix D5): the sole FrameSlot writer.
 * Reads live g_ppu, g_settings, g_snes_width/height,
 * g_scene_inspector_presentation, g_hd_replacements: legitimate here (this
 * runs on the game thread, immediately after RtlDrawPpuFrame() returns,
 * before the game thread touches any of this state again). present.c must
 * never do this; it only reads the FrameSlot this produces. */
void FrameSlot_Capture(FrameSlot *dst) {
  memset(dst, 0, sizeof(*dst));

  /* Ticks elapsed since the previous capture — the advancement unit for the
   * reactive-camera statistics below (see g_capture_ticks). Clamped: a
   * savestate load or long stall must not fire a giant catch-up burst. 0
   * while paused (frozen WRAM re-captures must not move averages or edge
   * state). First capture seeds without advancing. */
  { extern int snes_frame_counter;
    static int last_tick = -1;
    if (last_tick < 0) g_capture_ticks = 1;
    else {
      int elapsed = snes_frame_counter - last_tick;
      if (elapsed < 0) elapsed = 1;           /* counter reset (reload) */
      if (elapsed > 8) elapsed = 8;           /* stall/settings-menu clamp */
      g_capture_ticks = elapsed;
    }
    last_tick = snes_frame_counter;
  }
  /* R17/C3: publish it. Present-time interpolation needs the TRUE period of
   * the prev->curr pair, not an assumed single tick: the main loop's drain
   * runs up to kMaxCatchupFrames ticks in one iteration while the scroll
   * snapshot advances once per present, so a 2-tick pair carries two ticks of
   * camera motion. Dividing the sub-tick phase by this is what keeps
   * extrapolation from overshooting by that factor. Same clamped value the
   * reactive-camera statistics above use. */
  dst->capture_ticks = (uint8_t)g_capture_ticks;

  /* D2 publishes only the pitch-zero separated-composite capability, and
   * only after its same-frame CPU oracle found zero differing pixels. */
  if (s_pending_annotated_sim) {
    dst->sim = *s_pending_annotated_sim;   /* already annotated this frame */
  } else {
    SimRenderMetadata_CaptureFrame(
        &dst->sim, g_ram, g_settings.sim3d_mode,
        Settings_Sim3DRequestedFeatures(),
        g_settings.sim3d_diagnostic_layers, Sim3D_ImplementedFeatures());
    Sim3DTuning tuning = BuildSim3DTuning();
    Sim3D_AnnotateFrame(&dst->sim, &tuning);
    /* Accumulation itself happens once a frame at the always-run site below;
     * this only publishes the current canvas state into the slot. */
    dst->sim.town_canvas_serial = SimTownCanvas_Serial();
  }

  dst->snes_width = g_snes_width;
  dst->snes_height = g_snes_height;
  dst->display_mode = g_settings.display_mode;
  dst->pixel_aspect = g_active_pixel_aspect;
  dst->ws_active = g_ws_active;
  dst->ws_extra = g_ws_extra;
  dst->ignore_aspect_ratio = g_settings.ignore_aspect_ratio;
  dst->visible_x0 = Settings_VisibleX0();
  dst->visible_width = Settings_VisibleWidth();
  /* Density-corrected here, at the D6 producer, so present.c consumes a value
   * already expressed in PHYSICAL output pixels (0 = auto passes through). */
  dst->hud_scale_percent =
      Settings_ScalePercentToOutput(g_settings.hud_scale_percent);

  dst->diorama_active = g_diorama_frame_active;

  /* M7/§6.1: scroll snapshot for present-time interpolation. */
  dst->timestamp_ns = SDL_GetTicksNS();
  dst->turbo_active = g_turbo != 0;
  dst->interp_setting_enabled = g_settings.gpu_interp_enabled;
  dst->diorama_hud_flat = g_settings.diorama_hud_flat;
  /* B4-split (followup doc): both candidate camera poses, scaled the same
   * way Diorama_SeedCameraFromSettings does (g_diorama_cam and g_settings
   * are kept in lockstep by Diorama_AdjustCamera's write-back and
   * OnRuntimeSettingChanged's re-seed on menu edits, so reading straight
   * from g_settings here is equivalent to reading the live g_diorama_cam). */
  dst->diorama_camera_mode = g_settings.diorama_camera_mode;
  dst->diorama_free_pose = (DioramaCameraPose){
    (float)g_settings.diorama_tilt_x_mrad / 1000.0f,
    (float)g_settings.diorama_tilt_y_mrad / 1000.0f,
    (float)g_settings.diorama_distance_x100 / 100.0f,
  };
  dst->diorama_dyncam_baseline = (DioramaCameraPose){
    (float)g_settings.diorama_dyncam_baseline_tilt_x_mrad / 1000.0f,
    (float)g_settings.diorama_dyncam_baseline_tilt_y_mrad / 1000.0f,
    (float)g_settings.diorama_dyncam_baseline_distance_x100 / 100.0f,
  };
  dst->diorama_reactive_strength = g_settings.diorama_reactive_strength;
  /* B4-vellean (followup doc): same ReadWram16+cast pattern already used for
   * PlayerVelocityX/Y elsewhere (actraiser_rtl.c ~346-349). */
  int16_t vel_x = (int16_t)ActRaiser_ReadWram16(kActRaiserWram_PlayerVelocityX);
  int16_t vel_y = (int16_t)ActRaiser_ReadWram16(kActRaiserWram_PlayerVelocityY);
  dst->diorama_dyncam_lean_yaw =
      NormalizeReactiveVelocity(vel_x, &g_diorama_velx_avg);
  dst->diorama_dyncam_lean_pitch =
      NormalizeReactiveVelocity(vel_y, &g_diorama_vely_avg);

  /* B4-kick (followup doc): rising-edge event triggers. Hit was originally
   * the invuln-bit test AR_NO_KNOCKBACK already relies on elsewhere in this
   * file — REVISED (2026-07-21, live report + AR_FRAMELOG/AR_DYNCAM_LOG
   * correlation): that flag consistently lagged the real hit by ~10 game
   * frames (~167ms @ 60Hz) across 3 measured hits, apparently because the
   * game doesn't set it until after the knockback/hit-stun begins, not at
   * the instant damage applies. PlayerHp decreasing IS the instant damage
   * applies, so that's the trigger now — fires exactly on the hit frame,
   * no game-side lag to inherit. Landing has no documented WRAM flag, so
   * it's inferred from velocity: falling with |vely| clearly above the
   * recent-average scale, settling near zero in one tick — reuses
   * g_diorama_vely_avg (just updated above) instead of a guessed magic
   * threshold, same reasoning as B4-vellean's self-calibration. Boost is a
   * 0-to-nonzero read of the raw byte, matching how
   * PlayerInvulnerabilityTimer is read/pinned elsewhere (g_ram[...], not
   * ReadWram16 — it's a single byte). */
  uint8_t hp = g_ram[kActRaiserWram_PlayerHp];
  dst->diorama_dyncam_event_hit = hp < g_diorama_prev_hp;
  g_diorama_prev_hp = hp;

  bool was_falling =
      g_diorama_prev_vely > (int16_t)(g_diorama_vely_avg * 0.5f);
  bool now_settled = abs((int)vel_y) < (int)(g_diorama_vely_avg * 0.15f);
  dst->diorama_dyncam_event_land = was_falling && now_settled;
  g_diorama_prev_vely = vel_y;

  bool boost = g_ram[kActRaiserWram_PlayerBoost] != 0;
  dst->diorama_dyncam_event_boost = boost && !g_diorama_prev_boost;
  g_diorama_prev_boost = boost;

  CaptureSimDynamicCamera(
      dst, ActRaiser_IsSimulationTown(g_ram[kActRaiserWram_MapGroup],
                                      g_ram[kActRaiserWram_CurrentMap]));
  /* B1b (followup doc): the stable game-authored camera in WRAM, read
   * BEFORE HDMA touches the PPU scroll registers — see the long comment on
   * FrameSlot's timestamp_ns field (present.h) for why this replaced
   * g_ppu->hScroll[]/vScroll[]. g_ram is always valid (no g_ppu dependency,
   * unlike the PPU-register read this replaces). */
  dst->bg1_camera_x = (int16_t)ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraX);
  dst->bg1_camera_y = (int16_t)ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraY);
  dst->bg2_camera_x = (int16_t)ActRaiser_ReadWram16(kActRaiserWram_Bg2CameraX);
  dst->bg2_camera_y = (int16_t)ActRaiser_ReadWram16(kActRaiserWram_Bg2CameraY);

  if (g_ppu) {
    dst->hud_split_height = g_ppu->wsHudSplitHeight;
    dst->hud_left_end = g_ppu->wsHudLeftEnd;
    dst->hud_right_start = g_ppu->wsHudRightStart;
    dst->hud_player_row_y = g_ppu->wsHudPlayerRowY;
    dst->hud_left_only_y = g_ppu->wsHudLeftOnlyY;
    dst->extra_left_right = g_ppu->extraLeftRight;
    dst->inidisp = g_ppu->inidisp;
    dst->bg_mode = (uint8_t)PPU_mode(g_ppu);

    _Static_assert(kFrameSlotOverlaySourceCount == kPpuOverlaySource_Count,
                   "FrameSlot overlay source count must match the PPU's");
    _Static_assert(kFrameSlotOverlay_Bg3 == kPpuOverlaySource_Bg3 &&
                   kFrameSlotOverlay_Obj == kPpuOverlaySource_Obj,
                   "present.h's mirrored overlay source order must match ppu.h");
    _Static_assert(kFrameSlotOverlayFlag_RemoveFromGame ==
                   kPpuOverlayFlag_RemoveFromGame,
                   "present.h's mirrored overlay flag must match ppu.h");
    for (int i = 0; i < kFrameSlotOverlaySourceCount; i++) {
      const PpuOverlayCapture *src = &g_ppu->overlayCaptures[i];
      FrameSlotOverlayCapture *d = &dst->overlay_captures[i];
      d->x0 = src->x0; d->x1 = src->x1;
      d->y0 = src->y0; d->y1 = src->y1;
      d->flags = src->flags;
      d->oamFirst = src->oamFirst; d->oamCount = src->oamCount;
    }

    /* Only needed when an OBJ overlay/HUD icon is active this frame (§2.8
     * cost note). */
    if (g_ppu->overlayCaptures[kPpuOverlaySource_Obj].oamCount) {
      _Static_assert(sizeof(dst->oam) == sizeof(g_ppu->oam), "oam size (D18)");
      _Static_assert(sizeof(dst->high_oam) == sizeof(g_ppu->highOam),
                     "highOam size (D18)");
      memcpy(dst->oam, g_ppu->oam, sizeof(dst->oam));
      memcpy(dst->high_oam, g_ppu->highOam, sizeof(dst->high_oam));
      dst->oam_valid = true;
    }

    dst->m7_active = (g_ppu->m7Override.rgba != NULL);
  }

  dst->hd_entry_count = 0;
  for (int i = 0; i < g_hd_replacement_count && i < kHdMaxReplacements; i++) {
    const HdReplacement *e = &g_hd_replacements[i];
    FrameSlotHdEntry *d = &dst->hd_entries[dst->hd_entry_count++];
    d->active = e->active;
    d->source = e->source;
    d->brightness_mod = e->brightness_mod;
    d->texture = e->texture;
  }

  dst->scene_inspector_enabled = g_settings.scene_inspector;
  dst->inspector_selection = g_scene_inspector_presentation;
}
