#include <stdio.h>
#include <string.h>

#include "action_effects.h"
#include "actraiser_game.h"

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

static void TestLifecycleUsesEmulationTicks(void) {
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
  Write16(wram, beam + 0x02, 224);
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
  CHECK(frame.effects[0].world_x == 224);
  CHECK(frame.effects[0].world_y == 456);
  CHECK(frame.effects[0].velocity_x == 8);
  CHECK(frame.effects[0].left_extent == 0xFFE0);
  CHECK(frame.effects[0].visual == 0x30);
  CHECK(frame.effects[0].composition == 0x99E8);
  CHECK(frame.effects[0].geometry.data.rect.x0 == 0.0f);
  CHECK(frame.effects[0].geometry.data.rect.y0 == -1.0f);
  CHECK(frame.effects[0].geometry.data.rect.x1 == 16.0f);
  CHECK(frame.effects[0].geometry.data.rect.y1 == 31.0f);

  /* The alternate state uses a second visual/composition tuple but emits
   * the same six OAM parts and therefore the same light anchor. */
  SeedSwordBeam(wram, 0x14, false);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].animation_state == 0x14);
  CHECK(frame.effects[0].visual == 0x31);
  CHECK(frame.effects[0].composition == 0x9A17);
  CHECK(frame.effects[0].geometry.data.rect.y0 == -1.0f);
  CHECK(frame.effects[0].geometry.data.rect.y1 == 31.0f);

  SeedSwordBeam(wram, 0x13, true);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].velocity_x == -8);
  CHECK(frame.effects[0].flags & kActionEffectFlag_FlipHorizontal);
  CHECK(frame.effects[0].geometry.data.rect.x0 == 0.0f);
  CHECK(frame.effects[0].geometry.data.rect.x1 == 16.0f);

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
  CHECK(frame.effect_count == 2);
  CHECK(frame.visible_count == 2);
  CHECK(frame.effects[0].kind == kActionEffect_WallTorch);
  CHECK(frame.effects[0].phase == kActionEffectPhase_WallTorch);
  CHECK(frame.effects[0].world_x == 40);
  CHECK(frame.effects[0].world_y == 63);
  CHECK(frame.effects[0].projection_plane ==
        kActionEffectProjectionPlane_Bg1);
  CHECK(frame.effects[0].phase_ticks == 2479);
  CHECK(frame.effects[1].world_x == 72);
  CHECK(frame.effects[1].world_y == 63);
  CHECK(frame.effects[1].phase_ticks == frame.effects[0].phase_ticks);

  /* The same authored pair is present in Bloodpool map 5 and must not be
   * suppressed by a room-number allowlist. */
  wram[kActRaiserWram_CurrentMap] = 5;
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 2);
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Fillmore;
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Bloodpool;
  wram[kActRaiserWram_CurrentMap] = 3;
  wram[0x8000 + 0x30 + 4] = 0;
  wram[0x8000 + 0x40 + 4] = 0;
  wram[0x8000 + 0x40 + 2] = 0x4E;
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.effect_count == 0);
}

static void TestSceneCaptureCapacityFailsClosed(void) {
  uint8_t wram[kActRaiserWramSize];
  ActionSceneEffectFrame frame;
  ActionEffectObserver observer = {0};
  memset(wram, 0, sizeof(wram));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Fillmore;
  for (unsigned slot = 0; slot < kActionSceneEffectMaxInstances + 1; slot++)
    SeedMeasuredSceneObject(wram, slot, false);
  ActionSceneEffects_CaptureFrame(&observer, &frame, wram, sizeof(wram), 1);
  CHECK(frame.overflow != 0);
  CHECK(frame.effect_count == 0);
  CHECK(frame.visible_count == 0);

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
  TestLifecycleUsesEmulationTicks();
  TestMalformedInputsFailClosed();
  TestMeasuredSceneObjectIdentities();
  TestBloodpoolBossLightningIdentity();
  TestSwordBeamIdentityAndAuthoredGeometry();
  TestBloodpoolTorchMetatileIdentity();
  TestSceneCaptureCapacityFailsClosed();
  if (g_failures) {
    fprintf(stderr, "%d action-effects test(s) failed\\n", g_failures);
    return 1;
  }
  puts("action effects: all tests passed");
  return 0;
}
