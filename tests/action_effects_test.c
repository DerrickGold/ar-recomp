#include <stdio.h>
#include <string.h>

#include "action_effect_clock.h"
#include "action_effects.h"
#include "action_bg_plan.h"
#include "action_bg_world.h"
#include "actraiser_game.h"
#include "frame_timing.h"

static int g_failures;

#define CHECK(condition) do {                                                \
  if (!(condition)) {                                                        \
    fprintf(stderr, "%s:%d: check failed: %s\\n", __FILE__, __LINE__,       \
            #condition);                                                     \
    g_failures++;                                                            \
  }                                                                          \
} while (0)

static void Write16(uint8_t *wram, size_t address, uint16_t value) {
  wram[address] = (uint8_t)value;
  wram[address + 1] = (uint8_t)(value >> 8);
}

/* Raw offsets are deliberate here: this fixture is an independent assertion
 * of the reverse-engineered WRAM contract, not a tautology built from the
 * production field enum. */
static void SeedFireSlot(uint8_t *wram, unsigned slot, uint16_t visual) {
  static const uint16_t kFlips[] = { 0x0000, 0x4000, 0x8000, 0xC000 };
  size_t address = kActRaiserWram_ActionObjectTable +
      slot * kActRaiserActionObjectStride;
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
  Write16(wram, address + 0x28,
          (uint16_t)(kFlips[slot] | 0x3900));
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
  CHECK(frame.effects[0].render_layer ==
        kActionEffectRenderLayer_WorldOverlay);
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

  for (unsigned slot = 0; slot < 4; slot++) SeedFireSlot(wram, slot, 13);
  ActionEffects_CaptureFrame(&observer, &changed, wram, sizeof(wram), 2);
  CHECK(changed.effects[0].age_ticks == 5);
  CHECK(changed.effects[0].phase_ticks == 0);
  CHECK(changed.effects[0].pulse_ticks == 5);
  CHECK(changed.effects[0].generation == first.effects[0].generation);
  CHECK(changed.effects[0].pulse_generation ==
        first.effects[0].pulse_generation);

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
  CHECK(ActionEffectTickClock_Capture(&clock) ==
        kFrameTimingMaximumElapsedTicks);

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
    0x00, 0x00, 0xF6, 0x01, 0xDF, 0x01, 0x04, 0x00,
    0x02, 0x00, 0x2C, 0x00, 0x10, 0x00, 0x08, 0x00,
    0x09, 0x00, 0xB8, 0xA0, 0x00, 0x00, 0x00, 0xC0,
    0x07, 0x39, 0x03, 0x00, 0x05, 0x00, 0x00, 0x00,
    0x52, 0xC3, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x39, 0x01, 0x00, 0x00, 0x00, 0xC0, 0x00,
    0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  uint8_t wram[kActRaiserWramSize];
  ActionEffectFrame frame;
  ActionEffectObserver observer = {0};

  memset(wram, 0, sizeof(wram));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Fillmore;
  Write16(wram, kActRaiserWram_GameFrame, 1913);
  Write16(wram, kActRaiserWram_MagicController + 0x00, 0x0800);
  Write16(wram, kActRaiserWram_MagicController + 0x38, 0x0001);
  memcpy(wram + kActRaiserWram_ActionObjectTable, kLiveFireRecord,
         sizeof(kLiveFireRecord));

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
static void SeedSlot(uint8_t *wram, unsigned cohort, uint16_t animation,
                     uint8_t bank, uint16_t state, uint16_t visual,
                     uint16_t flips) {
  size_t address = kActRaiserWram_ActionObjectTable +
      cohort * kActRaiserActionObjectStride;
  Write16(wram, address + 0x00, 0x0000);          /* active */
  Write16(wram, address + 0x02, (uint16_t)(200 + cohort * 30));
  Write16(wram, address + 0x04, (uint16_t)(150 + cohort * 10));
  Write16(wram, address + 0x0A, 12);
  Write16(wram, address + 0x0C, 12);
  Write16(wram, address + 0x0E, 12);
  Write16(wram, address + 0x10, 12);
  Write16(wram, address + 0x16, animation);
  wram[address + 0x18] = bank;
  wram[address + 0x19] = 0x39;                    /* the separate byte at +$19 */
  Write16(wram, address + 0x1A, state);
  Write16(wram, address + 0x22, visual);
  Write16(wram, address + 0x20, 0xD100);          /* composition must be set */
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
  static const uint16_t kAuraFlips[] = { 0x0000, 0x4000, 0x8000, 0xC000 };
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
  SeedSlot(wram, 4, 0xC800, 0x07, 1, 7, 0x0000);   /* centre, visuals 5-9 */
  SeedSlot(wram, 5, 0xC800, 0x07, 1, 2, 0x0000);   /* column, visuals 1-4 */
  SeedSlot(wram, 6, 0xC800, 0x07, 1, 2, 0x4000);   /* mirrored column */
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

static void SeedMeasuredSceneObject(uint8_t *wram, unsigned slot,
                                    bool lightning) {
  size_t address = kActRaiserWram_ActionObjectTable +
      slot * kActRaiserActionObjectStride;
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

static void SeedMarahnaFireball(uint8_t *wram, unsigned slot,
                                int16_t world_x, int16_t world_y) {
  const size_t address = kActRaiserWram_ActionObjectTable +
      slot * kActRaiserActionObjectStride;
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

static void SetMarahnaFireballOrbFrame(uint8_t *wram, unsigned slot,
                                       uint16_t visual,
                                       uint16_t composition,
                                       int16_t velocity_x) {
  const size_t address = kActRaiserWram_ActionObjectTable +
      slot * kActRaiserActionObjectStride;
  Write16(wram, address + 0x06, (uint16_t)velocity_x);
  Write16(wram, address + 0x08, 0);
  Write16(wram, address + 0x20, composition);
  Write16(wram, address + 0x22, visual);
}

static void SeedMarahnaSplitFireball(uint8_t *wram, unsigned slot,
                                     unsigned parent_slot,
                                     int16_t velocity_x,
                                     int16_t velocity_y) {
  const size_t address = kActRaiserWram_ActionObjectTable +
      slot * kActRaiserActionObjectStride;
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
          velocity_x > 0 ? kActRaiserObjectFlip_Horizontal :
          velocity_y < 0 ? kActRaiserObjectFlip_Vertical : 0);
  Write16(wram, address + 0x2E, 0);
  Write16(wram, address + 0x30, 0x0020);
  Write16(wram, address + 0x32, 0xE047);
  Write16(wram, address + 0x3A, (uint16_t)(
      kActRaiserWram_ActionObjectTable +
      parent_slot * kActRaiserActionObjectStride));
}

static void SeedMarahnaSnakeFireballShot(
    uint8_t *wram, unsigned parent_slot, unsigned shot_slot,
    bool horizontal_flip, uint16_t visual, uint16_t composition) {
  const size_t parent = kActRaiserWram_ActionObjectTable +
      parent_slot * kActRaiserActionObjectStride;
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
  Write16(wram, parent + 0x28,
          horizontal_flip ? kActRaiserObjectFlip_Horizontal : 0);
  Write16(wram, parent + 0x32, 0xDE96);

  const size_t shot = kActRaiserWram_ActionObjectTable +
      shot_slot * kActRaiserActionObjectStride;
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
  Write16(wram, shot + 0x28,
          horizontal_flip ? kActRaiserObjectFlip_Horizontal : 0);
  Write16(wram, shot + 0x30, 0x0020);
  Write16(wram, shot + 0x32, 0xDE96);
  Write16(wram, shot + 0x38, 0x0006);
  Write16(wram, shot + 0x3A, (uint16_t)parent);
}

static void SeedAitosLavaFireball(uint8_t *wram, unsigned slot,
                                  int16_t world_x, int16_t world_y,
                                  uint16_t state) {
  const size_t address = kActRaiserWram_ActionObjectTable +
      slot * kActRaiserActionObjectStride;
  Write16(wram, address + 0x00, 0x0000);
  Write16(wram, address + 0x02, (uint16_t)world_x);
  Write16(wram, address + 0x04, (uint16_t)world_y);
  Write16(wram, address + 0x06, state == 0x0024 ? 0xFFFF : 0x0000);
  Write16(wram, address + 0x08,
          state == 0x0022 ? 0xFFFC : state == 0x0024 ? 0x0006 : 0x0000);
  Write16(wram, address + 0x0A, 0x0008);
  Write16(wram, address + 0x0C, 0x0008);
  Write16(wram, address + 0x0E, 0x0008);
  Write16(wram, address + 0x10, 0x0008);
  Write16(wram, address + 0x12,
          state == 0x0022 ? 0xCFE3 : state == 0x0024 ? 0xCFFE : 0x8661);
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

static void SeedAitosMoltenRock(uint8_t *wram, unsigned slot,
                                int16_t world_x, int16_t world_y,
                                int16_t velocity_x, int16_t velocity_y) {
  const size_t address = kActRaiserWram_ActionObjectTable +
      slot * kActRaiserActionObjectStride;
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
  Write16(wram, address + 0x28,
          velocity_x > 0 ? kActRaiserObjectFlip_Horizontal : 0);
  Write16(wram, address + 0x32, 0xCEEC);
}

static void SeedMarahnaLightningEndpoint(uint8_t *wram, unsigned slot,
                                         bool partner, bool vertical,
                                         int16_t world_x,
                                         int16_t world_y) {
  const size_t address = kActRaiserWram_ActionObjectTable +
      slot * kActRaiserActionObjectStride;
  Write16(wram, address + 0x00, 0x0000);
  Write16(wram, address + 0x02, (uint16_t)world_x);
  Write16(wram, address + 0x04, (uint16_t)world_y);
  Write16(wram, address + 0x12, 0x8683);
  Write16(wram, address + 0x16, 0x4000);
  wram[address + 0x18] = 0x7E;
  Write16(wram, address + 0x1A, partner ? 0x001D : 0x001A);
  Write16(wram, address + 0x20, partner
      ? (vertical ? 0x45DC : 0x45D0)
      : (vertical ? 0x45C4 : 0x45B8));
  Write16(wram, address + 0x22, partner
      ? (vertical ? 0x0010 : 0x000F)
      : (vertical ? 0x000E : 0x000D));
  Write16(wram, address + 0x30, 0x0020);
  Write16(wram, address + 0x32, partner ? 0xE254 : 0xE18E);
}

static void SeedMarahnaLightningLink(uint8_t *wram, unsigned slot,
                                     unsigned parent_slot, bool vertical,
                                     int16_t world_x, int16_t world_y) {
  const size_t address = kActRaiserWram_ActionObjectTable +
      slot * kActRaiserActionObjectStride;
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
  Write16(wram, address + 0x3A, (uint16_t)(
      kActRaiserWram_ActionObjectTable +
      parent_slot * kActRaiserActionObjectStride));
}

static void SeedBgMetatile(uint8_t *wram, unsigned world_width,
                           unsigned world_x, unsigned world_y,
                           uint8_t metatile) {
  const unsigned cells_wide = world_width / kActionBgMetatilePixels;
  const unsigned page_x = world_x / 256u;
  const unsigned page_y = world_y / 256u;
  const unsigned pages_wide = cells_wide / 16u;
  const unsigned cell_x = (world_x / kActionBgMetatilePixels) & 15u;
  const unsigned cell_y = (world_y / kActionBgMetatilePixels) & 15u;
  wram[0x8000 + (page_y * pages_wide + page_x) * 256u +
       cell_y * 16u + cell_x] = metatile;
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

  const size_t beam = kActRaiserWram_ActionObjectTable +
      9 * kActRaiserActionObjectStride;
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
  Write16(wram, beam + 0x28,
          hflip ? kActRaiserObjectFlip_Horizontal : 0x0000);
  Write16(wram, beam + 0x30, kActRaiserObjectFlag_Attacker);
  Write16(wram, beam + 0x32, 0x979A);
  Write16(wram, beam + 0x3A, kActRaiserWram_PlayerObject);
}

static void SeedAitosBossSwordVolley(uint8_t *wram, bool reflected) {
  const size_t boss = kActRaiserWram_ActionObjectTable +
      49 * kActRaiserActionObjectStride;
  const size_t parent = kActRaiserWram_ActionObjectTable +
      62 * kActRaiserActionObjectStride;
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
          reflected ? kActRaiserObjectFlip_Horizontal |
                          kActRaiserObjectFlip_Vertical
                    : 0);
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
    const size_t child = kActRaiserWram_ActionObjectTable +
        (63 + i) * kActRaiserActionObjectStride;
    Write16(wram, child + 0x00, 0x0000);
    Write16(wram, child + 0x02, 444);
    Write16(wram, child + 0x04, (uint16_t)kCrescents[i].world_y);
    Write16(wram, child + 0x06,
            (uint16_t)(int16_t)(reflected ? 3 : -3));
    Write16(wram, child + 0x08,
            (uint16_t)(int16_t)(reflected
                ? -kCrescents[i].velocity_y
                : kCrescents[i].velocity_y));
    Write16(wram, child + 0x0A, reflected ? 16 : 8);
    Write16(wram, child + 0x0C,
            reflected ? kCrescents[i].bottom_extent
                      : kCrescents[i].top_extent);
    Write16(wram, child + 0x0E, reflected ? 8 : 16);
    Write16(wram, child + 0x10,
            reflected ? kCrescents[i].top_extent
                      : kCrescents[i].bottom_extent);
    Write16(wram, child + 0x12, 0x8661);
    Write16(wram, child + 0x16, 0x5000);
    wram[child + 0x18] = 0x7E;
    Write16(wram, child + 0x1A, kCrescents[i].state);
    Write16(wram, child + 0x1C, 0x0001);
    Write16(wram, child + 0x1E, 0xA65D);
    Write16(wram, child + 0x20, kCrescents[i].composition);
    Write16(wram, child + 0x22, kCrescents[i].visual);
    Write16(wram, child + 0x28,
            reflected ? kActRaiserObjectFlip_Horizontal |
                            kActRaiserObjectFlip_Vertical
                      : 0);
    Write16(wram, child + 0x30, 0x0020);
    Write16(wram, child + 0x32, 0xD646);
    Write16(wram, child + 0x38, kCrescents[i].local_counter);
    Write16(wram, child + 0x3A, (uint16_t)parent);
  }
}

static void SeedBloodpoolBossLightningStrike(uint8_t *wram, unsigned slot,
                                             unsigned visual, bool hflip) {
  static const uint16_t kComposition[] = {
    0x5346, 0x5401, 0x5492, 0x54F2, 0x55C2, 0x5661,
  };
  static const uint8_t kLeft[] = {6, 6, 1, 48, 36, 30};
  static const uint8_t kRight[] = {11, 11, 11, 8, 8, 8};
  static const uint8_t kBottom[] = {117, 69, 21, 117, 69, 21};
  static const uint16_t kResume[] = {0xC02B, 0xC04B, 0xC051};
  const size_t address = kActRaiserWram_ActionObjectTable +
      slot * kActRaiserActionObjectStride;
  CHECK(visual < 6);
  Write16(wram, address + 0x00, 0x0000);
  Write16(wram, address + 0x02, 120);
  Write16(wram, address + 0x04, 160);
  Write16(wram, address + 0x0A,
          hflip ? kRight[visual] : kLeft[visual]);
  Write16(wram, address + 0x0C, 83);
  Write16(wram, address + 0x0E,
          hflip ? kLeft[visual] : kRight[visual]);
  Write16(wram, address + 0x10, kBottom[visual]);
  Write16(wram, address + 0x12, 0x8661);
  Write16(wram, address + 0x16, 0x5000);
  wram[address + 0x18] = 0x7E;
  Write16(wram, address + 0x1A, (uint16_t)(visual + 2));
  Write16(wram, address + 0x1C, 1);
  Write16(wram, address + 0x1E, kResume[visual % 3]);
  Write16(wram, address + 0x20, kComposition[visual]);
  Write16(wram, address + 0x22, (uint16_t)visual);
  Write16(wram, address + 0x28,
          hflip ? kActRaiserObjectFlip_Horizontal : 0);
  Write16(wram, address + 0x30, 0x0020);
  Write16(wram, address + 0x32, 0xBDFF);
  Write16(wram, address + 0x3A, 0x12E0);
}

static void SeedBloodpoolBossLightningImpact(uint8_t *wram, unsigned slot,
                                             unsigned visual) {
  static const uint16_t kComposition[] = {0x570A, 0x5716, 0x5729};
  static const uint8_t kExtent[] = {4, 8, 16};
  static const uint8_t kTop[] = {8, 8, 16};
  const size_t address = kActRaiserWram_ActionObjectTable +
      slot * kActRaiserActionObjectStride;
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
  ActionSceneEffectFrame first, paused, advanced, reused, source_reused,
      alternate, rejected;
  ActionEffectObserver observer = {0};
  memset(wram, 0, sizeof(wram));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Bloodpool;
  wram[kActRaiserWram_CurrentMap] = 5;
  Write16(wram, kActRaiserWram_GameFrame, 7397);
  SeedMeasuredSceneObject(wram, 22, false);  /* live address $0C20 */
  SeedMeasuredSceneObject(wram, 32, true);   /* live address $0EA0 */

  ActionSceneEffects_CaptureFrame(&observer, &first, wram, sizeof(wram), 1);
  CHECK(first.game_frame == 7397);
  CHECK(first.effect_count == 2);
  CHECK(first.visible_count == 2);
  CHECK(first.effects[0].record_address == 0x0C20);
  CHECK(first.effects[0].kind == kActionEffect_EnemyFireball);
  CHECK(first.effects[0].phase ==
        kActionEffectPhase_EnemyFireballFlight);
  CHECK(first.effects[0].velocity_x == 3);
  CHECK(first.effects[0].projection_plane ==
        kActionEffectProjectionPlane_Obj);
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
  Write16(wram, 0x0C20 + 0x02, 712);  /* +3 px/tick for three ticks */
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
  CHECK(reused.effects[0].pulse_generation !=
        advanced.effects[0].pulse_generation);
  CHECK(reused.effects[0].age_ticks == 0);
  CHECK(reused.effects[0].pulse_ticks == 0);

  Write16(wram, 0x0C20 + 0x02, 1203);
  Write16(wram, 0x0C20 + 0x32, 0xBD76);
  ActionSceneEffects_CaptureFrame(&observer, &source_reused, wram,
                                  sizeof(wram), 1);
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
      ActionSceneEffects_CaptureFrame(
          &observer, &frame, wram, sizeof(wram), 1);
      CHECK(frame.effect_count == 1);
      CHECK(frame.effects[0].record_address == 0x08E0);
      CHECK(frame.effects[0].kind == kActionEffect_BloodpoolBossLightning);
      CHECK(frame.effects[0].phase == kActionEffectPhase_BossLightningStrike);
      CHECK(frame.effects[0].visual == visual);
      CHECK(frame.effects[0].animation_state == visual + 2);
      CHECK(frame.effects[0].left_extent ==
            (flipped ? kRight[visual] : kLeft[visual]));
      CHECK(frame.effects[0].right_extent ==
            (flipped ? kLeft[visual] : kRight[visual]));
      CHECK(frame.effects[0].bottom_extent == kBottom[visual]);
      CHECK(((frame.effects[0].flags &
              kActionEffectFlag_FlipHorizontal) != 0) == (flipped != 0));
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
    ActionSceneEffects_CaptureFrame(&observer, &frame, wram,
                                    sizeof(wram), 1);
    CHECK(frame.effect_count == 2);
    CHECK(frame.effects[1].phase == kActionEffectPhase_BossLightningImpact);
    CHECK(frame.effects[1].visual == visual);
  }

  /* The boss animation bank is shared by the room. Map, control flow, linked
   * parent, transform, and exact composition tuple are all required. */
  wram[kActRaiserWram_CurrentMap] = 7;
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram,
                                  sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  wram[kActRaiserWram_CurrentMap] = 8;
  Write16(wram, 0x08E0 + 0x12, 0x8660);
  Write16(wram, 0x0920 + 0x12, 0x8660);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram,
                                  sizeof(wram), 1);
  CHECK(frame.effect_count == 0);

  SeedBloodpoolBossLightningStrike(wram, 9, 4, false);
  Write16(wram, 0x08E0 + 0x3A, 0x08A0);  /* player, not boss family */
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram,
                                  sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  SeedBloodpoolBossLightningStrike(wram, 9, 4, false);
  Write16(wram, 0x08E0 + 0x28, kActRaiserObjectFlip_Vertical);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram,
                                  sizeof(wram), 1);
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
  CHECK(frame.effects[0].projection_plane ==
        kActionEffectProjectionPlane_Obj);
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
  CHECK(frame.effects[0].world_x - 120 +
        frame.effects[0].geometry.data.rect.x0 == 144.0f);
  CHECK(frame.effects[0].world_y - 255 +
        frame.effects[0].geometry.data.rect.y0 == 168.0f);
  CHECK(frame.effects[0].world_x - 120 +
        frame.effects[0].geometry.data.rect.x1 == 160.0f);
  CHECK(frame.effects[0].world_y - 255 +
        frame.effects[0].geometry.data.rect.y1 == 200.0f);

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
  CHECK(frame.effects[0].world_x - 136 +
        frame.effects[0].geometry.data.rect.x0 == 300.0f);
  CHECK(frame.effects[0].world_y - 8 +
        frame.effects[0].geometry.data.rect.y0 == 43.0f);

  CHECK(frame.effects[1].record_address == 0x16A0);
  CHECK(frame.effects[1].visual == 0x20);
  CHECK(frame.effects[1].composition == 0x56BE);
  CHECK(frame.effects[1].velocity_y == -1);
  CHECK(frame.effects[1].obj_priority == 2);
  CHECK(frame.effects[1].geometry.data.rect.x0 == -8.0f);
  CHECK(frame.effects[1].geometry.data.rect.y0 == -9.0f);
  CHECK(frame.effects[1].geometry.data.rect.x1 == 16.0f);
  CHECK(frame.effects[1].geometry.data.rect.y1 == 15.0f);
  CHECK(frame.effects[1].world_y - 8 +
        frame.effects[1].geometry.data.rect.y0 == 27.0f);

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
  CHECK(frame.effects[0].world_x - 120 +
        frame.effects[0].geometry.data.rect.x0 == 202.0f);
  CHECK(frame.effects[0].world_y - 8 +
        frame.effects[0].geometry.data.rect.y0 == 33.0f);
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
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  SeedAitosBossSwordVolley(wram, false);
  Write16(wram, 0x12E0 + 0x32, 0xD645);
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
  CHECK(frame.decorations[0].projection_plane ==
        kActionEffectProjectionPlane_Bg1);
  CHECK(frame.decorations[0].render_layer ==
        kActionEffectRenderLayer_Bg1Plane);
  CHECK(frame.decorations[0].phase_ticks == 2479);
  CHECK(frame.decorations[1].world_x == 72);
  CHECK(frame.decorations[1].world_y == 63);
  CHECK(frame.decorations[1].phase_ticks ==
        frame.decorations[0].phase_ticks);

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
    CHECK(frame.decorations[0].projection_plane ==
          kActionEffectProjectionPlane_Bg1);
    CHECK(frame.decorations[0].render_layer ==
          kActionEffectRenderLayer_Bg1Plane);
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
    {0x0E0, 0x110}, {0x110, 0x110},
    {0x0C0, 0x130}, {0x130, 0x130},
    {0x0B0, 0x150}, {0x140, 0x150},
    {0x0C0, 0x170}, {0x130, 0x170},
    {0x0E0, 0x190}, {0x110, 0x190},
  };
  for (size_t i = 0;
       i < sizeof(kBossTorchCells) / sizeof(kBossTorchCells[0]); i++)
    SeedBgMetatile(wram, 512, kBossTorchCells[i][0],
                   kBossTorchCells[i][1], 0x43);
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

