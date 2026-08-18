/* The sole FrameSlot producer. FrameSlot_Capture runs immediately after
 * RtlDrawPpuFrame, snapshots live game state, and hands presentation an
 * isolated value copy. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "frame_slot.h"
#include "present.h"
#include "types.h"
#include "settings.h"
#include "diorama/diorama_planes.h"
#include "diorama/diorama.h"
#include "diorama/diorama_layer_order.h"
#include "sim/sim3d.h"
#include "sim/sim_render_metadata.h"
#include "sim/sim_background_voxels.h"
#include "sim/sim_town_canvas.h"
#include "sim/sim_world_navigation_capture.h"
#include "action/action_effect_clock.h"
#include "action/action_effects.h"
#include "action/action_bg_tuner.h"
#include "action/action_obj_interpolation.h"
#include "actraiser_game.h"
#include "constants.h"
#include "actraiser_rtl.h"
#include "common_rtl.h"      /* g_ram, g_ppu */
#include "frame_timing.h"
#include "snes/ppu.h"        /* PPU_mode, PpuOverlay* */

/* main.c-owned globals with no header declaration, read here. */
extern bool g_ws_active;
extern int g_ws_extra;
extern bool g_diorama_frame_active;
extern uint8_t *g_diorama_layer_pixels[kDioramaPlane_Count];

/* Self-calibrating velocity normalization uses a recent-activity EMA, not a
 * running or decaying peak. Live traces showed ordinary horizontal velocity at
 * only ~1-2 raw units, while a four-frame stage-entry fall dominated the
 * vertical peak and made later jumps nearly invisible. A ~0.8 s average lets
 * sustained movement set the scale while a scripted one-frame outlier changes
 * it by only kEmaAlpha. kNormMultiple leaves headroom for bursts above typical
 * recent motion. */
static float g_diorama_velx_avg = 4.0f;
static float g_diorama_vely_avg = 4.0f;

/* Captures are per PRESENTED frame, not per emulated tick: gameplay can batch
 * catch-up ticks into one capture below 60Hz present rates. Host pause/menu
 * redraws do not run an emulated tick and therefore produce a zero delta;
 * ActRaiser's native pause is different—it continues running emulated vblanks
 * and is intentionally filtered only by the action-effect gameplay clock.
 * Reactive-camera statistics follow this emulator-frame delta, not capture
 * call count, so their EMA remains anchored to the fixed 60.0988Hz rate. */
static int g_emulated_capture_ticks;
static ActionEffectObserver s_action_effect_observer;
static ActionEffectTickClock s_action_effect_tick_clock;

void FrameSlot_ResetActionEffects(void) {
  ActionEffectObserver_Reset(&s_action_effect_observer);
  ActionEffectTickClock_Reset(&s_action_effect_tick_clock);
}

