#include "actraiser/actraiser_room_profiles.h"
#include <stdio.h>
#include <string.h>

#include "action_effect_clock.h"
#include "action_effects.h"
#include "action_landing_dust.h"
#include "action_water_field.h"
#include "action_bg_plan.h"
#include "action_bg_world.h"
#include "actraiser_game.h"
#include "present/frame_timing.h"

static int s_failures;

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      fprintf(stderr, "%s:%d: check failed: %s\\n", __FILE__, __LINE__, #condition);               \
      s_failures++;                                                                                \
    }                                                                                              \
  } while (0)

static void Write16(uint8_t *wram, size_t address, uint16_t value) {
  wram[address] = (uint8_t)value;
  wram[address + 1] = (uint8_t)(value >> 8);
}

/* Raw offsets are deliberate here: this fixture is an independent assertion
 * of the reverse-engineered WRAM contract, not a tautology built from the
 * production field enum. */
static void SeedFireSlot(uint8_t *wram, unsigned slot, uint16_t visual) {
  static const uint16_t kFlips[] = {0x0000, 0x4000, 0x8000, 0xC000};
  size_t address = kActRaiserWram_ActionObjectTable + slot * kActRaiserActionObjectStride;
  Write16(wram, address + 0x00, 0x0000);
  Write16(wram, address + 0x02, (uint16_t)(100 + slot * 10));
  Write16(wram, address + 0x04, (uint16_t)(80 + slot * 5));
  Write16(wram, address + 0x06, (uint16_t)(int16_t)(-2 + (int)slot));
  Write16(wram, address + 0x08, (uint16_t)(int16_t)(3 - (int)slot));
  Write16(wram, address + 0x0A, (kFlips[slot] & 0x4000) ? 8 : 44);
  Write16(wram, address + 0x0C, (kFlips[slot] & 0x8000) ? 30 : 29);
  Write16(wram, address + 0x0E, (kFlips[slot] & 0x4000) ? 44 : 8);
  Write16(wram, address + 0x10, (kFlips[slot] & 0x8000) ? 29 : 30);
  Write16(wram, address + 0x16, 0xC000);
  /* +$18 is the animation BANK BYTE and +$19 is the record's base OAM
   * attribute byte — a distinct field. Seeding a full word of $0007 here is
   * what let the shipped 16-bit read pass its own test while never matching
   * live WRAM, which stores $07 then $39 (bank $07, attributes $39). Both
   * bytes are written independently so the fixture asserts the real layout. */
  wram[address + 0x18] = 0x07;
  wram[address + 0x19] = 0x39;
  Write16(wram, address + 0x1A, visual <= 12 ? 2 : 3);
  Write16(wram, address + 0x1C, slot);
  Write16(wram, address + 0x20, (uint16_t)(0xD000 + slot * 2));
  Write16(wram, address + 0x22, visual);
  /* Live records keep the current OAM attribute byte at +$29 and leave +$28
   * zero; the flip bits are the top two bits of that byte. */
  Write16(wram, address + 0x28, (uint16_t)(kFlips[slot] | 0x3900));
}

static void SeedFireCast(uint8_t *wram, uint16_t visual) {
  memset(wram, 0, kActRaiserWramSize);
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Fillmore;
  Write16(wram, kActRaiserWram_GameFrame, 120);
  Write16(wram, kActRaiserWram_MagicController + 0x00, 0x0000);
  Write16(wram, kActRaiserWram_MagicController + 0x38, 1);
  for (unsigned slot = 0; slot < 4; slot++)
    SeedFireSlot(wram, slot, visual);
}

static void TestControllerAndSlotIdentity(void) {
  uint8_t wram[kActRaiserWramSize];
  ActionEffectFrame frame;
  ActionEffectObserver observer = {0};
  memset(wram, 0, sizeof(wram));

  ActionEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);

  SeedFireCast(wram, 13);
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  ActionEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);

  /* The controller kind, not the animation pointer, is what names a spell:
   * Fire and Stardust genuinely share bank $07:C000. But sharing a bank is not
   * enough to BE that spell — Stardust's stages are exact (state 0/visual 0 in
   * flight, state 1/visuals 1-4 bursting), so Fire-shaped records under kind 2
   * match nothing and are censused rather than mislabelled as flying stars.
   * This is the property an earlier catch-all rule gave away. */
  SeedFireCast(wram, 13);
  Write16(wram, kActRaiserWram_MagicController + 0x38, 2);
  ActionEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.controller_kind == 2);
  CHECK(frame.effect_count == 0);
  CHECK(frame.unmatched_count == 4);

  SeedFireCast(wram, 13);
  Write16(wram, kActRaiserWram_MagicController + 0x00, 0x4000);
  ActionEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);

  SeedFireCast(wram, 13);
  Write16(wram, 0x06A0 + 0x16, 0xC800);
  ActionEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 3);
  CHECK(frame.effects[0].record_address == 0x06E0);

  SeedFireCast(wram, 13);
  Write16(wram, 0x06A0 + 0x1A, 4);
  ActionEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 3);

  SeedFireCast(wram, 13);
  Write16(wram, 0x06A0 + 0x00, 0x4000);
  ActionEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 3);

  SeedFireCast(wram, 13);
  Write16(wram, 0x06A0 + 0x28, 0x4000);
  ActionEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 3);
}

static void TestCapturedFieldsAndGeometry(void) {
  uint8_t wram[kActRaiserWramSize];
  ActionEffectFrame frame;
  ActionEffectObserver observer = {0};
  SeedFireCast(wram, 8);
  Write16(wram, 0x0720 + 0x00, kActRaiserObjectStatus_NoDraw);

  ActionEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.game_frame == 120);
  CHECK(frame.controller_kind == 1);
  CHECK(frame.effect_count == 4);
  CHECK(frame.visible_count == 3);
  CHECK(frame.effects[0].kind == kActionEffect_MagicalFire);
  CHECK(frame.effects[0].phase == kActionEffectPhase_FireIgnition);
  CHECK(frame.effects[0].world_x == 100);
  CHECK(frame.effects[0].world_y == 80);
  CHECK(frame.effects[0].velocity_x == -2);
  CHECK(frame.effects[0].velocity_y == 3);
  CHECK(frame.effects[0].left_extent == 44);
  CHECK(frame.effects[0].top_extent == 29);
  CHECK(frame.effects[0].right_extent == 8);
  CHECK(frame.effects[0].bottom_extent == 30);
  CHECK(frame.effects[0].composition == 0xD000);
  CHECK(frame.effects[0].visual == 8);
  CHECK(frame.effects[0].animation_state == 2);
  CHECK(frame.effects[0].animation_index == 0);
  CHECK(frame.effects[0].obj_priority == 0);
  CHECK(frame.effects[0].render_layer == kActionEffectRenderLayer_WorldOverlay);
  CHECK(frame.effects[0].geometry.kind == kActionEffectGeometry_Rect);
  CHECK(frame.effects[0].geometry.data.rect.x0 == -44.0f);
  CHECK(frame.effects[0].geometry.data.rect.y0 == -29.0f);
  CHECK(frame.effects[0].geometry.data.rect.x1 == 8.0f);
  CHECK(frame.effects[0].geometry.data.rect.y1 == 30.0f);
  CHECK(frame.effects[0].flags == kActionEffectFlag_Visible);
  CHECK((frame.effects[1].flags & kActionEffectFlag_FlipHorizontal) != 0);
  CHECK((frame.effects[2].flags & kActionEffectFlag_FlipVertical) != 0);
  CHECK((frame.effects[2].flags & kActionEffectFlag_Visible) == 0);
  CHECK((frame.effects[3].flags & kActionEffectFlag_FlipHorizontal) != 0);
  CHECK((frame.effects[3].flags & kActionEffectFlag_FlipVertical) != 0);
}

static void TestLifecycleUsesProducerTicks(void) {
  uint8_t wram[kActRaiserWramSize];
  ActionEffectFrame first, paused, advanced, changed, restarted;
  ActionEffectObserver observer = {0};
  SeedFireCast(wram, 8);

  ActionEffects_CaptureFrame(&observer, &first, wram, sizeof(wram), 1);
  CHECK(first.effects[0].age_ticks == 0);
  CHECK(first.effects[0].phase_ticks == 0);
  CHECK(first.effects[0].pulse_ticks == 0);
  CHECK(first.effects[0].generation != 0);
  CHECK(first.effects[0].pulse_generation != 0);

  ActionEffects_CaptureFrame(&observer, &paused, wram, sizeof(wram), 0);
  CHECK(paused.effects[0].age_ticks == 0);
  CHECK(paused.effects[0].generation == first.effects[0].generation);

  ActionEffects_CaptureFrame(&observer, &advanced, wram, sizeof(wram), 3);
  CHECK(advanced.effects[0].age_ticks == 3);
  CHECK(advanced.effects[0].phase_ticks == 3);
  CHECK(advanced.effects[0].pulse_ticks == 3);
  CHECK(advanced.effects[0].generation == first.effects[0].generation);

  for (unsigned slot = 0; slot < 4; slot++)
    SeedFireSlot(wram, slot, 13);
  ActionEffects_CaptureFrame(&observer, &changed, wram, sizeof(wram), 2);
  CHECK(changed.effects[0].age_ticks == 5);
  CHECK(changed.effects[0].phase_ticks == 0);
  CHECK(changed.effects[0].pulse_ticks == 5);
  CHECK(changed.effects[0].generation == first.effects[0].generation);
  CHECK(changed.effects[0].pulse_generation == first.effects[0].pulse_generation);

  Write16(wram, kActRaiserWram_MagicController + 0x38, 0);
  ActionEffects_CaptureFrame(&observer, &paused, wram, sizeof(wram), 1);
  CHECK(paused.effect_count == 0);
  Write16(wram, kActRaiserWram_MagicController + 0x38, 1);
  ActionEffects_CaptureFrame(&observer, &restarted, wram, sizeof(wram), 1);
  CHECK(restarted.effects[0].age_ticks == 0);
  CHECK(restarted.effects[0].generation != first.effects[0].generation);

  ActionEffectObserver_Reset(&observer);
  ActionEffects_CaptureFrame(&observer, &restarted, wram, sizeof(wram), 1);
  CHECK(restarted.effects[0].age_ticks == 0);
  CHECK(restarted.effects[0].phase_ticks == 0);
  CHECK(restarted.effects[0].pulse_ticks == 0);
}

static void TestGameplayTickClockTracksCompletedPasses(void) {
  ActionEffectTickClock clock = {0};
  const uint32_t initial_serial = ActionEffectGameplayClock_Serial();

  CHECK(ActionEffectTickClock_Capture(&clock) == 0);
  /* Native pause continues emulated frames but completes no $00:8C98 pass. */
  CHECK(ActionEffectTickClock_Capture(&clock) == 0);
  CHECK(ActionEffectGameplayClock_Serial() == initial_serial);

  ActionEffectGameplayClock_CompletePass();
  CHECK(ActionEffectGameplayClock_Serial() == initial_serial + 1u);
  CHECK(ActionEffectTickClock_Capture(&clock) == 1);

  ActionEffectGameplayClock_CompletePass();
  ActionEffectGameplayClock_CompletePass();
  CHECK(ActionEffectTickClock_Capture(&clock) == 2);

  for (unsigned i = 0; i < kFrameTimingMaximumElapsedTicks + 3u; i++)
    ActionEffectGameplayClock_CompletePass();
  CHECK(ActionEffectTickClock_Capture(&clock) == kFrameTimingMaximumElapsedTicks);

  ActionEffectTickClock_Reset(&clock);
  CHECK(ActionEffectTickClock_Capture(&clock) == 0);
  ActionEffectTickClock_Reset(NULL);
  CHECK(ActionEffectTickClock_Capture(NULL) == 0);
}

static void TestMalformedInputsFailClosed(void) {
  uint8_t tiny[8] = {0};
  ActionEffectFrame frame;
  ActionEffectObserver observer = {0};
  memset(&frame, 0xFF, sizeof(frame));
  ActionEffects_CaptureFrame(&observer, &frame, tiny, sizeof(tiny), 1);
  CHECK(frame.effect_count == 0);
  CHECK(frame.visible_count == 0);
  memset(&frame, 0xFF, sizeof(frame));
  ActionEffects_CaptureFrame(NULL, &frame, tiny, sizeof(tiny), 1);
  CHECK(frame.effect_count == 0);
  CHECK(frame.visible_count == 0);
  ActionEffects_CaptureFrame(&observer, NULL, tiny, sizeof(tiny), 1);
  ActionEffectObserver_Reset(NULL);
}

/* Regression: replay a real Magical Fire record byte-for-byte, straight out of
 * runs/20260803-162833/snapshots/snap_00_gf1913.wram.bin (Fillmore act 1,
 * game frame 1913, four fire parts alive on screen). The synthetic fixtures
 * above all agreed with the code rather than the game, so nothing caught the
 * 16-bit read of the animation-bank BYTE at +$18: live WRAM holds $07 there
 * and $39 (base OAM attributes) at +$19, so the word read yielded $3907, the
 * identity test rejected every part, and no spell ever reached the renderer.
 * Keep these bytes verbatim — their value is that no one chose them. */
static void TestLiveWramRecordIsRecognized(void) {
  static const uint8_t kLiveFireRecord[0x40] = {
      0x00, 0x00, 0xF6, 0x01, 0xDF, 0x01, 0x04, 0x00, 0x02, 0x00, 0x2C, 0x00, 0x10,
      0x00, 0x08, 0x00, 0x09, 0x00, 0xB8, 0xA0, 0x00, 0x00, 0x00, 0xC0, 0x07, 0x39,
      0x03, 0x00, 0x05, 0x00, 0x00, 0x00, 0x52, 0xC3, 0x12, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x39, 0x01, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x11, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  uint8_t wram[kActRaiserWramSize];
  ActionEffectFrame frame;
  ActionEffectObserver observer = {0};

  memset(wram, 0, sizeof(wram));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Fillmore;
  Write16(wram, kActRaiserWram_GameFrame, 1913);
  Write16(wram, kActRaiserWram_MagicController + 0x00, 0x0800);
  Write16(wram, kActRaiserWram_MagicController + 0x38, 0x0001);
  memcpy(wram + kActRaiserWram_ActionObjectTable, kLiveFireRecord, sizeof(kLiveFireRecord));

  ActionEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.game_frame == 1913);
  CHECK(frame.controller_kind == 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.visible_count == 1);
  CHECK(frame.effects[0].kind == kActionEffect_MagicalFire);
  CHECK(frame.effects[0].phase == kActionEffectPhase_FireBloom);
  CHECK(frame.effects[0].world_x == 0x01F6);
  CHECK(frame.effects[0].world_y == 0x01DF);
  CHECK(frame.effects[0].visual == 0x12);
  CHECK(frame.effects[0].left_extent == 44);
  CHECK(frame.effects[0].top_extent == 16);
  CHECK(frame.effects[0].right_extent == 8);
  CHECK(frame.effects[0].bottom_extent == 9);
  CHECK((frame.effects[0].flags & kActionEffectFlag_Visible) != 0);
  CHECK((frame.effects[0].flags & kActionEffectFlag_FlipHorizontal) == 0);
  CHECK((frame.effects[0].flags & kActionEffectFlag_FlipVertical) == 0);
}

/* Generic cohort seeding for the spells whose rules are transcribed rather
 * than measured. Raw offsets on purpose, same as SeedFireSlot: the fixture is
 * an independent statement of the WRAM contract, not a mirror of the field
 * enum the production code uses. */
static void SeedSlot(uint8_t *wram, unsigned cohort, uint16_t animation, uint8_t bank,
                     uint16_t state, uint16_t visual, uint16_t flips) {
  size_t address = kActRaiserWram_ActionObjectTable + cohort * kActRaiserActionObjectStride;
  Write16(wram, address + 0x00, 0x0000); /* active */
  Write16(wram, address + 0x02, (uint16_t)(200 + cohort * 30));
  Write16(wram, address + 0x04, (uint16_t)(150 + cohort * 10));
  Write16(wram, address + 0x0A, 12);
  Write16(wram, address + 0x0C, 12);
  Write16(wram, address + 0x0E, 12);
  Write16(wram, address + 0x10, 12);
  Write16(wram, address + 0x16, animation);
  wram[address + 0x18] = bank;
  wram[address + 0x19] = 0x39; /* the separate byte at +$19 */
  Write16(wram, address + 0x1A, state);
  Write16(wram, address + 0x22, visual);
  Write16(wram, address + 0x20, 0xD100); /* composition must be set */
  Write16(wram, address + 0x28, flips);
}

static void BeginCast(uint8_t *wram, uint16_t controller_kind) {
  memset(wram, 0, kActRaiserWramSize);
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Fillmore;
  Write16(wram, kActRaiserWram_MagicController + 0x00, 0x0000);
  Write16(wram, kActRaiserWram_MagicController + 0x38, controller_kind);
}

/* Every spell the catalogue documents must be positively identified, with the
 * right kind, phase and role. Fire is pinned elsewhere by real captured bytes;
 * these three are pinned to the ROM analysis they were transcribed from, so a
 * later correction from the live census has to update the test with it. */
static void TestEverySpellIsIdentified(void) {
  uint8_t wram[kActRaiserWramSize];
  ActionEffectFrame frame;
  ActionEffectObserver observer = {0};

  /* 2 Stardust, MEASURED from runs/20260805-073012: a star in flight is
   * state 0 / visual 0 carrying velocity (-8,+8); a burst is state 1 over
   * visuals 1..4. Both stages must be told apart, because they are styled as
   * different substances — a burning projectile and the cold sparkle it
   * detonates into — and only the flight stage is oriented to its heading. */
  BeginCast(wram, 2);
  for (unsigned slot = 0; slot < 4; slot++)
    SeedSlot(wram, slot, 0xC000, 0x07, 1, 3, 0x0000);
  ActionEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 4);
  CHECK(frame.effects[0].kind == kActionEffect_MagicalStardust);
  CHECK(frame.effects[0].phase == kActionEffectPhase_StardustBurst);
  CHECK(frame.effects[0].role == kActionEffectRole_Body);
  CHECK(frame.unmatched_count == 0);

  BeginCast(wram, 2);
  SeedSlot(wram, 0, 0xC000, 0x07, 0, 0, 0x0000);
  /* The measured 45-degree descent, which the renderer turns the comet body
   * and the flame trail to face. */
  Write16(wram, kActRaiserWram_ActionObjectTable + 0x06, (uint16_t)-8);
  Write16(wram, kActRaiserWram_ActionObjectTable + 0x08, 8);
  ActionEffectObserver_Reset(&observer);
  ActionEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].phase == kActionEffectPhase_StardustLaunch);
  CHECK(frame.effects[0].velocity_x == -8);
  CHECK(frame.effects[0].velocity_y == 8);
  CHECK(frame.unmatched_count == 0);

  /* The SAME state and visual with zero velocity is the pre-launch actor,
   * measured at spawn sitting on the player at world (308,520) before the
   * launch handler relocates it to the viewport edge. Motion is the only
   * discriminator, and getting it wrong is what drew a comet at the player's
   * feet ("stardust spawning in the ground"). It must still be IDENTIFIED, so
   * that a genuinely unknown stage is what reaches the census. */
  BeginCast(wram, 2);
  SeedSlot(wram, 0, 0xC000, 0x07, 0, 0, 0x0000);
  Write16(wram, kActRaiserWram_ActionObjectTable + 0x06, 0);
  Write16(wram, kActRaiserWram_ActionObjectTable + 0x08, 0);
  ActionEffectObserver_Reset(&observer);
  ActionEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].phase == kActionEffectPhase_StardustPreLaunch);
  CHECK(frame.unmatched_count == 0);

  /* 3 Aura: four flip combinations, $07:C800, state 3, visuals 10/11. */
  BeginCast(wram, 3);
  static const uint16_t kAuraFlips[] = {0x0000, 0x4000, 0x8000, 0xC000};
  for (unsigned slot = 0; slot < 4; slot++)
    SeedSlot(wram, slot, 0xC800, 0x07, 3, 10 + (slot & 1), kAuraFlips[slot]);
  ActionEffectObserver_Reset(&observer);
  ActionEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 4);
  CHECK(frame.effects[0].kind == kActionEffect_MagicalAura);
  CHECK(frame.effects[0].phase == kActionEffectPhase_AuraOrb);

  /* 4 Light: centre $07A0 and the two mirrored columns $07E0/$0820. The role
   * split is the whole point — the centre flare and the beams are styled
   * separately and must never be merged. */
  BeginCast(wram, 4);
  SeedSlot(wram, 4, 0xC800, 0x07, 1, 7, 0x0000); /* centre, visuals 5-9 */
  SeedSlot(wram, 5, 0xC800, 0x07, 1, 2, 0x0000); /* column, visuals 1-4 */
  SeedSlot(wram, 6, 0xC800, 0x07, 1, 2, 0x4000); /* mirrored column */
  ActionEffectObserver_Reset(&observer);
  ActionEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 3);
  CHECK(frame.effects[0].role == kActionEffectRole_Centre);
  CHECK(frame.effects[0].phase == kActionEffectPhase_LightFlare);
  CHECK(frame.effects[1].role == kActionEffectRole_Column);
  CHECK(frame.effects[1].phase == kActionEffectPhase_LightBeam);
  /* A column outside the beam visuals is the pre-beam stage, which must be
   * identified (so it can be drawn dim) rather than dropped. */
  SeedSlot(wram, 5, 0xC800, 0x07, 1, 12, 0x0000);
  ActionEffectObserver_Reset(&observer);
  ActionEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effects[1].phase == kActionEffectPhase_LightBeamCharge);
}

/* An active slot the table does not describe must be REPORTED, not silently
 * dropped and not rendered on a guess. This is the mechanism that makes the
 * transcribed rules self-correcting against a real cast. */
static void TestUnmatchedSlotsAreCensused(void) {
  uint8_t wram[kActRaiserWramSize];
  ActionEffectFrame frame;
  ActionEffectObserver observer = {0};

  /* Fire's controller, but one slot running an animation nobody declared. */
  SeedFireCast(wram, 13);
  SeedSlot(wram, 2, 0xB000, 0x05, 9, 99, 0x8000);
  ActionEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 3);
  CHECK(frame.unmatched_count == 1);
  CHECK(frame.unmatched[0].record_address == 0x0720);
  CHECK(frame.unmatched[0].animation_address == 0xB000);
  CHECK(frame.unmatched[0].animation_bank == 0x05);
  CHECK(frame.unmatched[0].visual == 99);

  /* An entirely unknown spell ID renders nothing but still censuses every
   * live slot, which is what a not-yet-mapped spell should look like. */
  BeginCast(wram, 9);
  for (unsigned slot = 0; slot < 3; slot++)
    SeedSlot(wram, slot, 0xC000, 0x07, 1, 3, 0x0000);
  ActionEffectObserver_Reset(&observer);
  ActionEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  CHECK(frame.visible_count == 0);
  CHECK(frame.unmatched_count == 3);
}

static void SeedMeasuredSceneObject(uint8_t *wram, unsigned slot, bool lightning) {
  size_t address = kActRaiserWram_ActionObjectTable + slot * kActRaiserActionObjectStride;
  Write16(wram, address + 0x00, 0x0000);
  Write16(wram, address + 0x02, lightning ? 1376 : 703);
  Write16(wram, address + 0x04, lightning ? 888 : 576);
  Write16(wram, address + 0x06, lightning ? 0 : 3);
  Write16(wram, address + 0x08, 0);
  Write16(wram, address + 0x0A, lightning ? 0 : 8);
  Write16(wram, address + 0x0C, lightning ? 88 : 8);
  Write16(wram, address + 0x0E, 8);
  Write16(wram, address + 0x10, lightning ? 88 : 8);
  Write16(wram, address + 0x12, lightning ? 0x8683 : 0xBDF0);
  Write16(wram, address + 0x16, 0x4000);
  wram[address + 0x18] = 0x7E;
  Write16(wram, address + 0x1A, lightning ? 0x0014 : 0x0023);
  Write16(wram, address + 0x1C, lightning ? 1 : 2);
  Write16(wram, address + 0x1E, lightning ? 0xBD69 : 0xBDD9);
  Write16(wram, address + 0x20, lightning ? 0x46FE : 0x4610);
  Write16(wram, address + 0x22, lightning ? 0x001F : 0x0018);
  Write16(wram, address + 0x30, 0x0020);
  Write16(wram, address + 0x32, lightning ? 0xBD2A : 0xBD84);
}

static void SeedMarahnaFireball(uint8_t *wram, unsigned slot, int16_t world_x, int16_t world_y) {
  const size_t address = kActRaiserWram_ActionObjectTable + slot * kActRaiserActionObjectStride;
  Write16(wram, address + 0x00, 0x0000);
  Write16(wram, address + 0x02, (uint16_t)world_x);
  Write16(wram, address + 0x04, (uint16_t)world_y);
  Write16(wram, address + 0x06, 0xFFFF);
  Write16(wram, address + 0x08, 0);
  Write16(wram, address + 0x0A, 0x08);
  Write16(wram, address + 0x0C, 0x08);
  Write16(wram, address + 0x0E, 0x08);
  Write16(wram, address + 0x10, 0x08);
  Write16(wram, address + 0x12, 0x8661);
  Write16(wram, address + 0x16, 0x4000);
  wram[address + 0x18] = 0x7E;
  Write16(wram, address + 0x1A, 0x000C);
  Write16(wram, address + 0x1C, 2);
  Write16(wram, address + 0x1E, 0xE061);
  Write16(wram, address + 0x20, 0x4528);
  Write16(wram, address + 0x22, 0x0008);
  Write16(wram, address + 0x30, 0x0020);
  Write16(wram, address + 0x32, 0xE047);
}

static void SetMarahnaFireballOrbFrame(uint8_t *wram, unsigned slot, uint16_t visual,
                                       uint16_t composition, int16_t velocity_x) {
  const size_t address = kActRaiserWram_ActionObjectTable + slot * kActRaiserActionObjectStride;
  Write16(wram, address + 0x06, (uint16_t)velocity_x);
  Write16(wram, address + 0x08, 0);
  Write16(wram, address + 0x20, composition);
  Write16(wram, address + 0x22, visual);
}

static void SeedMarahnaSplitFireball(uint8_t *wram, unsigned slot, unsigned parent_slot,
                                     int16_t velocity_x, int16_t velocity_y) {
  const size_t address = kActRaiserWram_ActionObjectTable + slot * kActRaiserActionObjectStride;
  Write16(wram, address + 0x00, 0x0000);
  Write16(wram, address + 0x02, 400);
  Write16(wram, address + 0x04, 500);
  Write16(wram, address + 0x06, (uint16_t)velocity_x);
  Write16(wram, address + 0x08, (uint16_t)velocity_y);
  Write16(wram, address + 0x0A, 0x0004);
  Write16(wram, address + 0x0C, 0x0004);
  Write16(wram, address + 0x0E, 0x0004);
  Write16(wram, address + 0x10, 0x0004);
  Write16(wram, address + 0x12, 0x8661);
  Write16(wram, address + 0x16, 0x4000);
  wram[address + 0x18] = 0x7E;
  const bool horizontal = velocity_x != 0;
  Write16(wram, address + 0x1A, horizontal ? 0x0010 : 0x000F);
  Write16(wram, address + 0x1C, 1);
  Write16(wram, address + 0x1E, 0xA65D);
  Write16(wram, address + 0x20, horizontal ? 0x4BD9 : 0x4BCD);
  Write16(wram, address + 0x22, horizontal ? 0x0033 : 0x0032);
  Write16(wram, address + 0x28,
          velocity_x > 0   ? kActRaiserObjectFlip_Horizontal
          : velocity_y < 0 ? kActRaiserObjectFlip_Vertical
                           : 0);
  Write16(wram, address + 0x2E, 0);
  Write16(wram, address + 0x30, 0x0020);
  Write16(wram, address + 0x32, 0xE047);
  Write16(
      wram, address + 0x3A,
      (uint16_t)(kActRaiserWram_ActionObjectTable + parent_slot * kActRaiserActionObjectStride));
}

