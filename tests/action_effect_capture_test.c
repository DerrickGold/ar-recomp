#include "action/action_effect_capture.h"

#include "support/test_assert.h"
#include <stdio.h>

#include "action/action_effect_clock.h"
#include "actraiser_game.h"
#include "app/settings.h"
#include "diorama/diorama_layer_order.h"
#include "present/frame_timing.h"
#include "present/present.h"

Settings g_settings;
uint8 g_ram[kActRaiserWramSize];
static FrameSlot frame;
static ActionSceneEffectFrame scene;
static ActionEffectObserver *seen_observer;
static unsigned expected_ticks, spell_calls, scene_calls, publications;
static unsigned environment_calls;
static uint8_t published_group, published_map, published_section;

void ActionEffects_CaptureFrame(ActionEffectObserver *observer, ActionEffectFrame *dst,
    const uint8_t *wram, size_t size, unsigned ticks) {
  assert(wram == g_ram && size == sizeof(g_ram) && ticks == expected_ticks);
  if (!seen_observer) seen_observer = observer;
  assert(observer == seen_observer);
  /* The same observer survives successive captures and is shared with scene
   * observation, even while lighting and particles are disabled. */
  assert(observer->next_generation == spell_calls);
  observer->next_generation = ++spell_calls;
  *dst = (ActionEffectFrame){.game_frame = g_ram[kActRaiserWram_GameFrame]};
}
void ActionSceneEffects_CaptureFrame(ActionEffectObserver *observer, ActionSceneEffectFrame *dst,
    const uint8_t *wram, size_t size, unsigned ticks) {
  assert(wram == g_ram && size == sizeof(g_ram) && ticks == expected_ticks);
  assert(observer == seen_observer && observer->next_generation == spell_calls);
  scene_calls++;
  assert(spell_calls == scene_calls);
  *dst = scene;
}
void ActionEnvironmentalEffects_CaptureFrame(ActionEffectObserver *observer,
    ActionSceneEffectFrame *dst, const uint8_t *wram, size_t size) {
  assert(observer == seen_observer && dst == &frame.action_scene_effects);
  assert(wram == g_ram && size == sizeof(g_ram));
  assert(g_settings.action_environmental_effects);
  environment_calls++;
}
void Diorama_PublishLiveLayerSection(uint8_t group, uint8_t map, uint8_t section) {
  assert(scene_calls == spell_calls);
  assert(frame.diorama_map_group == group && frame.diorama_map_number == map);
  assert(frame.diorama_layer_section == section);
  published_group = group;
  published_map = map;
  published_section = section;
  publications++;
}

static void Capture(unsigned ticks, uint8_t section) {
  expected_ticks = ticks;
  const unsigned previous_calls = spell_calls;
  const unsigned previous_environment_calls = environment_calls;
  ActionEffectCapture_CaptureFrame(&frame);
  assert(spell_calls == previous_calls + 1 && publications == spell_calls);
  assert(frame.action_effects.game_frame == g_ram[kActRaiserWram_GameFrame]);
  assert(frame.action_scene_effects.decoration_count ==
      (g_settings.action_environmental_effects ? scene.decoration_count : 0));
  assert(frame.action_scene_effects.decoration_visible_count ==
      (g_settings.action_environmental_effects ? scene.decoration_visible_count : 0));
  assert(frame.action_scene_effects.effect_count == scene.effect_count);
  assert(frame.action_effect_lighting == g_settings.action_effect_lighting);
  assert(frame.action_effect_particles == g_settings.action_effect_particles);
  assert(frame.action_environmental_effects == g_settings.action_environmental_effects);
  assert(environment_calls == previous_environment_calls +
      (g_settings.action_environmental_effects ? 1 : 0));
  assert(published_group == g_ram[kActRaiserWram_MapGroup]);
  assert(published_map == g_ram[kActRaiserWram_CurrentMap]);
  assert(published_section == section);
  assert(frame.timestamp_ns == 123 && frame.snes_width == 320 && frame.sim.master_enabled);
}

int main(void) {
  frame.timestamp_ns = 123;
  frame.snes_width = 320;
  frame.sim.master_enabled = true;
  g_ram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Aitos;
  g_ram[kActRaiserWram_CurrentMap] = 2;
  g_settings.action_effect_lighting = true;
  g_settings.action_effect_particles = false;
  g_settings.action_environmental_effects = true;
  for (int i = 0; i < 5; i++) ActionEffectGameplayClock_CompletePass();
  Capture(0, kDioramaLayerSection_Room); /* First capture seeds the clock. */
  /* Native pause advances emulated vblanks without completing gameplay/OAM.
   * Both it and repeated host-paused captures must supply a zero delta. */
  g_ram[kActRaiserWram_GameFrame] = 99;
  Capture(0, kDioramaLayerSection_Room);
  Capture(0, kDioramaLayerSection_Room);
  for (int i = 0; i < 3; i++) ActionEffectGameplayClock_CompletePass();
  scene.decoration_count = 2;
  scene.decorations[0].kind = kActionEffect_WallTorch;
  scene.decorations[1].kind = kActionEffect_AitosWaterfall;
  Capture(3, kDioramaLayerSection_AitosWaterfall);
  /* Off removes only ambient accents, keeps actor effects and still selects
   * the waterfall's authored layout. Re-enabling restores the current list. */
  scene.effect_count = scene.visible_count = 1;
  scene.decoration_visible_count = 2;
  seen_observer->landing_dust.valid = 1;
  seen_observer->landing_dust.puffs[0].active = 1;
  g_settings.action_environmental_effects = false;
  Capture(0, kDioramaLayerSection_AitosWaterfall);
  assert(!seen_observer->landing_dust.valid && !seen_observer->landing_dust.puffs[0].active);
  g_settings.action_environmental_effects = true;
  Capture(0, kDioramaLayerSection_AitosWaterfall);
  scene.decoration_overflow = true;
  Capture(0, kDioramaLayerSection_Room); /* Partial capture cannot select an override. */
  scene.decoration_overflow = false;
  scene.decoration_count = 1;
  Capture(0, kDioramaLayerSection_Room);
  scene.decoration_count = 0;
  g_ram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  g_ram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Fillmore;
  Capture(0, kDioramaLayerSection_Room); /* Do not retain a prior room's section. */
  g_settings.action_effect_lighting = false;
  g_settings.action_effect_particles = true;
  for (int i = 0; i < kFrameTimingMaximumElapsedTicks + 10; i++)
    ActionEffectGameplayClock_CompletePass();
  Capture(kFrameTimingMaximumElapsedTicks, kDioramaLayerSection_Room);
  g_settings.action_effect_particles = false;
  ActionEffectGameplayClock_CompletePass();
  Capture(1, kDioramaLayerSection_Room);
  puts("Action capture: shared gameplay clock, pause, settings and section publication passed");
  return 0;
}