static void SeedMarahnaBossParent(uint8_t *wram, unsigned slot,
                                  uint16_t state, uint16_t visual,
                                  uint16_t composition) {
  const size_t address = kActRaiserWram_ActionObjectTable +
      slot * kActRaiserActionObjectStride;
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

static void SeedMarahnaBossBolt(uint8_t *wram, unsigned slot,
                                unsigned parent_slot, bool right) {
  const size_t address = kActRaiserWram_ActionObjectTable +
      slot * kActRaiserActionObjectStride;
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
  Write16(wram, address + 0x28,
          right ? kActRaiserObjectFlip_Horizontal : 0);
  Write16(wram, address + 0x30, 0x0020);
  Write16(wram, address + 0x32, 0xE483);
  Write16(wram, address + 0x3A, (uint16_t)(
      kActRaiserWram_ActionObjectTable +
      parent_slot * kActRaiserActionObjectStride));
}

static void SeedMarahnaBossGroundCharge(uint8_t *wram, unsigned slot,
                                        unsigned parent_slot, bool right,
                                        uint16_t visual) {
  const size_t address = kActRaiserWram_ActionObjectTable +
      slot * kActRaiserActionObjectStride;
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
  Write16(wram, address + 0x28,
          right ? kActRaiserObjectFlip_Horizontal : 0);
  Write16(wram, address + 0x30, 0x0020);
  Write16(wram, address + 0x32, 0xE483);
  Write16(wram, address + 0x3A, (uint16_t)(
      kActRaiserWram_ActionObjectTable +
      parent_slot * kActRaiserActionObjectStride));
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
  CHECK(frame.effects[0].phase ==
        kActionEffectPhase_MarahnaBossLightningCharge);

  SeedMarahnaBossParent(wram, 49, 1, 0x000A, 0x59DE);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].phase ==
        kActionEffectPhase_MarahnaBossLightningOrb);

  SeedMarahnaBossParent(wram, 49, 1, 0x0003, 0x54AC);
  SeedMarahnaBossBolt(wram, 11, 49, false);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].phase ==
        kActionEffectPhase_MarahnaBossLightningBolt);
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
  Write16(wram, kActRaiserWram_ActionObjectTable +
          11 * kActRaiserActionObjectStride + 0x28, 0);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);

  /* The boss enters this exact repeat-animation tuple after impact while the
   * child becomes the ground-riding charge. It is still the backlink owner,
   * but no longer uses the pre-impact `$8661` handler. */
  const size_t parent = kActRaiserWram_ActionObjectTable +
      49 * kActRaiserActionObjectStride;
  Write16(wram, parent + 0x12, 0x8683);
  Write16(wram, parent + 0x1A, 0x000A);
  Write16(wram, parent + 0x1E, 0xE4D7);
  Write16(wram, parent + 0x20, 0x5307);
  Write16(wram, parent + 0x22, 0x0000);
  static const uint16_t kGroundVisuals[] = {0x0012, 0x0013, 0x0014};
  for (size_t i = 0;
       i < sizeof(kGroundVisuals) / sizeof(kGroundVisuals[0]); i++) {
    SeedMarahnaBossGroundCharge(wram, 11, 49, false, kGroundVisuals[i]);
    ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
    CHECK(frame.effect_count == 1);
    CHECK(frame.effects[0].phase ==
          kActionEffectPhase_MarahnaBossLightningGroundCharge);
    CHECK(frame.effects[0].visual == kGroundVisuals[i]);
    CHECK(frame.effects[0].geometry.data.rect.x0 ==
          (kGroundVisuals[i] == 0x0012 ? -8.0f : -16.0f));
  }
  SeedMarahnaBossGroundCharge(wram, 11, 49, true, 0x0014);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].velocity_x == 4);
  CHECK(frame.effects[0].flags & kActionEffectFlag_FlipHorizontal);

  Write16(wram, kActRaiserWram_ActionObjectTable +
          11 * kActRaiserActionObjectStride + 0x20, 0x5D0D);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);

  SeedMarahnaBossGroundCharge(wram, 11, 49, true, 0x0014);
  Write16(wram, parent + 0x1E, 0xE4F4);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);

  Write16(wram, parent + 0x1E, 0xE4D7);
  Write16(wram, kActRaiserWram_ActionObjectTable +
          11 * kActRaiserActionObjectStride + 0x06, 3);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  SeedMarahnaBossParent(wram, 49, 1, 0x0003, 0x54AC);
  SeedMarahnaBossBolt(wram, 11, 49, true);
  Write16(wram, kActRaiserWram_ActionObjectTable +
          49 * kActRaiserActionObjectStride + 0x32, 0xE482);
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
  CHECK(first.effects[0].phase ==
        kActionEffectPhase_MarahnaFireballOrb);
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
    {0x0007, 0x451C,  0},
    {0x0008, 0x4528, -1},
    {0x0008, 0x4528, -2},
    {0x0005, 0x4504,  0},
    {0x0006, 0x4510,  1},
    {0x0006, 0x4510,  2},
  };
  for (size_t i = 0; i < sizeof(kOrbFrames) / sizeof(kOrbFrames[0]); i++) {
    SetMarahnaFireballOrbFrame(
        wram, 20, kOrbFrames[i].visual, kOrbFrames[i].composition,
        kOrbFrames[i].velocity_x);
    ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
    CHECK(frame.effect_count == 1);
    CHECK(frame.effects[0].phase ==
          kActionEffectPhase_MarahnaFireballOrb);
  }
  SetMarahnaFireballOrbFrame(wram, 20, 0x0006, 0x4510, -1);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  ActionEffectObserver_Reset(&observer);
  SeedMarahnaFireball(wram, 20, 400, 500);
  ActionSceneEffects_CaptureFrame(&observer, &first, wram, sizeof(wram), 1);

  Write16(wram, kActRaiserWram_ActionObjectTable +
          20 * kActRaiserActionObjectStride + 0x02, 401);
  ActionSceneEffects_CaptureFrame(&observer, &advanced, wram,
                                  sizeof(wram), 1);
  CHECK(advanced.effects[0].generation == first.effects[0].generation);
  CHECK(advanced.effects[0].age_ticks == 1);

  /* Immediate same-slot reuse by the same directional source retains every
   * signature word. The discontinuous spawn position is therefore essential
   * to prevent a replacement fireball inheriting the old flame trail. */
  Write16(wram, kActRaiserWram_ActionObjectTable +
          20 * kActRaiserActionObjectStride + 0x02, 900);
  ActionSceneEffects_CaptureFrame(&observer, &reused, wram,
                                  sizeof(wram), 1);
  CHECK(reused.effect_count == 1);
  CHECK(reused.effects[0].generation != advanced.effects[0].generation);
  CHECK(reused.effects[0].age_ticks == 0);

  /* The exact orb becomes an inactive lifecycle anchor while its four
   * children travel down/left/up/right. Every child validates that backlink,
   * its measured cardinal velocity, artwork, bounds, and corresponding flip. */
  SeedMarahnaFireball(wram, 20, 400, 500);
  const size_t parent = kActRaiserWram_ActionObjectTable +
      20 * kActRaiserActionObjectStride;
  Write16(wram, parent + 0x00, 0x4000);
  Write16(wram, parent + 0x1A, 0x000E);
  Write16(wram, parent + 0x1E, 0xE0A6);
  Write16(wram, parent + 0x20, 0x4597);
  Write16(wram, parent + 0x22, 0x000C);
  static const int16_t kSplitVelocity[][2] = {
    {0, 3}, {-3, 0}, {0, -3}, {3, 0},
  };
  for (size_t i = 0; i < 4; i++)
    SeedMarahnaSplitFireball(wram, 35 + (unsigned)i, 20,
                            kSplitVelocity[i][0], kSplitVelocity[i][1]);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 4);
  for (size_t i = 0; i < frame.effect_count; i++) {
    CHECK(frame.effects[i].kind == kActionEffect_MarahnaFireball);
    CHECK(frame.effects[i].phase ==
          kActionEffectPhase_MarahnaFireballSplit);
    CHECK(frame.effects[i].geometry.data.rect.x0 == -4.0f);
    CHECK(frame.effects[i].geometry.data.rect.x1 == 4.0f);
  }
  CHECK(!(frame.effects[0].flags &
          (kActionEffectFlag_FlipHorizontal | kActionEffectFlag_FlipVertical)));
  CHECK(!(frame.effects[1].flags &
          (kActionEffectFlag_FlipHorizontal | kActionEffectFlag_FlipVertical)));
  CHECK(frame.effects[2].flags & kActionEffectFlag_FlipVertical);
  CHECK(frame.effects[3].flags & kActionEffectFlag_FlipHorizontal);

  const size_t child = kActRaiserWram_ActionObjectTable +
      35 * kActRaiserActionObjectStride;
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

  static const struct { bool flip; uint16_t visual, composition; } kShots[] = {
    {false, 0x001D, 0x4869}, {false, 0x001E, 0x487C},
    { true, 0x001D, 0x4869}, { true, 0x001E, 0x487C},
  };
  for (size_t i = 0; i < sizeof(kShots) / sizeof(kShots[0]); i++) {
    memset(wram + kActRaiserWram_ActionObjectTable, 0,
           kActRaiserActionObjectCount * kActRaiserActionObjectStride);
    SeedMarahnaSnakeFireballShot(
        wram, 20, 30, kShots[i].flip,
        kShots[i].visual, kShots[i].composition);
    ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
    CHECK(frame.effect_count == 1);
    CHECK(frame.effects[0].kind == kActionEffect_MarahnaFireball);
    CHECK(frame.effects[0].phase ==
          kActionEffectPhase_MarahnaSnakeFireballShot);
  }

  const size_t shot = kActRaiserWram_ActionObjectTable +
      30 * kActRaiserActionObjectStride;
  const size_t parent = kActRaiserWram_ActionObjectTable +
      20 * kActRaiserActionObjectStride;
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
  Write16(wram, reaper_parent + 0x28,
          kActRaiserObjectFlip_Horizontal);
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
  CHECK(frame.effects[0].phase ==
        kActionEffectPhase_MarahnaLightningActive);
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
  Write16(wram, kActRaiserWram_ActionObjectTable +
          53 * kActRaiserActionObjectStride + 0x02, 353);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  SeedMarahnaLightningLink(wram, 53, 30, true, 352, 648);
  Write16(wram, kActRaiserWram_ActionObjectTable +
          31 * kActRaiserActionObjectStride + 0x20, 0x45DD);
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
  CHECK(frame.decorations[0].projection_plane ==
        kActionEffectProjectionPlane_Bg1);
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
  CHECK(ActionEffects_IsAitosAct2LavaRoom(
      kActRaiserMapGroup_Aitos, 4));
  CHECK(ActionEffects_IsAitosAct2LavaRoom(
      kActRaiserMapGroup_Aitos, 5));
  CHECK(ActionEffects_IsAitosAct2LavaRoom(
      kActRaiserMapGroup_Aitos, 6));
  CHECK(!ActionEffects_IsAitosAct2LavaRoom(
      kActRaiserMapGroup_Aitos, 3));
  CHECK(!ActionEffects_IsAitosAct2LavaRoom(3, 5));
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
    SeedBgMetatile(wram, 512, x + cell * 16, y - 16,
                   cell & 1u ? 0x00 : 0x02);
    SeedBgMetatile(wram, 512, x + cell * 16, y + 16, 0x05);
  }
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_count == 1);
  CHECK(frame.decorations[0].kind == kActionEffect_AitosLavaReservoir);
  CHECK(frame.decorations[0].phase ==
        kActionEffectPhase_AitosLavaReservoir);
  CHECK(frame.decorations[0].world_x == 96);
  CHECK(frame.decorations[0].world_y == 100);
  CHECK(frame.decorations[0].geometry.data.rect.x0 == -48.0f);
  CHECK(frame.decorations[0].geometry.data.rect.x1 == 48.0f);
  CHECK(frame.decorations[0].geometry.data.rect.y0 == -4.0f);
  CHECK(frame.decorations[0].geometry.data.rect.y1 == 4.0f);
  CHECK(frame.decorations[0].projection_plane ==
        kActionEffectProjectionPlane_Bg1High);
  CHECK(frame.decorations[0].render_layer ==
        kActionEffectRenderLayer_Bg1HighPlane);

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
    SeedBgMetatile(wram, 1024, x + cell * 16, y - 16,
                   cell % 5u ? 0x00 : 0x77);
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
  CHECK(frame.effects[0].phase ==
        kActionEffectPhase_AitosMoltenRockFlight);
  CHECK(frame.effects[0].velocity_x == -2 &&
        frame.effects[0].velocity_y == -1);

  /* `$CEEC/$CF1C` stationary lava-mouth tiles share the artwork but are not
   * launched rocks. Resume, motion and flip all remain load-bearing. */
  const size_t address = kActRaiserWram_ActionObjectTable +
      41 * kActRaiserActionObjectStride;
  Write16(wram, address + 0x1E, 0xCF1C);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  SeedAitosMoltenRock(wram, 41, 2092, 786, 2, 1);
  Write16(wram, address + 0x28, 0);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
}