static void SeedMarahnaSnakeFireballShot(uint8_t *wram, unsigned parent_slot, unsigned shot_slot,
                                         bool horizontal_flip, uint16_t visual,
                                         uint16_t composition) {
  const size_t parent =
      kActRaiserWram_ActionObjectTable + parent_slot * kActRaiserActionObjectStride;
  Write16(wram, parent + 0x00, 0x0000);
  Write16(wram, parent + 0x02, 400);
  Write16(wram, parent + 0x04, 500);
  Write16(wram, parent + 0x0A, 16);
  Write16(wram, parent + 0x0C, 24);
  Write16(wram, parent + 0x0E, 16);
  Write16(wram, parent + 0x10, 24);
  Write16(wram, parent + 0x12, 0x8661);
  Write16(wram, parent + 0x16, 0x4000);
  wram[parent + 0x18] = 0x7E;
  Write16(wram, parent + 0x1A, 0x0005);
  Write16(wram, parent + 0x1E, 0xDF34);
  Write16(wram, parent + 0x20, 0x4435);
  Write16(wram, parent + 0x22, 0x0000);
  Write16(wram, parent + 0x28, horizontal_flip ? kActRaiserObjectFlip_Horizontal : 0);
  Write16(wram, parent + 0x32, 0xDE96);

  const size_t shot = kActRaiserWram_ActionObjectTable + shot_slot * kActRaiserActionObjectStride;
  Write16(wram, shot + 0x00, 0x0000);
  Write16(wram, shot + 0x02, 360);
  Write16(wram, shot + 0x04, 476);
  Write16(wram, shot + 0x06, horizontal_flip ? 4 : (uint16_t)-4);
  Write16(wram, shot + 0x08, 0);
  Write16(wram, shot + 0x0A, 8);
  Write16(wram, shot + 0x0C, 4);
  Write16(wram, shot + 0x0E, 8);
  Write16(wram, shot + 0x10, 4);
  Write16(wram, shot + 0x12, 0x8661);
  Write16(wram, shot + 0x16, 0x4000);
  wram[shot + 0x18] = 0x7E;
  Write16(wram, shot + 0x1A, 0x0006);
  Write16(wram, shot + 0x1C, 1);
  Write16(wram, shot + 0x1E, 0xA65D);
  Write16(wram, shot + 0x20, composition);
  Write16(wram, shot + 0x22, visual);
  Write16(wram, shot + 0x28, horizontal_flip ? kActRaiserObjectFlip_Horizontal : 0);
  Write16(wram, shot + 0x30, 0x0020);
  Write16(wram, shot + 0x32, 0xDE96);
  Write16(wram, shot + 0x38, 0x0006);
  Write16(wram, shot + 0x3A, (uint16_t)parent);
}

static void SeedAitosLavaFireball(uint8_t *wram, unsigned slot, int16_t world_x, int16_t world_y,
                                  uint16_t state) {
  const size_t address = kActRaiserWram_ActionObjectTable + slot * kActRaiserActionObjectStride;
  Write16(wram, address + 0x00, 0x0000);
  Write16(wram, address + 0x02, (uint16_t)world_x);
  Write16(wram, address + 0x04, (uint16_t)world_y);
  Write16(wram, address + 0x06, state == 0x0024 ? 0xFFFF : 0x0000);
  Write16(wram, address + 0x08, state == 0x0022 ? 0xFFFC : state == 0x0024 ? 0x0006 : 0x0000);
  Write16(wram, address + 0x0A, 0x0008);
  Write16(wram, address + 0x0C, 0x0008);
  Write16(wram, address + 0x0E, 0x0008);
  Write16(wram, address + 0x10, 0x0008);
  Write16(wram, address + 0x12, state == 0x0022 ? 0xCFE3 : state == 0x0024 ? 0xCFFE : 0x8661);
  Write16(wram, address + 0x16, 0x4000);
  wram[address + 0x18] = 0x7E;
  Write16(wram, address + 0x1A, state);
  Write16(wram, address + 0x1C, 1);
  Write16(wram, address + 0x1E, 0xCFCD);
  Write16(wram, address + 0x20, 0x4D21);
  Write16(wram, address + 0x22, 0x002A);
  Write16(wram, address + 0x30, 0x0020);
  Write16(wram, address + 0x32, 0xCF9E);
}

static void SeedAitosMoltenRock(uint8_t *wram, unsigned slot, int16_t world_x, int16_t world_y,
                                int16_t velocity_x, int16_t velocity_y) {
  const size_t address = kActRaiserWram_ActionObjectTable + slot * kActRaiserActionObjectStride;
  Write16(wram, address + 0x02, (uint16_t)world_x);
  Write16(wram, address + 0x04, (uint16_t)world_y);
  Write16(wram, address + 0x06, (uint16_t)velocity_x);
  Write16(wram, address + 0x08, (uint16_t)velocity_y);
  Write16(wram, address + 0x0A, 8);
  Write16(wram, address + 0x0C, 8);
  Write16(wram, address + 0x0E, 8);
  Write16(wram, address + 0x10, 8);
  Write16(wram, address + 0x12, 0x8661);
  Write16(wram, address + 0x16, 0x4000);
  wram[address + 0x18] = 0x7E;
  Write16(wram, address + 0x1A, 0x0027);
  Write16(wram, address + 0x1E, 0xCF16);
  Write16(wram, address + 0x20, 0x4D2D);
  Write16(wram, address + 0x22, 0x002B);
  Write16(wram, address + 0x28, velocity_x > 0 ? kActRaiserObjectFlip_Horizontal : 0);
  Write16(wram, address + 0x32, 0xCEEC);
}

static void SeedMarahnaLightningEndpoint(uint8_t *wram, unsigned slot, bool partner, bool vertical,
                                         int16_t world_x, int16_t world_y) {
  const size_t address = kActRaiserWram_ActionObjectTable + slot * kActRaiserActionObjectStride;
  Write16(wram, address + 0x00, 0x0000);
  Write16(wram, address + 0x02, (uint16_t)world_x);
  Write16(wram, address + 0x04, (uint16_t)world_y);
  Write16(wram, address + 0x12, 0x8683);
  Write16(wram, address + 0x16, 0x4000);
  wram[address + 0x18] = 0x7E;
  Write16(wram, address + 0x1A, partner ? 0x001D : 0x001A);
  Write16(wram, address + 0x20,
          partner ? (vertical ? 0x45DC : 0x45D0) : (vertical ? 0x45C4 : 0x45B8));
  Write16(wram, address + 0x22,
          partner ? (vertical ? 0x0010 : 0x000F) : (vertical ? 0x000E : 0x000D));
  Write16(wram, address + 0x30, 0x0020);
  Write16(wram, address + 0x32, partner ? 0xE254 : 0xE18E);
}

static void SeedMarahnaLightningLink(uint8_t *wram, unsigned slot, unsigned parent_slot,
                                     bool vertical, int16_t world_x, int16_t world_y) {
  const size_t address = kActRaiserWram_ActionObjectTable + slot * kActRaiserActionObjectStride;
  Write16(wram, address + 0x00, 0x0000);
  Write16(wram, address + 0x02, (uint16_t)world_x);
  Write16(wram, address + 0x04, (uint16_t)world_y);
  Write16(wram, address + 0x0A, vertical ? 5 : 40);
  Write16(wram, address + 0x0C, vertical ? 40 : 4);
  Write16(wram, address + 0x0E, vertical ? 5 : 40);
  Write16(wram, address + 0x10, vertical ? 40 : 4);
  Write16(wram, address + 0x12, 0x8683);
  Write16(wram, address + 0x16, 0x4000);
  wram[address + 0x18] = 0x7E;
  Write16(wram, address + 0x1A, vertical ? 0x0028 : 0x0027);
  Write16(wram, address + 0x1C, vertical ? 2 : 1);
  Write16(wram, address + 0x1E, 0xE24F);
  Write16(wram, address + 0x20, vertical ? 0x4B82 : 0x4AA1);
  Write16(wram, address + 0x22, vertical ? 0x0031 : 0x002E);
  Write16(wram, address + 0x30, 0x0020);
  Write16(wram, address + 0x32, 0xE18E);
  Write16(
      wram, address + 0x3A,
      (uint16_t)(kActRaiserWram_ActionObjectTable + parent_slot * kActRaiserActionObjectStride));
}

static void SeedBgMetatile(uint8_t *wram, unsigned world_width, unsigned world_x, unsigned world_y,
                           uint8_t metatile) {
  const unsigned cells_wide = world_width / kActionBgMetatilePixels;
  const unsigned page_x = world_x / 256u;
  const unsigned page_y = world_y / 256u;
  const unsigned pages_wide = cells_wide / 16u;
  const unsigned cell_x = (world_x / kActionBgMetatilePixels) & 15u;
  const unsigned cell_y = (world_y / kActionBgMetatilePixels) & 15u;
  wram[0x8000 + (page_y * pages_wide + page_x) * 256u + cell_y * 16u + cell_x] = metatile;
}

static void SeedBloodpoolBoss(uint8_t *wram) {
  const size_t address = 0x12E0;
  Write16(wram, address + 0x00, 0x0000);
  Write16(wram, address + 0x16, 0x5000);
  wram[address + 0x18] = 0x7E;
  Write16(wram, address + 0x20, 0x5847);
  Write16(wram, address + 0x32, 0xBDFF);
}

static void SeedSwordBeam(uint8_t *wram, unsigned state, bool hflip) {
  const size_t player = kActRaiserWram_PlayerObject;
  Write16(wram, player + 0x00, 0x0000);
  Write16(wram, player + 0x16, 0x8000);
  wram[player + 0x18] = 0x06;
  Write16(wram, player + 0x20, 0x899F);
  Write16(wram, player + 0x32, 0x979A);

  const size_t beam = kActRaiserWram_ActionObjectTable + 9 * kActRaiserActionObjectStride;
  Write16(wram, beam + 0x00, 0x0000);
  Write16(wram, beam + 0x02, 232);
  Write16(wram, beam + 0x04, 456);
  Write16(wram, beam + 0x06, hflip ? 0xFFF8 : 0x0008);
  Write16(wram, beam + 0x08, 0x0000);
  Write16(wram, beam + 0x0A, hflip ? 0x0030 : 0xFFE0);
  Write16(wram, beam + 0x0C, state == 0x13 ? 0x0020 : 0x0008);
  Write16(wram, beam + 0x0E, hflip ? 0xFFE0 : 0x0030);
  Write16(wram, beam + 0x10, state == 0x13 ? 0x0000 : 0x0018);
  Write16(wram, beam + 0x12, 0x9D1C);
  Write16(wram, beam + 0x16, 0x8000);
  wram[beam + 0x18] = 0x06;
  Write16(wram, beam + 0x1A, (uint16_t)state);
  Write16(wram, beam + 0x1C, 0x0000);
  Write16(wram, beam + 0x1E, 0x0000);
  Write16(wram, beam + 0x20, state == 0x13 ? 0x99E8 : 0x9A17);
  Write16(wram, beam + 0x22, state == 0x13 ? 0x0030 : 0x0031);
  Write16(wram, beam + 0x28, hflip ? kActRaiserObjectFlip_Horizontal : 0x0000);
  Write16(wram, beam + 0x30, kActRaiserObjectFlag_Attacker);
  Write16(wram, beam + 0x32, 0x979A);
  Write16(wram, beam + 0x3A, kActRaiserWram_PlayerObject);
}

static void SeedAitosBossSwordVolley(uint8_t *wram, bool reflected) {
  const size_t boss = kActRaiserWram_ActionObjectTable + 49 * kActRaiserActionObjectStride;
  const size_t parent = kActRaiserWram_ActionObjectTable + 62 * kActRaiserActionObjectStride;
  Write16(wram, boss + 0x00, 0x0000);
  Write16(wram, boss + 0x02, 408);
  Write16(wram, boss + 0x04, 108);
  Write16(wram, boss + 0x12, 0x8661);
  Write16(wram, boss + 0x16, 0x5000);
  wram[boss + 0x18] = 0x7E;
  Write16(wram, boss + 0x1A, 0x0009);
  Write16(wram, boss + 0x1E, 0xD6C3);
  Write16(wram, boss + 0x20, 0x548D);
  Write16(wram, boss + 0x22, 0x000F);
  Write16(wram, boss + 0x30, 0x4000);
  Write16(wram, boss + 0x32, 0xD646);

  Write16(wram, parent + 0x00, 0x4000);
  Write16(wram, parent + 0x02, 480);
  Write16(wram, parent + 0x04, 56);
  Write16(wram, parent + 0x0A, 8);
  Write16(wram, parent + 0x0C, 8);
  Write16(wram, parent + 0x0E, 8);
  Write16(wram, parent + 0x10, 8);
  Write16(wram, parent + 0x12, 0x8661);
  Write16(wram, parent + 0x16, 0x5000);
  wram[parent + 0x18] = 0x7E;
  Write16(wram, parent + 0x1A, 0x0000);
  Write16(wram, parent + 0x1E, 0xD793);
  Write16(wram, parent + 0x20, 0x56FE);
  Write16(wram, parent + 0x22, 0x0023);
  Write16(wram, parent + 0x28,
          reflected ? kActRaiserObjectFlip_Horizontal | kActRaiserObjectFlip_Vertical : 0);
  Write16(wram, parent + 0x30, 0x0020);
  Write16(wram, parent + 0x32, 0xD646);
  Write16(wram, parent + 0x38, 0x000D);
  Write16(wram, parent + 0x3A, (uint16_t)boss);

  static const struct {
    uint16_t state, visual, composition, local_counter;
    int16_t world_y, velocity_y;
    uint16_t top_extent, bottom_extent;
  } kCrescents[] = {
      {0x0001, 0x0021, 0x56D8, 0x0001, 68, 1, 16, 8},
      {0x0002, 0x0020, 0x56BE, 0x0002, 44, -1, 8, 16},
  };
  for (unsigned i = 0; i < 2; i++) {
    const size_t child = kActRaiserWram_ActionObjectTable + (63 + i) * kActRaiserActionObjectStride;
    Write16(wram, child + 0x00, 0x0000);
    Write16(wram, child + 0x02, 444);
    Write16(wram, child + 0x04, (uint16_t)kCrescents[i].world_y);
    Write16(wram, child + 0x06, (uint16_t)(int16_t)(reflected ? 3 : -3));
    Write16(wram, child + 0x08,
            (uint16_t)(int16_t)(reflected ? -kCrescents[i].velocity_y : kCrescents[i].velocity_y));
    Write16(wram, child + 0x0A, reflected ? 16 : 8);
    Write16(wram, child + 0x0C, reflected ? kCrescents[i].bottom_extent : kCrescents[i].top_extent);
    Write16(wram, child + 0x0E, reflected ? 8 : 16);
    Write16(wram, child + 0x10, reflected ? kCrescents[i].top_extent : kCrescents[i].bottom_extent);
    Write16(wram, child + 0x12, 0x8661);
    Write16(wram, child + 0x16, 0x5000);
    wram[child + 0x18] = 0x7E;
    Write16(wram, child + 0x1A, kCrescents[i].state);
    Write16(wram, child + 0x1C, 0x0001);
    Write16(wram, child + 0x1E, 0xA65D);
    Write16(wram, child + 0x20, kCrescents[i].composition);
    Write16(wram, child + 0x22, kCrescents[i].visual);
    Write16(wram, child + 0x28,
            reflected ? kActRaiserObjectFlip_Horizontal | kActRaiserObjectFlip_Vertical : 0);
    Write16(wram, child + 0x30, 0x0020);
    Write16(wram, child + 0x32, 0xD646);
    Write16(wram, child + 0x38, kCrescents[i].local_counter);
    Write16(wram, child + 0x3A, (uint16_t)parent);
  }
}

static void SeedBloodpoolBossLightningStrike(uint8_t *wram, unsigned slot, unsigned visual,
                                             bool hflip) {
  static const uint16_t kComposition[] = {
      0x5346, 0x5401, 0x5492, 0x54F2, 0x55C2, 0x5661,
  };
  static const uint8_t kLeft[] = {6, 6, 1, 48, 36, 30};
  static const uint8_t kRight[] = {11, 11, 11, 8, 8, 8};
  static const uint8_t kBottom[] = {117, 69, 21, 117, 69, 21};
  static const uint16_t kResume[] = {0xC02B, 0xC04B, 0xC051};
  const size_t address = kActRaiserWram_ActionObjectTable + slot * kActRaiserActionObjectStride;
  CHECK(visual < 6);
  Write16(wram, address + 0x00, 0x0000);
  Write16(wram, address + 0x02, 120);
  Write16(wram, address + 0x04, 160);
  Write16(wram, address + 0x0A, hflip ? kRight[visual] : kLeft[visual]);
  Write16(wram, address + 0x0C, 83);
  Write16(wram, address + 0x0E, hflip ? kLeft[visual] : kRight[visual]);
  Write16(wram, address + 0x10, kBottom[visual]);
  Write16(wram, address + 0x12, 0x8661);
  Write16(wram, address + 0x16, 0x5000);
  wram[address + 0x18] = 0x7E;
  Write16(wram, address + 0x1A, (uint16_t)(visual + 2));
  Write16(wram, address + 0x1C, 1);
  Write16(wram, address + 0x1E, kResume[visual % 3]);
  Write16(wram, address + 0x20, kComposition[visual]);
  Write16(wram, address + 0x22, (uint16_t)visual);
  Write16(wram, address + 0x28, hflip ? kActRaiserObjectFlip_Horizontal : 0);
  Write16(wram, address + 0x30, 0x0020);
  Write16(wram, address + 0x32, 0xBDFF);
  Write16(wram, address + 0x3A, 0x12E0);
}

static void SeedBloodpoolBossLightningImpact(uint8_t *wram, unsigned slot, unsigned visual) {
  static const uint16_t kComposition[] = {0x570A, 0x5716, 0x5729};
  static const uint8_t kExtent[] = {4, 8, 16};
  static const uint8_t kTop[] = {8, 8, 16};
  const size_t address = kActRaiserWram_ActionObjectTable + slot * kActRaiserActionObjectStride;
  CHECK(visual >= 8 && visual <= 10);
  const unsigned frame = visual - 8;
  Write16(wram, address + 0x00, 0x0000);
  Write16(wram, address + 0x02, 120);
  Write16(wram, address + 0x04, 224);
  Write16(wram, address + 0x0A, kExtent[frame]);
  Write16(wram, address + 0x0C, kTop[frame]);
  Write16(wram, address + 0x0E, kExtent[frame]);
  Write16(wram, address + 0x10, 0);
  Write16(wram, address + 0x12, 0x8661);
  Write16(wram, address + 0x16, 0x5000);
  wram[address + 0x18] = 0x7E;
  Write16(wram, address + 0x1A, 9);
  Write16(wram, address + 0x1C, (uint16_t)(frame + 1));
  Write16(wram, address + 0x1E, 0xC06A);
  Write16(wram, address + 0x20, kComposition[frame]);
  Write16(wram, address + 0x22, (uint16_t)visual);
  Write16(wram, address + 0x30, 0x0020);
  Write16(wram, address + 0x32, 0xBDFF);
  Write16(wram, address + 0x3A, 0x08E0);
}

static void TestMeasuredSceneObjectIdentities(void) {
  uint8_t wram[kActRaiserWramSize];
  ActionSceneEffectFrame first, paused, advanced, reused, source_reused, alternate, rejected;
  ActionEffectObserver observer = {0};
  memset(wram, 0, sizeof(wram));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Bloodpool;
  wram[kActRaiserWram_CurrentMap] = 5;
  Write16(wram, kActRaiserWram_GameFrame, 7397);
  SeedMeasuredSceneObject(wram, 22, false); /* live address $0C20 */
  SeedMeasuredSceneObject(wram, 32, true);  /* live address $0EA0 */

  ActionSceneEffects_CaptureFrame(&observer, &first, wram, sizeof(wram), 1);
  CHECK(first.game_frame == 7397);
  CHECK(first.effect_count == 2);
  CHECK(first.visible_count == 2);
  CHECK(first.effects[0].record_address == 0x0C20);
  CHECK(first.effects[0].kind == kActionEffect_EnemyFireball);
  CHECK(first.effects[0].phase == kActionEffectPhase_EnemyFireballFlight);
  CHECK(first.effects[0].velocity_x == 3);
  CHECK(first.effects[0].projection_plane == kActionEffectProjectionPlane_Obj);
  CHECK(first.effects[0].generation != 0);
  CHECK(first.effects[1].record_address == 0x0EA0);
  CHECK(first.effects[1].kind == kActionEffect_LightningTrap);
  CHECK(first.effects[1].phase == kActionEffectPhase_LightningActive);
  CHECK(first.effects[1].top_extent == 88);
  CHECK(first.effects[1].geometry.data.rect.y0 == -88.0f);
  CHECK(first.effects[1].geometry.data.rect.y1 == 88.0f);

  /* $BD36 hands the same lightning actor to shared repeat handler $8683.
   * That control-flow transition is part of one bolt, not slot reuse. */
  Write16(wram, 0x0EA0 + 0x12, 0xBD36);
  ActionSceneEffects_CaptureFrame(&observer, &paused, wram, sizeof(wram), 0);
  CHECK(paused.effects[0].age_ticks == 0);
  CHECK(paused.effects[0].generation == first.effects[0].generation);
  CHECK(paused.effects[1].generation == first.effects[1].generation);
  Write16(wram, 0x0C20 + 0x02, 712); /* +3 px/tick for three ticks */
  ActionSceneEffects_CaptureFrame(&observer, &advanced, wram, sizeof(wram), 3);
  CHECK(advanced.effects[0].age_ticks == 3);
  CHECK(advanced.effects[0].pulse_ticks == 3);
  CHECK(advanced.effects[0].generation == first.effects[0].generation);

  /* A same-kind actor can replace its predecessor in the same slot between
   * captures. The control-flow identity is unchanged, but teleporting back to
   * a source is discontinuous with the measured three-pixel flight and must
   * start a fresh renderer generation rather than inherit the old trail. */
  Write16(wram, 0x0C20 + 0x02, 1200);
  ActionSceneEffects_CaptureFrame(&observer, &reused, wram, sizeof(wram), 1);
  CHECK(reused.effect_count == 2);
  CHECK(reused.effects[0].generation != advanced.effects[0].generation);
  CHECK(reused.effects[0].pulse_generation != advanced.effects[0].pulse_generation);
  CHECK(reused.effects[0].age_ticks == 0);
  CHECK(reused.effects[0].pulse_ticks == 0);

  Write16(wram, 0x0C20 + 0x02, 1203);
  Write16(wram, 0x0C20 + 0x32, 0xBD76);
  ActionSceneEffects_CaptureFrame(&observer, &source_reused, wram, sizeof(wram), 1);
  CHECK(source_reused.effects[0].generation != reused.effects[0].generation);
  CHECK(source_reused.effects[0].age_ticks == 0);

  /* The opposite-facing source artwork uses the other measured pair. Both
   * belong to the same projectile family and must remain positively matched. */
  Write16(wram, 0x0C20 + 0x20, 0x45EF);
  Write16(wram, 0x0C20 + 0x22, 0x0017);
  ActionSceneEffects_CaptureFrame(&observer, &alternate, wram, sizeof(wram), 1);
  CHECK(alternate.effect_count == 2);
  CHECK(alternate.effects[0].kind == kActionEffect_EnemyFireball);

  /* Outside-activation is separate from status/no-draw and is the exact bit
   * carried by snap_04's second, offscreen lightning column. Keep tracking it
   * but never submit it. */
  Write16(wram, 0x0EA0 + 0x30, kActRaiserObjectFlag_OutsideActivation);
  ActionSceneEffects_CaptureFrame(&observer, &advanced, wram, sizeof(wram), 1);
  CHECK(advanced.effect_count == 2);
  CHECK(advanced.visible_count == 1);
  CHECK((advanced.effects[1].flags & kActionEffectFlag_Visible) == 0);

  /* A near miss must not be decorated merely because its visual and palette
   * resemble fire. */
  Write16(wram, 0x0C20 + 0x12, 0xBDEF);
  ActionSceneEffects_CaptureFrame(&observer, &rejected, wram, sizeof(wram), 1);
  CHECK(rejected.effect_count == 1);
  CHECK(rejected.effects[0].kind == kActionEffect_LightningTrap);
}

static void TestBloodpoolAct2SceneScopeAndRoomContinuity(void) {
  uint8_t wram[kActRaiserWramSize];
  ActionSceneEffectFrame first, last, rejected;
  ActionEffectObserver observer = {0};
  memset(wram, 0, sizeof(wram));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Bloodpool;
  wram[kActRaiserWram_CurrentMap] = 2;
  SeedMeasuredSceneObject(wram, 22, false);
  SeedMeasuredSceneObject(wram, 32, true);

  /* The ordinary enemy blob is shared throughout Bloodpool Act 2, so both
   * range endpoints remain valid even though the discovery capture was map 5. */
  ActionSceneEffects_CaptureFrame(&observer, &first, wram, sizeof(wram), 1);
  CHECK(first.effect_count == 2);
  CHECK(first.effects[0].kind == kActionEffect_EnemyFireball);
  CHECK(first.effects[1].kind == kActionEffect_LightningTrap);
  const uint32_t first_fireball_generation = first.effects[0].generation;
  const uint32_t first_lightning_generation = first.effects[1].generation;

  wram[kActRaiserWram_CurrentMap] = 8;
  ActionSceneEffects_CaptureFrame(&observer, &last, wram, sizeof(wram), 1);
  CHECK(last.effect_count == 2);
  CHECK(last.effects[0].generation != first_fireball_generation);
  CHECK(last.effects[1].generation != first_lightning_generation);
  CHECK(last.effects[0].age_ticks == 0);
  CHECK(last.effects[1].age_ticks == 0);

  /* The same polymorphic records are not Bloodpool Act-1 or cross-stage
   * identities merely because their control flow and current art still match. */
  wram[kActRaiserWram_CurrentMap] = 1;
  ActionSceneEffects_CaptureFrame(&observer, &rejected, wram, sizeof(wram), 1);
  CHECK(rejected.effect_count == 0);
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Aitos;
  wram[kActRaiserWram_CurrentMap] = 5;
  ActionSceneEffects_CaptureFrame(&observer, &rejected, wram, sizeof(wram), 1);
  CHECK(rejected.effect_count == 0);

  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Bloodpool;
  wram[kActRaiserWram_CurrentMap] = 5;
  Write16(wram, 0x0C20 + 0x32, 0xBD75);
  ActionSceneEffects_CaptureFrame(&observer, &rejected, wram, sizeof(wram), 1);
  CHECK(rejected.effect_count == 1);
  CHECK(rejected.effects[0].kind == kActionEffect_LightningTrap);

  Write16(wram, 0x0C20 + 0x32, 0xBD84);
  Write16(wram, 0x0EA0 + 0x32, 0xBD29);
  ActionSceneEffects_CaptureFrame(&observer, &rejected, wram, sizeof(wram), 1);
  CHECK(rejected.effect_count == 1);
  CHECK(rejected.effects[0].kind == kActionEffect_EnemyFireball);
}