static float NormalizeReactiveVelocity(int16_t v, float *avg) {
  static const float kFloor = 4.0f;
  static const float kEmaAlpha = 0.02f;      /* ~0.8s time constant, per-tick */
  static const float kNormMultiple = 3.0f;   /* "full lean" = 3x recent avg */
  float av = fabsf((float)v);
  for (int t = 0; t < g_emulated_capture_ticks; t++)
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

static uint32_t DioramaPlaneBit(int plane) {
  return 1u << (unsigned)plane;
}

static uint32_t CaptureDioramaPlaneRequestMask(void) {
  _Static_assert(kDioramaPlane_Count <= 32,
                 "diorama request mask needs one bit per plane");
  uint32_t mask = 0;
  if (g_settings.diorama_layer_backdrop &&
      g_settings.diorama_skybox != kDioramaSky_Only)
    mask |= DioramaPlaneBit(kDioramaPlane_Backdrop);
  if (g_settings.diorama_layer_bg1)
    mask |= DioramaPlaneBit(kPpuOverlaySource_Bg1) |
            DioramaPlaneBit(kDioramaPlane_Bg1Hi);
  if (g_settings.diorama_layer_bg2)
    mask |= DioramaPlaneBit(kPpuOverlaySource_Bg2) |
            DioramaPlaneBit(kDioramaPlane_Bg2Hi);
  else if (g_settings.diorama_skybox != kDioramaSky_Off)
    mask |= DioramaPlaneBit(kPpuOverlaySource_Bg2);
  if (g_settings.diorama_layer_obj)
    mask |= DioramaPlaneBit(kPpuOverlaySource_Obj) |
            DioramaPlaneBit(kDioramaPlane_Obj1) |
            DioramaPlaneBit(kDioramaPlane_Obj2) |
            DioramaPlaneBit(kDioramaPlane_Obj3);
  if (g_settings.diorama_layer_bg3 && !g_settings.diorama_hud_flat)
    mask |= DioramaPlaneBit(kPpuOverlaySource_Bg3);
  return mask;
}

static uint32_t CaptureDioramaPlaneContentMask(void) {
  uint32_t mask = DioramaPlaneBit(kDioramaPlane_Backdrop);
  static const struct {
    PpuOverlaySource source;
    uint8_t band;
    uint8_t plane;
  } kSurfaces[] = {
    { kPpuOverlaySource_Bg1, 0, kPpuOverlaySource_Bg1 },
    { kPpuOverlaySource_Bg2, 0, kPpuOverlaySource_Bg2 },
    { kPpuOverlaySource_Bg3, 0, kPpuOverlaySource_Bg3 },
    { kPpuOverlaySource_Obj, 0, kPpuOverlaySource_Obj },
    { kPpuOverlaySource_Bg1, 1, kDioramaPlane_Bg1Hi },
    { kPpuOverlaySource_Bg2, 1, kDioramaPlane_Bg2Hi },
    { kPpuOverlaySource_Obj, 1, kDioramaPlane_Obj1 },
    { kPpuOverlaySource_Obj, 2, kDioramaPlane_Obj2 },
    { kPpuOverlaySource_Obj, 3, kDioramaPlane_Obj3 },
  };
  for (size_t i = 0; i < sizeof(kSurfaces) / sizeof(kSurfaces[0]); i++) {
    if (PpuOverlaySurfaceHasContent(
            g_ppu, kSurfaces[i].source, kSurfaces[i].band))
      mask |= DioramaPlaneBit(kSurfaces[i].plane);
  }
  return mask;
}

static uint32_t CaptureDioramaAdditivePlaneMask(void) {
  uint32_t mask = 0;
  if (g_ppu->overlayCaptures[kPpuOverlaySource_Bg1].flags &
      kPpuOverlayFlag_MarkFullAddSubscreen)
    mask |= DioramaPlaneBit(kPpuOverlaySource_Bg1) |
            DioramaPlaneBit(kDioramaPlane_Bg1Hi);
  if (g_ppu->overlayCaptures[kPpuOverlaySource_Bg2].flags &
      kPpuOverlayFlag_MarkFullAddSubscreen)
    mask |= DioramaPlaneBit(kPpuOverlaySource_Bg2) |
            DioramaPlaneBit(kDioramaPlane_Bg2Hi);
  if (g_ppu->overlayCaptures[kPpuOverlaySource_Bg3].flags &
      kPpuOverlayFlag_MarkFullAddSubscreen)
    mask |= DioramaPlaneBit(kPpuOverlaySource_Bg3);
  if (g_ppu->overlayCaptures[kPpuOverlaySource_Obj].flags &
      kPpuOverlayFlag_MarkFullAddSubscreen)
    mask |= DioramaPlaneBit(kPpuOverlaySource_Obj) |
            DioramaPlaneBit(kDioramaPlane_Obj1) |
            DioramaPlaneBit(kDioramaPlane_Obj2) |
            DioramaPlaneBit(kDioramaPlane_Obj3);
  return mask;
}

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
  int sim_margin_top = 0, sim_margin_bottom = 0;
  ActRaiser_SimSpriteMargins(&sim_margin_left, &sim_margin_right,
                             &sim_margin_top, &sim_margin_bottom);
  SimCameraPose sim_pose = Sim3D_ActivePose();
  return (Sim3DTuning){
      .pitch_mrad = sim_pose.pitch_mrad,
      .yaw_mrad = sim_pose.yaw_mrad,
      .distance_x100 = sim_pose.distance_x100,
      .height_scale_x100 = g_settings.sim3d_height_scale_x100,
      .voxel_preset = g_settings.sim3d_voxel_preset,
      .voxel_detail = g_settings.sim3d_voxel_detail,
      .voxel_lod = g_settings.sim3d_voxel_lod,
      .voxel_shading = g_settings.sim3d_voxel_shading,
      .voxel_style = g_settings.sim3d_voxel_style,
      .voxel_facing = g_settings.sim3d_voxel_facing,
      .voxel_render_scale = g_settings.sim3d_voxel_render_scale,
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
      .world_navigation_lighting =
          g_settings.sim3d_world_navigation_lighting,
      .world_navigation_clouds =
          g_settings.sim3d_world_navigation_clouds,
      .world_navigation_backdrop = g_settings.sim3d_backdrop,
      .world_navigation_haze = g_settings.sim3d_cull_haze,
      .cull_lift_inset = g_settings.sim3d_cull_lift_inset,
      .backdrop_strength_pct = g_settings.sim3d_backdrop_strength_pct,
      .backdrop_horizon_pct = g_settings.sim3d_backdrop_horizon_pct,
      .windmill_wind_stops_all = g_settings.fix_windmill_wind_stop,
      .sprite_margin_left = sim_margin_left,
      .sprite_margin_right = sim_margin_right,
      .sprite_margin_top = sim_margin_top,
      .sprite_margin_bottom = sim_margin_bottom };
}