static void SeedAitosSplashPlatform(uint8_t *wram, unsigned world_width,
                                    unsigned x, unsigned y,
                                    unsigned cells) {
  for (unsigned cell = 0; cell < cells; cell++) {
    const bool left = cell == 0;
    const bool right = cell + 1u == cells;
    SeedBgMetatile(wram, world_width, x + cell * 16, y,
                   left ? 0x36 : right ? 0x81 : 0x5E);
    SeedBgMetatile(wram, world_width, x + cell * 16, y + 16,
                   left ? 0x4E : right ? 0x4F : 0xF4);
    SeedBgMetatile(wram, world_width, x + cell * 16, y + 32,
                   left ? 0xF6 : right ? 0xFE : 0xFC);
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
  CHECK(frame.decorations[0].world_x == 928 &&
        frame.decorations[0].world_y == 496);
  CHECK(frame.decorations[0].geometry.data.rect.x0 == -32.0f);
  CHECK(frame.decorations[0].geometry.data.rect.x1 == 32.0f);
  CHECK(frame.decorations[0].projection_plane ==
        kActionEffectProjectionPlane_Bg1);
  CHECK(frame.decorations[1].kind == kActionEffect_AitosWaterfall);
  CHECK(frame.decorations[1].projection_plane ==
        kActionEffectProjectionPlane_Bg2);
  CHECK(frame.decorations[1].render_layer ==
        kActionEffectRenderLayer_Bg2Plane);
  CHECK(frame.decorations[1].world_x == 856 &&
        frame.decorations[1].world_y == 600);
  CHECK(frame.decorations[1].geometry.data.rect.y0 == -176.0f);
  CHECK(frame.decorations[1].geometry.data.rect.y1 == 312.0f);
  CHECK(frame.decorations[2].kind == kActionEffect_AitosWaterfallMist);
  CHECK(frame.decorations[2].projection_plane ==
        kActionEffectProjectionPlane_Bg2);
  CHECK(frame.decorations[2].render_layer ==
        kActionEffectRenderLayer_Atmosphere);
  CHECK(frame.decorations[2].world_x == 856 &&
        frame.decorations[2].world_y == 736);
  CHECK(frame.decorations[2].world_y - 488 ==
        kActRaiserAuthenticHeight +
            kActionBgAitosWaterfallBottomExtensionPixels);
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
  CHECK(first.effects[0].phase ==
        kActionEffectPhase_AitosLavaFireballFlight);
  CHECK(first.effects[0].geometry.data.rect.x0 == -8.0f);
  CHECK(first.effects[0].geometry.data.rect.y0 == -8.0f);
  CHECK(first.effects[0].geometry.data.rect.x1 == 8.0f);
  CHECK(first.effects[0].geometry.data.rect.y1 == 8.0f);

  Write16(wram, kActRaiserWram_ActionObjectTable +
          20 * kActRaiserActionObjectStride + 0x04, 960);
  ActionSceneEffects_CaptureFrame(&observer, &advanced, wram,
                                  sizeof(wram), 1);
  CHECK(advanced.effects[0].generation == first.effects[0].generation);
  CHECK(advanced.effects[0].age_ticks == 1);

  /* The emitter cyclically reuses its slot. A new launch at the pit must not
   * inherit the prior projectile's particle clock. */
  Write16(wram, kActRaiserWram_ActionObjectTable +
          20 * kActRaiserActionObjectStride + 0x04, 1000);
  ActionSceneEffects_CaptureFrame(&observer, &reused, wram,
                                  sizeof(wram), 1);
  CHECK(reused.effects[0].generation != advanced.effects[0].generation);
  CHECK(reused.effects[0].age_ticks == 0);

  static const uint16_t kStates[] = {0x0022, 0x0023, 0x0024};
  for (size_t i = 0; i < sizeof(kStates) / sizeof(kStates[0]); i++) {
    SeedAitosLavaFireball(wram, 20, 3696, 900, kStates[i]);
    ActionSceneEffects_CaptureFrame(&observer, &frame, wram,
                                    sizeof(wram), 1);
    CHECK(frame.effect_count == 1);
    CHECK(frame.effects[0].kind == kActionEffect_AitosLavaFireball);
  }

  const size_t address = kActRaiserWram_ActionObjectTable +
      20 * kActRaiserActionObjectStride;
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

static void SeedAitosStatueFire(uint8_t *wram, unsigned slot,
                                uint16_t source, uint16_t state,
                                uint16_t visual, uint16_t composition) {
  const size_t address = kActRaiserWram_ActionObjectTable +
      slot * kActRaiserActionObjectStride;
  const bool flipped = source == 0xD5C0;
  uint16_t left = 16, right = 16;
  if (visual == 0x0017) left = right = 4;
  else if (visual == 0x001D) right = 32;
  else if (visual == 0x001E || visual == 0x001F) right = 48;
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
  Write16(wram, address + 0x12,
          state == 0x0019 ? 0x8683
                          : (source == 0xD5C0 ? 0xD5CC : 0xD5BD));
  Write16(wram, address + 0x16, 0x4000);
  wram[address + 0x18] = 0x7E;
  Write16(wram, address + 0x1A, state);
  Write16(wram, address + 0x1E, state == 0x0019 ? 0xD5EE : 0x0000);
  Write16(wram, address + 0x20, composition);
  Write16(wram, address + 0x22, visual);
  Write16(wram, address + 0x28,
          flipped ? kActRaiserObjectFlip_Horizontal : 0);
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
    CHECK(frame.effects[i].phase ==
          kActionEffectPhase_AitosStatueFireBreath);
    CHECK(frame.effects[i].obj_priority == 2);
  }
  CHECK(frame.effects[0].geometry.data.rect.x0 == -16.0f);
  CHECK(frame.effects[1].geometry.data.rect.x0 == -48.0f);
  CHECK((frame.effects[1].flags & kActionEffectFlag_FlipHorizontal) != 0);
  CHECK(frame.effects[2].visual == 0x001F);
  CHECK(frame.effects[2].geometry.data.rect.x1 == 48.0f);

  /* Drawing and activation are independent: retain lifecycle identity while
   * $0400 is set, but do not submit the frozen margin actor as a live effect. */
  const size_t first_address = kActRaiserWram_ActionObjectTable +
      34 * kActRaiserActionObjectStride;
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
  return kActRaiserWram_ActionObjectTable +
      slot * kActRaiserActionObjectStride;
}

static void SeedBossFamilyObject(uint8_t *wram, unsigned slot,
                                 uint16_t source, uint16_t composition,
                                 uint16_t visual, uint16_t state,
                                 uint16_t resume, uint16_t flags,
                                 uint16_t backlink) {
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
  CHECK(frame.effects[0].phase ==
        kActionEffectPhase_MarahnaBossLightningCharge);
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
  CHECK(frame.effects[0].phase ==
        kActionEffectPhase_MarahnaBossLightningGroundCharge);

  /* Minotaur axe in Fillmore and the first Death Heim rematch. */
  static const struct { uint8_t group, map; uint16_t source; } kAxeRooms[] = {
    {kActRaiserMapGroup_Fillmore, 4, 0xAF5D},
    {kActRaiserMapGroup_DeathHeim, 2, 0xF6CA},
  };
  for (size_t i = 0; i < sizeof(kAxeRooms) / sizeof(kAxeRooms[0]); i++) {
    memset(wram, 0, sizeof(wram));
    ActionEffectObserver_Reset(&observer);
    wram[kActRaiserWram_MapGroup] = kAxeRooms[i].group;
    wram[kActRaiserWram_CurrentMap] = kAxeRooms[i].map;
    SeedBossFamilyObject(wram, 49, kAxeRooms[i].source, 0x5300,
                         0, 0, 0, 0x4000, 0);
    SeedBossFamilyObject(wram, 11, kAxeRooms[i].source, 0x50FB,
                         0, 3, 0xB008, 0x0020,
                         (uint16_t)BossEffectSlot(49));
    ActionSceneEffects_CaptureFrame(&observer, &frame,
                                    wram, sizeof(wram), 1);
    CHECK(frame.effect_count == 1);
    CHECK(frame.effects[0].kind == kActionEffect_MinotaurAxe);
  }
  Write16(wram, BossEffectSlot(49) + 0x32, 0xAF5D);
  Write16(wram, BossEffectSlot(11) + 0x32, 0xAF5D);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);

  /* Flaming Wheel's body is the source: illuminate it in both boss rooms. */
  static const struct { uint8_t group, map; uint16_t source; } kWheelRooms[] = {
    {kActRaiserMapGroup_Aitos, 7, 0xD838},
    {kActRaiserMapGroup_DeathHeim, 5, 0xF712},
  };
  for (size_t i = 0; i < sizeof(kWheelRooms) / sizeof(kWheelRooms[0]); i++) {
    const uint8_t expected_priority = i == 0 ? 2 : 1;
    memset(wram, 0, sizeof(wram));
    ActionEffectObserver_Reset(&observer);
    wram[kActRaiserWram_MapGroup] = kWheelRooms[i].group;
    wram[kActRaiserWram_CurrentMap] = kWheelRooms[i].map;
    Write16(wram, kActRaiserWram_SpriteAttributeBias,
            (uint16_t)(expected_priority << 12));
    /* Recorded body frame: the wheel uses both repeat and delay handlers over
     * its lifecycle, so ownership—not a transient handler—is its discriminator. */
    SeedBossFamilyObject(wram, 49, kWheelRooms[i].source, 0x5276,
                         0x0005, 7, 0xD85E, 0x4000,
                         kWheelRooms[i].group == kActRaiserMapGroup_DeathHeim
                             ? 0x001C : 0);
    Write16(wram, BossEffectSlot(49) + 0x12, 0x8683);
    ActionSceneEffects_CaptureFrame(&observer, &frame,
                                    wram, sizeof(wram), 1);
    CHECK(frame.effect_count == 1);
    CHECK(frame.effects[0].kind == kActionEffect_FlamingWheel);
    CHECK(frame.effects[0].obj_priority == expected_priority);

    /* snap_05's five cyan shots are exact animation-$5000 children of that
     * root. Pin one direction/frame tuple in both original and rematch rooms. */
    SeedBossFamilyObject(wram, 11, kWheelRooms[i].source, 0x51B5,
                         0x0000, 0x0008, 0xA65D, 0x0020,
                         (uint16_t)BossEffectSlot(49));
    Write16(wram, BossEffectSlot(11) + 0x06, 0xFFFF);
    Write16(wram, BossEffectSlot(11) + 0x08, 0x0001);
    Write16(wram, BossEffectSlot(11) + 0x38, 0x0008);
    Write16(wram, BossEffectSlot(11) + 0x28, 0x4000);
    ActionSceneEffects_CaptureFrame(&observer, &frame,
                                    wram, sizeof(wram), 1);
    CHECK(frame.effect_count == 2);
    CHECK(frame.effects[0].kind == kActionEffect_FlamingWheelProjectile);
    CHECK(frame.effects[0].phase ==
          kActionEffectPhase_FlamingWheelProjectileFlight);
    CHECK(frame.effects[0].obj_priority == expected_priority);
    CHECK(frame.effects[1].obj_priority == expected_priority);
    Write16(wram, BossEffectSlot(11) + 0x06, 0xFFFE);
    ActionSceneEffects_CaptureFrame(&observer, &frame,
                                    wram, sizeof(wram), 1);
    CHECK(frame.effect_count == 1);

    /* The spawn source is shared by boss-family helpers. A visually plausible
     * child must not become a second full-body flame emitter. */
    SeedBossFamilyObject(wram, 12, kWheelRooms[i].source, 0x5276,
                         0x0005, 7, 0xD85E, 0x4000,
                         (uint16_t)BossEffectSlot(49));
    Write16(wram, BossEffectSlot(12) + 0x12, 0x8683);
    ActionSceneEffects_CaptureFrame(&observer, &frame,
                                    wram, sizeof(wram), 1);
    CHECK(frame.effect_count == 1);

    /* Conversely, a body that acquires a child-style backlink is no longer the
     * stable root/room-owned wheel and must fail closed. */
    Write16(wram, BossEffectSlot(49) + 0x3A,
            (uint16_t)BossEffectSlot(12));
    ActionSceneEffects_CaptureFrame(&observer, &frame,
                                    wram, sizeof(wram), 1);
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
      memset(wram + BossEffectSlot(11 + shot), 0,
             kActRaiserActionObjectStride);
    SeedBossFamilyObject(wram, 49, kWheelRooms[i].source, 0x5276,
                         0x0005, 7, 0xD85E, 0x4000,
                         kWheelRooms[i].group ==
                                 kActRaiserMapGroup_DeathHeim
                             ? 0x001C : 0);
    Write16(wram, BossEffectSlot(49) + 0x12, 0x8683);
    for (unsigned shot = 0; shot < 5; shot++) {
      const unsigned slot = 11 + shot;
      SeedBossFamilyObject(wram, slot, kWheelRooms[i].source,
                           kComposition[shot], kVisual[shot], kState[shot],
                           0xA65D, 0x0020,
                           (uint16_t)BossEffectSlot(49));
      Write16(wram, BossEffectSlot(slot) + 0x06,
              (uint16_t)kVelocity[shot][0]);
      Write16(wram, BossEffectSlot(slot) + 0x08,
              (uint16_t)kVelocity[shot][1]);
      Write16(wram, BossEffectSlot(slot) + 0x28, 0x4000);
      Write16(wram, BossEffectSlot(slot) + 0x38, kState[shot]);
    }
    ActionSceneEffects_CaptureFrame(&observer, &frame,
                                    wram, sizeof(wram), 1);
    CHECK(frame.effect_count == 6);
    CHECK(frame.visible_count == 6);
    for (unsigned shot = 0; shot < 5; shot++) {
      CHECK(frame.effects[shot].kind ==
            kActionEffect_FlamingWheelProjectile);
      CHECK(frame.effects[shot].obj_priority == expected_priority);
    }
    CHECK(frame.effects[5].kind == kActionEffect_FlamingWheel);
    CHECK(frame.effects[5].obj_priority == expected_priority);
  }

  /* Ice Dragon balls retain their exact eight-frame artwork in the rematch. */
  static const struct { uint8_t group, map; uint16_t source; } kIceRooms[] = {
    {kActRaiserMapGroup_Northwall, 8, 0xF161},
    {kActRaiserMapGroup_DeathHeim, 7, 0xF760},
  };
  for (size_t i = 0; i < sizeof(kIceRooms) / sizeof(kIceRooms[0]); i++) {
    memset(wram, 0, sizeof(wram));
    ActionEffectObserver_Reset(&observer);
    wram[kActRaiserWram_MapGroup] = kIceRooms[i].group;
    wram[kActRaiserWram_CurrentMap] = kIceRooms[i].map;
    SeedBossFamilyObject(wram, 54, kIceRooms[i].source, 0x5C00,
                         0x0011, 0x000C, 0xF280, 0x0020, 0);
    SeedBossFamilyObject(wram, 11, kIceRooms[i].source, 0x5D9C,
                         0x0012, 0x0019, 0xF2CA, 0x0020,
                         (uint16_t)BossEffectSlot(54));
    ActionSceneEffects_CaptureFrame(&observer, &frame,
                                    wram, sizeof(wram), 1);
    CHECK(frame.effect_count == 1);
    CHECK(frame.effects[0].kind == kActionEffect_IceDragonIceBall);
  }

  /* Tanzara is Death Heim-only and uses several exact projectile families. */
  memset(wram, 0, sizeof(wram));
  ActionEffectObserver_Reset(&observer);
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_DeathHeim;
  wram[kActRaiserWram_CurrentMap] = 8;
  SeedBossFamilyObject(wram, 11, 0xF80F, 0x5D17,
                       0x0016, 0x0008, 0xFD77, 0x0020, 0);
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
    {896,480,4}, {832,496,2}, {1072,496,2}, {1008,512,2},
    {1168,512,2}, {1248,528,2}, {752,544,2}, {1088,576,2},
    {832,592,2}, {1152,608,2}, {752,624,3}, {992,640,2},
    {864,656,5}, {1072,672,3},
  };
  for (size_t i = 0;
       i < sizeof(kMeasuredStructures) / sizeof(kMeasuredStructures[0]); i++)
    SeedAitosSplashPlatform(
        wram, 1792, kMeasuredStructures[i][0], kMeasuredStructures[i][1],
        kMeasuredStructures[i][2]);
  SeedSwordBeam(wram, 0x13, false);
  const size_t first_beam = kActRaiserWram_ActionObjectTable +
      9 * kActRaiserActionObjectStride;
  const size_t second_beam = first_beam + kActRaiserActionObjectStride;
  memcpy(wram + second_beam, wram + first_beam,
         kActRaiserActionObjectStride);
  Write16(wram, second_beam + 0x02, 248);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.decoration_overflow == 0);
  CHECK(frame.decoration_count == 16);
  CHECK(frame.effect_count == 2);
  CHECK(frame.overflow == 0);

  /* A forged fifteenth splash leaves no room for the required paired mist.
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

int main(void) {
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
  if (g_failures) {
    fprintf(stderr, "%d action-effects test(s) failed\\n", g_failures);
    return 1;
  }
  puts("action effects: all tests passed");
  return 0;
}