static void TestBloodpoolBossLightningIdentity(void) {
  uint8_t wram[kActRaiserWramSize];
  ActionSceneEffectFrame frame;
  ActionEffectObserver observer = {0};
  memset(wram, 0, sizeof(wram));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Bloodpool;
  wram[kActRaiserWram_CurrentMap] = 8;
  Write16(wram, kActRaiserWram_GameFrame, 11775);
  SeedBloodpoolBoss(wram);

  static const uint8_t kLeft[] = {6, 6, 1, 48, 36, 30};
  static const uint8_t kRight[] = {11, 11, 11, 8, 8, 8};
  static const uint8_t kBottom[] = {117, 69, 21, 117, 69, 21};
  for (unsigned visual = 0; visual < 6; visual++) {
    for (unsigned flipped = 0; flipped < 2; flipped++) {
      SeedBloodpoolBossLightningStrike(wram, 9, visual, flipped != 0);
      ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
      CHECK(frame.effect_count == 1);
      CHECK(frame.effects[0].record_address == 0x08E0);
      CHECK(frame.effects[0].kind == kActionEffect_BloodpoolBossLightning);
      CHECK(frame.effects[0].phase == kActionEffectPhase_BossLightningStrike);
      CHECK(frame.effects[0].visual == visual);
      CHECK(frame.effects[0].animation_state == visual + 2);
      CHECK(frame.effects[0].left_extent == (flipped ? kRight[visual] : kLeft[visual]));
      CHECK(frame.effects[0].right_extent == (flipped ? kLeft[visual] : kRight[visual]));
      CHECK(frame.effects[0].bottom_extent == kBottom[visual]);
      CHECK(((frame.effects[0].flags & kActionEffectFlag_FlipHorizontal) != 0) == (flipped != 0));
    }
  }

  /* $20/$5D2B is the blank half of every strike cycle, not a warning. */
  Write16(wram, 0x08E0 + 0x20, 0x5D2B);
  Write16(wram, 0x08E0 + 0x22, 0x0020);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);

  SeedBloodpoolBossLightningStrike(wram, 9, 5, false);
  for (unsigned visual = 8; visual <= 10; visual++) {
    SeedBloodpoolBossLightningImpact(wram, 10, visual);
    ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
    CHECK(frame.effect_count == 2);
    CHECK(frame.effects[1].phase == kActionEffectPhase_BossLightningImpact);
    CHECK(frame.effects[1].visual == visual);
  }

  /* The boss animation bank is shared by the room. Map, control flow, linked
   * parent, transform, and exact composition tuple are all required. */
  wram[kActRaiserWram_CurrentMap] = 7;
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  wram[kActRaiserWram_CurrentMap] = 8;
  Write16(wram, 0x08E0 + 0x12, 0x8660);
  Write16(wram, 0x0920 + 0x12, 0x8660);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);

  SeedBloodpoolBossLightningStrike(wram, 9, 4, false);
  Write16(wram, 0x08E0 + 0x3A, 0x08A0); /* player, not boss family */
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  SeedBloodpoolBossLightningStrike(wram, 9, 4, false);
  Write16(wram, 0x08E0 + 0x28, kActRaiserObjectFlip_Vertical);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
}

static void TestSwordBeamIdentityAndAuthoredGeometry(void) {
  uint8_t wram[kActRaiserWramSize];
  ActionSceneEffectFrame frame;
  ActionEffectObserver observer = {0};
  memset(wram, 0, sizeof(wram));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Bloodpool;
  wram[kActRaiserWram_CurrentMap] = 2;
  Write16(wram, kActRaiserWram_GameFrame, 1726);

  SeedSwordBeam(wram, 0x13, false);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.visible_count == 1);
  CHECK(frame.effects[0].record_address == 0x08E0);
  CHECK(frame.effects[0].kind == kActionEffect_SwordBeam);
  CHECK(frame.effects[0].phase == kActionEffectPhase_SwordBeamFlight);
  CHECK(frame.effects[0].obj_priority == 0);
  CHECK(frame.effects[0].projection_plane == kActionEffectProjectionPlane_Obj);
  CHECK(frame.effects[0].world_x == 232);
  CHECK(frame.effects[0].world_y == 456);
  CHECK(frame.effects[0].velocity_x == 8);
  CHECK(frame.effects[0].left_extent == 0xFFE0);
  CHECK(frame.effects[0].visual == 0x30);
  CHECK(frame.effects[0].composition == 0x99E8);
  CHECK(frame.effects[0].geometry.data.rect.x0 == 32.0f);
  CHECK(frame.effects[0].geometry.data.rect.y0 == -33.0f);
  CHECK(frame.effects[0].geometry.data.rect.x1 == 48.0f);
  CHECK(frame.effects[0].geometry.data.rect.y1 == -1.0f);
  /* Exact snap_01_gf1815 registration from run 20260810-184935: camera
   * (120,255) turns world hot point (232,456) into (112,201), and the decoded
   * local rect must land on the captured crescent OAM bounds 144..160 by
   * 168..200. */
  CHECK(frame.effects[0].world_x - 120 + frame.effects[0].geometry.data.rect.x0 == 144.0f);
  CHECK(frame.effects[0].world_y - 255 + frame.effects[0].geometry.data.rect.y0 == 168.0f);
  CHECK(frame.effects[0].world_x - 120 + frame.effects[0].geometry.data.rect.x1 == 160.0f);
  CHECK(frame.effects[0].world_y - 255 + frame.effects[0].geometry.data.rect.y1 == 200.0f);

  /* The alternate state uses the same six-part crescent with a different
   * signed composition origin, so its decoded anchor moves with the OAM. */
  SeedSwordBeam(wram, 0x14, false);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].animation_state == 0x14);
  CHECK(frame.effects[0].visual == 0x31);
  CHECK(frame.effects[0].composition == 0x9A17);
  CHECK(frame.effects[0].geometry.data.rect.x0 == 40.0f);
  CHECK(frame.effects[0].geometry.data.rect.y0 == -9.0f);
  CHECK(frame.effects[0].geometry.data.rect.x1 == 56.0f);
  CHECK(frame.effects[0].geometry.data.rect.y1 == 23.0f);

  SeedSwordBeam(wram, 0x13, true);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].velocity_x == -8);
  CHECK(frame.effects[0].flags & kActionEffectFlag_FlipHorizontal);
  CHECK(frame.effects[0].geometry.data.rect.x0 == -48.0f);
  CHECK(frame.effects[0].geometry.data.rect.y0 == -33.0f);
  CHECK(frame.effects[0].geometry.data.rect.x1 == -32.0f);
  CHECK(frame.effects[0].geometry.data.rect.y1 == -1.0f);

  /* $008F supplies the live room priority to both native beam compositions.
   * Bloodpool Act 1's boss uses band 2, not the old hard-coded band 0. */
  for (unsigned state = 0x13; state <= 0x14; ++state) {
    for (unsigned priority = 0; priority < 4; ++priority) {
      SeedSwordBeam(wram, state, priority & 1);
      Write16(wram, 0x8F, priority << 12);
      ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
      CHECK(frame.effect_count == 1 && frame.effects[0].obj_priority == priority);
    }
  }
  Write16(wram, 0x8F, 0);

  /* This is a player ability rather than a Bloodpool room signature. */
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Fillmore;
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);

  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Bloodpool;
  Write16(wram, 0x08E0 + 0x20, 0x99E9);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  SeedSwordBeam(wram, 0x13, false);
  Write16(wram, 0x08E0 + 0x3A, 0x0860);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  SeedSwordBeam(wram, 0x13, false);
  Write16(wram, kActRaiserWram_PlayerObject + 0x32, 0x9810);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  SeedSwordBeam(wram, 0x13, false);
  Write16(wram, 0x08E0 + 0x28, kActRaiserObjectFlip_Vertical);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
}

static void TestAitosBossSwordVolleyIdentityAndGeometry(void) {
  uint8_t wram[kActRaiserWramSize];
  ActionSceneEffectFrame frame;
  ActionEffectObserver observer = {0};
  memset(wram, 0, sizeof(wram));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Aitos;
  wram[kActRaiserWram_CurrentMap] = 3;
  Write16(wram, kActRaiserWram_GameFrame, 21056);
  Write16(wram, kActRaiserWram_Bg1CameraX, 136);
  Write16(wram, kActRaiserWram_Bg1CameraY, 8);
  SeedAitosBossSwordVolley(wram, false);

  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 2);
  CHECK(frame.visible_count == 2);
  CHECK(frame.effects[0].record_address == 0x1660);
  CHECK(frame.effects[0].kind == kActionEffect_SwordBeam);
  CHECK(frame.effects[0].phase == kActionEffectPhase_SwordBeamFlight);
  CHECK(frame.effects[0].visual == 0x21);
  CHECK(frame.effects[0].composition == 0x56D8);
  CHECK(frame.effects[0].velocity_x == -3);
  CHECK(frame.effects[0].velocity_y == 1);
  CHECK(frame.effects[0].obj_priority == 2);
  CHECK(frame.effects[0].geometry.data.rect.x0 == -8.0f);
  CHECK(frame.effects[0].geometry.data.rect.y0 == -17.0f);
  CHECK(frame.effects[0].geometry.data.rect.x1 == 16.0f);
  CHECK(frame.effects[0].geometry.data.rect.y1 == 7.0f);
  /* Captured OAM entries 71-73 occupy (300,43)..(316,67) after authentic
   * camera subtraction. */
  CHECK(frame.effects[0].world_x - 136 + frame.effects[0].geometry.data.rect.x0 == 300.0f);
  CHECK(frame.effects[0].world_y - 8 + frame.effects[0].geometry.data.rect.y0 == 43.0f);

  CHECK(frame.effects[1].record_address == 0x16A0);
  CHECK(frame.effects[1].visual == 0x20);
  CHECK(frame.effects[1].composition == 0x56BE);
  CHECK(frame.effects[1].velocity_y == -1);
  CHECK(frame.effects[1].obj_priority == 2);
  CHECK(frame.effects[1].geometry.data.rect.x0 == -8.0f);
  CHECK(frame.effects[1].geometry.data.rect.y0 == -9.0f);
  CHECK(frame.effects[1].geometry.data.rect.x1 == 16.0f);
  CHECK(frame.effects[1].geometry.data.rect.y1 == 15.0f);
  CHECK(frame.effects[1].world_y - 8 + frame.effects[1].geometry.data.rect.y0 == 27.0f);

  /* The generic child allocator may immediately recycle one branch's slot
   * for the other. Local counter 1/2 is part of continuity even when the new
   * child appears close enough to pass the bounded-motion discriminator. */
  const uint32_t lower_generation = frame.effects[0].generation;
  Write16(wram, 0x1660 + 0x04, 67);
  Write16(wram, 0x1660 + 0x08, (uint16_t)(int16_t)-1);
  Write16(wram, 0x1660 + 0x0C, 8);
  Write16(wram, 0x1660 + 0x10, 16);
  Write16(wram, 0x1660 + 0x1A, 0x0002);
  Write16(wram, 0x1660 + 0x20, 0x56BE);
  Write16(wram, 0x1660 + 0x22, 0x0020);
  Write16(wram, 0x1660 + 0x38, 0x0002);
  Write16(wram, 0x16A0 + 0x00, 0x4000);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].record_address == 0x1660);
  CHECK(frame.effects[0].generation != lower_generation);

  /* Run 20260812-224123 captures the controller's H+V-reflected facing. Its
   * state-$01 child at world (338,50), camera (120,8), emits OAM over
   * (202,33)..(226,57). The sibling tuple pins the second reflected diagonal
   * even though it had already become inactive in that particular frame. */
  SeedAitosBossSwordVolley(wram, true);
  Write16(wram, kActRaiserWram_Bg1CameraX, 120);
  Write16(wram, kActRaiserWram_Bg1CameraY, 8);
  Write16(wram, 0x1660 + 0x02, 338);
  Write16(wram, 0x1660 + 0x04, 50);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 2);
  CHECK(frame.effects[0].record_address == 0x1660);
  CHECK(frame.effects[0].velocity_x == 3);
  CHECK(frame.effects[0].velocity_y == -1);
  CHECK(frame.effects[0].flags & kActionEffectFlag_FlipHorizontal);
  CHECK(frame.effects[0].flags & kActionEffectFlag_FlipVertical);
  CHECK(frame.effects[0].geometry.data.rect.x0 == -16.0f);
  CHECK(frame.effects[0].geometry.data.rect.y0 == -9.0f);
  CHECK(frame.effects[0].geometry.data.rect.x1 == 8.0f);
  CHECK(frame.effects[0].geometry.data.rect.y1 == 15.0f);
  CHECK(frame.effects[0].world_x - 120 + frame.effects[0].geometry.data.rect.x0 == 202.0f);
  CHECK(frame.effects[0].world_y - 8 + frame.effects[0].geometry.data.rect.y0 == 33.0f);
  CHECK(frame.effects[1].record_address == 0x16A0);
  CHECK(frame.effects[1].velocity_x == 3);
  CHECK(frame.effects[1].velocity_y == 1);
  CHECK(frame.effects[1].geometry.data.rect.x0 == -16.0f);
  CHECK(frame.effects[1].geometry.data.rect.y0 == -17.0f);
  CHECK(frame.effects[1].geometry.data.rect.x1 == 8.0f);
  CHECK(frame.effects[1].geometry.data.rect.y1 == 7.0f);

  /* Reflection belongs to the complete controller/child lifecycle. A lone
   * reflected projectile cannot acquire the boss effect by visual tuple. */
  Write16(wram, 0x1620 + 0x28, 0);
  ActionEffectObserver_Reset(&observer); /* Test new admission, not an existing child. */
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);

  /* This loaded boss bank is shared across Aitos sections. Map, complete
   * child tuple, inactive controller, and live boss root all fail closed. */
  SeedAitosBossSwordVolley(wram, false);
  wram[kActRaiserWram_CurrentMap] = 2;
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  wram[kActRaiserWram_CurrentMap] = 3;
  Write16(wram, 0x1660 + 0x08, 0);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].record_address == 0x16A0);
  SeedAitosBossSwordVolley(wram, false);
  Write16(wram, 0x16A0 + 0x20, 0x56D8);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].record_address == 0x1660);
  SeedAitosBossSwordVolley(wram, false);
  Write16(wram, 0x1620 + 0x1E, 0xD794);
  ActionEffectObserver_Reset(&observer); /* Test new admission, not an existing child. */
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  SeedAitosBossSwordVolley(wram, false);
  Write16(wram, 0x12E0 + 0x32, 0xD645);
  ActionEffectObserver_Reset(&observer); /* Test new admission, not an existing child. */
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
}

static void TestBloodpoolTorchMetatileIdentity(void) {
  uint8_t wram[kActRaiserWramSize];
  ActionSceneEffectFrame frame;
  ActionEffectObserver observer = {0};
  memset(wram, 0, sizeof(wram));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Bloodpool;
  wram[kActRaiserWram_CurrentMap] = 3;
  Write16(wram, kActRaiserWram_GameFrame, 2479);
  Write16(wram, kActRaiserWram_Bg1Width, 256);
  Write16(wram, kActRaiserWram_Bg1Height, 256);
  Write16(wram, kActRaiserWram_BgMapPage, 0x8000);
  /* Two sconces share one animated BG tile clock even though their particle
   * seeds remain identity-specific. */
  /* World cell (32,48): one $47 torch top directly over its $4F base. */
  wram[0x8000 + 0x30 + 2] = 0x47;
  wram[0x8000 + 0x40 + 2] = 0x4F;
  wram[0x8000 + 0x30 + 4] = 0x47;
  wram[0x8000 + 0x40 + 4] = 0x4F;

  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 2);
  CHECK(frame.decoration_visible_count == 2);
  CHECK(frame.decorations[0].kind == kActionEffect_WallTorch);
  CHECK(frame.decorations[0].phase == kActionEffectPhase_WallTorch);
  CHECK(frame.decorations[0].world_x == 40);
  CHECK(frame.decorations[0].world_y == 63);
  CHECK(frame.decorations[0].projection_plane == kActionEffectProjectionPlane_Bg1);
  CHECK(frame.decorations[0].render_layer == kActionEffectRenderLayer_Bg1Plane);
  CHECK(frame.decorations[0].phase_ticks == 2479);
  CHECK(frame.decorations[1].world_x == 72);
  CHECK(frame.decorations[1].world_y == 63);
  CHECK(frame.decorations[1].phase_ticks == frame.decorations[0].phase_ticks);

  /* $0088 continues ticking on ActRaiser's pause screen. Only the gameplay
   * delta may advance map-backed lighting and particles. */
  Write16(wram, kActRaiserWram_GameFrame, 2600);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 0);
  CHECK(frame.decorations[0].phase_ticks == 2479);
  Write16(wram, kActRaiserWram_GameFrame, 2603);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 3);
  CHECK(frame.decorations[0].phase_ticks == 2482);

  /* The same authored pair is present in Bloodpool map 5 and must not be
   * suppressed by a room-number allowlist. */
  wram[kActRaiserWram_CurrentMap] = 5;
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 2);
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Fillmore;
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 0);
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Bloodpool;
  wram[kActRaiserWram_CurrentMap] = 3;
  wram[0x8000 + 0x30 + 4] = 0;
  wram[0x8000 + 0x40 + 4] = 0;
  wram[0x8000 + 0x40 + 2] = 0x4E;
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 0);
}

static void TestMarahnaTorchMetatileIdentityAndWindow(void) {
  uint8_t wram[kActRaiserWramSize];
  ActionSceneEffectFrame frame;
  ActionEffectObserver observer = {0};
  memset(wram, 0, sizeof(wram));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Marahna;
  Write16(wram, kActRaiserWram_GameFrame, 20296);
  Write16(wram, kActRaiserWram_Bg1CameraX, 1600);
  Write16(wram, kActRaiserWram_Bg1CameraY, 400);
  Write16(wram, kActRaiserWram_Bg1Width, 2304);
  Write16(wram, kActRaiserWram_Bg1Height, 1792);
  Write16(wram, kActRaiserWram_BgMapPage, 0x8000);
  SeedBgMetatile(wram, 2304, 1600, 400, 0x43);
  /* The shared world contains 31 torches. A camera-local semantic window is
   * part of this scene-frame capacity contract, so a distant valid $43 must
   * remain unreported until the camera approaches it. */
  SeedBgMetatile(wram, 2304, 1000, 400, 0x43);

  for (unsigned map = 4; map <= 8; map++) {
    wram[kActRaiserWram_CurrentMap] = (uint8_t)map;
    CHECK(ActionSceneEffects_RoomUsesBg1Decorations(wram, sizeof(wram)));
    ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
    CHECK(frame.decoration_count == 1);
    CHECK(frame.decoration_visible_count == 1);
    CHECK(frame.decorations[0].kind == kActionEffect_WallTorch);
    CHECK(frame.decorations[0].world_x == 1608);
    CHECK(frame.decorations[0].world_y == 411);
    CHECK(frame.decorations[0].geometry.data.rect.x0 == -5.0f);
    CHECK(frame.decorations[0].geometry.data.rect.y0 == -9.0f);
    CHECK(frame.decorations[0].geometry.data.rect.x1 == 5.0f);
    CHECK(frame.decorations[0].geometry.data.rect.y1 == 5.0f);
    CHECK(frame.decorations[0].projection_plane == kActionEffectProjectionPlane_Bg1);
    CHECK(frame.decorations[0].render_layer == kActionEffectRenderLayer_Bg1Plane);
    /* A room handoff retires both actor generations and the map-decoration
     * clock; each room seeds its authored effects from the current game frame. */
    CHECK(frame.decorations[0].phase_ticks == 20296);
  }

  /* Boss map $08 has a separate 512x512 BG1 and ten exact `$43` cells. Pin
   * the complete observed set from snap_03_gf16836 so admission cannot later
   * regress to a room gate that still misses or overflows authored torches. */
  memset(wram + 0x8000, 0, 0x10000);
  Write16(wram, kActRaiserWram_Bg1Width, 512);
  Write16(wram, kActRaiserWram_Bg1Height, 512);
  Write16(wram, kActRaiserWram_Bg1CameraX, 120);
  Write16(wram, kActRaiserWram_Bg1CameraY, 255);
  static const uint16_t kBossTorchCells[][2] = {
      {0x0E0, 0x110}, {0x110, 0x110}, {0x0C0, 0x130}, {0x130, 0x130}, {0x0B0, 0x150},
      {0x140, 0x150}, {0x0C0, 0x170}, {0x130, 0x170}, {0x0E0, 0x190}, {0x110, 0x190},
  };
  for (size_t i = 0; i < sizeof(kBossTorchCells) / sizeof(kBossTorchCells[0]); i++)
    SeedBgMetatile(wram, 512, kBossTorchCells[i][0], kBossTorchCells[i][1], 0x43);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 10);
  CHECK(frame.decoration_visible_count == 10);

  /* Death Heim room $06 reuses Viper's boss background and its exact `$43`
   * torch cells. The rematch needs the same map-derived treatment even though
   * its map group and boss source identity differ from Marahna Act 2. */
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_DeathHeim;
  wram[kActRaiserWram_CurrentMap] = 6;
  CHECK(ActionSceneEffects_RoomUsesBg1Decorations(wram, sizeof(wram)));
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 10);
  CHECK(frame.decoration_visible_count == 10);
  for (unsigned i = 0; i < frame.decoration_count; i++)
    CHECK(frame.decorations[i].kind == kActionEffect_WallTorch);
  wram[kActRaiserWram_CurrentMap] = 5;
  CHECK(!ActionSceneEffects_RoomUsesBg1Decorations(wram, sizeof(wram)));
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 0);
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Marahna;
  wram[kActRaiserWram_CurrentMap] = 8;

  /* Restore the shared act-world fixture used by the scan-edge checks. */
  memset(wram + 0x8000, 0, 0x10000);
  Write16(wram, kActRaiserWram_Bg1Width, 2304);
  Write16(wram, kActRaiserWram_Bg1Height, 1792);
  Write16(wram, kActRaiserWram_Bg1CameraX, 1600);
  Write16(wram, kActRaiserWram_Bg1CameraY, 400);
  SeedBgMetatile(wram, 2304, 1600, 400, 0x43);
  SeedBgMetatile(wram, 2304, 1000, 400, 0x43);
  wram[kActRaiserWram_CurrentMap] = 7;

  /* The shared scan bound preserves the old predicate exactly: a metatile on
   * the inclusive camera-margin edge is visited, while the immediately prior
   * cell is not. Moving the camera one pixel advances the aligned start. */
  SeedBgMetatile(wram, 2304, 1344, 400, 0x43);
  SeedBgMetatile(wram, 2304, 1328, 400, 0x43);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 2);
  CHECK(frame.decorations[0].world_x == 1352);
  Write16(wram, kActRaiserWram_Bg1CameraX, 1601);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 1);
  CHECK(frame.decorations[0].world_x == 1608);
  SeedBgMetatile(wram, 2304, 1344, 400, 0);
  SeedBgMetatile(wram, 2304, 1328, 400, 0);
  Write16(wram, kActRaiserWram_Bg1CameraX, 1600);

  wram[kActRaiserWram_CurrentMap] = 3;
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 0);
  wram[kActRaiserWram_CurrentMap] = 5;
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Aitos;
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 0);
}

static void SeedMarahnaBossParent(uint8_t *wram, unsigned slot, uint16_t state, uint16_t visual,
                                  uint16_t composition) {
  const size_t address = kActRaiserWram_ActionObjectTable + slot * kActRaiserActionObjectStride;
  Write16(wram, address + 0x00, 0x0000);
  Write16(wram, address + 0x02, 256);
  Write16(wram, address + 0x04, 360);
  Write16(wram, address + 0x0A, 48);
  Write16(wram, address + 0x0C, 40);
  Write16(wram, address + 0x0E, 48);
  Write16(wram, address + 0x10, 8);
  Write16(wram, address + 0x12, 0x8661);
  Write16(wram, address + 0x16, 0x5000);
  wram[address + 0x18] = 0x7E;
  Write16(wram, address + 0x1A, state);
  Write16(wram, address + 0x1C, 0x0019);
  Write16(wram, address + 0x1E, state ? 0xE4F4 : 0xE4E5);
  Write16(wram, address + 0x20, composition);
  Write16(wram, address + 0x22, visual);
  Write16(wram, address + 0x2E, 0x0080);
  Write16(wram, address + 0x30, 0x4000);
  Write16(wram, address + 0x32, 0xE483);
}

static void SeedMarahnaBossBolt(uint8_t *wram, unsigned slot, unsigned parent_slot, bool right) {
  const size_t address = kActRaiserWram_ActionObjectTable + slot * kActRaiserActionObjectStride;
  Write16(wram, address + 0x00, 0x0000);
  Write16(wram, address + 0x02, right ? 288 : 204);
  Write16(wram, address + 0x04, right ? 392 : 412);
  Write16(wram, address + 0x06, right ? 4 : 0xFFFC);
  Write16(wram, address + 0x08, 4);
  Write16(wram, address + 0x0A, right ? 0 : 32);
  Write16(wram, address + 0x0C, 0);
  Write16(wram, address + 0x0E, right ? 32 : 0);
  Write16(wram, address + 0x10, 32);
  Write16(wram, address + 0x12, 0x8661);
  Write16(wram, address + 0x16, 0x5000);
  wram[address + 0x18] = 0x7E;
  Write16(wram, address + 0x1A, 4);
  Write16(wram, address + 0x1C, 1);
  Write16(wram, address + 0x1E, 0xE578);
  Write16(wram, address + 0x20, 0x5CE0);
  Write16(wram, address + 0x22, 0x0011);
  Write16(wram, address + 0x28, right ? kActRaiserObjectFlip_Horizontal : 0);
  Write16(wram, address + 0x30, 0x0020);
  Write16(wram, address + 0x32, 0xE483);
  Write16(
      wram, address + 0x3A,
      (uint16_t)(kActRaiserWram_ActionObjectTable + parent_slot * kActRaiserActionObjectStride));
}

