#include "action/action_effect_capture.h"

#include <stdio.h>

#include "action/action_effect_clock.h"
#include "action/action_effects.h"
#include "actraiser_game.h"
#include "app/settings.h"
#include "diorama/diorama.h"
#include "diorama/diorama_layer_order.h"
#include "present/present.h"

/* Capture history follows this running game, independently of GPU resources.
 * Only completed gameplay/OAM passes advance it; native pause and host-paused
 * redraws do not age effects. Both spell and scene capture share one delta. */
static ActionEffectObserver s_action_effect_observer;
static ActionEffectTickClock s_action_effect_tick_clock;

static void ReportCapturedActionEffects(const FrameSlot *dst) {
  /* Capture-side twin of present_action_effects.c's "[action-fx] first spell geometry
   * submitted". Together the two lines localise any future silence: neither
   * means no spell was ever identified in WRAM, capture-only means the
   * identification works but the renderer never drew it. Chasing that
   * distinction by hand is what exposed an incorrectly wide animation-bank
   * read. */
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

  /* Census: print the exact identity of active cohort slots the spell table
   * did not recognise. These slots remain unrendered; the report supplies
   * evidence for correcting the rules in action_effects.c. Rate-limit to one
   * report per controller kind so a long cast cannot flood the log. */
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
}

void ActionEffectCapture_CaptureFrame(FrameSlot *dst) {
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
  ReportCapturedActionEffects(dst);
}