static void CaptureSimDynamicCamera(FrameSlot *dst, bool in_town) {
  dst->sim_camera_mode = g_settings.sim3d_camera_mode;
  dst->sim_dyncam_strength = g_settings.sim3d_reactive_strength;
  Sim3DCamera_GetDynamicOrbit(&dst->sim_manual_orbit_yaw,
                              &dst->sim_manual_orbit_pitch);

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

/* DrawAndPresentFrame annotates the canonical SimFrameData once per frame and
 * publishes it around HostDisplay_SubmitFrame;
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

  /* Emulated ticks since the previous capture—the advancement unit for the
   * reactive-camera statistics below. Clamp stalls and discontinuities to the
   * shared presentation-observer limit. Host-paused redraws produce zero;
   * native in-game pause still produces emulated ticks. The first produced
   * frame carries one tick because it has no prior presentation pair. */
  { extern int snes_frame_counter;
    static int last_emulated_tick = -1;
    if (last_emulated_tick < 0) g_emulated_capture_ticks = 1;
    else {
      int elapsed = snes_frame_counter - last_emulated_tick;
      if (elapsed < 0) elapsed = 1;           /* counter reset (reload) */
      if (elapsed > kFrameTimingMaximumElapsedTicks)
        elapsed = kFrameTimingMaximumElapsedTicks;
      g_emulated_capture_ticks = elapsed;
    }
    last_emulated_tick = snes_frame_counter;
  }
  /* R17/C3: publish it. Present-time interpolation needs the TRUE period of
   * the prev->curr pair, not an assumed single tick: the main loop's drain
   * runs up to kMaxCatchupFrames ticks in one iteration while the snapshot
   * advances once per present. The delayed phase uses this to start exactly
   * one tick behind curr. Same clamped value the reactive-camera statistics
   * above use. */
  dst->capture_ticks = (uint8_t)g_emulated_capture_ticks;

  /* $00:8C98 publishes only completed gameplay/OAM passes and is skipped by
   * native pause/freeze. Capture through the shared adapter so production and
   * its regression consume the identical publisher/read/delta chain. */
  const unsigned action_effect_ticks =
      ActionEffectTickClock_Capture(&s_action_effect_tick_clock);
  ActionEffects_CaptureFrame(&s_action_effect_observer, &dst->action_effects,
                             g_ram,
                             kActRaiserWramSize, action_effect_ticks);
  ActionSceneEffects_CaptureFrame(&s_action_effect_observer,
                                  &dst->action_scene_effects, g_ram,
                                  kActRaiserWramSize, action_effect_ticks);
  dst->diorama_map_group = g_ram[kActRaiserWram_MapGroup];
  dst->diorama_map_number = g_ram[kActRaiserWram_CurrentMap];
  dst->diorama_layer_section = kDioramaLayerSection_Room;
  if (!dst->action_scene_effects.decoration_overflow) {
    for (unsigned i = 0;
         i < dst->action_scene_effects.decoration_count; i++) {
      if (dst->action_scene_effects.decorations[i].kind ==
          kActionEffect_AitosWaterfall) {
        dst->diorama_layer_section =
            kDioramaLayerSection_AitosWaterfall;
        break;
      }
    }
  }
  Diorama_PublishLiveLayerSection(
      dst->diorama_map_group, dst->diorama_map_number,
      dst->diorama_layer_section);
  dst->action_effect_lighting = g_settings.action_effect_lighting;
  dst->action_effect_particles = g_settings.action_effect_particles;
  /* Capture-side twin of present.c's "[action-fx] first spell geometry
   * submitted". Together the two lines localise any future silence: neither
   * means no spell was ever identified in WRAM, capture-only means the
   * identification works but the renderer never drew it. Chasing that
   * distinction by hand is what cost a session when the animation-bank read
   * was the wrong width (docs/bug-ledger.md §32). */
  if (dst->action_effects.effect_count) {
    static bool announced;
    if (!announced) {
      announced = true;
      fprintf(stderr, "[action-fx] first spell captured: kind=%u part(s)=%u "
              "visible=%u (lighting=%d particles=%d)\n",
              dst->action_effects.controller_kind,
              dst->action_effects.effect_count,
              dst->action_effects.visible_count,
              g_settings.action_effect_lighting,
              g_settings.action_effect_particles);
    }
  }
  if (dst->action_scene_effects.effect_count ||
      dst->action_scene_effects.decoration_count) {
    static bool announced_scene;
    if (!announced_scene) {
      announced_scene = true;
      fprintf(stderr,
              "[action-fx] first scene accents captured: actors=%u/%u "
              "decorations=%u/%u (lighting=%d particles=%d)\n",
              dst->action_scene_effects.effect_count,
              dst->action_scene_effects.visible_count,
              dst->action_scene_effects.decoration_count,
              dst->action_scene_effects.decoration_visible_count,
              g_settings.action_effect_lighting,
              g_settings.action_effect_particles);
    }
  }
  /* Spawn probe for the reported "Stardust starts at ground level" bug. The
   * catalogue says a star's launch position is chosen at the VIEWPORT TOP/
   * RIGHT EDGE and it then descends; if it instead appears at the ground it
   * exits the bottom of the screen almost immediately. Mid-flight snapshots
   * cannot distinguish those, so this reports the FIRST frame of each actor
   * (age_ticks 0) with the camera and the screen-relative Y that the launch
   * arithmetic is supposed to have produced. Bounded so a 16-launch cast
   * cannot flood the log. */
  if (dst->action_effects.effect_count) {
    static unsigned spawn_reports;
    int16_t camera_x = (int16_t)ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraX);
    int16_t camera_y = (int16_t)ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraY);
    int ground = (int)(ActRaiser_ReadWram16(kActRaiserWram_PlayerPositionY) -
                       camera_y + 16);
    for (uint8_t i = 0;
         i < dst->action_effects.effect_count && spawn_reports < 24u; i++) {
      const ActionEffectInstance *e = &dst->action_effects.effects[i];
      /* Two distinct moments, and confusing them is what made the first pass
       * at this misleading:
       *   CREATE — the actor appears on the player, still (velocity 0).
       *   LAUNCH — the handler has relocated it and given it a velocity. THIS
       *            is the position the catalogue says should be the viewport
       *            top/right edge, and the one to compare against the ground.
       * phase_ticks resets on every phase change, so ==0 is the entry frame. */
      const char *moment = NULL;
      if (e->phase == kActionEffectPhase_StardustPreLaunch && !e->age_ticks)
        moment = "CREATE";
      else if (e->phase == kActionEffectPhase_StardustLaunch &&
               !e->phase_ticks)
        moment = "LAUNCH";
      if (!moment) continue;
      spawn_reports++;
      fprintf(stderr,
              "[action-fx spawn] %s slot=$%04X world=(%d,%d) cam=(%d,%d) "
              "screen=(%d,%d) vel=(%d,%d) age=%u "
              "[viewport top screen_y=0, ground screen_y~%d]\n",
              moment, e->record_address, e->world_x, e->world_y,
              camera_x, camera_y, e->world_x - camera_x, e->world_y - camera_y,
              e->velocity_x, e->velocity_y, e->age_ticks, ground);
    }
  }

  /* Census: an active cohort slot the spell table did not recognise. Magical
   * Fire's rules are measured, but Stardust/Aura/Light were transcribed from
   * the ROM analysis and have never been seen against live WRAM — so rather
   * than render them on a guess, an unrecognised slot prints its exact
   * identity here. One cast of each spell (Cheats > Cycle magic spell) turns
   * these lines into corrected rules in action_effects.c. Rate-limited to one
   * report per controller kind so a 99-tick cast cannot flood the log. */
  if (dst->action_effects.unmatched_count) {
    static uint16_t reported_kinds;
    uint16_t bit = (uint16_t)(1u << (dst->action_effects.controller_kind & 15));
    if (!(reported_kinds & bit)) {
      reported_kinds |= bit;
      for (uint8_t i = 0; i < dst->action_effects.unmatched_count; i++) {
        const ActionEffectUnmatched *u = &dst->action_effects.unmatched[i];
        fprintf(stderr,
                "[action-fx census] spell=%u slot=$%04X unmatched: "
                "anim=$%02X:%04X state=%u visual=%u comp=$%04X "
                "flip=$%04X status=$%04X\n",
                dst->action_effects.controller_kind, u->record_address,
                u->animation_bank, u->animation_address, u->animation_state,
                u->visual, u->composition, u->flip_attributes, u->status);
      }
    }
  }
  dst->magic_cycle_armed = g_settings.cheat_magic_cycle;
  dst->magic_cycle_selected =
      g_settings.cheat_magic_cycle ? ActRaiser_SelectedMagic() : 0;

  /* D2 publishes only the pitch-zero separated-composite capability, and
   * only after its same-frame CPU oracle found zero differing pixels. */
  if (s_pending_annotated_sim) {
    dst->sim = *s_pending_annotated_sim;   /* already annotated this frame */
  } else {
    SimRenderMetadata_CaptureFrame(
        &dst->sim, g_ram, g_settings.sim3d_mode,
        g_settings.sim3d_world_navigation,
        Settings_Sim3DRequestedFeatures(),
        g_settings.sim3d_diagnostic_layers, Sim3D_ImplementedFeatures());
    Sim3DTuning tuning = BuildSim3DTuning();
    Sim3D_AnnotateFrame(&dst->sim, &tuning);
    SimWorldNavigationCapture_Capture(&dst->sim, g_ppu);
    /* Accumulation itself happens once a frame at the always-run site below;
     * this only publishes the current canvas state into the slot. */
    dst->sim.town_canvas_serial = SimTownCanvas_Serial();
    dst->sim.background_voxel_serial = SimBackgroundVoxels_Serial();
  }

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
  dst->obj_apron = kPpuObjApron;
  /* Density-corrected here, at the D6 producer, so present.c consumes a value
   * already expressed in PHYSICAL output pixels (0 = auto passes through). */
  dst->hud_scale_percent =
      Settings_ScalePercentToOutput(g_settings.hud_scale_percent);

  dst->diorama_active = g_diorama_frame_active;
  dst->diorama_plane_request_mask = 0;
  dst->diorama_plane_content_mask = 0;
  dst->diorama_plane_additive_mask = 0;
  if (dst->diorama_active) {
    dst->diorama_plane_request_mask = CaptureDioramaPlaneRequestMask();
    dst->diorama_plane_content_mask = CaptureDioramaPlaneContentMask();
    dst->diorama_plane_additive_mask = CaptureDioramaAdditivePlaneMask();
  }

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
    (float)g_settings.diorama_tilt_x_mrad / (float)kPermilleScale,
    (float)g_settings.diorama_tilt_y_mrad / (float)kPermilleScale,
    (float)g_settings.diorama_distance_x100 / (float)kPercentScale,
  };
  dst->diorama_dyncam_baseline = (DioramaCameraPose){
    (float)g_settings.diorama_dyncam_baseline_tilt_x_mrad /
        (float)kPermilleScale,
    (float)g_settings.diorama_dyncam_baseline_tilt_y_mrad /
        (float)kPermilleScale,
    (float)g_settings.diorama_dyncam_baseline_distance_x100 /
        (float)kPercentScale,
  };
  Diorama_GetDynamicCameraOrbit(&dst->diorama_manual_orbit_yaw,
                                &dst->diorama_manual_orbit_pitch);
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
    /* Fix B: from the latch, NOT g_ppu — see the field comment in present.h and
     * the latch in ActRaiserDrawPpuFrame. */
    {
      int live_left = 0, live_right = 0;
      ActRaiser_LiveMargins(&live_left, &live_right);
      dst->extra_left_cur = (uint8_t)live_left;
      dst->extra_right_cur = (uint8_t)live_right;
      ActRaiser_LiveActionBgPlan(&dst->action_bg_plan,
                                 &dst->bg_capture_pad_to_budget);
      dst->action_bg_extent_guides = ActionBgTuner_GuidesEnabled();
    }
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
    _Static_assert(kFrameSlotOverlayFlag_MarkFullAddSubscreen ==
                   kPpuOverlayFlag_MarkFullAddSubscreen,
                   "present.h's mirrored full-add flag must match ppu.h");
    dst->action_bg2_mask_valid =
        !dst->diorama_active &&
        (g_ppu->overlayCaptures[kPpuOverlaySource_Bg2].flags &
         kPpuOverlayFlag_MarkMainScreenWinner) != 0 &&
        PpuOverlaySurfaceHasContent(g_ppu, kPpuOverlaySource_Bg2, 0);
    /* IJ1: this one is load-bearing arithmetic, not just a layout mirror — it
     * is the denominator that normalizes every U-axis offset into the layer
     * textures. Dividing by snes_width instead cost 1.75x too much horizontal
     * interpolation shift. */
    _Static_assert(kFrameSlotLayerTextureWidth == kPpuSurfaceWidth,
                   "present.h's mirrored layer texture width must match ppu.h");
    _Static_assert(kFrameSlotLayerTextureHeight == kPpuBufHeight,
                   "present.h's mirrored layer texture height must match ppu.h");
    _Static_assert(kFrameSlotAuthenticWidth == kActRaiserAuthenticWidth,
                   "present.h's mirrored authentic width must match "
                   "actraiser_game.h");
    _Static_assert(kFrameSlotAuthenticHeight == kActRaiserAuthenticHeight,
                   "present.h's mirrored authentic height must match "
                   "actraiser_game.h");
    for (int i = 0; i < kFrameSlotOverlaySourceCount; i++) {
      const PpuOverlayCapture *src = &g_ppu->overlayCaptures[i];
      FrameSlotOverlayCapture *d = &dst->overlay_captures[i];
      d->x0 = src->x0; d->x1 = src->x1;
      d->y0 = src->y0; d->y1 = src->y1;
      d->flags = src->flags;
      d->oamFirst = src->oamFirst; d->oamCount = src->oamCount;
    }

    ActRaiser_HudObjIconRange(&dst->hud_icon_first, &dst->hud_icon_count);

    /* Only needed when an OBJ overlay/HUD icon is active this frame (§2.8
     * cost note). */
    if (g_ppu->overlayCaptures[kPpuOverlaySource_Obj].oamCount ||
        dst->hud_icon_count) {
      _Static_assert(sizeof(dst->oam) == sizeof(g_ppu->oam), "oam size (D18)");
      _Static_assert(sizeof(dst->high_oam) == sizeof(g_ppu->highOam),
                     "highOam size (D18)");
      memcpy(dst->oam, g_ppu->oam, sizeof(dst->oam));
      memcpy(dst->high_oam, g_ppu->highOam, sizeof(dst->high_oam));
      dst->oam_valid = true;
    }

    if (dst->diorama_active && dst->interp_setting_enabled &&
        g_ppu->overlayCaptures[kPpuOverlaySource_Obj].oamCount == 128) {
      const uint8_t *obj_planes[4] = {0};
      uint8_t obj_content = 0;
      for (unsigned priority = 0; priority < 4; priority++) {
        const int plane = DioramaPlaneForObjectPriority(priority);
        obj_planes[priority] = g_diorama_layer_pixels[plane];
        if (dst->diorama_plane_content_mask & (1u << (unsigned)plane))
          obj_content |= (uint8_t)(1u << priority);
      }
      const uint8_t excluded_count =
          dst->diorama_hud_flat ? dst->hud_icon_count : 0;
      ActionObjInterpolation_BuildFrame(
          g_ppu, &dst->action_obj_interpolation,
          obj_planes, obj_content,
          dst->snes_width + dst->obj_apron * 2,
          dst->snes_height + dst->ws_extra_top + dst->ws_extra_bottom,
          dst->ws_extra + dst->obj_apron, dst->ws_extra_top,
          dst->hud_icon_first, excluded_count, dst->timestamp_ns);
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