static void SeedMarahnaBossGroundCharge(uint8_t *wram, unsigned slot, unsigned parent_slot,
                                        bool right, uint16_t visual) {
  const size_t address = kActRaiserWram_ActionObjectTable + slot * kActRaiserActionObjectStride;
  uint16_t composition = 0;
  uint16_t extent = 0;
  switch (visual) {
  case 0x0012:
    composition = 0x5D01;
    extent = 8;
    break;
  case 0x0013:
    composition = 0x5D0D;
    extent = 16;
    break;
  case 0x0014:
    composition = 0x5D2E;
    extent = 16;
    break;
  default:
    break;
  }
  Write16(wram, address + 0x00, 0x0000);
  Write16(wram, address + 0x02, right ? 320 : 192);
  Write16(wram, address + 0x04, 480);
  Write16(wram, address + 0x06, right ? 4 : 0xFFFC);
  Write16(wram, address + 0x08, 0);
  Write16(wram, address + 0x0A, extent);
  Write16(wram, address + 0x0C, extent);
  Write16(wram, address + 0x0E, extent);
  Write16(wram, address + 0x10, extent);
  Write16(wram, address + 0x12, 0x8661);
  Write16(wram, address + 0x16, 0x5000);
  wram[address + 0x18] = 0x7E;
  Write16(wram, address + 0x1A, 0x0007);
  Write16(wram, address + 0x1C, 1);
  Write16(wram, address + 0x1E, 0xE57E);
  Write16(wram, address + 0x20, composition);
  Write16(wram, address + 0x22, visual);
  Write16(wram, address + 0x28, right ? kActRaiserObjectFlip_Horizontal : 0);
  Write16(wram, address + 0x30, 0x0020);
  Write16(wram, address + 0x32, 0xE483);
  Write16(
      wram, address + 0x3A,
      (uint16_t)(kActRaiserWram_ActionObjectTable + parent_slot * kActRaiserActionObjectStride));
}

static void TestMarahnaBossLightningIdentityAndStages(void) {
  uint8_t wram[kActRaiserWramSize];
  ActionSceneEffectFrame frame;
  ActionEffectObserver observer = {0};
  memset(wram, 0, sizeof(wram));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Marahna;
  wram[kActRaiserWram_CurrentMap] = 8;

  SeedMarahnaBossParent(wram, 49, 0, 0x0007, 0x57C2);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].kind == kActionEffect_MarahnaBossLightning);
  CHECK(frame.effects[0].phase == kActionEffectPhase_MarahnaBossLightningCharge);

  SeedMarahnaBossParent(wram, 49, 1, 0x000A, 0x59DE);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].phase == kActionEffectPhase_MarahnaBossLightningOrb);

  SeedMarahnaBossParent(wram, 49, 1, 0x0003, 0x54AC);
  SeedMarahnaBossBolt(wram, 11, 49, false);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].phase == kActionEffectPhase_MarahnaBossLightningBolt);
  CHECK(frame.effects[0].geometry.data.rect.x0 == -32.0f);
  CHECK(frame.effects[0].geometry.data.rect.y0 == 0.0f);
  CHECK(frame.effects[0].geometry.data.rect.x1 == 0.0f);
  CHECK(frame.effects[0].geometry.data.rect.y1 == 32.0f);

  SeedMarahnaBossBolt(wram, 11, 49, true);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].geometry.data.rect.x0 == 0.0f);
  CHECK(frame.effects[0].geometry.data.rect.x1 == 32.0f);

  /* Direction and flip are one measured tuple. This rejects a same-shape
   * impostor before testing the separate post-impact ground lifecycle. */
  Write16(wram, kActRaiserWram_ActionObjectTable + 11 * kActRaiserActionObjectStride + 0x28, 0);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);

  /* The boss enters this exact repeat-animation tuple after impact while the
   * child becomes the ground-riding charge. It is still the backlink owner,
   * but no longer uses the pre-impact `$8661` handler. */
  const size_t parent = kActRaiserWram_ActionObjectTable + 49 * kActRaiserActionObjectStride;
  Write16(wram, parent + 0x12, 0x8683);
  Write16(wram, parent + 0x1A, 0x000A);
  Write16(wram, parent + 0x1E, 0xE4D7);
  Write16(wram, parent + 0x20, 0x5307);
  Write16(wram, parent + 0x22, 0x0000);
  static const uint16_t kGroundVisuals[] = {0x0012, 0x0013, 0x0014};
  for (size_t i = 0; i < sizeof(kGroundVisuals) / sizeof(kGroundVisuals[0]); i++) {
    SeedMarahnaBossGroundCharge(wram, 11, 49, false, kGroundVisuals[i]);
    ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
    CHECK(frame.effect_count == 1);
    CHECK(frame.effects[0].phase == kActionEffectPhase_MarahnaBossLightningGroundCharge);
    CHECK(frame.effects[0].visual == kGroundVisuals[i]);
    CHECK(frame.effects[0].geometry.data.rect.x0 == (kGroundVisuals[i] == 0x0012 ? -8.0f : -16.0f));
  }
  SeedMarahnaBossGroundCharge(wram, 11, 49, true, 0x0014);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].velocity_x == 4);
  CHECK(frame.effects[0].flags & kActionEffectFlag_FlipHorizontal);

  Write16(wram, kActRaiserWram_ActionObjectTable + 11 * kActRaiserActionObjectStride + 0x20,
          0x5D0D);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);

  SeedMarahnaBossGroundCharge(wram, 11, 49, true, 0x0014);
  Write16(wram, parent + 0x1E, 0xE4F4);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);

  Write16(wram, parent + 0x1E, 0xE4D7);
  Write16(wram, kActRaiserWram_ActionObjectTable + 11 * kActRaiserActionObjectStride + 0x06, 3);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  SeedMarahnaBossParent(wram, 49, 1, 0x0003, 0x54AC);
  SeedMarahnaBossBolt(wram, 11, 49, true);
  Write16(wram, kActRaiserWram_ActionObjectTable + 49 * kActRaiserActionObjectStride + 0x32,
          0xE482);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
}

static void TestMarahnaFireballIdentityAndContinuity(void) {
  uint8_t wram[kActRaiserWramSize];
  ActionSceneEffectFrame first, advanced, reused, frame;
  ActionEffectObserver observer = {0};
  memset(wram, 0, sizeof(wram));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Marahna;
  wram[kActRaiserWram_CurrentMap] = 7;
  SeedMarahnaFireball(wram, 20, 400, 500);

  ActionSceneEffects_CaptureFrame(&observer, &first, wram, sizeof(wram), 1);
  CHECK(first.effect_count == 1);
  CHECK(first.visible_count == 1);
  CHECK(first.effects[0].kind == kActionEffect_MarahnaFireball);
  CHECK(first.effects[0].phase == kActionEffectPhase_MarahnaFireballOrb);
  CHECK(first.effects[0].geometry.data.rect.x0 == -8.0f);
  CHECK(first.effects[0].geometry.data.rect.y0 == -8.0f);
  CHECK(first.effects[0].geometry.data.rect.x1 == 8.0f);
  CHECK(first.effects[0].geometry.data.rect.y1 == 8.0f);

  /* State $0C is one eight-entry left/idle/right/idle animation, not a
   * left-only actor. Decode every unique live tuple from `$7E:4000`: four
   * artwork pairs and both one/two-pixel movement entries. */
  static const struct {
    uint16_t visual, composition;
    int16_t velocity_x;
  } kOrbFrames[] = {
      {0x0007, 0x451C, 0}, {0x0008, 0x4528, -1}, {0x0008, 0x4528, -2},
      {0x0005, 0x4504, 0}, {0x0006, 0x4510, 1},  {0x0006, 0x4510, 2},
  };
  for (size_t i = 0; i < sizeof(kOrbFrames) / sizeof(kOrbFrames[0]); i++) {
    SetMarahnaFireballOrbFrame(wram, 20, kOrbFrames[i].visual, kOrbFrames[i].composition,
                               kOrbFrames[i].velocity_x);
    ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
    CHECK(frame.effect_count == 1);
    CHECK(frame.effects[0].phase == kActionEffectPhase_MarahnaFireballOrb);
  }
  SetMarahnaFireballOrbFrame(wram, 20, 0x0006, 0x4510, -1);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  ActionEffectObserver_Reset(&observer);
  SeedMarahnaFireball(wram, 20, 400, 500);
  ActionSceneEffects_CaptureFrame(&observer, &first, wram, sizeof(wram), 1);

  Write16(wram, kActRaiserWram_ActionObjectTable + 20 * kActRaiserActionObjectStride + 0x02, 401);
  ActionSceneEffects_CaptureFrame(&observer, &advanced, wram, sizeof(wram), 1);
  CHECK(advanced.effects[0].generation == first.effects[0].generation);
  CHECK(advanced.effects[0].age_ticks == 1);

  /* Immediate same-slot reuse by the same directional source retains every
   * signature word. The discontinuous spawn position is therefore essential
   * to prevent a replacement fireball inheriting the old flame trail. */
  Write16(wram, kActRaiserWram_ActionObjectTable + 20 * kActRaiserActionObjectStride + 0x02, 900);
  ActionSceneEffects_CaptureFrame(&observer, &reused, wram, sizeof(wram), 1);
  CHECK(reused.effect_count == 1);
  CHECK(reused.effects[0].generation != advanced.effects[0].generation);
  CHECK(reused.effects[0].age_ticks == 0);

  /* The exact orb becomes an inactive lifecycle anchor while its four
   * children travel down/left/up/right. Every child validates that backlink,
   * its measured cardinal velocity, artwork, bounds, and corresponding flip. */
  SeedMarahnaFireball(wram, 20, 400, 500);
  const size_t parent = kActRaiserWram_ActionObjectTable + 20 * kActRaiserActionObjectStride;
  Write16(wram, parent + 0x00, 0x4000);
  Write16(wram, parent + 0x1A, 0x000E);
  Write16(wram, parent + 0x1E, 0xE0A6);
  Write16(wram, parent + 0x20, 0x4597);
  Write16(wram, parent + 0x22, 0x000C);
  static const int16_t kSplitVelocity[][2] = {
      {0, 3},
      {-3, 0},
      {0, -3},
      {3, 0},
  };
  for (size_t i = 0; i < 4; i++)
    SeedMarahnaSplitFireball(wram, 35 + (unsigned)i, 20, kSplitVelocity[i][0],
                             kSplitVelocity[i][1]);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 4);
  for (size_t i = 0; i < frame.effect_count; i++) {
    CHECK(frame.effects[i].kind == kActionEffect_MarahnaFireball);
    CHECK(frame.effects[i].phase == kActionEffectPhase_MarahnaFireballSplit);
    CHECK(frame.effects[i].geometry.data.rect.x0 == -4.0f);
    CHECK(frame.effects[i].geometry.data.rect.x1 == 4.0f);
  }
  CHECK(!(frame.effects[0].flags &
          (kActionEffectFlag_FlipHorizontal | kActionEffectFlag_FlipVertical)));
  CHECK(!(frame.effects[1].flags &
          (kActionEffectFlag_FlipHorizontal | kActionEffectFlag_FlipVertical)));
  CHECK(frame.effects[2].flags & kActionEffectFlag_FlipVertical);
  CHECK(frame.effects[3].flags & kActionEffectFlag_FlipHorizontal);

  const size_t child = kActRaiserWram_ActionObjectTable + 35 * kActRaiserActionObjectStride;
  Write16(wram, child + 0x08, 4);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 3);

  /* The $34/$4BE5 records observed in snap_02_gf7970 are moving platforms,
   * despite their earlier fire-like appearance. Reproduce that tempting
   * combination and prove it cannot enter the projectile family. */
  memset(wram + child, 0, kActRaiserActionObjectStride);
  Write16(wram, child + 0x02, 400);
  Write16(wram, child + 0x04, 500);
  Write16(wram, child + 0x0A, 16);
  Write16(wram, child + 0x0C, 8);
  Write16(wram, child + 0x0E, 16);
  Write16(wram, child + 0x10, 8);
  Write16(wram, child + 0x12, 0x8661);
  Write16(wram, child + 0x16, 0x4000);
  wram[child + 0x18] = 0x7E;
  Write16(wram, child + 0x1A, 0x0033);
  Write16(wram, child + 0x1E, 0xE33C);
  Write16(wram, child + 0x20, 0x4BE5);
  Write16(wram, child + 0x22, 0x0034);
  Write16(wram, child + 0x30, 0x0020);
  Write16(wram, child + 0x32, 0xE304);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 3);

  memset(wram, 0, sizeof(wram));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Marahna;
  wram[kActRaiserWram_CurrentMap] = 7;
  SeedMarahnaFireball(wram, 20, 400, 500);
  wram[kActRaiserWram_CurrentMap] = 3;
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
}

static void TestMarahnaSnakeFireballIdentity(void) {
  uint8_t wram[kActRaiserWramSize];
  ActionSceneEffectFrame frame;
  ActionEffectObserver observer = {0};
  memset(wram, 0, sizeof(wram));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Marahna;
  wram[kActRaiserWram_CurrentMap] = 6;

  static const struct {
    bool flip;
    uint16_t visual, composition;
  } kShots[] = {
      {false, 0x001D, 0x4869},
      {false, 0x001E, 0x487C},
      {true, 0x001D, 0x4869},
      {true, 0x001E, 0x487C},
  };
  for (size_t i = 0; i < sizeof(kShots) / sizeof(kShots[0]); i++) {
    memset(wram + kActRaiserWram_ActionObjectTable, 0,
           kActRaiserActionObjectCount * kActRaiserActionObjectStride);
    SeedMarahnaSnakeFireballShot(wram, 20, 30, kShots[i].flip, kShots[i].visual,
                                 kShots[i].composition);
    ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
    CHECK(frame.effect_count == 1);
    CHECK(frame.effects[0].kind == kActionEffect_MarahnaFireball);
    CHECK(frame.effects[0].phase == kActionEffectPhase_MarahnaSnakeFireballShot);
  }

  const size_t shot = kActRaiserWram_ActionObjectTable + 30 * kActRaiserActionObjectStride;
  const size_t parent = kActRaiserWram_ActionObjectTable + 20 * kActRaiserActionObjectStride;
  Write16(wram, shot + 0x06, 3);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  Write16(wram, shot + 0x06, 4);
  Write16(wram, shot + 0x38, 5);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  Write16(wram, shot + 0x38, 6);
  Write16(wram, parent + 0x28, 0);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  Write16(wram, parent + 0x28, kActRaiserObjectFlip_Horizontal);
  Write16(wram, shot + 0x20, 0x487D);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  Write16(wram, shot + 0x20, 0x487C);
  Write16(wram, shot + 0x3A, 0x1234);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);

  /* Run 20260811-232640, snap 0: the reaper's source-$E0BA falling orb uses
   * the same generic fire presentation kind but is not a snake projectile. */
  memset(wram + kActRaiserWram_ActionObjectTable, 0,
         kActRaiserActionObjectCount * kActRaiserActionObjectStride);
  const size_t reaper_parent = 0x0C60;
  Write16(wram, reaper_parent + 0x00, 0x0000);
  Write16(wram, reaper_parent + 0x02, 2096);
  Write16(wram, reaper_parent + 0x04, 504);
  Write16(wram, reaper_parent + 0x0A, 16);
  Write16(wram, reaper_parent + 0x0C, 24);
  Write16(wram, reaper_parent + 0x0E, 32);
  Write16(wram, reaper_parent + 0x10, 24);
  Write16(wram, reaper_parent + 0x12, 0x8661);
  Write16(wram, reaper_parent + 0x16, 0x4000);
  wram[reaper_parent + 0x18] = 0x7E;
  Write16(wram, reaper_parent + 0x1A, 0x0014);
  Write16(wram, reaper_parent + 0x1C, 1);
  Write16(wram, reaper_parent + 0x1E, 0xE0F4);
  Write16(wram, reaper_parent + 0x20, 0x4654);
  Write16(wram, reaper_parent + 0x22, 0x0013);
  Write16(wram, reaper_parent + 0x28, kActRaiserObjectFlip_Horizontal);
  Write16(wram, reaper_parent + 0x32, 0xE0BA);
  const size_t reaper = kActRaiserWram_ActionObjectTable;
  Write16(wram, reaper + 0x00, 0x0000);
  Write16(wram, reaper + 0x02, 2132);
  Write16(wram, reaper + 0x04, 629);
  Write16(wram, reaper + 0x08, 3);
  Write16(wram, reaper + 0x0A, 8);
  Write16(wram, reaper + 0x0C, 8);
  Write16(wram, reaper + 0x0E, 8);
  Write16(wram, reaper + 0x10, 8);
  Write16(wram, reaper + 0x12, 0x8661);
  Write16(wram, reaper + 0x16, 0x4000);
  wram[reaper + 0x18] = 0x7E;
  Write16(wram, reaper + 0x1A, 0x003C);
  Write16(wram, reaper + 0x1E, 0xE181);
  Write16(wram, reaper + 0x20, 0x4827);
  Write16(wram, reaper + 0x22, 0x001B);
  Write16(wram, reaper + 0x28, kActRaiserObjectFlip_Horizontal);
  Write16(wram, reaper + 0x30, 0x0020);
  Write16(wram, reaper + 0x32, 0xE0BA);
  Write16(wram, reaper + 0x3A, (uint16_t)reaper_parent);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
}

static void TestMarahnaLightningLinkIdentityAndOrientations(void) {
  uint8_t wram[kActRaiserWramSize];
  ActionSceneEffectFrame frame;
  ActionEffectObserver observer = {0};
  memset(wram, 0, sizeof(wram));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Marahna;
  wram[kActRaiserWram_CurrentMap] = 6;

  SeedMarahnaLightningEndpoint(wram, 30, false, false, 280, 680);
  SeedMarahnaLightningEndpoint(wram, 31, true, false, 360, 680);
  SeedMarahnaLightningLink(wram, 53, 30, false, 320, 680);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.visible_count == 1);
  CHECK(frame.effects[0].kind == kActionEffect_MarahnaLightningLink);
  CHECK(frame.effects[0].phase == kActionEffectPhase_MarahnaLightningActive);
  CHECK(frame.effects[0].visual == 0x2E);
  CHECK(frame.effects[0].geometry.data.rect.x0 == -40.0f);
  CHECK(frame.effects[0].geometry.data.rect.y0 == -4.0f);
  CHECK(frame.effects[0].geometry.data.rect.x1 == 40.0f);
  CHECK(frame.effects[0].geometry.data.rect.y1 == 4.0f);

  SeedMarahnaLightningEndpoint(wram, 30, false, true, 352, 608);
  SeedMarahnaLightningEndpoint(wram, 31, true, true, 352, 688);
  SeedMarahnaLightningLink(wram, 53, 30, true, 352, 648);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].visual == 0x31);
  CHECK(frame.effects[0].animation_state == 0x28);
  CHECK(frame.effects[0].geometry.data.rect.x0 == -5.0f);
  CHECK(frame.effects[0].geometry.data.rect.y0 == -40.0f);
  CHECK(frame.effects[0].geometry.data.rect.x1 == 5.0f);
  CHECK(frame.effects[0].geometry.data.rect.y1 == 40.0f);

  /* Composition identity alone is insufficient. The child must be exactly
   * between its validated source/partner actors. */
  Write16(wram, kActRaiserWram_ActionObjectTable + 53 * kActRaiserActionObjectStride + 0x02, 353);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  SeedMarahnaLightningLink(wram, 53, 30, true, 352, 648);
  Write16(wram, kActRaiserWram_ActionObjectTable + 31 * kActRaiserActionObjectStride + 0x20,
          0x45DD);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  SeedMarahnaLightningEndpoint(wram, 31, true, true, 352, 688);
  wram[kActRaiserWram_CurrentMap] = 8;
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
}

static void TestAitosLavaPitIdentityAndWindow(void) {
  uint8_t wram[kActRaiserWramSize];
  ActionSceneEffectFrame frame;
  ActionEffectObserver observer = {0};
  memset(wram, 0, sizeof(wram));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Aitos;
  wram[kActRaiserWram_CurrentMap] = 1;
  Write16(wram, kActRaiserWram_GameFrame, 49363);
  Write16(wram, kActRaiserWram_Bg1CameraX, 3645);
  Write16(wram, kActRaiserWram_Bg1CameraY, 760);
  Write16(wram, kActRaiserWram_Bg1Width, 4096);
  Write16(wram, kActRaiserWram_Bg1Height, 1024);
  Write16(wram, kActRaiserWram_BgMapPage, 0x8000);

  /* The observed wide pit is $DC + six $DD + $DE, over complete $DF/$E7
   * bubbly rows. */
  SeedBgMetatile(wram, 4096, 3616, 928, 0xDC);
  for (unsigned cell = 1; cell <= 6; cell++)
    SeedBgMetatile(wram, 4096, 3616 + cell * 16, 928, 0xDD);
  SeedBgMetatile(wram, 4096, 3728, 928, 0xDE);
  for (unsigned cell = 0; cell < 8; cell++)
    SeedBgMetatile(wram, 4096, 3616 + cell * 16, 944, 0xDF);
  for (unsigned cell = 0; cell < 8; cell++)
    SeedBgMetatile(wram, 4096, 3616 + cell * 16, 960, 0xE7);

  /* A second exact pit exists in the shared world but must not consume scene
   * capacity until its own camera region is active. */
  SeedBgMetatile(wram, 4096, 1648, 976, 0xDC);
  SeedBgMetatile(wram, 4096, 1664, 976, 0xDD);
  SeedBgMetatile(wram, 4096, 1680, 976, 0xDD);
  SeedBgMetatile(wram, 4096, 1696, 976, 0xDE);
  for (unsigned cell = 0; cell < 4; cell++)
    SeedBgMetatile(wram, 4096, 1648 + cell * 16, 992, 0xDF);
  for (unsigned cell = 0; cell < 4; cell++)
    SeedBgMetatile(wram, 4096, 1648 + cell * 16, 1008, 0xE7);

  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 1);
  CHECK(frame.decoration_visible_count == 1);
  CHECK(frame.decorations[0].kind == kActionEffect_AitosLavaPit);
  CHECK(frame.decorations[0].phase == kActionEffectPhase_AitosLavaPit);
  CHECK(frame.decorations[0].world_x == 3680);
  CHECK(frame.decorations[0].world_y == 960);
  CHECK(frame.decorations[0].geometry.data.rect.x0 == -64.0f);
  CHECK(frame.decorations[0].geometry.data.rect.y0 == -16.0f);
  CHECK(frame.decorations[0].geometry.data.rect.x1 == 64.0f);
  CHECK(frame.decorations[0].geometry.data.rect.y1 == 16.0f);
  CHECK(frame.decorations[0].projection_plane == kActionEffectProjectionPlane_Bg1);
  CHECK(frame.decorations[0].phase_ticks == 49363);

  /* When an `$E7` row fits inside the authored map, every cell remains
   * load-bearing; a partial lower bubble row must not publish a shorter glow. */
  SeedBgMetatile(wram, 4096, 3728, 960, 0xF7);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 0);
  SeedBgMetatile(wram, 4096, 3728, 960, 0xE7);

  /* Rim artwork without its exact first fill row is not the lava semantic. */
  SeedBgMetatile(wram, 4096, 3616, 944, 0xE7);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 0);
  SeedBgMetatile(wram, 4096, 3616, 944, 0xDF);
  wram[kActRaiserWram_CurrentMap] = 2;
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 0);
}

static void TestAitosAct2SideLavaReservoirIdentity(void) {
  uint8_t wram[kActRaiserWramSize] = {0};
  ActionSceneEffectFrame frame;
  ActionEffectObserver observer = {0};
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Aitos;
  wram[kActRaiserWram_CurrentMap] = 5;
  CHECK((ActRaiserRoom_ProfileFor(kActRaiserMapGroup_Aitos, 4) ==
         kActRaiserRoomProfile_AitosAct2Lava));
  CHECK((ActRaiserRoom_ProfileFor(kActRaiserMapGroup_Aitos, 5) ==
         kActRaiserRoomProfile_AitosAct2Lava));
  CHECK((ActRaiserRoom_ProfileFor(kActRaiserMapGroup_Aitos, 6) ==
         kActRaiserRoomProfile_AitosAct2Lava));
  CHECK(!(ActRaiserRoom_ProfileFor(kActRaiserMapGroup_Aitos, 3) ==
          kActRaiserRoomProfile_AitosAct2Lava));
  CHECK(!(ActRaiserRoom_ProfileFor(3, 5) == kActRaiserRoomProfile_AitosAct2Lava));
  Write16(wram, kActRaiserWram_GameFrame, 5009);
  Write16(wram, kActRaiserWram_Bg1Width, 512);
  Write16(wram, kActRaiserWram_Bg1Height, 256);
  Write16(wram, kActRaiserWram_BgMapPage, 0x8000);

  /* A maximal six-cell $01 lip, its measured $2C/$32 banks, animated air
   * cells above, and red $05 body below. This is the side-on Act 2 semantic,
   * not Act 1's $DC..$E7 isometric pit mouth. */
  const unsigned x = 48, y = 96, cells = 6;
  SeedBgMetatile(wram, 512, x - 16, y, 0x2C);
  SeedBgMetatile(wram, 512, x + cells * 16, y, 0x32);
  for (unsigned cell = 0; cell < cells; cell++) {
    SeedBgMetatile(wram, 512, x + cell * 16, y, 0x01);
    SeedBgMetatile(wram, 512, x + cell * 16, y - 16, cell & 1u ? 0x00 : 0x02);
    SeedBgMetatile(wram, 512, x + cell * 16, y + 16, 0x05);
  }
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 1);
  CHECK(frame.decorations[0].kind == kActionEffect_AitosLavaReservoir);
  CHECK(frame.decorations[0].phase == kActionEffectPhase_AitosLavaReservoir);
  CHECK(frame.decorations[0].world_x == 96);
  CHECK(frame.decorations[0].world_y == 100);
  CHECK(frame.decorations[0].geometry.data.rect.x0 == -48.0f);
  CHECK(frame.decorations[0].geometry.data.rect.x1 == 48.0f);
  CHECK(frame.decorations[0].geometry.data.rect.y0 == -4.0f);
  CHECK(frame.decorations[0].geometry.data.rect.y1 == 4.0f);
  CHECK(frame.decorations[0].projection_plane == kActionEffectProjectionPlane_Bg1High);
  CHECK(frame.decorations[0].render_layer == kActionEffectRenderLayer_Bg1HighPlane);

  /* Animation, body, and exact banks are all load-bearing: an isolated $01
   * floor texture must never turn into a room-wide heat emitter. */
  for (unsigned cell = 0; cell < cells; cell++)
    SeedBgMetatile(wram, 512, x + cell * 16, y - 16, 0x00);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 0);
  SeedBgMetatile(wram, 512, x, y - 16, 0x02);
  SeedBgMetatile(wram, 512, x + cells * 16, y, 0x31);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 0);
  SeedBgMetatile(wram, 512, x + cells * 16, y, 0x32);
  wram[kActRaiserWram_CurrentMap] = 7;
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 0);

  /* Map $06's measured lake is wider than a camera. Its true left bank can
   * sit beyond the bounded scan window while the visible window begins on a
   * $01 interior cell; the capture must still recover one maximal emitter. */
  memset(wram + 0x8000, 0, 0x10000);
  wram[kActRaiserWram_CurrentMap] = 6;
  Write16(wram, kActRaiserWram_Bg1Width, 1024);
  Write16(wram, kActRaiserWram_Bg1CameraX, 500);
  const unsigned wide_cells = 40;
  SeedBgMetatile(wram, 1024, x - 16, y, 0x33);
  SeedBgMetatile(wram, 1024, x + wide_cells * 16, y, 0x34);
  for (unsigned cell = 0; cell < wide_cells; cell++) {
    SeedBgMetatile(wram, 1024, x + cell * 16, y, 0x01);
    SeedBgMetatile(wram, 1024, x + cell * 16, y - 16, cell % 5u ? 0x00 : 0x77);
    SeedBgMetatile(wram, 1024, x + cell * 16, y + 16, 0x05);
  }
  ActionEffectObserver_Reset(&observer);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 1);
  CHECK(frame.decorations[0].world_x == 368);
  CHECK(frame.decorations[0].geometry.data.rect.x0 == -320.0f);
  CHECK(frame.decorations[0].geometry.data.rect.x1 == 320.0f);
}

static void TestAitosMoltenRockIdentity(void) {
  uint8_t wram[kActRaiserWramSize] = {0};
  ActionSceneEffectFrame frame;
  ActionEffectObserver observer = {0};
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Aitos;
  wram[kActRaiserWram_CurrentMap] = 1;
  SeedAitosMoltenRock(wram, 41, 2092, 786, -2, -1);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].kind == kActionEffect_AitosMoltenRock);
  CHECK(frame.effects[0].phase == kActionEffectPhase_AitosMoltenRockFlight);
  CHECK(frame.effects[0].velocity_x == -2 && frame.effects[0].velocity_y == -1);

  /* `$CEEC/$CF1C` stationary lava-mouth tiles share the artwork but are not
   * launched rocks. Resume, motion and flip all remain load-bearing. */
  const size_t address = kActRaiserWram_ActionObjectTable + 41 * kActRaiserActionObjectStride;
  Write16(wram, address + 0x1E, 0xCF1C);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  SeedAitosMoltenRock(wram, 41, 2092, 786, 2, 1);
  Write16(wram, address + 0x28, 0);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
}

static void SeedAitosSplashPlatform(uint8_t *wram, unsigned world_width, unsigned x, unsigned y,
                                    unsigned cells) {
  for (unsigned cell = 0; cell < cells; cell++) {
    const bool left = cell == 0;
    const bool right = cell + 1u == cells;
    SeedBgMetatile(wram, world_width, x + cell * 16, y, left ? 0x36 : right ? 0x81 : 0x5E);
    SeedBgMetatile(wram, world_width, x + cell * 16, y + 16, left ? 0x4E : right ? 0x4F : 0xF4);
    SeedBgMetatile(wram, world_width, x + cell * 16, y + 32, left ? 0xF6 : right ? 0xFE : 0xFC);
  }
}

static void TestAitosWaterfallSplashIdentity(void) {
  uint8_t wram[kActRaiserWramSize] = {0};
  ActionSceneEffectFrame frame;
  ActionEffectObserver observer = {0};
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Aitos;
  wram[kActRaiserWram_CurrentMap] = 2;
  Write16(wram, kActRaiserWram_GameFrame, 12108);
  Write16(wram, kActRaiserWram_Bg1CameraX, 728);
  Write16(wram, kActRaiserWram_Bg1CameraY, 488);
  Write16(wram, kActRaiserWram_Bg2CameraX, 728);
  Write16(wram, kActRaiserWram_Bg2CameraY, 488);
  Write16(wram, kActRaiserWram_Bg1Width, 1792);
  Write16(wram, kActRaiserWram_Bg1Height, 768);
  Write16(wram, kActRaiserWram_BgMapPage, 0x8000);
  SeedAitosSplashPlatform(wram, 1792, 896, 480, 4);

  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 3);
  CHECK(frame.decorations[0].kind == kActionEffect_AitosWaterSplash);
  CHECK(frame.decorations[0].world_x == 928 && frame.decorations[0].world_y == 496);
  CHECK(frame.decorations[0].geometry.data.rect.x0 == -32.0f);
  CHECK(frame.decorations[0].geometry.data.rect.x1 == 32.0f);
  CHECK(frame.decorations[0].projection_plane == kActionEffectProjectionPlane_Bg1);
  CHECK(frame.decorations[1].kind == kActionEffect_AitosWaterfall);
  CHECK(frame.decorations[1].projection_plane == kActionEffectProjectionPlane_Bg2);
  CHECK(frame.decorations[1].render_layer == kActionEffectRenderLayer_Bg2Plane);
  CHECK(frame.decorations[1].world_x == 856 && frame.decorations[1].world_y == 600);
  CHECK(frame.decorations[1].geometry.data.rect.y0 == -176.0f);
  CHECK(frame.decorations[1].geometry.data.rect.y1 == 312.0f);
  CHECK(frame.decorations[2].kind == kActionEffect_AitosWaterfallMist);
  CHECK(frame.decorations[2].projection_plane == kActionEffectProjectionPlane_Bg2);
  CHECK(frame.decorations[2].render_layer == kActionEffectRenderLayer_Atmosphere);
  CHECK(frame.decorations[2].world_x == 856 && frame.decorations[2].world_y == 736);
  CHECK(frame.decorations[2].world_y - 488 ==
        kActRaiserAuthenticHeight + kActionBgAitosWaterfallBottomExtensionPixels);
  CHECK(frame.decorations[2].geometry.data.rect.y0 == -64.0f);
  CHECK(frame.decorations[2].geometry.data.rect.y1 == 152.0f);

  /* Both observed waterfall subsections use the same exact positive
   * structure; the map range itself must not accidentally stop at `$02`. */
  wram[kActRaiserWram_CurrentMap] = 3;
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 3);
  CHECK(frame.decorations[0].kind == kActionEffect_AitosWaterSplash);
  CHECK(frame.decorations[1].kind == kActionEffect_AitosWaterfall);
  CHECK(frame.decorations[2].kind == kActionEffect_AitosWaterfallMist);
  wram[kActRaiserWram_CurrentMap] = 2;

  /* The shared map's cave section has no camera-local splash signature and
   * therefore must not publish a waterfall overlay. */
  Write16(wram, kActRaiserWram_Bg1CameraX, 120);
  Write16(wram, kActRaiserWram_Bg1CameraY, 32);
  Write16(wram, kActRaiserWram_Bg2CameraX, 120);
  Write16(wram, kActRaiserWram_Bg2CameraY, 32);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 0);

  /* One wrong inner row rejects both the platform and its inferred backdrop
   * veil, avoiding visual-number-only matching. */
  Write16(wram, kActRaiserWram_Bg1CameraX, 728);
  Write16(wram, kActRaiserWram_Bg1CameraY, 488);
  SeedBgMetatile(wram, 1792, 912, 496, 0xF5);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 0);
}

static void TestAitosLavaFireballIdentityAndContinuity(void) {
  uint8_t wram[kActRaiserWramSize];
  ActionSceneEffectFrame first, advanced, reused, frame;
  ActionEffectObserver observer = {0};
  memset(wram, 0, sizeof(wram));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Aitos;
  wram[kActRaiserWram_CurrentMap] = 1;
  SeedAitosLavaFireball(wram, 20, 3696, 964, 0x0022);

  ActionSceneEffects_CaptureFrame(&observer, &first, wram, sizeof(wram), 1);
  CHECK(first.effect_count == 1);
  CHECK(first.visible_count == 1);
  CHECK(first.effects[0].kind == kActionEffect_AitosLavaFireball);
  CHECK(first.effects[0].phase == kActionEffectPhase_AitosLavaFireballFlight);
  CHECK(first.effects[0].geometry.data.rect.x0 == -8.0f);
  CHECK(first.effects[0].geometry.data.rect.y0 == -8.0f);
  CHECK(first.effects[0].geometry.data.rect.x1 == 8.0f);
  CHECK(first.effects[0].geometry.data.rect.y1 == 8.0f);

  Write16(wram, kActRaiserWram_ActionObjectTable + 20 * kActRaiserActionObjectStride + 0x04, 960);
  ActionSceneEffects_CaptureFrame(&observer, &advanced, wram, sizeof(wram), 1);
  CHECK(advanced.effects[0].generation == first.effects[0].generation);
  CHECK(advanced.effects[0].age_ticks == 1);

  /* The emitter cyclically reuses its slot. A new launch at the pit must not
   * inherit the prior projectile's particle clock. */
  Write16(wram, kActRaiserWram_ActionObjectTable + 20 * kActRaiserActionObjectStride + 0x04, 1000);
  ActionSceneEffects_CaptureFrame(&observer, &reused, wram, sizeof(wram), 1);
  CHECK(reused.effects[0].generation != advanced.effects[0].generation);
  CHECK(reused.effects[0].age_ticks == 0);

  static const uint16_t kStates[] = {0x0022, 0x0023, 0x0024};
  for (size_t i = 0; i < sizeof(kStates) / sizeof(kStates[0]); i++) {
    SeedAitosLavaFireball(wram, 20, 3696, 900, kStates[i]);
    ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
    CHECK(frame.effect_count == 1);
    CHECK(frame.effects[0].kind == kActionEffect_AitosLavaFireball);
  }

  const size_t address = kActRaiserWram_ActionObjectTable + 20 * kActRaiserActionObjectStride;
  Write16(wram, address + 0x20, 0x4D20);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);

  /* State/handler alone is not the measured lifecycle. A corrupted or reused
   * slot with the wrong motion must fail closed before presentation derives a
   * trail heading from it. */
  SeedAitosLavaFireball(wram, 20, 3696, 900, 0x0022);
  Write16(wram, address + 0x08, 0xFFFD);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);

  SeedAitosLavaFireball(wram, 20, 3696, 900, 0x0022);
  wram[kActRaiserWram_CurrentMap] = 2;
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
}

static void SeedAitosStatueFire(uint8_t *wram, unsigned slot, uint16_t source, uint16_t state,
                                uint16_t visual, uint16_t composition) {
  const size_t address = kActRaiserWram_ActionObjectTable + slot * kActRaiserActionObjectStride;
  const bool flipped = source == 0xD5C0;
  uint16_t left = 16, right = 16;
  if (visual == 0x0017)
    left = right = 4;
  else if (visual == 0x001D)
    right = 32;
  else if (visual == 0x001E || visual == 0x001F)
    right = 48;
  if (flipped) {
    const uint16_t swap = left;
    left = right;
    right = swap;
  }
  Write16(wram, address + 0x00, 0x0000);
  Write16(wram, address + 0x02, (uint16_t)(1400 + slot * 8));
  Write16(wram, address + 0x04, (uint16_t)(480 + slot * 16));
  Write16(wram, address + 0x0A, left);
  Write16(wram, address + 0x0C, visual == 0x0017 ? 4 : 8);
  Write16(wram, address + 0x0E, right);
  Write16(wram, address + 0x10, visual == 0x0017 ? 4 : 8);
  Write16(wram, address + 0x12, state == 0x0019 ? 0x8683 : (source == 0xD5C0 ? 0xD5CC : 0xD5BD));
  Write16(wram, address + 0x16, 0x4000);
  wram[address + 0x18] = 0x7E;
  Write16(wram, address + 0x1A, state);
  Write16(wram, address + 0x1E, state == 0x0019 ? 0xD5EE : 0x0000);
  Write16(wram, address + 0x20, composition);
  Write16(wram, address + 0x22, visual);
  Write16(wram, address + 0x28, flipped ? kActRaiserObjectFlip_Horizontal : 0);
  Write16(wram, address + 0x30, 0x0030);
  Write16(wram, address + 0x32, source);
}

static void TestAitosStatueFireIdentityAndPriority(void) {
  uint8_t wram[kActRaiserWramSize];
  ActionSceneEffectFrame frame;
  ActionEffectObserver observer = {0};
  memset(wram, 0, sizeof(wram));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Aitos;
  wram[kActRaiserWram_CurrentMap] = 6;
  Write16(wram, kActRaiserWram_SpriteAttributeBias, 0x2000);

  SeedAitosStatueFire(wram, 34, 0xD5B1, 0x0018, 0x001C, 0x4763);
  /* Exact sustained full-pillar frame measured in
   * runs/20260824-041410/snapshots/snap_00_gf6670. */
  SeedAitosStatueFire(wram, 35, 0xD5C0, 0x0019, 0x001E, 0x4790);
  SeedAitosStatueFire(wram, 36, 0xD5B1, 0x0019, 0x001F, 0x47B1);
  /* State $1A's held $17 mouth frame is the inactive interval. It remains a
   * timed actor for activation purposes but must not publish a fire effect. */
  SeedAitosStatueFire(wram, 37, 0xD5C0, 0x001A, 0x0017, 0x46FD);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 3);
  CHECK(frame.visible_count == 3);
  for (unsigned i = 0; i < 3; i++) {
    CHECK(frame.effects[i].kind == kActionEffect_AitosStatueFire);
    CHECK(frame.effects[i].phase == kActionEffectPhase_AitosStatueFireBreath);
    CHECK(frame.effects[i].obj_priority == 2);
  }
  CHECK(frame.effects[0].geometry.data.rect.x0 == -16.0f);
  CHECK(frame.effects[1].geometry.data.rect.x0 == -48.0f);
  CHECK((frame.effects[1].flags & kActionEffectFlag_FlipHorizontal) != 0);
  CHECK(frame.effects[2].visual == 0x001F);
  CHECK(frame.effects[2].geometry.data.rect.x1 == 48.0f);

  /* Drawing and activation are independent: retain lifecycle identity while
   * $0400 is set, but do not submit the frozen margin actor as a live effect. */
  const size_t first_address = kActRaiserWram_ActionObjectTable + 34 * kActRaiserActionObjectStride;
  Write16(wram, first_address + 0x30, 0x0430);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 3);
  CHECK(frame.visible_count == 2);

  /* Same art outside the exact room/source/graphics tuple must fail closed. */
  Write16(wram, first_address + 0x30, 0x0030);
  Write16(wram, first_address + 0x32, 0xD5B0);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 2);
  wram[kActRaiserWram_CurrentMap] = 5;
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
}

static size_t BossEffectSlot(unsigned slot) {
  return kActRaiserWram_ActionObjectTable + slot * kActRaiserActionObjectStride;
}

static void SeedBossFamilyObject(uint8_t *wram, unsigned slot, uint16_t source,
                                 uint16_t composition, uint16_t visual, uint16_t state,
                                 uint16_t resume, uint16_t flags, uint16_t backlink) {
  const size_t address = BossEffectSlot(slot);
  Write16(wram, address + 0x00, 0x0000);
  Write16(wram, address + 0x02, (uint16_t)(240 + slot));
  Write16(wram, address + 0x04, (uint16_t)(180 + slot));
  Write16(wram, address + 0x06, 0xFFFC);
  Write16(wram, address + 0x08, 2);
  Write16(wram, address + 0x0A, 8);
  Write16(wram, address + 0x0C, 8);
  Write16(wram, address + 0x0E, 8);
  Write16(wram, address + 0x10, 8);
  Write16(wram, address + 0x12, 0x8661);
  Write16(wram, address + 0x16, 0x5000);
  wram[address + 0x18] = 0x7E;
  Write16(wram, address + 0x1A, state);
  Write16(wram, address + 0x1E, resume);
  Write16(wram, address + 0x20, composition);
  Write16(wram, address + 0x22, visual);
  Write16(wram, address + 0x30, flags);
  Write16(wram, address + 0x32, source);
  Write16(wram, address + 0x3A, backlink);
}

static void TestBossEffectsCarryIntoDeathHeim(void) {
  uint8_t wram[kActRaiserWramSize];
  ActionSceneEffectFrame frame;
  ActionEffectObserver observer = {0};

  /* Wizard: Death Heim changes only the owning source family. */
  memset(wram, 0, sizeof(wram));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_DeathHeim;
  wram[kActRaiserWram_CurrentMap] = 3;
  SeedBloodpoolBoss(wram);
  SeedBloodpoolBossLightningStrike(wram, 9, 2, false);
  Write16(wram, 0x12E0 + 0x32, 0xF6E2);
  Write16(wram, BossEffectSlot(9) + 0x32, 0xF6E2);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].kind == kActionEffect_BloodpoolBossLightning);
  SeedBloodpoolBossLightningImpact(wram, 10, 9);
  Write16(wram, BossEffectSlot(10) + 0x32, 0xF6E2);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 2);
  CHECK(frame.effects[1].phase == kActionEffectPhase_BossLightningImpact);

  /* Viper: its rematch parent retains the native $001C owner backlink. */
  memset(wram, 0, sizeof(wram));
  ActionEffectObserver_Reset(&observer);
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_DeathHeim;
  wram[kActRaiserWram_CurrentMap] = 6;
  SeedMarahnaBossParent(wram, 49, 0, 0x0007, 0x57C2);
  Write16(wram, BossEffectSlot(49) + 0x32, 0xF72A);
  Write16(wram, BossEffectSlot(49) + 0x3A, 0x001C);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].phase == kActionEffectPhase_MarahnaBossLightningCharge);
  SeedMarahnaBossParent(wram, 49, 1, 0x0003, 0x54AC);
  SeedMarahnaBossBolt(wram, 11, 49, false);
  Write16(wram, BossEffectSlot(49) + 0x32, 0xF72A);
  Write16(wram, BossEffectSlot(49) + 0x3A, 0x001C);
  Write16(wram, BossEffectSlot(11) + 0x32, 0xF72A);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].kind == kActionEffect_MarahnaBossLightning);
  const size_t viper_parent = BossEffectSlot(49);
  Write16(wram, viper_parent + 0x12, 0x8683);
  Write16(wram, viper_parent + 0x1A, 0x000A);
  Write16(wram, viper_parent + 0x1E, 0xE4D7);
  Write16(wram, viper_parent + 0x20, 0x5307);
  Write16(wram, viper_parent + 0x22, 0x0000);
  SeedMarahnaBossGroundCharge(wram, 11, 49, false, 0x0013);
  Write16(wram, BossEffectSlot(11) + 0x32, 0xF72A);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].phase == kActionEffectPhase_MarahnaBossLightningGroundCharge);

  /* Minotaur axe in Fillmore and the first Death Heim rematch. */
  static const struct {
    uint8_t group, map;
    uint16_t source;
  } kAxeRooms[] = {
      {kActRaiserMapGroup_Fillmore, 4, 0xAF5D},
      {kActRaiserMapGroup_DeathHeim, 2, 0xF6CA},
  };
  for (size_t i = 0; i < sizeof(kAxeRooms) / sizeof(kAxeRooms[0]); i++) {
    memset(wram, 0, sizeof(wram));
    ActionEffectObserver_Reset(&observer);
    wram[kActRaiserWram_MapGroup] = kAxeRooms[i].group;
    wram[kActRaiserWram_CurrentMap] = kAxeRooms[i].map;
    SeedBossFamilyObject(wram, 49, kAxeRooms[i].source, 0x5300, 0, 0, 0, 0x4000, 0);
    SeedBossFamilyObject(wram, 11, kAxeRooms[i].source, 0x50FB, 0, 3, 0xB008, 0x0020,
                         (uint16_t)BossEffectSlot(49));
    ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
    CHECK(frame.effect_count == 1);
    CHECK(frame.effects[0].kind == kActionEffect_MinotaurAxe);
  }
  Write16(wram, BossEffectSlot(49) + 0x32, 0xAF5D);
  Write16(wram, BossEffectSlot(11) + 0x32, 0xAF5D);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);

  /* Flaming Wheel's body is the source: illuminate it in both boss rooms. */
  static const struct {
    uint8_t group, map;
    uint16_t source;
  } kWheelRooms[] = {
      {kActRaiserMapGroup_Aitos, 7, 0xD838},
      {kActRaiserMapGroup_DeathHeim, 5, 0xF712},
  };
  for (size_t i = 0; i < sizeof(kWheelRooms) / sizeof(kWheelRooms[0]); i++) {
    const uint8_t expected_priority = i == 0 ? 2 : 1;
    memset(wram, 0, sizeof(wram));
    ActionEffectObserver_Reset(&observer);
    wram[kActRaiserWram_MapGroup] = kWheelRooms[i].group;
    wram[kActRaiserWram_CurrentMap] = kWheelRooms[i].map;
    Write16(wram, kActRaiserWram_SpriteAttributeBias, (uint16_t)(expected_priority << 12));
    /* Recorded body frame: the wheel uses both repeat and delay handlers over
     * its lifecycle, so ownership—not a transient handler—is its discriminator. */
    SeedBossFamilyObject(wram, 49, kWheelRooms[i].source, 0x5276, 0x0005, 7, 0xD85E, 0x4000,
                         kWheelRooms[i].group == kActRaiserMapGroup_DeathHeim ? 0x001C : 0);
    Write16(wram, BossEffectSlot(49) + 0x12, 0x8683);
    ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
    CHECK(frame.effect_count == 1);
    CHECK(frame.effects[0].kind == kActionEffect_FlamingWheel);
    CHECK(frame.effects[0].obj_priority == expected_priority);

    /* snap_05's five cyan shots are exact animation-$5000 children of that
     * root. Pin one direction/frame tuple in both original and rematch rooms. */
    SeedBossFamilyObject(wram, 11, kWheelRooms[i].source, 0x51B5, 0x0000, 0x0008, 0xA65D, 0x0020,
                         (uint16_t)BossEffectSlot(49));
    Write16(wram, BossEffectSlot(11) + 0x06, 0xFFFF);
    Write16(wram, BossEffectSlot(11) + 0x08, 0x0001);
    Write16(wram, BossEffectSlot(11) + 0x38, 0x0008);
    Write16(wram, BossEffectSlot(11) + 0x28, 0x4000);
    ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
    CHECK(frame.effect_count == 2);
    CHECK(frame.effects[0].kind == kActionEffect_FlamingWheelProjectile);
    CHECK(frame.effects[0].phase == kActionEffectPhase_FlamingWheelProjectileFlight);
    CHECK(frame.effects[0].obj_priority == expected_priority);
    CHECK(frame.effects[1].obj_priority == expected_priority);
    Write16(wram, BossEffectSlot(11) + 0x06, 0xFFFE);
    ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
    CHECK(frame.effect_count == 1);

    /* The spawn source is shared by boss-family helpers. A visually plausible
     * child must not become a second full-body flame emitter. */
    SeedBossFamilyObject(wram, 12, kWheelRooms[i].source, 0x5276, 0x0005, 7, 0xD85E, 0x4000,
                         (uint16_t)BossEffectSlot(49));
    Write16(wram, BossEffectSlot(12) + 0x12, 0x8683);
    ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
    CHECK(frame.effect_count == 1);

    /* Conversely, a body that acquires a child-style backlink is no longer the
     * stable root/room-owned wheel and must fail closed. */
    Write16(wram, BossEffectSlot(49) + 0x3A, (uint16_t)BossEffectSlot(12));
    ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
    CHECK(frame.effect_count == 0);

    /* The reported boss frame contains five simultaneous children. They are
     * all independently identified by source/animation/parent ancestry and
     * all inherit the room's live sprite band. This also proves that a Death
     * Heim priority change cannot silently decorate only one direction. */
    static const uint16_t kState[] = {8, 9, 10, 11, 12};
    static const uint16_t kVisual[] = {0, 1, 2, 3, 0};
    static const uint16_t kComposition[] = {
        0x51B5, 0x51C1, 0x51CD, 0x51D9, 0x51B5,
    };
    static const int16_t kVelocity[][2] = {
        {-1, 1}, {0, 1}, {1, 1}, {-1, 0}, {1, 0},
    };
    for (unsigned shot = 0; shot < 5; shot++)
      memset(wram + BossEffectSlot(11 + shot), 0, kActRaiserActionObjectStride);
    SeedBossFamilyObject(wram, 49, kWheelRooms[i].source, 0x5276, 0x0005, 7, 0xD85E, 0x4000,
                         kWheelRooms[i].group == kActRaiserMapGroup_DeathHeim ? 0x001C : 0);
    Write16(wram, BossEffectSlot(49) + 0x12, 0x8683);
    for (unsigned shot = 0; shot < 5; shot++) {
      const unsigned slot = 11 + shot;
      SeedBossFamilyObject(wram, slot, kWheelRooms[i].source, kComposition[shot], kVisual[shot],
                           kState[shot], 0xA65D, 0x0020, (uint16_t)BossEffectSlot(49));
      Write16(wram, BossEffectSlot(slot) + 0x06, (uint16_t)kVelocity[shot][0]);
      Write16(wram, BossEffectSlot(slot) + 0x08, (uint16_t)kVelocity[shot][1]);
      Write16(wram, BossEffectSlot(slot) + 0x28, 0x4000);
      Write16(wram, BossEffectSlot(slot) + 0x38, kState[shot]);
    }
    ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
    CHECK(frame.effect_count == 6);
    CHECK(frame.visible_count == 6);
    for (unsigned shot = 0; shot < 5; shot++) {
      CHECK(frame.effects[shot].kind == kActionEffect_FlamingWheelProjectile);
      CHECK(frame.effects[shot].obj_priority == expected_priority);
    }
    CHECK(frame.effects[5].kind == kActionEffect_FlamingWheel);
    CHECK(frame.effects[5].obj_priority == expected_priority);
  }

  /* Ice Dragon balls retain their exact eight-frame artwork in the rematch. */
  static const struct {
    uint8_t group, map;
    uint16_t source;
  } kIceRooms[] = {
      {kActRaiserMapGroup_Northwall, 8, 0xF161},
      {kActRaiserMapGroup_DeathHeim, 7, 0xF760},
  };
  for (size_t i = 0; i < sizeof(kIceRooms) / sizeof(kIceRooms[0]); i++) {
    memset(wram, 0, sizeof(wram));
    ActionEffectObserver_Reset(&observer);
    wram[kActRaiserWram_MapGroup] = kIceRooms[i].group;
    wram[kActRaiserWram_CurrentMap] = kIceRooms[i].map;
    SeedBossFamilyObject(wram, 54, kIceRooms[i].source, 0x5C00, 0x0011, 0x000C, 0xF280, 0x0020, 0);
    SeedBossFamilyObject(wram, 11, kIceRooms[i].source, 0x5D9C, 0x0012, 0x0019, 0xF2CA, 0x0020,
                         (uint16_t)BossEffectSlot(54));
    ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
    CHECK(frame.effect_count == 1);
    CHECK(frame.effects[0].kind == kActionEffect_IceDragonIceBall);
  }

  /* Tanzara is Death Heim-only and uses several exact projectile families. */
  memset(wram, 0, sizeof(wram));
  ActionEffectObserver_Reset(&observer);
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_DeathHeim;
  wram[kActRaiserWram_CurrentMap] = 8;
  SeedBossFamilyObject(wram, 11, 0xF80F, 0x5D17, 0x0016, 0x0008, 0xFD77, 0x0020, 0);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].kind == kActionEffect_TanzaraProjectile);
  wram[kActRaiserWram_CurrentMap] = 7;
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
}

static void TestSceneCaptureCapacityFailsClosed(void) {
  uint8_t wram[kActRaiserWramSize];
  ActionSceneEffectFrame frame;
  ActionEffectObserver observer = {0};
  memset(wram, 0, sizeof(wram));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Bloodpool;
  wram[kActRaiserWram_CurrentMap] = 2;
  for (unsigned slot = 0; slot < kActionSceneEffectMaxInstances + 1; slot++)
    SeedMeasuredSceneObject(wram, slot, false);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.overflow != 0);
  CHECK(frame.effect_count == 0);
  CHECK(frame.visible_count == 0);

  /* Run 20260812-000613's legal Aitos camera at (864,384) contains fourteen
   * exact splash structures. Together with its BG2 veil and paired bottom
   * mist this exactly fills all 16 decoration slots, while two valid actors
   * must retain independent capacity and publish in the same frame. */
  memset(wram, 0, sizeof(wram));
  ActionEffectObserver_Reset(&observer);
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Aitos;
  wram[kActRaiserWram_CurrentMap] = 2;
  Write16(wram, kActRaiserWram_Bg1CameraX, 864);
  Write16(wram, kActRaiserWram_Bg1CameraY, 384);
  Write16(wram, kActRaiserWram_Bg2CameraX, 864);
  Write16(wram, kActRaiserWram_Bg2CameraY, 384);
  Write16(wram, kActRaiserWram_Bg1Width, 1792);
  Write16(wram, kActRaiserWram_Bg1Height, 768);
  Write16(wram, kActRaiserWram_BgMapPage, 0x8000);
  static const uint16_t kMeasuredStructures[][3] = {
      {896, 480, 4},  {832, 496, 2}, {1072, 496, 2}, {1008, 512, 2}, {1168, 512, 2},
      {1248, 528, 2}, {752, 544, 2}, {1088, 576, 2}, {832, 592, 2},  {1152, 608, 2},
      {752, 624, 3},  {992, 640, 2}, {864, 656, 5},  {1072, 672, 3},
  };
  for (size_t i = 0; i < sizeof(kMeasuredStructures) / sizeof(kMeasuredStructures[0]); i++)
    SeedAitosSplashPlatform(wram, 1792, kMeasuredStructures[i][0], kMeasuredStructures[i][1],
                            kMeasuredStructures[i][2]);
  SeedSwordBeam(wram, 0x13, false);
  const size_t first_beam = kActRaiserWram_ActionObjectTable + 9 * kActRaiserActionObjectStride;
  const size_t second_beam = first_beam + kActRaiserActionObjectStride;
  memcpy(wram + second_beam, wram + first_beam, kActRaiserActionObjectStride);
  Write16(wram, second_beam + 0x02, 248);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_overflow == 0);
  CHECK(frame.decoration_count == 16);
  CHECK(frame.effect_count == 2);
  CHECK(frame.overflow == 0);

  /* A forged fifteenth splash exceeds this stage's measured splash budget.
   * The decoration list fails closed rather than publishing a partial
   * waterfall treatment, while the independent actors remain intact. */
  SeedAitosSplashPlatform(wram, 1792, 1200, 400, 2);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_overflow != 0);
  CHECK(frame.decoration_count == 0);
  CHECK(frame.decoration_visible_count == 0);
  CHECK(frame.effect_count == 2);

  memset(&frame, 0xFF, sizeof(frame));
  ActionSceneEffects_CaptureFrame(NULL, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  CHECK(frame.visible_count == 0);
}

static void TestFirstActBossMagic(void) {
  static const struct {
    uint16_t source, state, visual, composition, resume, handler;
    uint8_t group, map, kind, phase;
  } cases[] = {
#define CENTAUR(st, v, comp, res, h, ph)                                                           \
  {0xAD45, st, v, comp, res, h, 1, 1, kActionEffect_CentaurLightning, ph}
#define NORTHWALL(st, v, comp, res, ph)                                                            \
  {0xE7C6, st, v, comp, res, 0x8661, 6, 4, kActionEffect_NorthwallBossMagic, ph}
      CENTAUR(2, 8, 0x5537, 0xAE0C, 0x8683, kActionEffectPhase_CentaurStaffCharge),
      CENTAUR(2, 9, 0x5677, 0xAE0C, 0x8683, kActionEffectPhase_CentaurStaffCharge),
      CENTAUR(2, 10, 0x57B7, 0xAE0C, 0x8683, kActionEffectPhase_CentaurStaffCharge),
      CENTAUR(8, 11, 0x58F7, 0xAE15, 0x8683, kActionEffectPhase_CentaurStaffCharge),
      CENTAUR(0xD, 0x1D, 0x63FB, 0xAEC7, 0x8661, kActionEffectPhase_BossLightningStrike),
      CENTAUR(0xD, 0x1E, 0x640E, 0xAEC7, 0x8661, kActionEffectPhase_BossLightningStrike),
      CENTAUR(0xD, 0x1F, 0x642F, 0xAEC7, 0x8661, kActionEffectPhase_BossLightningStrike),
      CENTAUR(0xE, 0x20, 0x645E, 0xAECD, 0x8661, kActionEffectPhase_BossLightningStrike),
      CENTAUR(0xE, 0x20, 0x645E, 0xAEEC, 0x8683, kActionEffectPhase_BossLightningStrike),
      CENTAUR(0xB, 0x19, 0x635B, 0xAEF7, 0x8661, kActionEffectPhase_BossLightningStrike),
      CENTAUR(0xB, 0x1A, 0x636E, 0xAEF7, 0x8661, kActionEffectPhase_BossLightningStrike),
      CENTAUR(0xB, 0x1B, 0x638F, 0xAEF7, 0x8661, kActionEffectPhase_BossLightningStrike),
      CENTAUR(0xC, 0x1C, 0x63BE, 0xAEFD, 0x8661, kActionEffectPhase_BossLightningStrike),
      CENTAUR(0xC, 0x1C, 0x63BE, 0xAF1C, 0x8683, kActionEffectPhase_BossLightningStrike),
      CENTAUR(4, 0x21, 0x649B, 0xAF58, 0x8661, kActionEffectPhase_BossLightningImpact),
      CENTAUR(4, 0x22, 0x64A7, 0xAF58, 0x8661, kActionEffectPhase_BossLightningImpact),
      CENTAUR(4, 0x23, 0x64C8, 0xAF58, 0x8661, kActionEffectPhase_BossLightningImpact),
      NORTHWALL(2, 0xA, 0x51A7, 0xE85E, kActionEffectPhase_NorthwallMagicCharge),
      NORTHWALL(2, 0xB, 0x522A, 0xE85E, kActionEffectPhase_NorthwallMagicCharge),
      NORTHWALL(2, 0xC, 0x52AD, 0xE85E, kActionEffectPhase_NorthwallMagicCharge),
      NORTHWALL(2, 0xD, 0x5314, 0xE85E, kActionEffectPhase_NorthwallMagicCharge),
      NORTHWALL(2, 0xE, 0x53AC, 0xE85E, kActionEffectPhase_NorthwallMagicCharge),
      NORTHWALL(0, 9, 0x518D, 0xE8AE, kActionEffectPhase_NorthwallMagicFall),
      NORTHWALL(1, 3, 0x5114, 0xE8BD, kActionEffectPhase_NorthwallMagicImpact),
      NORTHWALL(1, 4, 0x5120, 0xE8BD, kActionEffectPhase_NorthwallMagicImpact),
      NORTHWALL(1, 5, 0x5133, 0xE8BD, kActionEffectPhase_NorthwallMagicImpact),
      NORTHWALL(1, 6, 0x5146, 0xE8BD, kActionEffectPhase_NorthwallMagicImpact),
      NORTHWALL(1, 7, 0x5159, 0xE8BD, kActionEffectPhase_NorthwallMagicImpact),
      NORTHWALL(1, 8, 0x516C, 0xE8BD, kActionEffectPhase_NorthwallMagicImpact),
      NORTHWALL(1, 8, 0x5F00, 0xE8BD, kActionEffectPhase_NorthwallMagicImpact),
      NORTHWALL(1, 8, 0x5F40, 0xE8BD, kActionEffectPhase_NorthwallMagicImpact),
      NORTHWALL(1, 8, 0x5F80, 0xE8BD, kActionEffectPhase_NorthwallMagicImpact),
      NORTHWALL(1, 8, 0x5FC0, 0xE8BD, kActionEffectPhase_NorthwallMagicImpact),
#undef CENTAUR
#undef NORTHWALL
  };
  uint8_t wram[kActRaiserWramSize];
  ActionSceneEffectFrame frame;
  for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
    for (unsigned flip = 0; flip < 2; ++flip) {
      memset(wram, 0, sizeof(wram));
      ActionEffectObserver observer = {0};
      wram[0x18] = cases[c].group;
      wram[0x19] = cases[c].map;
      Write16(wram, 0x8F, 0x2000);
      const unsigned at = 0x8E0;
      Write16(wram, at + 2, 300);
      Write16(wram, at + 4, 200);
      Write16(wram, at + 0x12, cases[c].handler);
      Write16(wram, at + 0x16, 0x5000);
      wram[at + 0x18] = 0x7E;
      Write16(wram, at + 0x1A, cases[c].state);
      Write16(wram, at + 0x1E, cases[c].resume);
      Write16(wram, at + 0x20, cases[c].composition);
      Write16(wram, at + 0x22, cases[c].visual);
      Write16(wram, at + 0x28, flip ? 0x4000 : 0);
      Write16(wram, at + 0x30, 0x4000);
      Write16(wram, at + 0x32, cases[c].source);
      Write16(wram, at + 0x3A, 0x12E0);
      /* No parent liveness dependency: impact actors outlive their allocator. */
      Write16(wram, 0x12E0, 0x4000);
      ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
      CHECK(frame.effect_count == 1 && frame.visible_count == 1);
      const ActionEffectInstance original = frame.effects[0];
      CHECK(original.kind == cases[c].kind && original.phase == cases[c].phase);
      CHECK(original.world_x == 300 && original.world_y == 200 && original.obj_priority == 2);
      CHECK(!!(original.flags & kActionEffectFlag_FlipHorizontal) == !!flip);
      if (original.phase == kActionEffectPhase_CentaurStaffCharge) {
        CHECK((original.geometry.data.rect.x0 + original.geometry.data.rect.x1) * .5f ==
              (flip ? 24 : -24));
        CHECK(original.geometry.data.rect.y0 == -65);
        Write16(wram, at + 0x20, 0x5413);
        Write16(wram, at + 0x22, 7);
        ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 0);
        CHECK(frame.effect_count == 1 && frame.visible_count == 0);
        CHECK(frame.effects[0].generation == original.generation);
        Write16(wram, at + 0x20, cases[c].composition);
        Write16(wram, at + 0x22, cases[c].visual);
      }
      ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 0);
      CHECK(frame.effects[0].generation == original.generation &&
            frame.effects[0].age_ticks == original.age_ticks);
      if (original.kind == kActionEffect_CentaurLightning &&
          original.phase != kActionEffectPhase_CentaurStaffCharge) {
        Write16(wram, at + 0x20, 0x634F);
        Write16(wram, at + 0x22, 0x18);
        ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
        CHECK(frame.effect_count == 1 && frame.visible_count == 0);
        CHECK(frame.effects[0].generation == original.generation);
        Write16(wram, at + 0x20, cases[c].composition);
        Write16(wram, at + 0x22, cases[c].visual);
      }
      /* Check the published workspace independently of its shared constants:
       * only the four pose starts are valid, not slot interiors or adjacent
       * aligned addresses. Both horizontal facings use the same workspace. */
      if (cases[c].source == 0xE7C6 && cases[c].composition == 0x5F00) {
        for (unsigned composition = 0x5EC0; composition <= 0x6040; ++composition) {
          Write16(wram, at + 0x20, (uint16_t)composition);
          ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
          const bool expected = composition == 0x5F00 || composition == 0x5F40 ||
                                composition == 0x5F80 || composition == 0x5FC0;
          CHECK(frame.effect_count == (expected ? 1 : 0));
          if (expected) {
            CHECK(frame.effects[0].kind == kActionEffect_NorthwallBossMagic);
            CHECK(frame.effects[0].phase == kActionEffectPhase_NorthwallMagicImpact);
          }
        }
        Write16(wram, at + 0x20, cases[c].composition);
      }
      /* Exact family guards reject stale/copied art and wrong rooms. */
      const unsigned fields[] = {0x12, 0x16, 0x1E, 0x20, 0x32};
      for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        unsigned field = at + fields[i];
        uint16_t old = (uint16_t)(wram[field] | (wram[field + 1] << 8));
        Write16(wram, field, old ^ 1);
        ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
        CHECK(frame.effect_count == 0);
        Write16(wram, field, old);
      }
      wram[0x19] = 8;
      ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
      CHECK(frame.effect_count == 0);
      wram[0x19] = cases[c].map;
      Write16(wram, at + 0x28, 0x8000);
      ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
      CHECK(frame.effect_count == 0);
    }
  }
}

static void TestForestEnvironmentalCapture(void) {
  static uint8_t wram[kActRaiserWramSize], before[kActRaiserWramSize];
  ActionEffectObserver observer = {0};
  ActionSceneEffectFrame frame;
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Fillmore;
  wram[kActRaiserWram_CurrentMap] = 1;
  Write16(wram, kActRaiserWram_GameFrame, 120);
  Write16(wram, kActRaiserWram_Bg1Width, 4096);
  Write16(wram, kActRaiserWram_Bg1Height, 768);
  Write16(wram, kActRaiserWram_Bg2Width, 2304);
  Write16(wram, kActRaiserWram_Bg2Height, 512);
  Write16(wram, 0x4A, 0xC000);
  /* Room readiness signature: page 0, cells (9,7) and (9,8). */
  wram[0xC000 + 7 * 16 + 9] = 0x0F;
  wram[0xC000 + 8 * 16 + 9] = 0x01;
  memcpy(before, wram, sizeof(wram));
  CHECK(ActionSceneEffects_RoomUsesBg2Decorations(wram, sizeof(wram)));
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 0);
  CHECK(frame.decoration_count == 0); /* Optional capture is a separate gate. */
  ActionEnvironmentalEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram));
  CHECK(frame.decoration_count == 3 && !frame.decoration_overflow);
  CHECK(frame.effects[0].kind == kActionEffect_None && frame.effect_count == 0);
  const ActionEffectInstance first = frame.decorations[0];
  CHECK(first.world_x == 128 && first.world_y == -160);
  CHECK(first.projection_plane == kActionEffectProjectionPlane_BetweenBackgrounds);
  CHECK(first.render_layer == kActionEffectRenderLayer_Bg2Plane);
  CHECK(!memcmp(wram, before, sizeof(wram)));

  /* Vblank/frame changes while paused do not age host atmosphere. */
  Write16(wram, kActRaiserWram_GameFrame, 160);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 0);
  ActionEnvironmentalEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram));
  CHECK(!memcmp(&first, &frame.decorations[0], sizeof(first)));
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 3);
  ActionEnvironmentalEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram));
  CHECK(frame.decorations[0].phase_ticks == first.phase_ticks + 3);
  CHECK(frame.decorations[0].generation == first.generation);
  /* Traverse the full camera range: wash, foliage and foreground light, stable
   * identities, offscreen origin, and exact finite world clipping bounds. */
  for (int cx = 0; cx <= 3840; cx += 64) {
    Write16(wram, kActRaiserWram_Bg1CameraX, (uint16_t)cx);
    Write16(wram, kActRaiserWram_Bg2CameraX, (uint16_t)(cx / 2));
    Write16(wram, kActRaiserWram_Bg1CameraY, 320);
    Write16(wram, kActRaiserWram_Bg2CameraY, 192);
    ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 0);
    ActionEnvironmentalEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram));
    CHECK(frame.decoration_count == 3);
    CHECK(!frame.decoration_overflow && frame.effect_count == 0);
    for (unsigned i = 0; i < frame.decoration_count; i++) {
      const ActionEffectInstance *light = &frame.decorations[i];
      CHECK(light->world_y == 96); /* mean camera 256 minus offscreen 160 */
      CHECK(light->world_x == cx * 3 / 4 + 128);
      CHECK(light->generation == first.generation);
      CHECK(light->phase_ticks == first.phase_ticks + 3);
      CHECK(light->flags & kActionEffectFlag_ClipToRect);
      CHECK(light->clip_rect.x0 + light->world_x - cx * 3 / 4 + cx / 2 == 0);
      CHECK(light->clip_rect.x1 - light->clip_rect.x0 == 2304);
      CHECK(light->clip_rect.y0 + light->world_y - 256 + 192 == 0);
      CHECK(light->clip_rect.y1 - light->clip_rect.y0 == 512);
    }
  }
  wram[0xC000 + 8 * 16 + 9] = 0;
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 0);
  ActionEnvironmentalEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram));
  CHECK(frame.decoration_count == 0); /* Incomplete/replaced source fails closed. */
  wram[kActRaiserWram_CurrentMap] = 2;
  CHECK(!ActionSceneEffects_RoomUsesBg2Decorations(wram, sizeof(wram)));
  CHECK(!ActionSceneEffects_RoomUsesBg2Decorations(wram, 20));
  CHECK(!ActionSceneEffects_RoomUsesBg2Decorations(NULL, 0));
}

static unsigned CaveTestTileAddress(unsigned base, unsigned width, unsigned x, unsigned y) {
  return base + ((y >> 8) * (width >> 8) + (x >> 8)) * 256 +
      (y & 240) + ((x & 240) >> 4);
}

static void TestCastleEnvironmentCapture(void) {
  static uint8_t ram[kActRaiserWramSize], before[kActRaiserWramSize];
  ActionEffectObserver observer = {0};
  ActionSceneEffectFrame frame;
  memset(ram,0,sizeof(ram));
  ram[0x18] = 2;
  ram[0x19] = 8;
  Write16(ram,0x2E,256);
  Write16(ram,0x30,256);
  Write16(ram,0x32,256);
  Write16(ram,0x34,256);
  Write16(ram,0x46,0x8000);
  Write16(ram,0x4A,0xC000);
  Write16(ram,0x52,0x2100);
  Write16(ram,0x88,1234);
  const uint16_t words[] = {0x04EE,0x44EE,0x04FE,0x44FE};
  for (unsigned i = 0; i < 4; i++) Write16(ram,0x2100+0x44*8+i*2,words[i]);
  ram[0x80F0] = 0x77;
  ram[0x800F] = 0x09;
  ram[0x8027] = 0x51;
  ram[0x8037] = 0x61;
  ram[0x80A8] = 0x89; /* Visible bottom frame beneath the boss window. */
  ram[0xC037] = 0x10;
  ram[0xC048] = 0x05;
  for (unsigned x = 2; x < 14; x++) ram[0x80E0+x] = 0x5F;
  ram[0x05A0+0x5F] = 15;
  memcpy(before,ram,sizeof(ram));
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),0);
  CHECK(frame.decoration_count == 0);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(frame.decoration_count == 3 && !frame.decoration_overflow);
  CHECK(frame.decorations[0].kind == kActionEffect_CastleLight);
  CHECK(frame.decorations[0].source_mask == 1);
  CHECK(frame.decorations[1].kind == kActionEffect_CastleMist);
  CHECK(frame.decorations[1].world_y == 224);
  CHECK(frame.decorations[2].kind == kActionEffect_CastleSky);
  CHECK(!memcmp(before,ram,sizeof(ram)));
  const unsigned phase = frame.decorations[0].phase_ticks;
  Write16(ram,0x88,1400);
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),0);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(frame.decorations[0].phase_ticks == phase);
  ram[0x80E8] = 0; /* One missing support tile removes the whole floor span. */
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),1);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(frame.decoration_count == 2);
  CHECK(frame.decorations[1].kind == kActionEffect_CastleSky);
  ram[0x80A8] = 0; /* A missing sill must not leave detached rays. */
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),1);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(frame.decoration_count == 1 && frame.decorations[0].kind == kActionEffect_CastleSky);
  ram[0x80A8] = 0x89;
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),1);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(frame.decoration_count == 2 && frame.decorations[0].source_mask == 1);
  ram[0x8027] = 0; /* Changed source removes only its window light. */
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),1);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(frame.decoration_count == 1 && frame.decorations[0].kind == kActionEffect_CastleSky);
  Write16(ram,0x2100+0x44*8,0);
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),1);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(frame.decoration_count == 0);
  CHECK(!ActionSceneEffects_RoomUsesBg2Decorations(ram,30));
}

static void TestCastleGalleryAndWaterCapture(void) {
  static uint8_t ram[kActRaiserWramSize], before[kActRaiserWramSize];
  ActionEffectObserver observer = {0};
  ActionSceneEffectFrame frame;
  memset(ram,0,sizeof(ram));
  ram[0x18] = 2;
  ram[0x19] = 7;
  Write16(ram,0x2E,1024); Write16(ram,0x30,1024);
  Write16(ram,0x32,256); Write16(ram,0x34,256);
  Write16(ram,0x46,0x8000);
  Write16(ram,0x4A,0xC000);
  Write16(ram,0x52,0x2100); Write16(ram,0x56,0x2900);
  const uint16_t window[] = {0x04EE,0x44EE,0x04FE,0x44FE};
  for (unsigned i = 0; i < 4; i++) Write16(ram,0x2100+0x44*8+i*2,window[i]);
  ram[CaveTestTileAddress(0x8000,1024,0,1008)] = 0x09;
  ram[CaveTestTileAddress(0x8000,1024,1008,0)] = 0x09;
  /* Native alternating arch blocks. Each of the eleven openings needs light. */
  const unsigned windows[][5] = {
    {368,384,0x51,0x59,0x8C},{400,424,0x70,0x78,0x89},
    {448,464,0x73,0x7B,0x8C},{480,504,0x70,0x78,0x89},
    {528,544,0x73,0x7B,0x8C},{560,584,0x70,0x78,0x89},
    {608,624,0x73,0x7B,0x8C},{640,664,0x70,0x78,0x89},
    {688,704,0x73,0x7B,0x8C},{720,744,0x70,0x78,0x89},
    {768,784,0x73,0x7B,0x6A},
  };
  for (unsigned i = 0; i < 11; i++) {
    ram[CaveTestTileAddress(0x8000,1024,windows[i][0],112)] = (uint8_t)windows[i][2];
    ram[CaveTestTileAddress(0x8000,1024,windows[i][0],128)] = (uint8_t)windows[i][3];
    ram[CaveTestTileAddress(0x8000,1024,windows[i][1],203)] = (uint8_t)windows[i][4];
  }
  ram[0xC037] = 0x10; ram[0xC048] = 0x05;
  memcpy(before,ram,sizeof(ram));
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),1);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(frame.decoration_count == 2);
  CHECK(frame.decorations[0].source_mask == 0x7FF);
  CHECK(frame.decorations[1].kind == kActionEffect_CastleSky);
  CHECK(!memcmp(before,ram,sizeof(ram)));
  ram[CaveTestTileAddress(0x8000,1024,528,112)] = 0;
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),1);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(frame.decorations[0].source_mask == (0x7FF & ~(1u<<6)));

  /* BG2 water is blended into BG1 scenery, with an empty row above it. */
  memset(ram+0x8000,0,0x5000);
  ram[0x19] = 5;
  Write16(ram,0x2E,1792); Write16(ram,0x32,1792); Write16(ram,0x34,1024);
  ram[CaveTestTileAddress(0x8000,1792,0,1008)] = 0x09;
  ram[CaveTestTileAddress(0x8000,1792,1776,0)] = 0xA3;
  const uint16_t surface[] = {0x0470,0x0471,0x0402,0x0403};
  for (unsigned i = 0; i < 4; i++) Write16(ram,0x2900+0x15*8+i*2,surface[i]);
  for (unsigned x = 592; x < 1744; x += 16)
    ram[CaveTestTileAddress(0xC000,1792,x,944)] = 0x15;
  CHECK(!ActionSceneEffects_RoomUsesBg2Decorations(ram,sizeof(ram)));
  memcpy(before,ram,sizeof(ram));
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),1);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(frame.decoration_count == 1);
  CHECK(frame.decorations[0].kind == kActionEffect_CastleWater);
  CHECK(frame.decorations[0].source_mask == 255);
  CHECK(frame.decorations[0].render_layer == kActionEffectRenderLayer_Bg1Plane);
  CHECK(frame.decorations[0].world_y == 944);
  CHECK(!memcmp(before,ram,sizeof(ram)));
  ram[CaveTestTileAddress(0xC000,1792,608,944)] = 0;
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),1);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(frame.decorations[0].source_mask == 254);
  Write16(ram,0x2900+0x15*8,0);
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),1);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(frame.decoration_count == 0);
}

static void TestBloodpoolEnvironmentCapture(void) {
  static uint8_t ram[kActRaiserWramSize], before[kActRaiserWramSize];
  memset(ram,0,sizeof(ram));
  ram[0x18] = 2;
  ram[0x19] = 1;
  Write16(ram,0x2E,4096);
  Write16(ram,0x30,512);
  Write16(ram,0x32,256);
  Write16(ram,0x34,256);
  Write16(ram,0x46,0x8000);
  ram[CaveTestTileAddress(0x8000,4096,0,432)] = 0x8A;
  ram[CaveTestTileAddress(0x8000,4096,736,320)] = 0xB9;
  const int spans[][2] = {{176,880},{960,1184},{1248,1360},{1440,2144},
                         {2240,2368},{2464,2560},{2688,2864},{2912,4096}};
  for (unsigned i = 0; i < 8; i++)
    for (int x = spans[i][0]; x < spans[i][1]; x += 16)
      ram[CaveTestTileAddress(0x8000,4096,x,480)] = 0x20;
  ActionEffectObserver observer = {0};
  ActionSceneEffectFrame frame;
  CHECK(ActionSceneEffects_RoomUsesBg1Decorations(ram,sizeof(ram)));
  CHECK(ActionSceneEffects_RoomUsesBg2Decorations(ram,sizeof(ram)));
  for (unsigned x = 0; x <= 3840; x += 128) {
    Write16(ram,0x22,(uint16_t)x);
    memcpy(before,ram,sizeof(ram));
    ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),0);
    CHECK(frame.decoration_count == 0); /* Toggle Off: capture has no ambient fields. */
    ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
    CHECK(frame.decoration_count == 2 && frame.decoration_visible_count == 2);
    CHECK(frame.effect_count == 0 && !frame.decoration_overflow);
    CHECK(!memcmp(ram,before,sizeof(ram)));
    for (unsigned i = 0; i < 2; i++) {
      CHECK(frame.decorations[i].world_x == x+128 && frame.decorations[i].world_y == 480);
      CHECK(frame.decorations[i].source_mask == 0xFF && frame.decorations[i].phase_ticks == 0);
    }
    CHECK(frame.decorations[0].projection_plane == kActionEffectProjectionPlane_Bg1High);
    CHECK(frame.decorations[1].projection_plane == kActionEffectProjectionPlane_Bg1);
    CHECK(frame.decorations[1].render_layer == kActionEffectRenderLayer_Bg2HighAlpha);
  }
  /* Visible moon signature plus native material words enable the light field. */
  Write16(ram,0x22,0);
  Write16(ram,0x52,0x2100);
  Write16(ram,0x54,0xECFF);
  Write16(ram,0x4A,0xC000);
  ram[0x6B] = 0x10;
  ram[CaveTestTileAddress(0xC000,256,112,48)] = 0x3C;
  ram[CaveTestTileAddress(0xC000,256,112,64)] = 0x44;
  for (unsigned i = 0; i < 1024; i++) Write16(ram,0x2100+i*2,0xFF);
  const unsigned blocker = CaveTestTileAddress(0x8000,4096,112,128);
  ram[blocker] = 1;
  const uint16_t words[] = {0x1074,0x3074,0x10FF,0x1088,0x5088,0x9088};
  /* Independent CHR cutout fixture: transparency and both native flips must
   * survive run merging, including the single-pixel ends of the silhouette. */
  static const char *cutout[] = {"00001100","00011110","00011111","00111111",
                                "01111111","11111111","11111111","11111111"};
  for (unsigned i = 0; i < sizeof(words)/sizeof(words[0]); i++) {
    Write16(ram,0x2108,words[i]);
    memcpy(before,ram,sizeof(ram));
    ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),0);
    ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
    CHECK(frame.decoration_count == 7 && frame.moonlight.valid);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
      bool opaque = false;
      for (unsigned j = 0; j < frame.moonlight.count; j++) {
        const ActionMoonlightOccluder *r = &frame.moonlight.rectangles[j];
        opaque |= x+112 >= r->x0 && x+112 < r->x1 && y+128 >= r->y0 && y+128 < r->y1;
      }
      const bool expected = i == 0 || (i >= 3 && cutout[i == 5 ? 7-y : y][i == 4 ? 7-x : x] == '1');
      CHECK(opaque == expected);
    }
    CHECK(frame.decorations[2].world_x == 112 && frame.decorations[2].world_y == 62);
    for(unsigned family=0;family<7;++family)
      CHECK(!!(frame.decorations[family].flags&kActionEffectFlag_StaticAnchor)==(family==2||family==6));
    CHECK(!memcmp(before,ram,sizeof(ram)));
  }
  /* Capture the retained native HDMA table, not a reconstruction from clock
   * or camera. The inherited upper bytes can contain arbitrary CHR data. */
  ram[0x6000] = 127;
  for (unsigned row = 0; row < kActionBloodpoolWaterScrollRows; row++) {
    ram[0x6003+row*3] = 1;
    Write16(ram,0x6004+row*3,(uint16_t)(0x7C00+row*17));
  }
  memcpy(before,ram,sizeof(ram));
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),0);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(frame.bloodpool.water_scroll_valid && !memcmp(before,ram,sizeof(ram)));
  for (unsigned row = 0; row < kActionBloodpoolWaterScrollRows; row++)
    CHECK(frame.bloodpool.water_scroll[row] == ((row*17)&1023));
  const ActionBloodpoolDetails held_water = frame.bloodpool;
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),0);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(!memcmp(held_water.water_scroll,frame.bloodpool.water_scroll,
                sizeof(held_water.water_scroll)));
  ram[0x6030] = 2; /* Unknown row counts must omit only the wave caps. */
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),0);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(!frame.bloodpool.water_scroll_valid && frame.decoration_count == 7);
  ram[0x6030] = 1;
  ActionMoonlightOcclusion previous = frame.moonlight;
  Write16(ram,0x22,16); /* Capture remains world anchored as the camera scrolls. */
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),0);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(frame.moonlight.count == previous.count && frame.decorations[2].world_x == 112);
  CHECK(!memcmp(frame.moonlight.rectangles,previous.rectangles,
      previous.count*sizeof(previous.rectangles[0])));
  /* A verified timber underside emits a drop; actual lower solids stop it.
   * Post contacts use native timber ink, independent of priority. */
  const unsigned timber = CaveTestTileAddress(0x8000,4096,208,352);
  ram[timber] = 0x77;
  const uint16_t timber_words[] = {0x0A77,0x0A77,0x00FF,0x00FF};
  for (unsigned q = 0; q < 4; q++) Write16(ram,0x2100+0x77*8+q*2,timber_words[q]);
  const unsigned lower = CaveTestTileAddress(0x8000,4096,208,400);
  ram[lower] = 2;
  ram[CaveTestTileAddress(0x8000,4096,272,464)] = 2;
  ram[CaveTestTileAddress(0x8000,4096,272,480)] = 0x21;
  for (unsigned q = 0; q < 4; q++) Write16(ram,0x2110+q*2,0x1074);
  memcpy(before,ram,sizeof(ram));
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),0);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(!memcmp(before,ram,sizeof(ram)));
  CHECK(frame.bloodpool.valid && frame.bloodpool.timber_count == 1);
  CHECK(frame.bloodpool.post_count == 1);
  CHECK(frame.bloodpool.posts[0].x0 == 272 && frame.bloodpool.posts[0].x1 == 288 &&
        frame.bloodpool.posts[0].y == 480);
  CHECK(frame.bloodpool.timber[0].y == 352 && frame.bloodpool.timber[0].drip_y == 360);
  CHECK(frame.bloodpool.timber[0].landing_y == 400 && !frame.bloodpool.timber[0].water_landing);
  ram[lower] = 0;
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),0);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(frame.bloodpool.timber[0].landing_y == 488 && frame.bloodpool.timber[0].water_landing);
  Write16(ram,0x2100+0x77*8,0x0A78); /* Changed art must not inherit the material rule. */
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),0);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(frame.bloodpool.timber_count == 0);
  /* Native $21 joins a high-priority broad post and an ordinary thin one.
   * Their centers are only 8px apart, but their submerged ends differ by 7px.
   * Red lake ink below the timber must not extend either contact to row 512. */
  Write16(ram,0x2110+4,0x2862);
  Write16(ram,0x2110+6,0x0863);
  const uint16_t post_words[] = {0x2856,0x0857,0x2866,0x2867};
  for (unsigned q = 0; q < 4; q++) Write16(ram,0x2100+0x21*8+q*2,post_words[q]);
  for (unsigned x = 0; x <= 384; x += 16) {
    Write16(ram,0x22,(uint16_t)x);
    memcpy(before,ram,sizeof(ram));
    ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),0);
    ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
    CHECK(!memcmp(before,ram,sizeof(ram)));
    CHECK(frame.bloodpool.valid && frame.bloodpool.post_count == 2);
    CHECK(frame.bloodpool.posts[0].x0 == 273 && frame.bloodpool.posts[0].x1 == 280 &&
          frame.bloodpool.posts[0].y == 492);
    CHECK(frame.bloodpool.posts[1].x0 == 282 && frame.bloodpool.posts[1].x1 == 287 &&
          frame.bloodpool.posts[1].y == 485);
  }
  ram[CaveTestTileAddress(0xC000,256,112,48)] = 0;
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),0);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(frame.decoration_count == 2 && !frame.moonlight.valid); /* No inherited moon/shadows. */
  ram[CaveTestTileAddress(0x8000,4096,256,480)] = 0; /* Invalid interior omits its pool. */
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),1);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(frame.decoration_count == 2 && frame.decorations[0].source_mask == 0xFE);
  ram[CaveTestTileAddress(0x8000,4096,736,320)] = 0;
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),0);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(frame.decoration_count == 0); /* Inherited/incomplete room fails closed. */
  ram[0x19] = 2;
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),0);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(frame.decoration_count == 0 && !ActionSceneEffects_RoomUsesBg2Decorations(ram,sizeof(ram)));
  CHECK(!ActionSceneEffects_RoomUsesBg2Decorations(ram,20));
}

static void TestCaveEnvironmentalCapture(void) {
  static uint8_t wram[kActRaiserWramSize], before[kActRaiserWramSize];
  static const unsigned dimensions[][4] = {
    {2048,1280,2048,1280}, {1024,1792,256,512}, {512,256,256,256},
  };
  static const unsigned signatures[][6] = {
    {326,352,0xB8,420,432,0xB9}, {896,128,0x39,448,1664,0x26},
    {80,80,0x08,96,192,0x26},
  };
  ActionEffectObserver observer = {0};
  ActionSceneEffectFrame frame;
  for (unsigned room = 2; room <= 4; room++) {
    memset(wram, 0, sizeof(wram));
    wram[0x18] = 1;
    wram[0x19] = (uint8_t)room;
    const unsigned *d = dimensions[room-2], *sig = signatures[room-2];
    for (unsigned i = 0; i < 4; i++) Write16(wram, 0x2E + i*2, (uint16_t)d[i]);
    Write16(wram, 0x46, 0x8000);
    Write16(wram, 0x4A, 0xC000);
    const unsigned signature_at = CaveTestTileAddress(0x8000, d[0], sig[0], sig[1]);
    wram[signature_at] = (uint8_t)sig[2];
    wram[CaveTestTileAddress(0x8000, d[0], sig[3], sig[4])] = (uint8_t)sig[5];
    if (room == 2) {
      wram[CaveTestTileAddress(0xC000, 2048, 0, 896)] = 1;
      wram[CaveTestTileAddress(0xC000, 2048, 720, 0)] = 2;
    }
    CHECK(ActionSceneEffects_RoomUsesBg1Decorations(wram, sizeof(wram)) == (room <= 3));
    if (room == 3) {
      Write16(wram, 0x24, 1512);
      CHECK(ActionSceneEffects_RoomUsesBg1Decorations(wram, sizeof(wram)));
      Write16(wram, 0x24, 1415);
      CHECK(ActionSceneEffects_RoomUsesBg1Decorations(wram, sizeof(wram)));
      Write16(wram, 0x24, 1414);
      CHECK(ActionSceneEffects_RoomUsesBg1Decorations(wram, sizeof(wram)));
      Write16(wram, 0x24, 1664);
      CHECK(ActionSceneEffects_RoomUsesBg1Decorations(wram, sizeof(wram)));
      Write16(wram, 0x24, 1712);
      CHECK(ActionSceneEffects_RoomUsesBg1Decorations(wram, sizeof(wram)));
    }
    CHECK(ActionSceneEffects_RoomUsesBg2Decorations(wram, sizeof(wram)) == (room == 2));
    observer.landing_dust = (ActionLandingDustState){.valid = 1};
    observer.landing_dust.puffs[0] = (ActionLandingDustPuff){.active = 1, .strength = 1};
    /* The room handoff below must discard a retained previous-room cloud. */
    for (unsigned camera = 0; camera < d[0]; camera += 32) {
      Write16(wram, 0x22, (uint16_t)camera);
      Write16(wram, 0x24, 320);
      Write16(wram, 0x26, (uint16_t)(camera / 2));
      Write16(wram, 0x28, 192);
      memcpy(before, wram, sizeof(wram));
      ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
      ActionEnvironmentalEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram));
      CHECK(!memcmp(before, wram, sizeof(wram)));
      CHECK(frame.decoration_count == (room == 2 ? 7 : room == 3 ? 3 : 2));
      CHECK(!frame.decoration_overflow && !frame.effect_count);
      for (unsigned i = 0; i < frame.decoration_count; i++) {
        const ActionEffectInstance *e = &frame.decorations[i];
        const bool water = e->kind == kActionEffect_CaveWater || e->kind == kActionEffect_CaveMist;
        CHECK(e->world_x == (water ? camera / 2 : camera) + 128);
        CHECK(e->world_y == (water ? 32 : 160));
        CHECK(e->clip_rect.x0 + e->world_x == 0);
        CHECK(e->clip_rect.y0 + e->world_y == 0);
        CHECK(e->clip_rect.x1 + e->world_x == d[0]);
        CHECK(e->clip_rect.y1 + e->world_y == d[1]);
        CHECK(e->visual == room && e->phase == kActionEffectPhase_CaveEnvironment);
        if (e->kind == kActionEffect_CaveAmbientLight)
          CHECK(e->render_layer == kActionEffectRenderLayer_ForegroundLight);
        CHECK(e->projection_plane == (water ? kActionEffectProjectionPlane_Bg2High :
                                              kActionEffectProjectionPlane_Bg1));
      }
    }
    const ActionEffectInstance first = frame.decorations[0];
    ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 0);
    ActionEnvironmentalEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram));
    CHECK(!memcmp(&first, &frame.decorations[0], sizeof(first)));
    wram[signature_at] = 0;
    ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 0);
    ActionEnvironmentalEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram));
    CHECK(frame.decoration_count == 0);
    wram[signature_at] = (uint8_t)sig[2];
    Write16(wram, 0x2E, 4096); /* Old dimensions during a room transition. */
    ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 0);
    ActionEnvironmentalEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram));
    CHECK(frame.decoration_count == 0);
  }
}

static void TestTempleMistCollisionCapture(void) {
  static uint8_t ram[kActRaiserWramSize], before[kActRaiserWramSize];
  ActionEffectObserver observer = {0};
  ActionSceneEffectFrame frame;
  memset(ram, 0, sizeof(ram));
  ram[0x18] = 1;
  ram[0x19] = 3;
  Write16(ram, 0x2E, 1024);
  Write16(ram, 0x30, 1792);
  Write16(ram, 0x32, 256);
  Write16(ram, 0x34, 512);
  Write16(ram, 0x46, 0x8000);
  ram[CaveTestTileAddress(0x8000,1024,896,128)] = 0x39;
  ram[CaveTestTileAddress(0x8000,1024,448,1664)] = 0x26;
  ram[0x05A0+0x25] = ram[0x05A0+0x26] = 15;
  ram[0x05A0+0x56] = 3;
  /* Measured native lower-hall surfaces, not the decorative ledge at 1664. */
  const int surfaces[][3] = {
    {528,592,1664}, {592,656,1680}, {688,736,1680}, {768,832,1680}, {864,928,1664},
  };
  for (unsigned i = 0; i < 5; i++)
    for (int x = surfaces[i][0]; x < surfaces[i][1]; x += 16) {
      ram[CaveTestTileAddress(0x8000,1024,x,surfaces[i][2])] = i == 4 ? 0x56 : 0x25;
      ram[CaveTestTileAddress(0x8000,1024,x,surfaces[i][2]-16)] = i == 4 ? 0x39 : 0x37;
    }
  for (int x = 656; x < 960; x += 16) {
    const bool pit = x < 688 || (x >= 736 && x < 768) ||
        (x >= 832 && x < 864) || x >= 928;
    if (!pit) continue;
    ram[CaveTestTileAddress(0x8000,1024,x,1712)] = 0x25;
    ram[CaveTestTileAddress(0x8000,1024,x,1696)] = 0x20;
  }
  /* An obstructed top at the left must not become a mist surface. */
  ram[CaveTestTileAddress(0x8000,1024,512,1664)] = 0x25;
  ram[CaveTestTileAddress(0x8000,1024,512,1648)] = 0x25;
  for (unsigned view = 0; view < 2; view++) {
    Write16(ram, 0x22, (uint16_t)(400 + view*200));
    Write16(ram, 0x24, (uint16_t)(1512 - view*800));
    memcpy(before,ram,sizeof(ram));
    ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),1);
    ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
    CHECK(!memcmp(before,ram,sizeof(ram)));
    CHECK(frame.decoration_count == 12 && !frame.decoration_overflow);
    const int settled[][3] = {
      {528,592,1664}, {592,656,1680}, {656,688,1712}, {688,736,1680},
      {736,768,1712}, {768,832,1680}, {832,864,1712}, {864,928,1664}, {928,960,1712},
    };
    for (unsigned i = 0; i < 9; i++) {
      const ActionEffectInstance *e = &frame.decorations[3+i];
      CHECK(e->kind == kActionEffect_TempleGroundMist);
      CHECK(e->world_x == settled[i][0] && e->world_y == settled[i][2]);
      CHECK(e->geometry.data.rect.x0 == 0);
      CHECK(e->geometry.data.rect.x1 == settled[i][1]-settled[i][0]);
      CHECK(e->geometry.data.rect.y0 == -26 && e->geometry.data.rect.y1 == 0);
      CHECK(e->render_layer == kActionEffectRenderLayer_Bg1Mist);
      CHECK(e->projection_plane == kActionEffectProjectionPlane_Bg1);
    }
  }
  /* Change the collision shape within one old pocket: capture must split it. */
  ram[CaveTestTileAddress(0x8000,1024,592,1680)] = 0;
  ram[CaveTestTileAddress(0x8000,1024,592,1696)] = 0x25;
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),0);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(frame.decoration_count == 13);
  CHECK(frame.decorations[4].world_x == 592 && frame.decorations[4].world_y == 1696);
  CHECK(frame.decorations[4].geometry.data.rect.x1 == 16);
  CHECK(frame.decorations[5].world_x == 608 && frame.decorations[5].world_y == 1680);
  /* A fragmented/changed map exceeds the cosmetic budget: keep other effects. */
  memset(ram+0x8000,0,1024/256*1792/256*256);
  ram[CaveTestTileAddress(0x8000,1024,896,128)] = 0x39;
  ram[CaveTestTileAddress(0x8000,1024,448,1664)] = 0x26;
  for (int x = 512; x < 960; x += 16)
    ram[CaveTestTileAddress(0x8000,1024,x,1664+(x&16))] = 0x25;
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),0);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(frame.decoration_count == 3 && frame.decoration_visible_count == 3);
  CHECK(!frame.decoration_overflow);
}

static void TestLowerCaveMistCollisionCapture(void) {
  static uint8_t ram[kActRaiserWramSize], before[kActRaiserWramSize];
  ActionEffectObserver observer = {0};
  ActionSceneEffectFrame frame;
  ram[0x18] = 1;
  ram[0x19] = 2;
  Write16(ram,0x2E,2048);
  Write16(ram,0x30,1280);
  Write16(ram,0x32,2048);
  Write16(ram,0x34,1280);
  Write16(ram,0x46,0x8000);
  Write16(ram,0x4A,0xC000);
  ram[CaveTestTileAddress(0x8000,2048,326,352)] = 0xB8;
  ram[CaveTestTileAddress(0x8000,2048,420,432)] = 0xB9;
  ram[CaveTestTileAddress(0xC000,2048,0,896)] = 1;
  ram[CaveTestTileAddress(0xC000,2048,720,0)] = 2;
  ram[0x05A0+0x25] = 15;
  /* Measured first-room lower temple. The side steps and intervening spike
   * pits must not be flattened into one floating band. */
  const int surfaces[][3] = {
    {912,1072,1216}, {1152,1312,1216}, {1344,1392,1184}, {1424,1456,1152},
    {1504,1536,1152}, {1600,1728,1152},
  };
  for (unsigned i = 0; i < 6; i++)
    for (int x = surfaces[i][0]; x < surfaces[i][1]; x += 16)
      ram[CaveTestTileAddress(0x8000,2048,x,surfaces[i][2])] = 0x25;
  ram[CaveTestTileAddress(0x8000,2048,1456,1216)] = 0x25;
  ram[CaveTestTileAddress(0x8000,2048,1456,1200)] = 0x20;
  memcpy(before,ram,sizeof(ram));
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),1);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram));
  CHECK(!memcmp(before,ram,sizeof(ram)));
  CHECK(frame.decoration_count == 14 && !frame.decoration_overflow);
  const int settled[][3] = {
    {912,1072,1216}, {1152,1312,1216}, {1344,1392,1184}, {1424,1456,1152},
    {1456,1472,1216}, {1504,1536,1152}, {1600,1728,1152},
  };
  for (unsigned i = 0; i < 7; i++) {
    const ActionEffectInstance *e = &frame.decorations[7+i];
    CHECK(e->kind == kActionEffect_TempleGroundMist && e->environment_room == 2);
    CHECK(e->world_x == settled[i][0] && e->world_y == settled[i][2]);
    CHECK(e->geometry.data.rect.x1 == settled[i][1]-settled[i][0]);
  }
}

static void TestLandingDustContacts(void) {
  static uint8_t ram[kActRaiserWramSize], before[kActRaiserWramSize];
  ActionLandingDustState state = {0};
  ActionSceneEffectFrame frame = {0};
  ActionBgMapView map;
  memset(ram, 0, sizeof(ram));
  CHECK(ActionBgMapView_Init(&map, ram, sizeof(ram), 512, 256, 0x8000));
  for (int x = 0; x < 512; x += 16)
    ram[CaveTestTileAddress(0x8000, 512, x, 192)] = 0x25;
  ram[0x5C5] = 15; /* Loaded native collision LUT: a solid stone floor. */
  const unsigned actors[] = {0x8A0,0xAE0,0xB20,0xB60,0xBA0,0xBE0,0xC20,0xC60};
  for (unsigned i = 0; i < 8; i++) {
    const unsigned at = actors[i];
    Write16(ram, at + 2, (uint16_t)(128 + i*32));
    Write16(ram, at + 4, 128);
    Write16(ram, at + 0x10, 24);
    Write16(ram, at + 0x12, i ? 0x8661 : 0x9996);
    Write16(ram, at + 0x16, i ? 0x5000 : 0x8000);
    ram[at + 0x18] = i ? 0x7E : 6;
    Write16(ram, at + 0x2C, i ? 24 : 0);
    Write16(ram, at + 0x32, i ? 0xAF5D : 0x9810);
  }
  ActionLandingDust_Capture(&state, &frame, ram, sizeof(ram), &map, 4, 65530);
  CHECK(!frame.decoration_count); /* First observation cannot invent an impact. */
  for (unsigned t = 1; t <= 10; t++) {
    frame = (ActionSceneEffectFrame){0};
    for (unsigned i = 0; i < 8; i++) Write16(ram, actors[i] + 4, (uint16_t)(128 + 4*t));
    memcpy(before, ram, sizeof(ram));
    ActionLandingDust_Capture(&state, &frame, ram, sizeof(ram), &map, 4,
        (uint16_t)(65530+t));
    CHECK(!memcmp(before, ram, sizeof(ram)));
    CHECK(frame.decoration_count == (t == 10 ? kActionLandingDustMaxPuffs : 0));
  }
  CHECK(!frame.decoration_overflow && !frame.effect_count);
  CHECK(frame.decorations[0].record_address == 0x8A0 && frame.decorations[0].visual == 1);
  CHECK(frame.decorations[1].record_address == 0xAE0 && frame.decorations[1].visual == 3);
  const ActionSceneEffectFrame retained = frame;
  frame = (ActionSceneEffectFrame){0};
  ActionLandingDust_Capture(&state, &frame, ram, sizeof(ram), &map, 4, 4);
  CHECK(!memcmp(&frame, &retained, sizeof(frame))); /* Pause and clock wrap. */
  for (unsigned t = 5; t <= 53; t++) {
    Write16(ram, 0x8A2, (uint16_t)(128+t)); /* Walking leaves each puff at its impact. */
    frame = (ActionSceneEffectFrame){0};
    ActionLandingDust_Capture(&state, &frame, ram, sizeof(ram), &map, 4, (uint16_t)t);
    if (t < 52) CHECK(frame.decorations[0].world_x == 128);
  }
  CHECK(!frame.decoration_count);
  for (unsigned rejected = 0; rejected < 6; rejected++) {
    memset(&state, 0, sizeof(state));
    for (unsigned i = 1; i < 8; i++) Write16(ram, actors[i], 0x8000);
    Write16(ram, 0x8A2, 128);
    Write16(ram, 0x8A4, 148);
    Write16(ram, 0x8D2, 0x9810);
    ram[0x5C5] = rejected == 0 ? 3 : 15; /* Unknown one-way artwork is not an identified capital. */
    frame = (ActionSceneEffectFrame){0};
    ActionLandingDust_Capture(&state, &frame, ram, sizeof(ram), &map, 4, 60);
    Write16(ram, 0x8A4, 156);
    ActionLandingDust_Capture(&state, &frame, ram, sizeof(ram), &map, 4, 61);
    if (rejected == 1) Write16(ram, 0x8D2, 0x979A); /* Slot reuse. */
    if (rejected == 2) Write16(ram, 0x8A2, 400); /* Teleport. */
    if (rejected == 5) Write16(ram, 0x8B0, 32); /* Changed pose extent. */
    if (rejected == 3) Write16(ram, 0x8B2, 0x9C64); /* Hit reaction. */
    Write16(ram, 0x8A4, rejected == 5 ? 160 : 168);
    ActionLandingDust_Capture(&state, &frame, ram, sizeof(ram), &map, 4,
        rejected == 4 ? 70 : 62); /* Missing gameplay interval. */
    CHECK(!frame.decoration_count);
    Write16(ram, 0x8B2, 0x9996);
    Write16(ram, 0x8B0, 24);
  }
  const unsigned sources[] = {0xB041,0xB0B4,0xB28D,0xB2FD};
  for (unsigned family = 0; family < 4; family++) {
    memset(&state, 0, sizeof(state));
    Write16(ram, 0x8A0, 0x8000);
    Write16(ram, 0xAE0, 0);
    Write16(ram, 0xAF6, 0x4000);
    Write16(ram, 0xB12, (uint16_t)sources[family]);
    for (unsigned t = 0; t < 11; t++) {
      Write16(ram, 0xAE4, (uint16_t)(128+t*4));
      frame = (ActionSceneEffectFrame){0};
      ActionLandingDust_Capture(&state, &frame, ram, sizeof(ram), &map, 3, (uint16_t)t);
      CHECK(frame.decoration_count == (t == 10 ? 1 : 0));
    }
    CHECK(frame.decorations[0].visual == 1 && frame.decorations[0].world_y == 192);
  }
  /* A child with a live boss source is still a projectile, never a jumper. */
  memset(&state, 0, sizeof(state));
  Write16(ram, 0x8A0, 0x8000);
  Write16(ram, 0xAE0, 0);
  Write16(ram, 0xAF6, 0x5000);
  Write16(ram, 0xB12, 0xAF5D);
  Write16(ram, 0xB1A, 0x1320);
  for (unsigned t = 0; t < 11; t++) {
    Write16(ram, 0xAE4, (uint16_t)(128+t*4));
    frame = (ActionSceneEffectFrame){0};
    ActionLandingDust_Capture(&state, &frame, ram, sizeof(ram), &map, 4, (uint16_t)t);
    CHECK(!frame.decoration_count);
  }
}

static void TestTempleCapitalContacts(void) {
  static uint8_t ram[kActRaiserWramSize], before[kActRaiserWramSize];
  const struct { uint8_t tile, above, collision; int x; bool emits; } cases[] = {
    /* Actual 01/03 column-cap metatiles, including the non-solid column behind. */
    {0x54,0x2F,3,136,true}, {0x55,0x38,3,136,true},
    {0x56,0x39,3,136,true}, {0x57,0x2F,3,136,true},
    {0x54,0x2F,3,121,true}, {0x57,0x2F,3,151,true}, /* Native edge probes. */
    {0x54,0x18,3,136,false}, {0x55,0x20,3,136,false}, /* Spikes on a capital. */
    {0x54,0x25,3,136,false}, /* Stone above: not an exposed top. */
    {0x53,0x2F,3,136,false}, {0x54,0x2F,0,136,false}, /* Unproved surface. */
    {0x25,0x20,15,136,false}, /* Spikes on ordinary stone also fail closed. */
  };
  for (unsigned c = 0; c < sizeof(cases)/sizeof(cases[0]); c++) {
    memset(ram, 0, sizeof(ram));
    ActionLandingDustState state = {0};
    ActionSceneEffectFrame frame = {0};
    ActionBgMapView map;
    CHECK(ActionBgMapView_Init(&map, ram, sizeof(ram), 512, 256, 0x8000));
    ram[CaveTestTileAddress(0x8000, 512, 128, 192)] = cases[c].tile;
    ram[CaveTestTileAddress(0x8000, 512, 128, 176)] = cases[c].above;
    ram[0x5C5] = 15;
    ram[0x5A0 + cases[c].tile] = cases[c].collision;
    Write16(ram, 0x8A2, (uint16_t)cases[c].x);
    Write16(ram, 0x8B0, 24);
    Write16(ram, 0x8B2, 0x9996);
    Write16(ram, 0x8B6, 0x8000);
    ram[0x8B8] = 6;
    for (unsigned t = 0; t < 3; t++) {
      Write16(ram, 0x8A4, (uint16_t)(152 + t*8));
      frame = (ActionSceneEffectFrame){0};
      memcpy(before, ram, sizeof(ram));
      ActionLandingDust_Capture(&state, &frame, ram, sizeof(ram), &map, 3, (uint16_t)t);
      CHECK(!memcmp(before, ram, sizeof(ram)));
      CHECK(frame.decoration_count == (t == 2 && cases[c].emits ? 1 : 0));
    }
    if (cases[c].emits) {
      CHECK(frame.decorations[0].world_x == cases[c].x);
      CHECK(frame.decorations[0].world_y == 192);
    }
  }
}

static uint16_t CapturedWetSourceMask(uint8_t *ram) {
  ActionEffectObserver observer = {0};
  ActionSceneEffectFrame frame;
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,kActRaiserWramSize,1);
  ActionEnvironmentalEffects_CaptureFrame(&observer,&frame,ram,kActRaiserWramSize);
  for (unsigned i = 0; i < frame.decoration_count; i++)
    if (frame.decorations[i].kind == kActionEffect_CaveDrips)
      return frame.decorations[i].source_mask;
  return 0;
}

static void TestCaveWetMaterials(void) {
  static uint8_t ram[kActRaiserWramSize];
  memset(ram, 0, sizeof(ram));
  ram[0x18] = 1;
  ram[0x19] = 2;
  Write16(ram,0x2E,2048);
  Write16(ram,0x30,1280);
  Write16(ram,0x32,2048);
  Write16(ram,0x34,1280);
  Write16(ram,0x46,0x8000);
  Write16(ram,0x4A,0xC000);
  ram[CaveTestTileAddress(0x8000,2048,326,352)] = 0xB8;
  ram[CaveTestTileAddress(0x8000,2048,420,432)] = 0xB9;
  ram[CaveTestTileAddress(0xC000,2048,0,896)] = 1;
  ram[CaveTestTileAddress(0xC000,2048,720,0)] = 2;
  for (unsigned i = 0; i < (unsigned)ActionWaterField_Bundled()->Counts[2]; i++) {
    const ActionCaveWetSource source=ActionWaterField_Wet(ActionWaterField_Bundled(),i);
    const ActionCaveWetSource *s=&source;
    ram[CaveTestTileAddress(0x8000,2048,s->x,s->ceiling_y-1)] = s->ceiling_tile;
    ram[CaveTestTileAddress(0x8000,2048,s->x,s->landing_y)] = s->landing_tile;
  }
  CHECK(CapturedWetSourceMask(ram) == 0xFF);
  /* Real foreground and decorative boulders can both be BG1-low. Replacing
   * either physical endpoint with background art must disable that emitter. */
  const unsigned tip = CaveTestTileAddress(0x8000,2048,326,370);
  ram[tip] = 0x69;
  CHECK(CapturedWetSourceMask(ram) == 0xFD);
  ram[tip] = 0xC0;
  ram[CaveTestTileAddress(0x8000,2048,326,451)] = 0x58;
  CHECK(CapturedWetSourceMask(ram) == 0xFD);
  ram[0x19] = 3;
  CHECK(!CapturedWetSourceMask(ram));
}

static void TestDustSettling(void) {
  static uint8_t ram[kActRaiserWramSize];
  memset(ram, 0, sizeof(ram));
  ActionLandingDustState state = {0};
  ActionSceneEffectFrame frame = {0};
  ActionBgMapView map;
  CHECK(ActionBgMapView_Init(&map, ram, sizeof(ram), 512, 256, 0x8000));
  for (unsigned x = 0; x < 512; x += 16)
    ram[CaveTestTileAddress(0x8000,512,x,192)] = 0x25;
  ram[0x5C5] = 15;
  Write16(ram,0x8A2,128);
  Write16(ram,0x8B0,24);
  Write16(ram,0x8B2,0x9996);
  Write16(ram,0x8B6,0x8000);
  ram[0x8B8] = 6;
  for (unsigned t = 0; t <= 203; t++) {
    /* Identical repeated jumps at one spot; another landing 64px away is
     * independent. The third actor landing tests shared patch depletion. */
    const unsigned jump = t % 50;
    Write16(ram,0x8A4,(uint16_t)(jump < 3 ? 152+jump*8 : 168));
    Write16(ram,0x8A2,(uint16_t)(t >= 100 && t < 150 ? 192 : 128));
    if (t == 50) {
      Write16(ram,0x8A0,0x8000);
      memcpy(ram+0xAE0,ram+0x8A0,64);
      Write16(ram,0xAE0,0);
      Write16(ram,0xAF6,0x5000);
      ram[0xAF8] = 0x7E;
      Write16(ram,0xB0C,24);
      Write16(ram,0xB12,0xAF5D);
    }
    if (t >= 50 && t < 100) Write16(ram,0xAE4,(uint16_t)(jump < 3 ? 152+jump*8 : 168));
    if (t == 100) { Write16(ram,0xAE0,0x8000); Write16(ram,0x8A0,0); }
    frame = (ActionSceneEffectFrame){0};
    ActionLandingDust_Capture(&state,&frame,ram,sizeof(ram),&map,4,(uint16_t)(65500+t));
    if (t == 2) CHECK(state.next_generation == 1);
    if (t == 52) CHECK(state.next_generation == 1 && !frame.decoration_count);
    if (t == 102) CHECK(state.next_generation == 2);
    if (t == 202) CHECK(state.next_generation >= 3);
    if (t == 20) {
      const ActionLandingDustState paused = state;
      frame = (ActionSceneEffectFrame){0};
      ActionLandingDust_Capture(&state,&frame,ram,sizeof(ram),&map,4,(uint16_t)(65500+t));
      CHECK(!memcmp(&state,&paused,sizeof(state)));
    }
  }
}

static void TestFillmoreStatueOrbs(void) {
  static uint8_t ram[kActRaiserWramSize], before[kActRaiserWramSize];
  memset(ram,0,sizeof(ram));
  ram[0x18] = 1;
  ram[0x19] = 3;
  const unsigned child = 0x8E0, parent = 0xDE0;
  for (unsigned at = child; at <= parent; at += parent-child) {
    Write16(ram,at+2,944);
    Write16(ram,at+4,1496);
    Write16(ram,at+0x16,0x4000);
    ram[at+0x18] = 0x7E;
    Write16(ram,at+0x32,0xB3BF);
  }
  Write16(ram,parent+0x12,0x8683);
  Write16(ram,parent+0x1A,0x24);
  Write16(ram,parent+0x20,0x4A33);
  Write16(ram,parent+0x22,0x25);
  Write16(ram,child+0x1E,0xB3E9);
  Write16(ram,child+0x3A,parent);
  ActionEffectObserver observer = {0};
  ActionSceneEffectFrame frame;
  for (unsigned mode = 0; mode < 2; mode++) {
    Write16(ram,child+0x12,mode ? 0xB42F : 0xB406);
    Write16(ram,child+0x1A,(uint16_t)(0xA+mode));
    for (unsigned visual = 0x1B; visual <= 0x1E; visual++) {
      Write16(ram,child+0x22,(uint16_t)visual);
      Write16(ram,child+0x20,(uint16_t)(0x48F0+(visual-0x1B)*12));
      memcpy(before,ram,sizeof(ram));
      ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),1);
      CHECK(frame.effect_count == 1 && frame.effects[0].kind == kActionEffect_FillmoreStatueOrb);
      CHECK(!memcmp(before,ram,sizeof(ram)));
    }
  }
  /* Reused parent, unaligned backlink, other room, or a different projectile
   * using the generic animation address must never acquire this light. */
  const unsigned addresses[] = {parent+0x32,child+0x3A,0x18,child+0x20,child+0x1A};
  const uint16_t values[] = {0xB2FD,0xDE1,0x0302,0x48EF,0x24};
  for (unsigned i = 0; i < 5; i++) {
    const uint16_t saved = (uint16_t)(ram[addresses[i]] | ram[addresses[i]+1]<<8);
    Write16(ram,addresses[i],values[i]);
    ActionEffectObserver_Reset(&observer); /* Test new admission, not an existing child. */
    ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),1);
    CHECK(!frame.effect_count);
    Write16(ram,addresses[i],saved);
  }
}

static void SeedBloodpoolAct1BossFireball(uint8_t *ram) {
  memset(ram, 0, kActRaiserWramSize);
  ram[0x18] = 2;
  ram[0x19] = 1;
  const unsigned parent = 0x0CA0, child = 0x0CE0;
  Write16(ram, parent + 0x16, 0x5000);
  ram[parent + 0x18] = 0x7E;
  Write16(ram, parent + 0x20, 0x5327);
  Write16(ram, parent + 0x30, 0x4000);
  Write16(ram, parent + 0x32, 0xB786);
  Write16(ram, child + 2, 3900);
  Write16(ram, child + 4, 344);
  Write16(ram, child + 0x0A, 16);
  Write16(ram, child + 0x0C, 8);
  Write16(ram, child + 0x0E, 16);
  Write16(ram, child + 0x10, 8);
  Write16(ram, child + 0x12, 0xB90D);
  Write16(ram, child + 0x16, 0x5000);
  ram[child + 0x18] = 0x7E;
  Write16(ram, child + 0x1A, 1);
  Write16(ram, child + 0x32, 0xB786);
  Write16(ram, child + 0x3A, parent);
  Write16(ram, child + 6, (uint16_t)-4);
  Write16(ram, child + 0x1E, 0xB82D);
  Write16(ram, child + 0x20, 0x5207);
  Write16(ram, child + 0x22, 6);
}

static void TestBloodpoolAct1BossFireballs(void) {
  static uint8_t ram[kActRaiserWramSize], unchanged[kActRaiserWramSize];
  static ActionSceneEffectFrame frame;
  ActionEffectObserver observer = {0};
  SeedBloodpoolAct1BossFireball(ram);
  const unsigned parent = 0x0CA0, child = 0x0CE0;
  const unsigned resumes[] = {0xB82D, 0xB841, 0xB867, 0xB87B};
  for (unsigned r = 0; r < 4; ++r) {
    Write16(ram, child + 0x1E, resumes[r]);
    for (unsigned facing = 0; facing < 2; ++facing) {
      Write16(ram, child + 6, facing ? 4 : (uint16_t)-4);
      Write16(ram, child + 0x28, facing ? 0x4000 : 0);
      /* Raw part priority is zero; the room supplies the actual OBJ band. */
      Write16(ram, 0x8F, facing ? 0x3000 : 0x2000);
      for (unsigned visual = 6; visual <= 7; ++visual) {
        Write16(ram, child + 0x22, visual);
        Write16(ram, child + 0x20, visual == 6 ? 0x5207 : 0x521A);
        memcpy(unchanged, ram, sizeof(ram));
        ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
        CHECK(!memcmp(ram, unchanged, sizeof(ram)));
        CHECK(frame.effect_count == 1 && frame.visible_count == 1);
        CHECK(frame.effects[0].kind == kActionEffect_EnemyFireball);
        CHECK(frame.effects[0].phase == kActionEffectPhase_EnemyFireballFlight);
        CHECK(frame.effects[0].record_address == child);
        CHECK(frame.effects[0].velocity_x == (facing ? 4 : -4));
        CHECK(frame.effects[0].obj_priority == (facing ? 3 : 2));
        CHECK(frame.effects[0].geometry.data.rect.x0 == -16);
        CHECK(frame.effects[0].geometry.data.rect.y1 == 8);
      }
    }
  }
  const uint32_t generation = frame.effects[0].generation;
  const unsigned age = frame.effects[0].age_ticks;
  ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 0);
  CHECK(frame.effects[0].generation == generation && frame.effects[0].age_ticks == age);
  Write16(ram, child + 2, 3904);
  ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
  CHECK(frame.effects[0].generation == generation && frame.effects[0].age_ticks == age + 1);
  Write16(ram, child + 0x30, 0x0400);
  ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
  CHECK(frame.effect_count == 0 && frame.visible_count == 0);
  Write16(ram, child + 0x30, 0);

  /* Formation, death debris, stale parents and cross-room lookalikes fail closed. */
  const unsigned addresses[] = {child, child + 0x12, child + 0x16, child + 0x18,
      child + 0x1A, child + 0x1E, child + 0x20, child + 0x22, child + 0x28,
      child + 0x32, child + 0x3A, parent + 0x16, parent + 0x18,
      parent + 0x32, parent + 0x3A, 0x18};
  const uint16_t values[] = {0x4000, 0xB8FB, 0x4000, 7, 0, 0xB8DE, 0x522D, 8,
      0x8000, 0xBD76, 0x0CA1, 0x4000, 7, 0xBDFF, child, 0x0802};
  for (unsigned i = 0; i < sizeof(addresses) / sizeof(addresses[0]); ++i) {
    memcpy(unchanged, ram, sizeof(ram));
    Write16(ram, addresses[i], values[i]);
    ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
    CHECK(frame.effect_count == 0);
    memcpy(ram, unchanged, sizeof(ram));
  }
  ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
  CHECK(frame.effects[0].generation != generation && frame.effects[0].age_ticks == 0);

  ActionEffectObserver_Reset(&observer);
  Write16(ram, 0x88, 65530); /* Smoke age is independent of the wrapping scene clock. */
  ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
  CHECK(frame.fireball_smoke.count == 1);
  const int smoke_x = frame.fireball_smoke.puffs[0].x;
  CHECK(smoke_x == 3892 && frame.fireball_smoke.puffs[0].priority == 3);
  for (unsigned tick = 1; tick <= 84; ++tick) {
    Write16(ram, child + 2, 3904 + tick * 4);
    ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
  }
  CHECK(frame.fireball_smoke.count == 22);
  CHECK(frame.fireball_smoke.puffs[0].x == smoke_x);
  CHECK(frame.fireball_smoke.puffs[0].age == 84);
  const ActionFireballSmoke retained = frame.fireball_smoke;
  ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 0);
  CHECK(frame.fireball_smoke.count == retained.count);
  CHECK(frame.fireball_smoke.puffs[0].age == retained.puffs[0].age);

  Write16(ram, child + 2, 1000); /* Reused slot: no smoke bridge across the teleport. */
  ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
  CHECK(frame.effects[0].age_ticks == 0);
  CHECK(frame.fireball_smoke.count == retained.count + 1);
  CHECK(frame.fireball_smoke.puffs[frame.fireball_smoke.count - 1].x == 988);
  CHECK(retained.puffs[0].age == 84); /* Retained frame owns its samples. */
  Write16(ram, child, 0x4000);
  ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 30);
  CHECK(frame.effect_count == 0 && frame.fireball_smoke.count > 0);
  ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 144);
  CHECK(frame.fireball_smoke.count == 0);

  /* Skipped captures must produce the same trail as individual gameplay ticks. */
  ActionFireballSmoke every_tick = {0};
  Write16(ram, child, 0);
  for (unsigned step = 1; step <= 8; step *= 2) {
    ActionEffectObserver_Reset(&observer);
    Write16(ram, child + 2, 1000);
    ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
    for (unsigned tick = step; tick <= 160; tick += step) {
      Write16(ram, child + 2, 1000 + tick * 4);
      ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), step);
    }
    if (step == 1) every_tick = frame.fireball_smoke;
    CHECK(frame.fireball_smoke.count == every_tick.count);
    for (unsigned i = 0; i < frame.fireball_smoke.count; ++i) {
      const ActionFireballSmokePuff *a = &every_tick.puffs[i];
      const ActionFireballSmokePuff *b = &frame.fireball_smoke.puffs[i];
      CHECK(a->seed == b->seed && a->age == b->age);
      CHECK(a->x == b->x && a->y == b->y && a->priority == b->priority);
    }
  }
  Write16(ram, child + 2, 1000);
  ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 8);
  CHECK(frame.effects[0].age_ticks == 0);
  CHECK(frame.fireball_smoke.count == every_tick.count - 1);
  CHECK(frame.fireball_smoke.puffs[frame.fireball_smoke.count - 1].x == 988);

  for (unsigned i = 1; i < 4; ++i) memcpy(ram + child + i * 64, ram + child, 64);
  ActionEffectObserver_Reset(&observer);
  for (unsigned tick = 0; tick <= 112; ++tick) {
    for (unsigned i = 0; i < 4; ++i) Write16(ram, child + i * 64 + 2, 1000 + tick * 4);
    ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
    CHECK(frame.fireball_smoke.count <= kActionFireballSmokeMaxPuffs && !frame.overflow);
  }
  CHECK(frame.fireball_smoke.count == kActionFireballSmokeMaxPuffs);
  SeedSwordBeam(ram, 0x13, false);
  ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 0);
  CHECK(frame.fireball_smoke.count == kActionFireballSmokeMaxPuffs);
  CHECK(!frame.overflow && frame.effect_count == 5 && frame.visible_count == 5);
  CHECK(frame.effects[0].kind == kActionEffect_SwordBeam && frame.effects[0].obj_priority == 3);
  /* The boss leaves old fireballs alive outside its activation window. Their
   * history must not exhaust the 16 render records and erase the sword beam. */
  for (unsigned i = 4; i < 24; ++i) {
    memcpy(ram + child + i * 64, ram + child, 64);
    Write16(ram, child + i * 64 + 0x30, 0x0400);
  }
  ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
  CHECK(!frame.overflow && frame.effect_count == 5 && frame.visible_count == 5);
  CHECK(frame.effects[0].kind == kActionEffect_SwordBeam);
  ram[0x19] = 2;
  ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
  CHECK(frame.fireball_smoke.count == 0);
}

static void TestBloodpoolFireballDeathLifetime(void) {
  static uint8_t ram[kActRaiserWramSize], unchanged[kActRaiserWramSize];
  static ActionSceneEffectFrame frame;
  ActionEffectObserver observer = {0};
  const unsigned parent = 0x0CA0, child = 0x0CE0;
  SeedBloodpoolAct1BossFireball(ram);
  for (unsigned tick = 0; tick <= 20; ++tick) {
    Write16(ram, child + 2, 3900 - tick * 4);
    ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
  }
  CHECK(frame.effect_count == 1 && frame.fireball_smoke.count == 6);
  const ActionEffectInstance moving = frame.effects[0];
  const ActionFireballSmoke trail = frame.fireball_smoke;

  /* A lethal hit changes the root to $A593, HP0, flags $0032. The child
   * remains its own live projectile and may keep moving during the flash. */
  Write16(ram, parent + 0x12, 0xA593);
  Write16(ram, parent + 0x30, 0x0032);
  Write16(ram, child + 2, 3816);
  ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
  CHECK(frame.effect_count == 1 && frame.visible_count == 1);
  CHECK(frame.effects[0].generation == moving.generation);
  CHECK(frame.effects[0].age_ticks == moving.age_ticks + 1);
  CHECK(frame.fireball_smoke.puffs[0].age == trail.puffs[0].age + 1);
  const ActionEffectInstance stopped = frame.effects[0];
  const ActionFireballSmoke stopped_trail = frame.fireball_smoke;

  /* Freeze on actual lack of motion, even if native velocity stays nonzero.
   * Clearing velocity later must not rotate or restart the attached wake. */
  for (unsigned tick = 0; tick < 180; ++tick) {
    if (tick == 40) Write16(ram, child + 6, 0);
    if (tick == 80) {
      Write16(ram, parent, 0x4000);
      Write16(ram, parent + 0x20, 0);
    }
    memcpy(unchanged, ram, sizeof(ram));
    ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
    CHECK(!memcmp(ram, unchanged, sizeof(ram)));
    CHECK(frame.effect_count == 1 && frame.visible_count == 1);
    CHECK(!memcmp(&frame.effects[0], &stopped, sizeof(stopped)));
    CHECK(frame.fireball_smoke.count == stopped_trail.count);
    for (unsigned i = 0; i < frame.fireball_smoke.count; ++i) {
      CHECK(frame.fireball_smoke.puffs[i].frozen);
      CHECK(frame.fireball_smoke.puffs[i].age == stopped_trail.puffs[i].age);
      CHECK(frame.fireball_smoke.puffs[i].seed == stopped_trail.puffs[i].seed);
    }
  }
  /* A fresh capture during death still recognizes the child's own artwork. */
  ActionEffectObserver fresh = {0};
  ActionSceneEffects_CaptureFrame(&fresh, &frame, ram, sizeof(ram), 0);
  CHECK(frame.effect_count == 1 && frame.visible_count == 1);

  /* A resumed source advances normally instead of staying latched frozen. */
  Write16(ram, child + 6, (uint16_t)-4);
  Write16(ram, child + 2, 3812);
  ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
  CHECK(frame.effects[0].age_ticks == stopped.age_ticks + 1);
  CHECK(!frame.fireball_smoke.puffs[0].frozen);
  CHECK(frame.fireball_smoke.puffs[0].age == stopped_trail.puffs[0].age + 1);
  ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
  Write16(ram, child, 0x4000);
  ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
  CHECK(frame.effect_count == 0 && frame.fireball_smoke.count == 0);

  /* Smoke follows each generation independently: a stopped shot cannot
   * freeze a moving neighbour, and hidden/recycled sources release it. */
  for (unsigned removal = 0; removal < 3; ++removal) {
    SeedBloodpoolAct1BossFireball(ram);
    ActionEffectObserver_Reset(&observer);
    const unsigned other = child + 0x40;
    memcpy(ram + other, ram + child, 0x40);
    for (unsigned tick = 0; tick <= 12; ++tick) {
      Write16(ram, child + 2, 3900 - tick * 4);
      Write16(ram, other + 2, 3800 - tick * 4);
      ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
    }
    const uint32_t generation = frame.effects[0].generation;
    for (unsigned tick = 1; tick <= 8; ++tick) {
      Write16(ram, other + 2, 3752 - tick * 4);
      ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
    }
    CHECK(frame.effects[0].age_ticks == 12 && frame.effects[1].age_ticks == 20);
    unsigned frozen_count = 0;
    for (unsigned i = 0; i < frame.fireball_smoke.count; ++i) {
      const ActionFireballSmokePuff *puff = &frame.fireball_smoke.puffs[i];
      CHECK(puff->frozen == (puff->source_generation == generation));
      frozen_count += puff->frozen;
    }
    CHECK(frozen_count == 4);
    if (removal == 0) Write16(ram, child, 0x2000); /* Native no-draw. */
    if (removal == 1) Write16(ram, child + 0x30, 0x0400);
    if (removal == 2) Write16(ram, child + 2, 1000); /* Same-slot reuse. */
    Write16(ram, other + 2, 3716);
    ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
    for (unsigned i = 0; i < frame.fireball_smoke.count; ++i)
      CHECK(frame.fireball_smoke.puffs[i].source_generation != generation);
    CHECK(frame.fireball_smoke.count > 0);
  }
}

static void TestGenericActorFacts(void) {
  static uint8_t ram[kActRaiserWramSize],unchanged[kActRaiserWramSize];
  static ActionSceneEffectFrame frame;static ActionEffectObserver observer;
  memset(ram,0,sizeof(ram));ActionEffectObserver_Reset(&observer);ram[0x18]=2;ram[0x19]=1;
  const unsigned parent=0x12e0,child=0x1320;
  for(unsigned at=parent;at<=child;at+=0x40){Write16(ram,at+2,120);Write16(ram,at+4,96);Write16(ram,at+0x16,0x5000);
    ram[at+0x18]=0x7e;Write16(ram,at+0x20,0x5220);Write16(ram,at+0x32,0xb786);}
  Write16(ram,parent+0x1a,7);Write16(ram,child+0x1a,1);Write16(ram,child+0x3a,parent);Write16(ram,child+0x12,0xb90d);
  memcpy(unchanged,ram,sizeof(ram));ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),1);
  CHECK(!memcmp(ram,unchanged,sizeof(ram)));CHECK(frame.actor_count==2&&frame.effect_count==0);
  CHECK(frame.actors[1].source==0xb786&&frame.actors[1].parent_source==0xb786&&frame.actors[1].state==1&&frame.actors[1].handler==0xb90d);
  const uint32_t generation=frame.actors[1].generation;
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),7);CHECK(frame.actors[1].generation==generation&&frame.actors[1].age==7);
  ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),0);CHECK(frame.actors[1].age==7);
  Write16(ram,child,0x4000);ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),1);CHECK(frame.actor_count==1);
  Write16(ram,child,0);ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),1);CHECK(frame.actors[1].generation!=generation&&frame.actors[1].age==0);
  const uint32_t replacement=frame.actors[1].generation;ram[0x19]=2;ActionSceneEffects_CaptureFrame(&observer,&frame,ram,sizeof(ram),1);CHECK(frame.actors[1].generation!=replacement);
}

#include "action_effect_lifecycle_test.inc"

int main(void) {
  TestSceneLifecycleContracts();
  TestAuthoredParentProvenance();
  TestBloodpoolAct1BossFireballs();
  TestBloodpoolFireballDeathLifetime();
  TestGenericActorFacts();
  TestBloodpoolEnvironmentCapture();
  TestCastleEnvironmentCapture();
  TestCastleGalleryAndWaterCapture();
  TestCaveWetMaterials();
  TestDustSettling();
  TestFillmoreStatueOrbs();
  TestTempleMistCollisionCapture();
  TestLowerCaveMistCollisionCapture();
  TestTempleCapitalContacts();
  TestLandingDustContacts();
  TestCaveEnvironmentalCapture();
  TestForestEnvironmentalCapture();
  TestFirstActBossMagic();
  TestControllerAndSlotIdentity();
  TestEverySpellIsIdentified();
  TestUnmatchedSlotsAreCensused();
  TestCapturedFieldsAndGeometry();
  TestLiveWramRecordIsRecognized();
  TestLifecycleUsesProducerTicks();
  TestGameplayTickClockTracksCompletedPasses();
  TestMalformedInputsFailClosed();
  TestMeasuredSceneObjectIdentities();
  TestBloodpoolAct2SceneScopeAndRoomContinuity();
  TestBloodpoolBossLightningIdentity();
  TestSwordBeamIdentityAndAuthoredGeometry();
  TestAitosBossSwordVolleyIdentityAndGeometry();
  TestBloodpoolTorchMetatileIdentity();
  TestMarahnaTorchMetatileIdentityAndWindow();
  TestMarahnaFireballIdentityAndContinuity();
  TestMarahnaSnakeFireballIdentity();
  TestMarahnaLightningLinkIdentityAndOrientations();
  TestMarahnaBossLightningIdentityAndStages();
  TestAitosLavaPitIdentityAndWindow();
  TestAitosAct2SideLavaReservoirIdentity();
  TestAitosMoltenRockIdentity();
  TestAitosWaterfallSplashIdentity();
  TestAitosLavaFireballIdentityAndContinuity();
  TestAitosStatueFireIdentityAndPriority();
  TestBossEffectsCarryIntoDeathHeim();
  TestSceneCaptureCapacityFailsClosed();
  if (s_failures) {
    fprintf(stderr, "%d action-effects test(s) failed\\n", s_failures);
    return 1;
  }
  puts("action effects: all tests passed");
  return 0;
}
