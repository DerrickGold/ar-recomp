#include "sim_render_metadata.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser_game.h"
#include "sim_world_map.h"

static int failures;

#define CHECK(expr) do {                                                    \
  if (!(expr)) {                                                            \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    failures++;                                                             \
  }                                                                         \
} while (0)

static void Write16(uint8 *wram, uint32 address, uint16 value) {
  wram[address] = (uint8)value;
  wram[address + 1] = (uint8)(value >> 8);
}

static void Begin(uint16 record, bool world, uint16 composition,
                  uint16 cursor) {
  SimRenderMetadata_BeginRecord(
      record, world, false, composition, 0x0120, 0x00A0,
      world ? 0x13 : 0x02, world ? 7 : 0, 0, cursor);
}

static void TestFeatureDependencies(void) {
  SimRenderFeatureMask all = kSimFeature_All;
  CHECK(Sim3D_ResolveFeatureMask(all, all, kSimView_Enhanced,
                                 true, true) == all);
  CHECK(Sim3D_ResolveFeatureMask(all, 0, kSimView_Enhanced,
                                 true, true) == 0);
  CHECK(Sim3D_ResolveFeatureMask(all, all, kSimView_AuthenticPicker,
                                 true, true) == 0);
  /* Step 3 publishes the full-plane scene, but its presentation feature
   * profile does not become effective until Step 4 adds Palace/UI ownership. */
  CHECK(Sim3D_ResolveFeatureMask(all, all, kSimView_WorldNavigation,
                                 true, true) == 0);
  CHECK(Sim3D_ResolveFeatureMask(all, all, kSimView_Enhanced,
                                 false, true) == 0);

  SimRenderFeatureMask no_billboards =
      all & ~kSimFeature_ObjectBillboards;
  SimRenderFeatureMask resolved = Sim3D_ResolveFeatureMask(
      no_billboards, all, kSimView_Enhanced, true, true);
  CHECK(!(resolved & kSimFeature_VirtualHeight));
  CHECK(!(resolved & kSimFeature_Shadows));
  CHECK(!(resolved & kSimFeature_SoftShadows));
  CHECK(!(resolved & kSimFeature_RimLight));
  CHECK(resolved & kSimFeature_EffectLighting);
  CHECK(resolved & kSimFeature_Particles);
  CHECK(resolved & kSimFeature_GroundProjection);
  CHECK(resolved & kSimFeature_Backdrop);

  /* SoftShadows depends on Shadows and on nothing else. D4b's blur is
   * ordinary blended draws over the mask target, so unlike the original
   * contract it must NOT be cleared for want of a shader — a missing blur
   * target degrades to the hard silhouette at draw time instead. */
  SimRenderFeatureMask no_shadows = all & ~kSimFeature_Shadows;
  resolved = Sim3D_ResolveFeatureMask(
      no_shadows, all, kSimView_Enhanced, true, true);
  CHECK(!(resolved & kSimFeature_SoftShadows));
  resolved = Sim3D_ResolveFeatureMask(
      all, all, kSimView_Enhanced, true, true);
  CHECK(resolved & kSimFeature_Shadows);
  CHECK(resolved & kSimFeature_SoftShadows);

  SimRenderFeatureMask no_ground = all & ~kSimFeature_GroundProjection;
  resolved = Sim3D_ResolveFeatureMask(
      no_ground, all, kSimView_Enhanced, true, true);
  CHECK(!(resolved & kSimFeature_EffectLighting));
  CHECK(!(resolved & kSimFeature_Particles));

  resolved = Sim3D_ResolveFeatureMask(
      all, all, kSimView_Enhanced, true, false);
  CHECK(!(resolved & kSimFeature_EffectLighting));
  CHECK(!(resolved & kSimFeature_Particles));
}

static void TestLightningMiracleEffectCapture(void) {
  uint8 wram[kActRaiserWramSize] = {0};
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  wram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Fillmore;
  Write16(wram, kActRaiserWram_SimMiracleKind, 1);
  Write16(wram, kActRaiserWram_SimUserMiracleActive, 1);
  Write16(wram, kActRaiserWram_SimMiracleVisualComplete, 0x1234);
  Write16(wram, kActRaiserWram_SimMiracleActorDone, 0x5678);

  CHECK(Sim3D_IsLightningMiracleComposition(0xDA4B));
  CHECK(Sim3D_IsLightningMiracleComposition(0xDAA1));
  CHECK(Sim3D_IsLightningMiracleComposition(0xDAF7));
  CHECK(Sim3D_IsLightningMiracleComposition(0xDB5C));
  CHECK(!Sim3D_IsLightningMiracleComposition(0xD9E5));

  static const struct {
    uint16_t composition;
    SimEffectPhase phase;
    uint16_t strike_y;
    uint16_t age_ticks, phase_ticks, pulse_ticks, ticks_since_visible;
    uint32_t pulse_generation;
    bool visible;
  } sequence[] = {
    { 0xD9E5, kSimEffectPhase_LightningCloud,   64, 0, 0, 0, UINT16_MAX, 0, false },
    { 0xDA4B, kSimEffectPhase_LightningLead,    60, 1, 0, 0, 0,          1, true  },
    { 0xDA4B, kSimEffectPhase_LightningLead,    60, 2, 1, 1, 0,          1, true  },
    { 0xDAA1, kSimEffectPhase_LightningBranch,  60, 3, 0, 2, 0,          1, true  },
    { 0xD9E5, kSimEffectPhase_LightningCloud,   64, 4, 0, 3, 1,          1, false },
    { 0xDB5C, kSimEffectPhase_LightningImpactB, 64, 5, 0, 0, 0,          2, true  },
  };

  SimRenderMetadata_Reset();
  SimFrameData frame;
  uint32_t generation = 0;
  for (size_t i = 0; i < sizeof(sequence) / sizeof(sequence[0]); i++) {
    SimRenderMetadata_BeginRecord(
        kActRaiserWram_SimWorldRecords, true, false,
        sequence[i].composition, 152, 112, 0x02, 2, 0, 0);
    SimRenderMetadata_RecordPart(0, 2u << 12);
    SimRenderMetadata_EndRecord(4);
    SimRenderMetadata_CaptureFrame(
        &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
    CHECK(frame.metadata_valid);
    CHECK(frame.effect_metadata_valid);
    CHECK(frame.effect_count == 1);
    CHECK(frame.effect_visible_count == (sequence[i].visible ? 1 : 0));
    CHECK(frame.effect_overflow_count == 0);
    const SimEffectInstance *effect = &frame.effects[0];
    if (!generation) generation = effect->generation;
    CHECK(effect->generation == generation);
    CHECK(effect->pulse_generation == sequence[i].pulse_generation);
    CHECK(effect->age_ticks == sequence[i].age_ticks);
    CHECK(effect->phase_ticks == sequence[i].phase_ticks);
    CHECK(effect->pulse_ticks == sequence[i].pulse_ticks);
    CHECK(effect->ticks_since_visible == sequence[i].ticks_since_visible);
    CHECK(effect->kind == kSimEffect_LightningMiracle);
    CHECK(effect->phase == sequence[i].phase);
    CHECK(effect->record_address == kActRaiserWram_SimWorldRecords);
    CHECK(effect->source_index == 0);
    CHECK(effect->composition == sequence[i].composition);
    CHECK(effect->world_x == 152 && effect->world_y == 112);
    CHECK(effect->geometry.kind == kSimEffectGeometry_Point);
    CHECK(effect->geometry.space == kSimEffectSpace_RecordLocal);
    CHECK(effect->geometry.data.point.x == 8);
    CHECK(effect->geometry.data.point.y == sequence[i].strike_y);
    CHECK(effect->geometry.data.point.height == 0);
    CHECK(((effect->flags & kSimEffectFlag_Visible) != 0) ==
          sequence[i].visible);
    CHECK(effect->flags & kSimEffectFlag_UserLifecycle);
    CHECK(effect->flags & kSimEffectFlag_VisualComplete);
    CHECK(effect->flags & kSimEffectFlag_ActorDone);
  }

  CHECK(!strcmp(Sim3D_EffectKindName(kSimEffect_LightningMiracle),
                "lightning_miracle"));
  CHECK(!strcmp(Sim3D_EffectPhaseName(kSimEffectPhase_LightningImpactB),
                "lightning_impact_b"));
  CHECK(!strcmp(Sim3D_EffectGeometryName(kSimEffectGeometry_Point), "point"));
  CHECK(!strcmp(Sim3D_EffectSpaceName(kSimEffectSpace_RecordLocal),
                "record_local"));

  /* Recapturing one immutable producer build must not advance lifecycle age. */
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.effects[0].age_ticks == 5);
  CHECK(frame.effects[0].pulse_ticks == 0);

  /* Either authentic outer lifecycle is sufficient; kind alone is not. */
  Write16(wram, kActRaiserWram_SimUserMiracleActive, 0);
  Write16(wram, kActRaiserWram_SimPostedMiracleActive, 1);
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.effect_count == 1);
  CHECK(!(frame.effects[0].flags & kSimEffectFlag_UserLifecycle));
  CHECK(frame.effects[0].flags & kSimEffectFlag_PostedLifecycle);
  Write16(wram, kActRaiserWram_SimPostedMiracleActive, 0);
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.effect_count == 0);
  Write16(wram, kActRaiserWram_SimUserMiracleActive, 1);
  Write16(wram, kActRaiserWram_SimMiracleKind, 2);
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.effect_count == 0);

  /* Lifecycle context cannot turn an unrelated composition or class into an
   * emitter. These are separate producer builds so stale tracker state cannot
   * accidentally make the assertion pass. */
  Write16(wram, kActRaiserWram_SimMiracleKind, 1);
  SimRenderMetadata_BeginRecord(
      kActRaiserWram_SimWorldRecords, true, false, 0xDA22,
      152, 112, 0x02, 2, 0, 0);
  SimRenderMetadata_RecordPart(0, 2u << 12);
  SimRenderMetadata_EndRecord(4);
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.effect_count == 0);
  SimRenderMetadata_BeginRecord(
      kActRaiserWram_SimWorldRecords, true, false, 0xDA4B,
      152, 112, 0x03, 2, 0, 0);
  SimRenderMetadata_RecordPart(0, 2u << 12);
  SimRenderMetadata_EndRecord(4);
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.effect_count == 0);

  /* Stopping the outer lifecycle retires identity even if the same immutable
   * producer build is recaptured immediately with the slot reused. */
  Write16(wram, kActRaiserWram_SimMiracleKind, 1);
  SimRenderMetadata_BeginRecord(
      kActRaiserWram_SimWorldRecords, true, false, 0xDA4B,
      152, 112, 0x02, 2, 0, 0);
  SimRenderMetadata_RecordPart(0, 2u << 12);
  SimRenderMetadata_EndRecord(4);
  Write16(wram, kActRaiserWram_SimUserMiracleActive, 0);
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.effect_count == 0);
  Write16(wram, kActRaiserWram_SimUserMiracleActive, 1);
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effects[0].generation != generation);
  CHECK(frame.effects[0].age_ticks == 0);
}

static void TestTownCreationLightningEffectCapture(void) {
  uint8 wram[kActRaiserWramSize] = {0};
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  wram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Fillmore;
  const uint16_t records[] = {
    0x0E02, 0x0E28,
  };
  static const struct {
    uint16_t composition;
    SimEffectPhase phase;
    uint32_t pulse_generation;
    uint16_t phase_ticks;
    uint16_t ticks_since_visible;
    bool visible;
  } sequence[] = {
    { 0xE9CC, kSimEffectPhase_TownCreationBoltA, 1, 0, 0, true },
    { 0xE9CC, kSimEffectPhase_TownCreationBoltA, 1, 1, 0, true },
    { 0xE527, kSimEffectPhase_TownCreationGap,   1, 0, 1, false },
    { 0xE527, kSimEffectPhase_TownCreationGap,   1, 1, 2, false },
    { 0xEA27, kSimEffectPhase_TownCreationBoltB, 2, 0, 0, true },
    { 0xEA27, kSimEffectPhase_TownCreationBoltB, 2, 1, 0, true },
    { 0xE527, kSimEffectPhase_TownCreationGap,   2, 0, 1, false },
    { 0xE527, kSimEffectPhase_TownCreationGap,   2, 1, 2, false },
    { 0xEA82, kSimEffectPhase_TownCreationBoltC, 3, 0, 0, true },
    { 0xEA82, kSimEffectPhase_TownCreationBoltC, 3, 1, 0, true },
    { 0xE527, kSimEffectPhase_TownCreationGap,   3, 0, 1, false },
    { 0xE527, kSimEffectPhase_TownCreationGap,   3, 1, 2, false },
    { 0xEAEC, kSimEffectPhase_TownCreationBoltD, 4, 0, 0, true },
    { 0xEAEC, kSimEffectPhase_TownCreationBoltD, 4, 1, 0, true },
    { 0xE527, kSimEffectPhase_TownCreationGap,   4, 0, 1, false },
    { 0xE527, kSimEffectPhase_TownCreationGap,   4, 1, 2, false },
  };

  SimRenderMetadata_Reset();
  SimFrameData frame;
  uint32_t generations[2] = {0};
  for (size_t tick = 0; tick < sizeof(sequence) / sizeof(sequence[0]); tick++) {
    uint16_t cursor = 0;
    for (size_t strike = 0; strike < 2; strike++) {
      SimRenderMetadata_BeginRecord(
          records[strike], true, false, sequence[tick].composition,
          (uint16_t)(0x0150 + strike * 0x10), 0x0068,
          0x000E, 0, 0, cursor);
      SimRenderMetadata_RecordWord06(0xA8BB);
      SimRenderMetadata_RecordPart(cursor, 2u << 9);
      cursor = (uint16_t)(cursor + 4);
      SimRenderMetadata_EndRecord(cursor);
    }
    SimRenderMetadata_CaptureFrame(
        &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
    CHECK(frame.metadata_valid);
    CHECK(frame.effect_metadata_valid);
    CHECK(frame.effect_count == 2);
    CHECK(frame.effect_visible_count == (sequence[tick].visible ? 2 : 0));
    for (size_t strike = 0; strike < 2; strike++) {
      const SimEffectInstance *effect = &frame.effects[strike];
      if (!generations[strike]) generations[strike] = effect->generation;
      CHECK(effect->generation == generations[strike]);
      CHECK(effect->record_address == records[strike]);
      CHECK(effect->source_index == strike);
      CHECK(frame.sources[strike].tier == kSimRecordTier_World);
      CHECK(frame.sources[strike].type == 0x000E);
      CHECK(frame.sources[strike].record_word06 == 0xA8BB);
      CHECK(effect->kind == kSimEffect_TownCreationLightning);
      CHECK(effect->phase == sequence[tick].phase);
      CHECK(effect->color_family == kSimEffectColor_LightningBlue);
      CHECK(effect->age_ticks == tick);
      CHECK(effect->phase_ticks == sequence[tick].phase_ticks);
      CHECK(effect->pulse_generation == sequence[tick].pulse_generation);
      CHECK(effect->ticks_since_visible ==
            sequence[tick].ticks_since_visible);
      CHECK(effect->geometry.kind == kSimEffectGeometry_Point);
      CHECK(effect->geometry.space == kSimEffectSpace_RecordLocal);
      CHECK(effect->geometry.data.point.x == 8);
      CHECK(effect->geometry.data.point.y == 80);
      CHECK(effect->geometry.data.point.height == 0);
      CHECK(effect->world_x == (uint16_t)(0x0150 + strike * 0x10));
      CHECK(effect->world_y == 0x0068);
      CHECK(effect->flags & kSimEffectFlag_RecordLifecycle);
      CHECK(((effect->flags & kSimEffectFlag_Visible) != 0) ==
            sequence[tick].visible);
    }
    CHECK(generations[0] != generations[1]);
  }

  CHECK(!strcmp(Sim3D_EffectKindName(kSimEffect_TownCreationLightning),
                "town_creation_lightning"));
  CHECK(!strcmp(Sim3D_EffectPhaseName(
                    kSimEffectPhase_TownCreationBoltD),
                "town_creation_bolt_d"));

  /* World tier, process identity, script base, and an exact composition are
   * all required. In particular, $E527 also belongs to cursor lists 40-48. */
  SimRenderMetadata_Reset();
  SimRenderMetadata_BeginRecord(
      records[0], true, false, 0xE9CC, 0x0150, 0x0068,
      0x000D, 0, 0, 0);
  SimRenderMetadata_RecordWord06(0xA8BB);
  SimRenderMetadata_RecordPart(0, 2u << 9);
  SimRenderMetadata_EndRecord(4);
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.effect_count == 0);
  SimRenderMetadata_BeginRecord(
      records[0], true, false, 0xE527, 0x0150, 0x0068,
      0x000E, 0, 0, 0);
  SimRenderMetadata_RecordWord06(0xA840);
  SimRenderMetadata_RecordPart(0, 0);
  SimRenderMetadata_EndRecord(4);
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.effect_count == 0);

  SimRenderMetadata_BeginRecord(
      kActRaiserWram_SimFixedRecords, false, false, 0xE9CC,
      0x0150, 0x0068, 0x000E, 0, 0, 0);
  SimRenderMetadata_RecordWord06(0xA8BB);
  SimRenderMetadata_RecordPart(0, 2u << 9);
  SimRenderMetadata_EndRecord(4);
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.effect_count == 0);
}

static void TestEnemyLightningAndFireEffectCapture(void) {
  uint8 wram[kActRaiserWramSize] = {0};
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  wram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Fillmore;
  SimFrameData frame;
  const uint16_t record = kActRaiserWram_SimWorldRecords;

  #define CAPTURE_EFFECT(type_, state_, composition_, palette_) do {       \
    SimRenderMetadata_BeginRecord(                                        \
        record, true, false, (composition_), 0x00F1, 0x0056,              \
        (type_), (state_), 0, 0);                                         \
    SimRenderMetadata_RecordPart(0, (uint16_t)(palette_) << 9);            \
    SimRenderMetadata_EndRecord(4);                                       \
    SimRenderMetadata_CaptureFrame(                                       \
        &frame, wram, true, false, kSimFeature_All, 0,                    \
        kSimFeature_All);                                                 \
  } while (0)

  SimRenderMetadata_Reset();
  CAPTURE_EFFECT(0x12, 6, 0xE13F, 2);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effect_visible_count == 0);
  CHECK(frame.effects[0].kind == kSimEffect_BlueDragonLightning);
  CHECK(frame.effects[0].color_family == kSimEffectColor_LightningBlue);
  CHECK(frame.effects[0].phase == kSimEffectPhase_BlueDragonAttack);
  CHECK(frame.effects[0].flags & kSimEffectFlag_RecordLifecycle);
  CHECK(!(frame.effects[0].flags & kSimEffectFlag_Visible));
  uint32_t dragon_generation = frame.effects[0].generation;

  CAPTURE_EFFECT(0x12, 6, 0xE1BD, 2);
  CHECK(frame.effects[0].generation == dragon_generation);
  CHECK(frame.effects[0].age_ticks == 1);
  CHECK(frame.effects[0].pulse_generation == 1);
  CHECK(frame.effects[0].pulse_ticks == 0);
  CHECK(frame.effects[0].phase == kSimEffectPhase_BlueDragonBoltA);
  CHECK(frame.effects[0].geometry.data.point.x == 8);
  CHECK(frame.effects[0].geometry.data.point.y == 52);
  CHECK(frame.effects[0].geometry.data.point.height == 0);

  CAPTURE_EFFECT(0x12, 6, 0xE13F, 2);
  CHECK(frame.effects[0].ticks_since_visible == 1);
  CAPTURE_EFFECT(0x12, 6, 0xE209, 2);
  CHECK(frame.effects[0].pulse_generation == 2);
  CHECK(frame.effects[0].phase == kSimEffectPhase_BlueDragonBoltB);
  CAPTURE_EFFECT(0x12, 6, 0xE255, 2);
  CHECK(frame.effects[0].phase == kSimEffectPhase_BlueDragonBoltC);
  CHECK(frame.effects[0].geometry.data.point.y == 56);

  /* The bolt art alone is not a lifecycle: state 6 is the semantic gate. */
  CAPTURE_EFFECT(0x12, 5, 0xE1BD, 2);
  CHECK(frame.effect_count == 0);

  SimRenderMetadata_Reset();
  CAPTURE_EFFECT(0x14, 7, 0xE300, 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effect_visible_count == 0);
  CHECK(frame.effects[0].kind == kSimEffect_RedDemonFire);
  CHECK(frame.effects[0].color_family == kSimEffectColor_FireRed);
  CHECK(frame.effects[0].phase == kSimEffectPhase_RedDemonAttack);
  uint32_t demon_generation = frame.effects[0].generation;

  CAPTURE_EFFECT(0x14, 7, 0xE340, 1);
  CHECK(frame.effects[0].generation == demon_generation);
  CHECK(frame.effects[0].phase == kSimEffectPhase_RedFireSmall);
  CHECK(frame.effects[0].geometry.data.point.y == 18);
  CHECK(frame.effects[0].geometry.data.point.height ==
        kSimVirtualHeight_Flying);
  CAPTURE_EFFECT(0x14, 7, 0xE35A, 1);
  CHECK(frame.effects[0].phase == kSimEffectPhase_RedFireMedium);
  CHECK(frame.effects[0].geometry.data.point.y == 20);
  CAPTURE_EFFECT(0x14, 8, 0xE383, 1);
  CHECK(frame.effects[0].phase == kSimEffectPhase_RedFireLarge);
  CHECK(frame.effects[0].geometry.data.point.y == 22);
  CAPTURE_EFFECT(0x14, 9, 0xE300, 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effect_visible_count == 0);
  CAPTURE_EFFECT(0x14, 10, 0xE383, 1);
  CHECK(frame.effect_count == 0);

  /* Ground fire is composition-owned and deliberately survives a record-class
   * transition. Its colour is not: the runtime-built compositions select OBJ
   * palette 1 for the scripted red blaze and palette 2 for post-Lightning blue
   * fire, even though both families use these exact pointer values. */
  SimRenderMetadata_Reset();
  CAPTURE_EFFECT(0x10, 0, 0xE6CA, 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effect_visible_count == 1);
  CHECK(frame.sources[0].obj_palette_mask == (1u << 1));
  CHECK(frame.effects[0].kind == kSimEffect_GroundFire);
  CHECK(frame.effects[0].phase == kSimEffectPhase_GroundFireA);
  CHECK(frame.effects[0].color_family == kSimEffectColor_FireRed);
  uint32_t fire_generation = frame.effects[0].generation;
  CAPTURE_EFFECT(0x12, 0, 0xE6D0, 1);
  CHECK(frame.effects[0].generation == fire_generation);
  CHECK(frame.effects[0].phase == kSimEffectPhase_GroundFireB);
  CHECK(frame.effects[0].color_family == kSimEffectColor_FireRed);
  CHECK(frame.effects[0].pulse_ticks == 1);
  CAPTURE_EFFECT(0x13, 0, 0xE6D6, 2);
  CHECK(frame.effects[0].generation == fire_generation);
  CHECK(frame.sources[0].obj_palette_mask == (1u << 2));
  CHECK(frame.effects[0].phase == kSimEffectPhase_GroundFireC);
  CHECK(frame.effects[0].color_family == kSimEffectColor_FireBlue);
  CHECK(frame.effects[0].geometry.data.point.x == 8);
  CHECK(frame.effects[0].geometry.data.point.y == 8);
  CHECK(frame.effects[0].geometry.data.point.height == 0);
  CAPTURE_EFFECT(0x13, 0, 0xE6DC, 2);
  CHECK(frame.effect_count == 0);

  /* Run 20260803-130945 identified the scripted house blaze as three
   * world-tier $0A01 records on the dedicated $A838 loop. Its 16x16 art is
   * grounded at local (8,16), and the phase loop must remain one continuous
   * emitter rather than respawning particles every source-frame change. */
  SimRenderMetadata_Reset();
  CAPTURE_EFFECT(0x0A01, 2, 0xDD2D, 1);
  CHECK(frame.effect_count == 1);
  CHECK(frame.effect_visible_count == 1);
  CHECK(frame.effects[0].kind == kSimEffect_HouseFire);
  CHECK(frame.effects[0].phase == kSimEffectPhase_HouseFireA);
  CHECK(frame.effects[0].color_family == kSimEffectColor_FireRed);
  CHECK(frame.effects[0].geometry.data.point.x == 8);
  CHECK(frame.effects[0].geometry.data.point.y == 16);
  CHECK(frame.effects[0].geometry.data.point.height == 0);
  uint32_t house_fire_generation = frame.effects[0].generation;
  CAPTURE_EFFECT(0x0A01, 2, 0xDD33, 1);
  CHECK(frame.effects[0].generation == house_fire_generation);
  CHECK(frame.effects[0].phase == kSimEffectPhase_HouseFireB);
  CHECK(frame.effects[0].pulse_generation == 1);
  CHECK(frame.effects[0].pulse_ticks == 1);
  CAPTURE_EFFECT(0x0A01, 2, 0xDD39, 1);
  CHECK(frame.effects[0].phase == kSimEffectPhase_HouseFireC);
  CHECK(frame.effects[0].pulse_ticks == 2);

  /* Reproduce the three simultaneous records in snap_00_gf19950 rather than
   * proving only an isolated synthetic slot. */
  static const uint16_t house_records[] = { 0x0F0C, 0x0F32, 0x0F58 };
  static const uint16_t house_x[] = { 0x00F0, 0x00F0, 0x00C0 };
  static const uint16_t house_y[] = { 0x0080, 0x0090, 0x00B0 };
  SimRenderMetadata_Reset();
  for (size_t i = 0; i < 3; i++) {
    SimRenderMetadata_BeginRecord(
        house_records[i], true, false, 0xDD33, house_x[i], house_y[i],
        0x0A01, 2, 0, (uint16_t)(i * 4));
    SimRenderMetadata_RecordPart((uint16_t)(i * 4), 1u << 9);
    SimRenderMetadata_EndRecord((uint16_t)((i + 1) * 4));
  }
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.effect_count == 3);
  CHECK(frame.effect_visible_count == 3);
  for (size_t i = 0; i < 3; i++) {
    CHECK(frame.effects[i].record_address == house_records[i]);
    CHECK(frame.effects[i].world_x == house_x[i]);
    CHECK(frame.effects[i].world_y == house_y[i]);
    CHECK(frame.effects[i].kind == kSimEffect_HouseFire);
    CHECK(frame.effects[i].phase == kSimEffectPhase_HouseFireB);
  }
  CHECK(frame.effects[0].generation != frame.effects[1].generation);
  CHECK(frame.effects[1].generation != frame.effects[2].generation);

  /* Neither the packed event identity nor neighbouring composition bytes are
   * sufficient on their own. */
  CAPTURE_EFFECT(0x0001, 2, 0xDD2D, 1);
  CHECK(frame.effect_count == 0);
  CAPTURE_EFFECT(0x0A01, 2, 0xDD2E, 1);
  CHECK(frame.effect_count == 0);

  CHECK(!strcmp(Sim3D_EffectKindName(kSimEffect_BlueDragonLightning),
                "blue_dragon_lightning"));
  CHECK(!strcmp(Sim3D_EffectKindName(kSimEffect_RedDemonFire),
                "red_demon_fire"));
  CHECK(!strcmp(Sim3D_EffectKindName(kSimEffect_GroundFire),
                "ground_fire"));
  CHECK(!strcmp(Sim3D_EffectKindName(kSimEffect_HouseFire),
                "house_fire"));
  CHECK(!strcmp(Sim3D_EffectColorName(kSimEffectColor_FireRed),
                "fire_red"));
  CHECK(!strcmp(Sim3D_EffectColorName(kSimEffectColor_FireBlue),
                "fire_blue"));
  CHECK(!strcmp(Sim3D_EffectPhaseName(kSimEffectPhase_RedFireLarge),
                "red_fire_large"));
  CHECK(!strcmp(Sim3D_EffectPhaseName(kSimEffectPhase_HouseFireC),
                "house_fire_c"));

  #undef CAPTURE_EFFECT
}

static void TestEffectOverflowFailsClosed(void) {
  uint8 wram[kActRaiserWramSize] = {0};
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  wram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Fillmore;
  Write16(wram, kActRaiserWram_SimMiracleKind, 1);
  Write16(wram, kActRaiserWram_SimUserMiracleActive, 1);
  SimRenderMetadata_Reset();

  const int emitter_count = kSimMaxEffectInstances + 1;
  for (int i = 0; i < emitter_count; i++) {
    uint16_t record = (uint16_t)(kActRaiserWram_SimWorldRecords +
        i * kActRaiserSimWorldRecordStride);
    uint16_t cursor = (uint16_t)(i * 4);
    SimRenderMetadata_BeginRecord(
        record, true, false, 0xDA4B, (uint16_t)(100 + i), 112,
        0x02, 2, 0, cursor);
    SimRenderMetadata_RecordPart(cursor, 2u << 12);
    SimRenderMetadata_EndRecord((uint16_t)(cursor + 4));
  }

  SimFrameData frame;
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.metadata_valid);
  CHECK(!frame.effect_metadata_valid);
  CHECK(frame.effect_count == kSimMaxEffectInstances);
  CHECK(frame.effect_visible_count == emitter_count);
  CHECK(frame.effect_overflow_count == 1);
  CHECK(!(frame.effective_features & kSimFeature_EffectLighting));
  CHECK(!(frame.effective_features & kSimFeature_Particles));
}

static void TestResolvedPartOverflowFailsClosed(void) {
  uint8 wram[kActRaiserWramSize] = {0};
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  wram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Fillmore;
  SimRenderMetadata_Reset();

  Begin(kActRaiserWram_SimWorldRecords, true, 0xE71B, 0);
  const PpuObjPart synthetic = {
    .x = 258,
    .y = 112,
    .tile_attr = 2u << 12,
    .size = 16,
  };
  for (int i = 0; i <= kSimMaxResolvedParts; i++)
    SimRenderMetadata_RecordSyntheticPart(0, &synthetic);
  SimRenderMetadata_EndRecord(0);

  SimFrameData frame;
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(!frame.metadata_valid);
  CHECK(frame.integrity_flags & kSimMetadataIntegrity_Overflow);
  CHECK(frame.synthetic_part_count == kSimMaxResolvedParts);
  CHECK(frame.synthetic_part_overflow_count == 1);
}

static void TestResolvedPartContractFailsClosed(void) {
  uint8 wram[kActRaiserWramSize] = {0};
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  wram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Fillmore;
  const PpuObjPart part = {
    .x = 258,
    .y = 112,
    .tile_attr = 2u << 12,
    .size = 16,
  };

  SimRenderMetadata_Reset();
  Begin(kActRaiserWram_SimWorldRecords, true, 0xE71B, 0);
  SimRenderMetadata_RecordPart(0, 2u << 12);
  PpuObjPart wrong_priority = part;
  wrong_priority.tile_attr = 3u << 12;
  SimRenderMetadata_RecordExactOamPart(&wrong_priority);
  SimRenderMetadata_EndRecord(4);

  SimFrameData frame;
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(!frame.metadata_valid);
  CHECK(frame.integrity_flags & kSimMetadataIntegrity_PartContract);

  SimRenderMetadata_Reset();
  Begin(kActRaiserWram_SimWorldRecords, true, 0xE71B, 0);
  SimRenderMetadata_RecordSyntheticPart(2, &part);  /* unaligned cursor */
  SimRenderMetadata_EndRecord(0);
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(!frame.metadata_valid);
  CHECK(frame.integrity_flags & kSimMetadataIntegrity_PartContract);
}

static void TestRecordPartitionAndClippedReset(void) {
  uint8 wram[kActRaiserWramSize] = {0};
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  wram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Fillmore;
  Write16(wram, kActRaiserWram_GameFrame, 123);
  Write16(wram, kActRaiserWram_Bg1CameraX, 0x40);
  Write16(wram, kActRaiserWram_Bg1CameraY, 0x60);

  SimRenderMetadata_Reset();

  /* A fully clipped first record leaves cursor zero.  The following higher
   * address at cursor zero belongs to the same build, not a new build. */
  Begin(kActRaiserWram_SimFixedRecords, false, 0xD000, 0);
  SimRenderMetadata_EndRecord(0);

  Begin(kActRaiserWram_SimFixedRecords + kActRaiserSimFixedRecordStride,
        false, 0xD100, 0);
  SimRenderMetadata_RecordPart(0, 1u << 12);
  SimRenderMetadata_EndRecord(4);

  Begin(kActRaiserWram_SimWorldRecords, true, 0xE71B, 4);
  SimRenderMetadata_RecordPart(4, 2u << 12);
  SimRenderMetadata_RecordPart(8, 2u << 12);
  SimRenderMetadata_RecordPart(12, 3u << 12);
  SimRenderMetadata_EndRecord(16);

  SimFrameData frame;
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.view == kSimView_Enhanced);
  CHECK(frame.game_frame == 123);
  CHECK(frame.build_serial == 1);
  CHECK(frame.metadata_valid);
  CHECK(frame.integrity_flags == 0);
  CHECK(frame.source_count == 3);
  CHECK(frame.zero_oam_source_count == 1);
  CHECK(frame.emitted_oam_count == 4);
  CHECK(frame.claimed_oam_count == 4);
  CHECK(frame.object_count == 3);
  CHECK(frame.world_oam_first == 1);
  CHECK(frame.world_oam_count == 3);
  CHECK(frame.sources[0].oam_count == 0);
  CHECK(frame.sources[2].fragment_first == 1);
  CHECK(frame.sources[2].fragment_count == 2);
  CHECK(frame.objects[1].priority == 2);
  CHECK(frame.objects[1].oam_first == 1);
  CHECK(frame.objects[1].oam_count == 2);
  CHECK(frame.objects[2].priority == 3);
  CHECK(!frame.objects[2].atlas_valid);
  CHECK(frame.effective_features == kSimFeature_All);

  SimAtlasBuildInput atlas;
  CHECK(SimRenderMetadata_CopyAtlasInput(&atlas));
  CHECK(atlas.build_serial == frame.build_serial);
  CHECK(atlas.object_count == frame.object_count);
  for (uint16_t i = 0; i < atlas.object_count; i++) {
    atlas.objects[i].local_x0 = -4;
    atlas.objects[i].local_y0 = -8;
    atlas.objects[i].local_x1 = 4;
    atlas.objects[i].local_y1 = 0;
    atlas.objects[i].atlas_x = (uint16_t)(1 + i * 9);
    atlas.objects[i].atlas_y = 1;
    atlas.objects[i].atlas_w = 8;
    atlas.objects[i].atlas_h = 8;
    atlas.objects[i].atlas_valid = 1;
  }
  CHECK(SimRenderMetadata_CommitAtlas(
      atlas.build_serial, atlas.objects, atlas.object_count, true,
      64, 64, 28, 9, 0));
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.atlas_valid);
  CHECK(frame.atlas_width == 64 && frame.atlas_height == 64);
  CHECK(frame.atlas_used_width == 28 && frame.atlas_used_height == 9);
  CHECK(frame.objects[2].atlas_valid);
  CHECK(frame.objects[2].local_x0 == -4);

  Write16(wram, kActRaiserWram_SimMapPickerFlag, 0x0100);
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  /* $7F:9215 is still published either way; only the resulting view depends
   * on the compiled picker policy. */
  CHECK(frame.picker_flag == 0x0100);
#if AR_SIM3D_PICKER_TOPDOWN
  CHECK(frame.view == kSimView_AuthenticPicker);
  CHECK(frame.effective_features == 0);
#else
  CHECK(frame.view == kSimView_Enhanced);
  CHECK(frame.effective_features != 0);
#endif

  /* A fresh zero cursor plus an address restart begins build serial 2. */
  Begin(kActRaiserWram_SimFixedRecords, false, 0xD200, 0);
  SimRenderMetadata_EndRecord(0);
  Write16(wram, kActRaiserWram_SimMapPickerFlag, 0);
  SimRenderMetadata_CaptureFrame(
      &frame, wram, false, false, kSimFeature_SeparatedComposite, 0, 0);
  CHECK(frame.build_serial == 2);
  CHECK(frame.source_count == 1);
  CHECK(frame.zero_oam_source_count == 1);
  CHECK(frame.metadata_valid);
  CHECK(frame.effective_features == 0);
}

static void TestIntegrityFallback(void) {
  uint8 wram[kActRaiserWramSize] = {0};
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  wram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Bloodpool;

  SimRenderMetadata_Reset();
  Begin(kActRaiserWram_SimWorldRecords, true, 0xE000, 0);
  SimRenderMetadata_RecordPart(0, 0);
  SimRenderMetadata_RecordPart(0, 0);  /* duplicate OAM ownership */
  SimRenderMetadata_EndRecord(8);

  SimFrameData frame;
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(!frame.metadata_valid);
  CHECK(frame.integrity_flags & kSimMetadataIntegrity_Overlap);
  /* Broken object metadata costs the sprites, not the view. Dropping the
   * whole frame to the authentic composite meant a one-frame perspective
   * flash -- a far louder artifact than the missing sprite it was hiding --
   * so the enhanced view holds and the resolver clears only the object
   * stages. The ground, projection and world underlay do not depend on the
   * semantic record pass at all. */
  CHECK(frame.view == kSimView_Enhanced);
  CHECK(frame.effective_features ==
        (kSimFeature_SeparatedComposite | kSimFeature_GroundProjection |
         kSimFeature_Backdrop | kSimFeature_PickerExitEase |
         kSimFeature_WorldUnderlay | kSimFeature_CloudShroud |
         kSimFeature_CullHaze));
}

static void TestMapPlaneSelectorTrait(void) {
  uint8 wram[kActRaiserWramSize] = {0};
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  wram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Fillmore;

  SimRenderMetadata_Reset();
  Begin(kActRaiserWram_SimWorldRecords, true, 0xD2C4, 0);
  SimRenderMetadata_RecordPart(0, 2u << 12);
  SimRenderMetadata_EndRecord(4);

  SimFrameData frame;
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.object_count == 1);
  CHECK(frame.objects[0].traits & kSimObjectTrait_MapPlane);
  CHECK(frame.objects[0].traits & kSimObjectTrait_SelectionOverlay);
  CHECK(!(frame.objects[0].traits & kSimObjectTrait_Overhead));

  SimRenderMetadata_Reset();
  Begin(kActRaiserWram_SimWorldRecords, true, 0xD32B, 0);
  SimRenderMetadata_RecordPart(0, 2u << 12);
  SimRenderMetadata_EndRecord(4);
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(!(frame.objects[0].traits & kSimObjectTrait_MapPlane));
  CHECK(!(frame.objects[0].traits & kSimObjectTrait_SelectionOverlay));
}

/* D3c: the locked height policy is a data table, so each documented identity
 * is asserted directly against the pure classifier. */
static void TestVirtualHeightClassification(void) {
  const uint16 world = kActRaiserWram_SimWorldRecords;
  struct {
    const char *label;
    uint8 tier;
    uint16 type, state, record, composition;
    SimHeightClass height_class;
    int16 virtual_height;
    uint8 traits;
  } cases[] = {
    { "person", kSimRecordTier_World, 0x00, 0, world, 0xE85C,
      kSimHeightClass_Grounded, 0, 0 },
    { "people group", kSimRecordTier_World, 0x00, 0, world, 0xE676,
      kSimHeightClass_Grounded, 0, 0 },
    { "horse", kSimRecordTier_World, 0x00, 0, world, 0xE940,
      kSimHeightClass_Grounded, 0, 0 },
    { "boat", kSimRecordTier_World, 0x00, 0, world, 0xE9B4,
      kSimHeightClass_WaterPlane, 0,
      kSimObjectTrait_WaterPlane | kSimObjectTrait_NoShadow },
    { "blue dragon", kSimRecordTier_World, 0x12, 3, world, 0xE0A0,
      kSimHeightClass_Flying, kSimVirtualHeight_Flying,
      kSimObjectTrait_Overhead },
    { "napper bat", kSimRecordTier_World, 0x13, 5, world, 0xE500,
      kSimHeightClass_Flying, kSimVirtualHeight_Flying,
      kSimObjectTrait_Overhead },
    { "red demon", kSimRecordTier_World, 0x14, 1, world, 0xE300,
      kSimHeightClass_Flying, kSimVirtualHeight_Flying,
      kSimObjectTrait_Overhead },
    { "skull head", kSimRecordTier_World, 0x15, 1, world, 0xE400,
      kSimHeightClass_Flying, kSimVirtualHeight_Flying,
      kSimObjectTrait_Overhead },
    { "angel record", kSimRecordTier_World, 0x0C, 0,
      kActRaiserWram_SimAngelRecord, 0xA627,
      kSimHeightClass_Flying, kSimVirtualHeight_Flying,
      kSimObjectTrait_Overhead },
    /* Miracle cloud family: the art spans cloud to ground, so it stays on the
     * map plane with the ROM's own anchor and shadow -- and is Overhead, so
     * D3b's row sort cannot let a nearer tree draw over a cloud. */
    { "miracle cloud", kSimRecordTier_World, 0x02, 1, world, 0xD9E5,
      kSimHeightClass_GroundEffect, 0,
      kSimObjectTrait_RecordOriginAnchor | kSimObjectTrait_NoShadow |
      kSimObjectTrait_Overhead },
    { "miracle bolt", kSimRecordTier_World, 0x02, 2, world, 0xDA4B,
      kSimHeightClass_GroundEffect, 0,
      kSimObjectTrait_RecordOriginAnchor | kSimObjectTrait_NoShadow |
      kSimObjectTrait_Overhead },
    { "miracle rain", kSimRecordTier_World, 0x03, 2, world, 0xDC77,
      kSimHeightClass_GroundEffect, 0,
      kSimObjectTrait_RecordOriginAnchor | kSimObjectTrait_NoShadow |
      kSimObjectTrait_Overhead },
    /* The one member of the range that must NOT come forward: it is the ROM's
     * own shadow ellipse, drawn 40-72px below the shared anchor. It lies on
     * the ground and anything standing there should occlude it. */
    { "miracle cloud shadow", kSimRecordTier_World, 0x08, 1, world, 0xDA22,
      kSimHeightClass_GroundEffect, 0,
      kSimObjectTrait_RecordOriginAnchor | kSimObjectTrait_NoShadow },
    /* The angel's pose frames are borrowed by miracle effect records; only
     * the angel's own record/class may claim the flight plane. */
    { "borrowed angel pose", kSimRecordTier_World, 0x04, 0,
      kActRaiserWram_SimWorldRecords + kActRaiserSimWorldRecordStride, 0xA627,
      kSimHeightClass_Grounded, 0, 0 },
    { "arrow record", kSimRecordTier_World, 0x00, 0,
      kActRaiserWram_SimAngelArrowRecord, 0xD967,
      kSimHeightClass_FlyingProjectile, kSimVirtualHeight_Flying,
      kSimObjectTrait_RecordOriginAnchor | kSimObjectTrait_NoShadow },
    { "arrow composition", kSimRecordTier_World, 0x14, 2, world, 0xD988,
      kSimHeightClass_FlyingProjectile, kSimVirtualHeight_Flying,
      kSimObjectTrait_RecordOriginAnchor | kSimObjectTrait_NoShadow },
    { "building zap", kSimRecordTier_World, 0x12, 9, world, 0xE209,
      kSimHeightClass_GroundEffect, 0,
      kSimObjectTrait_RecordOriginAnchor | kSimObjectTrait_NoShadow },
    { "town creation bolt", kSimRecordTier_World, 0x000E, 0,
      0x0E02, 0xEA82,
      kSimHeightClass_GroundEffect, 0,
      kSimObjectTrait_RecordOriginAnchor | kSimObjectTrait_NoShadow },
    { "town creation range interior", kSimRecordTier_World, 0x19, 0,
      world, 0xEA00, kSimHeightClass_Grounded, 0, 0 },
    { "ground fire", kSimRecordTier_World, 0x19, 0, world, 0xE6D0,
      kSimHeightClass_GroundEffect, 0, kSimObjectTrait_NoShadow },
    { "scripted house fire", kSimRecordTier_World, 0x0A01, 2, world, 0xDD33,
      kSimHeightClass_GroundEffect, 0, kSimObjectTrait_NoShadow },
    { "napper pluck", kSimRecordTier_World, 0x13, 11, world, 0xE73A,
      kSimHeightClass_SemiGrounded, kSimVirtualHeight_SemiGrounded, 0 },
    { "map cursor", kSimRecordTier_World, 0x11, 0, world, 0xD2C4,
      kSimHeightClass_MapPlane, 0,
      kSimObjectTrait_MapPlane | kSimObjectTrait_NoShadow |
      kSimObjectTrait_SelectionOverlay },
    /* The 64x64 hollow path-selection square arrives on a class-$09 record
     * and sits between the cursor family and the miracle cloud effects; it
     * must lie on the ground, not billboard. */
    { "path select square", kSimRecordTier_World, 0x09, 1, world, 0xD993,
      kSimHeightClass_MapPlane, 0,
      kSimObjectTrait_MapPlane | kSimObjectTrait_NoShadow |
      kSimObjectTrait_SelectionOverlay },
    { "fixed UI", kSimRecordTier_Fixed, 0x02, 0,
      kActRaiserWram_SimFixedRecords, 0xD32B,
      kSimHeightClass_None, 0, 0 },
  };

  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    SimObjectClassification result = Sim3D_ClassifyObject(
        cases[i].tier, cases[i].type, cases[i].state, cases[i].record,
        cases[i].composition);
    if (result.height_class != cases[i].height_class ||
        result.virtual_height != cases[i].virtual_height ||
        result.traits != cases[i].traits) {
      fprintf(stderr,
              "%s:%d: %s classified as %s/%d/$%02X, expected %s/%d/$%02X\n",
              __FILE__, __LINE__, cases[i].label,
              Sim3D_HeightClassName((SimHeightClass)result.height_class),
              (int)result.virtual_height, (unsigned)result.traits,
              Sim3D_HeightClassName(cases[i].height_class),
              (int)cases[i].virtual_height, (unsigned)cases[i].traits);
      failures++;
    }
    /* Only classified flight planes may lift, and a lifted object is never
     * simultaneously painted onto the map plane. */
    CHECK(result.virtual_height >= 0);
    CHECK(!result.virtual_height ||
          !(result.traits & kSimObjectTrait_MapPlane));
    CHECK(result.height_class != kSimHeightClass_Grounded ||
          result.virtual_height == 0);
  }

  /* The producer must publish the classification with the fragment. */
  uint8 wram[kActRaiserWramSize] = {0};
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  wram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Fillmore;
  SimRenderMetadata_Reset();
  /* Begin() reports record type $13 for world records. */
  Begin(kActRaiserWram_SimWorldRecords, true, 0xE500, 0);
  SimRenderMetadata_RecordPart(0, 0);
  SimRenderMetadata_EndRecord(4);
  SimFrameData frame;
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.object_count == 1);
  CHECK(frame.objects[0].height_class == kSimHeightClass_Flying);
  CHECK(frame.objects[0].virtual_height == kSimVirtualHeight_Flying);
}

/* The Blue Dragon's strike state lowers the body onto the bolt's plane, and
 * the bolt itself keeps its ROM-positioned record-origin anchor. */
static void TestGroundStrikeOverride(void) {
  const uint16 world = kActRaiserWram_SimWorldRecords;
  SimObjectClassification cruising = Sim3D_ClassifyObject(
      kSimRecordTier_World, 0x12, 1, world, 0xE13F);
  CHECK(cruising.height_class == kSimHeightClass_Flying);
  CHECK(cruising.virtual_height == kSimVirtualHeight_Flying);

  SimObjectClassification striking = Sim3D_ClassifyObject(
      kSimRecordTier_World, 0x12, 6, world, 0xE13F);
  CHECK(striking.height_class == kSimHeightClass_GroundStrike);
  CHECK(striking.virtual_height == 0);
  CHECK(Sim3D_HeightClassIsContactExact(kSimHeightClass_GroundStrike));

  SimObjectClassification bolt = Sim3D_ClassifyObject(
      kSimRecordTier_World, 0x12, 6, world, 0xE1BD);
  CHECK(bolt.height_class == kSimHeightClass_GroundEffect);
  CHECK(bolt.virtual_height == 0);
  CHECK(bolt.traits & kSimObjectTrait_RecordOriginAnchor);
  CHECK(Sim3D_HeightClassIsContactExact(kSimHeightClass_GroundEffect));

  /* Only the Blue Dragon's state 6 is proven; other enemies keep flying. */
  CHECK(Sim3D_ClassifyObject(kSimRecordTier_World, 0x13, 6, world, 0xE3FA)
            .height_class == kSimHeightClass_Flying);
}

/* Height easing runs only for an enhanced 3D frame, ramps continuous records,
 * and snaps everywhere a stale ramp would be wrong. */
static void TestHeightSlew(void) {
  uint8 wram[kActRaiserWramSize] = {0};
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  wram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Fillmore;
  SimFrameData frame;

  /* One world record whose composition selects the requested plane. */
  #define BUILD(composition, state, master, picker) do {                     \
    Write16(wram, kActRaiserWram_SimMapPickerFlag, (picker));                \
    SimRenderMetadata_BeginRecord(                                          \
        kActRaiserWram_SimWorldRecords, true, false, (composition),         \
        0x0120, 0x00A0, 0x13, (state), 0, 0);                               \
    SimRenderMetadata_RecordPart(0, 0);                                     \
    SimRenderMetadata_EndRecord(4);                                         \
    SimRenderMetadata_CaptureFrame(                                         \
        &frame, wram, (master), false, kSimFeature_All, 0,                  \
        kSimFeature_All);                                                   \
  } while (0)

  SimRenderMetadata_Reset();
  /* First enhanced frame snaps to the classified flight plane. */
  BUILD(0xE3FA, 5, true, 0);
  CHECK(frame.view == kSimView_Enhanced);
  CHECK(frame.objects[0].virtual_height == kSimVirtualHeight_Flying);

  /* Entering the pluck phase ramps 24 -> 8 instead of teleporting. */
  BUILD(0xE71B, 5, true, 0);
  CHECK(frame.objects[0].virtual_height == 20);
  BUILD(0xE71B, 5, true, 0);
  CHECK(frame.objects[0].virtual_height == 16);
  BUILD(0xE71B, 5, true, 0);
  CHECK(frame.objects[0].virtual_height == 12);
  BUILD(0xE71B, 5, true, 0);
  CHECK(frame.objects[0].virtual_height == kSimVirtualHeight_SemiGrounded);
  /* The ramp stops on the classified plane and never overshoots. */
  BUILD(0xE71B, 5, true, 0);
  CHECK(frame.objects[0].virtual_height == kSimVirtualHeight_SemiGrounded);
  /* Leaving it eases back up. */
  BUILD(0xE3FA, 5, true, 0);
  CHECK(frame.objects[0].virtual_height == 12);

  /* A contact-critical class lands exactly on its first frame. */
  SimRenderMetadata_Reset();
  BUILD(0xE3FA, 5, true, 0);
  CHECK(frame.objects[0].virtual_height == kSimVirtualHeight_Flying);
  BUILD(0xE6D0, 0, true, 0);
  CHECK(frame.objects[0].virtual_height == 0);

  /* With the SIM 3D master off, the classified plane is published unchanged
   * and no easing state survives to replay when it is switched back on. */
  SimRenderMetadata_Reset();
  BUILD(0xE71B, 5, false, 0);
  CHECK(frame.objects[0].virtual_height == kSimVirtualHeight_SemiGrounded);
  BUILD(0xE3FA, 5, true, 0);
  CHECK(frame.objects[0].virtual_height == kSimVirtualHeight_Flying);

#if AR_SIM3D_PICKER_TOPDOWN
  /* A picker frame is authentic top-down, so the following enhanced frame
   * must be immediately correct rather than ramping out of the picker. */
  BUILD(0xE71B, 5, true, 1);
  CHECK(frame.view == kSimView_AuthenticPicker);
  BUILD(0xE71B, 5, true, 0);
  CHECK(frame.objects[0].virtual_height == kSimVirtualHeight_SemiGrounded);
#else
  /* With the top-down switch compiled out the picker keeps the projected
   * view, so easing continues across it instead of resetting. */
  BUILD(0xE3FA, 5, true, 0);
  CHECK(frame.view == kSimView_Enhanced);
  CHECK(frame.objects[0].virtual_height == kSimVirtualHeight_Flying);
  BUILD(0xE71B, 5, true, 1);
  CHECK(frame.view == kSimView_Enhanced);
  CHECK(frame.objects[0].virtual_height == 20);
  BUILD(0xE71B, 5, true, 1);
  CHECK(frame.objects[0].virtual_height == 16);
#endif
  #undef BUILD
}

static void TestObjColorMathPartition(void) {
  uint8 wram[kActRaiserWramSize] = {0};
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  wram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Fillmore;

  SimRenderMetadata_Reset();
  Begin(kActRaiserWram_SimWorldRecords, true, 0xE000, 0);
  SimRenderMetadata_RecordPart(0, 2u << 12);            /* palette 0 */
  SimRenderMetadata_RecordPart(4, (2u << 12) | 0x0800); /* palette 4 */
  SimRenderMetadata_RecordPart(8, (2u << 12) | 0x0a00); /* palette 5 */
  SimRenderMetadata_RecordPart(12, 2u << 12);           /* palette 0 */
  SimRenderMetadata_EndRecord(16);

  SimFrameData frame;
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.object_count == 3);
  CHECK(!frame.objects[0].color_math_eligible);
  CHECK(frame.objects[1].color_math_eligible);
  CHECK(frame.objects[1].oam_count == 2);
  CHECK(!frame.objects[2].color_math_eligible);
}

static void TestAtlasFailureFallback(void) {
  uint8 wram[kActRaiserWramSize] = {0};
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  wram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Aitos;

  SimRenderMetadata_Reset();
  Begin(kActRaiserWram_SimWorldRecords, true, 0xE000, 0);
  SimRenderMetadata_RecordPart(0, 1u << 12);
  SimRenderMetadata_RecordPart(4, 2u << 12);
  SimRenderMetadata_EndRecord(8);

  SimAtlasBuildInput atlas;
  CHECK(SimRenderMetadata_CopyAtlasInput(&atlas));
  CHECK(atlas.object_count == 2);
  /* A failure reported by the sole atlas builder fails the atlas closed. */
  CHECK(SimRenderMetadata_CommitAtlas(
      atlas.build_serial, atlas.objects, atlas.object_count, false,
      64, 64, 0, 0, kSimMetadataIntegrity_AtlasRasterFailure));

  SimFrameData frame;
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(!frame.atlas_valid);
  CHECK(!frame.metadata_valid);
  CHECK(frame.integrity_flags & kSimMetadataIntegrity_AtlasRasterFailure);
  /* Same rule for a failed atlas: the ground keeps its perspective. */
  CHECK(frame.view == kSimView_Enhanced);
  CHECK(!(frame.effective_features & kSimFeature_ObjectBillboards));
  CHECK(frame.effective_features & kSimFeature_GroundProjection);
  CHECK(!frame.objects[0].atlas_valid && !frame.objects[1].atlas_valid);
}

/* Cloud shroud coverage. The clear rectangle is the sprite-drawable area, so
 * getting this wrong either veils actors the player can see or -- the failure
 * that prompted the inset -- leaves a visibly clear band in which sprites are
 * already being culled, so actors vanish into nothing. */
static void TestCloudCoverage(void) {
  const float x0 = 100, x1 = 300, y0 = 0, y1 = 224;
  const float inset = 32, falloff = 64;

  /* The edge itself must already be substantially covered, not at the foot of
   * the ramp -- otherwise there is a band that culls sprites while still
   * looking like clear sky. */
  float at_edge = Sim3D_CloudCoverage(x0, 100, x0, x1, y0, y1, inset, falloff);
  CHECK(at_edge > 0.30f && at_edge < 0.36f);   /* inset / (inset + falloff) */
  /* Full cover once past the falloff, and it saturates rather than growing. */
  CHECK(Sim3D_CloudCoverage(x0 - falloff, 100, x0, x1, y0, y1, inset,
                            falloff) == 1.0f);
  CHECK(Sim3D_CloudCoverage(x0 - 500, 100, x0, x1, y0, y1, inset,
                            falloff) == 1.0f);
  /* Clear well inside, so the playable centre is never veiled. */
  CHECK(Sim3D_CloudCoverage(200, 112, x0, x1, y0, y1, inset, falloff) == 0.0f);
  /* Monotonic outward. */
  float a = Sim3D_CloudCoverage(x0 + 16, 100, x0, x1, y0, y1, inset, falloff);
  float b = Sim3D_CloudCoverage(x0 - 20, 100, x0, x1, y0, y1, inset, falloff);
  CHECK(a > 0.0f && a < at_edge && b > at_edge);

  /* Symmetric on every edge -- OAM cannot place a sprite above or below the
   * window either -- and a corner takes the larger axis, so it is never
   * thinner than the edges meeting there. */
  CHECK(Sim3D_CloudCoverage(x1, 100, x0, x1, y0, y1, inset, falloff)
        == at_edge);
  CHECK(Sim3D_CloudCoverage(200, y0, x0, x1, y0, y1, inset, falloff)
        == at_edge);
  CHECK(Sim3D_CloudCoverage(200, y1, x0, x1, y0, y1, inset, falloff)
        == at_edge);
  CHECK(Sim3D_CloudCoverage(x0, y0, x0, x1, y0, y1, inset, falloff)
        == at_edge);

  /* The inset is charged against BOTH sides of each axis, and the vertical
   * extent is much the shorter, so an unclamped inset would meet itself in
   * the middle and veil the playable centre. A huge inset must still leave
   * the centre clear. */
  CHECK(Sim3D_CloudCoverage(200, 112, x0, x1, y0, y1, 5000.0f, falloff)
        == 0.0f);
  CHECK(Sim3D_CloudCoverage(200, 112, x0, x1, y0, y1, 100.0f, falloff)
        == 0.0f);
  /* A negative inset is treated as none rather than pushing the ramp out. */
  CHECK(Sim3D_CloudCoverage(x0, 100, x0, x1, y0, y1, -50.0f, falloff)
        == 0.0f);

  /* A degenerate ramp is a hard edge, not a division by zero. */
  CHECK(Sim3D_CloudCoverage(200, 112, x0, x1, y0, y1, 0.0f, 0.0f) == 0.0f);
  CHECK(Sim3D_CloudCoverage(x0 - 1, 112, x0, x1, y0, y1, 0.0f, 0.0f) == 1.0f);
}

/* D5a cull lead. The property that matters is directional: cover must reach
 * full strength BEFORE the sprite window rejects a record, never after. */
static void TestCullProximity(void) {
  const int lead = 48, ml = 64, mr = 64, sq = 0, li = 0;
  const int x1 = kSimSpriteWindowBiasedWidth + mr;

  /* Deep inside the window is clear, and the edge itself is already total. */
  CHECK(Sim3D_CullProximity(136, 120, ml, mr, 0, 0, lead, sq, li) == 0.0f);
  CHECK(Sim3D_CullProximity((int16_t)x1, 120, ml, mr, 0, 0, lead, sq, li) == 1.0f);
  CHECK(Sim3D_CullProximity((int16_t)-ml, 120, ml, mr, 0, 0, lead, sq, li) == 1.0f);
  /* Past the edge stays saturated rather than overshooting. */
  CHECK(Sim3D_CullProximity((int16_t)(x1 + 400), 120, ml, mr, 0, 0, lead, sq, li)
        == 1.0f);

  /* The ramp lives entirely inside the window: `lead` px short of the edge is
   * where it starts, and it rises monotonically from there. */
  CHECK(Sim3D_CullProximity((int16_t)(x1 - lead), 120, ml, mr, 0, 0, lead, sq, li)
        == 0.0f);
  float near = Sim3D_CullProximity((int16_t)(x1 - 8), 120, ml, mr, 0, 0, lead, sq, li);
  float far = Sim3D_CullProximity((int16_t)(x1 - 40), 120, ml, mr, 0, 0, lead, sq, li);
  CHECK(near > far && far > 0.0f && near < 1.0f);

  /* With zero vertical margins the authentic top/bottom edges retain their
   * old behavior. */
  CHECK(Sim3D_CullProximity(136, kSimSpriteWindowBiasedHeight, ml, mr, 0, 0, lead,
                            sq, li) == 1.0f);
  CHECK(Sim3D_CullProximity(136, -1, ml, mr, 0, 0, lead, sq, li) == 1.0f);
  /* Exact synthetic parts make top/bottom reach explicit. The same old edge
   * is now inside the clear window, and the cues move to the new boundaries. */
  CHECK(Sim3D_CullProximity(136, kSimSpriteWindowBiasedHeight,
                            ml, mr, 32, 48, lead, sq, li) == 0.0f);
  CHECK(Sim3D_CullProximity(136,
                            kSimSpriteWindowBiasedHeight + 48,
                            ml, mr, 32, 48, lead, sq, li) == 1.0f);
  CHECK(Sim3D_CullProximity(136, -32, ml, mr, 32, 48,
                            lead, sq, li) == 1.0f);

  /* A corner is covered at least as much as either edge meeting there. */
  float corner = Sim3D_CullProximity((int16_t)(x1 - 16), 8, ml, mr, 0, 0, lead, sq, li);
  CHECK(corner >=
        Sim3D_CullProximity((int16_t)(x1 - 16), 120, ml, mr, 0, 0, lead, sq, li));

  /* A degenerate lead is a hard edge, not a division by zero. */
  CHECK(Sim3D_CullProximity(136, 120, ml, mr, 0, 0, 0, sq, li) == 0.0f);
  CHECK(Sim3D_CullProximity((int16_t)x1, 120, ml, mr, 0, 0, 0, sq, li) == 1.0f);

  /* Widening the margins moves the window, so the same record is further
   * from the edge and earns less cover. That is the whole reason the
   * predicate is evaluated against the live margins. */
  float narrow = Sim3D_CullProximity(300, 120, 0, 0, 0, 0, lead, sq, li);
  float wide = Sim3D_CullProximity(300, 120, 64, 64, 0, 0, lead, sq, li);
  CHECK(narrow > wide);
}

/* Corner rounding may only ever ADD cover. The lit region is allowed to be a
 * rounded rectangle for the look of it, but not at the cost of exposing a
 * record the sprite window was about to take away. */
static void TestCullCornerRounding(void) {
  const int lead = 48, ml = 64, mr = 64, radius = 88, li = 0;
  const int x1 = kSimSpriteWindowBiasedWidth + mr;

  /* Flat edges are unmoved: the rounded distance still reads zero exactly on
   * the edge, so the cull boundary itself does not shift. */
  CHECK(Sim3D_CullProximity((int16_t)x1, 120, ml, mr, 0, 0, lead, radius, li) == 1.0f);
  CHECK(Sim3D_CullProximity((int16_t)-ml, 120, ml, mr, 0, 0, lead, radius, li) == 1.0f);
  CHECK(Sim3D_CullProximity(136, kSimSpriteWindowBiasedHeight, ml, mr, 0, 0, lead,
                            radius, li) == 1.0f);

  /* The centre stays clear whatever the radius says. */
  CHECK(Sim3D_CullProximity(136, 120, ml, mr, 0, 0, lead, radius, li) == 0.0f);

  /* Diagonals gain cover, which is what rounds the corner inward. */
  for (int inset = 0; inset <= 40; inset += 8) {
    float flat = Sim3D_CullProximity((int16_t)(x1 - inset),
                                     (int16_t)(kSimSpriteWindowBiasedHeight -
                                               inset),
                                     ml, mr, 0, 0, lead, 0, li);
    float round = Sim3D_CullProximity((int16_t)(x1 - inset),
                                      (int16_t)(kSimSpriteWindowBiasedHeight -
                                                inset),
                                      ml, mr, 0, 0, lead, radius, li);
    CHECK(round >= flat);
  }

  /* An absurd radius is clamped to the shorter half-extent rather than
   * collapsing the window or reaching past it. */
  CHECK(Sim3D_CullProximity(136, 120, ml, mr, 0, 0, lead, 100000, li) >= 0.0f);
  CHECK(Sim3D_CullProximity((int16_t)x1, 120, ml, mr, 0, 0, lead, 100000, li) == 1.0f);
  /* Negative is treated as no rounding. */
  CHECK(Sim3D_CullProximity((int16_t)(x1 - 16), 8, ml, mr, 0, 0, lead, -50, li) ==
        Sim3D_CullProximity((int16_t)(x1 - 16), 8, ml, mr, 0, 0, lead, 0, li));
}

/* Cover is only ever created by the sprite window. A record the game itself
 * declined to draw is legitimately absent, and covering it would assert
 * something false about the world. */
static void TestSourceCullCover(void) {
  const int lead = 48, ml = 0, mr = 0, sq = 0, li = 0;
  SimSourceRecord clipping = {
    .tier = kSimRecordTier_World,
    .anchor_valid = 1,
    .anchor_x = (int16_t)(kSimSpriteWindowBiasedWidth - 4),
    .anchor_y = 120,
    .oam_count = 2,
    .clipped_parts = 3,
    .clip_reason = kSimClip_Horizontal,
  };
  CHECK(Sim3D_SourceCullCover(&clipping, ml, mr, 0, 0, lead, sq, li) > 0.9f);

  /* Approaching but not yet clipped still earns cover -- that is the lead. */
  SimSourceRecord approaching = clipping;
  approaching.clipped_parts = 0;
  approaching.clip_reason = 0;
  CHECK(Sim3D_SourceCullCover(&approaching, ml, mr, 0, 0, lead, sq, li) > 0.0f);

  /* No parts and no clipping: the record never asked to be drawn. */
  SimSourceRecord silent = clipping;
  silent.oam_count = 0;
  silent.clipped_parts = 0;
  silent.clip_reason = 0;
  CHECK(Sim3D_SourceCullCover(&silent, ml, mr, 0, 0, lead, sq, li) == 0.0f);

  /* A synthetic-only record is drawable even though it consumed no OAM. */
  SimSourceRecord synthetic = clipping;
  synthetic.oam_count = 0;
  synthetic.clipped_parts = 0;
  synthetic.synthetic_parts = 1;
  CHECK(Sim3D_SourceCullCover(&synthetic, ml, mr, 0, 0,
                              lead, sq, li) > 0.9f);

  /* Fixed-tier furniture is screen space and has no town position. */
  SimSourceRecord fixed = clipping;
  fixed.tier = kSimRecordTier_Fixed;
  CHECK(Sim3D_SourceCullCover(&fixed, ml, mr, 0, 0, lead, sq, li) == 0.0f);

  /* A producer that never supplied an anchor must not be read as one at the
   * origin -- every pre-D5a caller drives BeginRecord without one. */
  SimSourceRecord anchorless = clipping;
  anchorless.anchor_valid = 0;
  CHECK(Sim3D_SourceCullCover(&anchorless, ml, mr, 0, 0, lead, sq, li) == 0.0f);

  CHECK(Sim3D_SourceCullCover(NULL, ml, mr, 0, 0, lead, sq, li) == 0.0f);
}

/* The lit window's bottom inset. It exists so the ground-painted boundary is
 * true for lifted records too, and it must move only that edge. */
static void TestCullLiftInset(void) {
  const int lead = 48, ml = 64, mr = 64, sq = 0;
  const int inset = 24;
  const int bottom = kSimSpriteWindowBiasedHeight;

  /* The bottom edge moves up by the inset: what used to be the boundary is
   * now well past it, and the new boundary sits `inset` rows higher. */
  CHECK(Sim3D_CullProximity(136, (int16_t)(bottom - inset), ml, mr, 0, 0, lead, sq,
                            inset) == 1.0f);
  /* ...and a record that far in was still clear without the inset. */
  CHECK(Sim3D_CullProximity(136, (int16_t)(bottom - inset), ml, mr, 0, 0, lead, sq,
                            0) < 1.0f);

  /* A lifted record culls on its unlifted anchor, so the test that matters is
   * that the anchor's cull row is already fully covered. */
  CHECK(Sim3D_CullProximity(136, (int16_t)bottom, ml, mr, 0, 0, lead, sq, inset)
        == 1.0f);

  /* The TOP edge is untouched -- lift is toward negative y, so that side is
   * already conservative and insetting it would only cost bright area. */
  CHECK(Sim3D_CullProximity(136, 0, ml, mr, 0, 0, lead, sq, inset) ==
        Sim3D_CullProximity(136, 0, ml, mr, 0, 0, lead, sq, 0));
  CHECK(Sim3D_CullProximity(136, (int16_t)(0 + inset), ml, mr, 0, 0, lead, sq,
                            inset) ==
        Sim3D_CullProximity(136, (int16_t)(0 + inset), ml, mr, 0, 0, lead, sq, 0));

  /* Horizontal is untouched too: the lift is vertical. */
  CHECK(Sim3D_CullProximity((int16_t)(kSimSpriteWindowBiasedWidth + mr), 120,
                            ml, mr, 0, 0, lead, sq, inset) == 1.0f);
  CHECK(Sim3D_CullProximity(136, 120, ml, mr, 0, 0, lead, sq, inset) ==
        Sim3D_CullProximity(136, 120, ml, mr, 0, 0, lead, sq, 0));

  /* An absurd inset cannot invert the window or collapse it onto a line. */
  CHECK(Sim3D_CullProximity(136, 120, ml, mr, 0, 0, lead, sq, 100000) >= 0.0f);
  CHECK(Sim3D_CullProximity(136, 120, ml, mr, 0, 0, lead, sq, 100000) <= 1.0f);
  /* Negative is treated as none. */
  CHECK(Sim3D_CullProximity(136, (int16_t)(bottom - 8), ml, mr, 0, 0, lead, sq, -9)
        == Sim3D_CullProximity(136, (int16_t)(bottom - 8), ml, mr, 0, 0, lead, sq,
                               0));
}

/* The inset must be a constant of the classifier, not of the live record
 * list: an inset that tracked whatever happens to be flying would drift the
 * ground fade up and down while nothing on screen moved. */
static void TestMaxDrawLift(void) {
  CHECK(Sim3D_MaxDrawLift(100) == kSimVirtualHeight_Flying);
  CHECK(Sim3D_MaxDrawLift(200) == kSimVirtualHeight_Flying * 2);
  CHECK(Sim3D_MaxDrawLift(0) == 0);

  /* No record may ever be lifted past the inset, or it would cull inside the
   * bright area after all. */
  SimSourceRecord flying = {
    .tier = kSimRecordTier_World,
    .anchor_valid = 1,
    .record_address = kActRaiserWram_SimAngelRecord,
    .type = 0x0C,
    .clipped_parts = 1,
  };
  for (unsigned scale = 50; scale <= 400; scale += 50)
    CHECK(Sim3D_SourceDrawLift(&flying, scale) <= Sim3D_MaxDrawLift(scale));
}

/* Cover timing and cover placement are separate queries. The lift must be
 * available for a record that emitted nothing at all -- that record has no
 * entry in objects[], and it is the one whose placement matters most. */
static void TestSourceDrawLift(void) {
  SimSourceRecord flying = {
    .tier = kSimRecordTier_World,
    .anchor_valid = 1,
    .record_address = kActRaiserWram_SimAngelRecord,
    .type = 0x0C,
    .oam_count = 0,       /* fully culled: no fragment to read a height from */
    .clipped_parts = 4,
  };
  CHECK(Sim3D_SourceDrawLift(&flying, 100) == kSimVirtualHeight_Flying);
  /* The presentation height scale carries through, so cover follows the
   * sprite when the scale is turned up rather than staying at its feet. */
  CHECK(Sim3D_SourceDrawLift(&flying, 200) == kSimVirtualHeight_Flying * 2);
  CHECK(Sim3D_SourceDrawLift(&flying, 0) == 0);

  /* Grounded records are not lifted, so the two queries agree for them and
   * the placement path costs nothing. */
  SimSourceRecord grounded = flying;
  grounded.record_address = kActRaiserWram_SimWorldRecords;
  grounded.type = 0x02;
  CHECK(Sim3D_SourceDrawLift(&grounded, 100) == 0);

  /* Fixed-tier furniture lives in screen space and is never lifted. */
  SimSourceRecord fixed = flying;
  fixed.tier = kSimRecordTier_Fixed;
  CHECK(Sim3D_SourceDrawLift(&fixed, 100) == 0);

  CHECK(Sim3D_SourceDrawLift(NULL, 100) == 0);
}

/* Overhead is a sort trait, and the properties that matter are what it does
 * NOT change. It must not become a height (the cloud family's ground contact
 * is the ROM's), must not resurrect a shadow, and must not spread to either
 * the cloud shadow ellipse or ordinary grounded actors. */
static void TestOverheadTrait(void) {
  const uint16_t sky[] = { 0xD9E5, 0xDA4B, 0xDAA1, 0xDAF7, 0xDB5C,
                           0xDC77, 0xDBC1, 0xDC1C, 0xDCD2 };
  for (unsigned i = 0; i < sizeof(sky) / sizeof(sky[0]); i++) {
    SimObjectClassification c = Sim3D_ClassifyObject(
        kSimRecordTier_World, 0x02, 1, kActRaiserWram_SimWorldRecords, sky[i]);
    CHECK(c.traits & kSimObjectTrait_Overhead);
    /* Still ground-anchored: an overhead sort must not become a lift, or the
     * bolt detaches from the terrain it strikes. */
    CHECK(c.height_class == kSimHeightClass_GroundEffect);
    CHECK(c.virtual_height == 0);
    CHECK(c.traits & kSimObjectTrait_NoShadow);
    CHECK(c.traits & kSimObjectTrait_RecordOriginAnchor);
  }

  SimObjectClassification shadow = Sim3D_ClassifyObject(
      kSimRecordTier_World, 0x08, 1, kActRaiserWram_SimWorldRecords, 0xDA22);
  CHECK(!(shadow.traits & kSimObjectTrait_Overhead));

  /* The player angel and airborne enemies are above voxel terrain, while an
   * ordinary grounded record stays in the terrain depth sort. */
  SimObjectClassification angel = Sim3D_ClassifyObject(
      kSimRecordTier_World, 0x0C, 0,
      kActRaiserWram_SimAngelRecord, 0xA627);
  CHECK(angel.traits & kSimObjectTrait_Overhead);
  CHECK(angel.height_class == kSimHeightClass_Flying);
  CHECK(angel.virtual_height == kSimVirtualHeight_Flying);

  SimObjectClassification grounded = Sim3D_ClassifyObject(
      kSimRecordTier_World, 0x02, 0, kActRaiserWram_SimWorldRecords, 0xE000);
  CHECK(!(grounded.traits & kSimObjectTrait_Overhead));
  SimObjectClassification enemy = Sim3D_ClassifyObject(
      kSimRecordTier_World, 0x12, 0,
      kActRaiserWram_SimWorldRecords, 0xE0A0);
  CHECK(enemy.traits & kSimObjectTrait_Overhead);
  /* A classified contact state must not inherit the airborne ordering. */
  SimObjectClassification striking = Sim3D_ClassifyObject(
      kSimRecordTier_World, 0x12, 6,
      kActRaiserWram_SimWorldRecords, 0xE0A0);
  CHECK(!(striking.traits & kSimObjectTrait_Overhead));
  CHECK(striking.height_class == kSimHeightClass_GroundStrike);
}

/* D4a caster selection is pure data: the shadow pass must never re-derive it
 * from a height test, because grounded actors cast too and lifted effects
 * deliberately do not. */
static void TestShadowCasterSelection(void) {
  SimRenderObject object = {
    .tier = kSimRecordTier_World,
    .atlas_valid = 1,
    .atlas_w = 16, .atlas_h = 24,
    .local_x0 = -8, .local_y0 = -24, .local_x1 = 8, .local_y1 = 0,
  };
  CHECK(Sim3D_ObjectCastsShadow(&object));

  /* A grounded actor still casts: the silhouette simply lands on its feet. */
  object.height_class = kSimHeightClass_Grounded;
  object.virtual_height = 0;
  CHECK(Sim3D_ObjectCastsShadow(&object));

  object.height_class = kSimHeightClass_Flying;
  object.virtual_height = kSimVirtualHeight_Flying;
  CHECK(Sim3D_ObjectCastsShadow(&object));

  /* Every D3c NoShadow class stays out, including the ones that carry a
   * height (the arrow) and the ones that supply their own ROM shadow art
   * (the miracle cloud family). */
  SimRenderObject excluded = object;
  excluded.traits = kSimObjectTrait_NoShadow;
  CHECK(!Sim3D_ObjectCastsShadow(&excluded));
  excluded.traits = kSimObjectTrait_MapPlane;
  CHECK(!Sim3D_ObjectCastsShadow(&excluded));

  /* Fixed-tier UI is screen space and has no ground point at all. */
  SimRenderObject fixed = object;
  fixed.tier = kSimRecordTier_Fixed;
  CHECK(!Sim3D_ObjectCastsShadow(&fixed));

  /* No art, no silhouette: a record dropped from the atlas must leave nothing
   * behind on the ground. */
  SimRenderObject no_atlas = object;
  no_atlas.atlas_valid = 0;
  CHECK(!Sim3D_ObjectCastsShadow(&no_atlas));
  SimRenderObject empty = object;
  empty.atlas_w = 0;
  CHECK(!Sim3D_ObjectCastsShadow(&empty));
  SimRenderObject degenerate = object;
  degenerate.local_x1 = degenerate.local_x0;
  CHECK(!Sim3D_ObjectCastsShadow(&degenerate));
  CHECK(!Sim3D_ObjectCastsShadow(NULL));

  /* Spot-check that the classifier's own output agrees, so the two cannot
   * drift apart: the classified traits are the only input that matters. */
  SimObjectClassification arrow = Sim3D_ClassifyObject(
      kSimRecordTier_World, 0x04, 0, kActRaiserWram_SimAngelArrowRecord,
      0xD967);
  SimRenderObject arrow_object = object;
  arrow_object.traits = arrow.traits;
  CHECK(!Sim3D_ObjectCastsShadow(&arrow_object));

  SimObjectClassification person = Sim3D_ClassifyObject(
      kSimRecordTier_World, 0x02, 0, 0x0B34, 0xE676);
  SimRenderObject person_object = object;
  person_object.traits = person.traits;
  CHECK(Sim3D_ObjectCastsShadow(&person_object));
}

static void MakeDevelopedWorldMapAvailable(void) {
  enum { kSyntheticRomSize = 0x100000 };
  uint8_t *rom = (uint8_t *)calloc(1, kSyntheticRomSize);
  CHECK(rom != NULL);
  if (!rom) return;
  CHECK(SimWorldMap_Init(rom, kSyntheticRomSize));
  const uint8_t *baseline = SimWorldMap_Baseline();
  CHECK(baseline != NULL);
  if (baseline) SimWorldMap_PublishBuiltTilemap(baseline);
  CHECK(SimWorldMap_DevelopedAvailable());
  free(rom);
}

static void CheckSteadyWorldNavigation(const uint8 *wram) {
  SimFrameData frame;
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, true, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.view == kSimView_WorldNavigation);
  CHECK(frame.master_enabled);
  CHECK(frame.town == 0);
  CHECK(frame.world_navigation_state_valid);
  CHECK(frame.world_navigation.focus_x == 0x0300);
  CHECK(frame.world_navigation.focus_y == 0x0200);
  CHECK(frame.camera_x == 0x0280);
  CHECK(frame.camera_y == 0x0190);
  CHECK(frame.world_navigation.matrix[0] == 0x0200);
  CHECK(frame.world_navigation.matrix[1] == 0);
  CHECK(frame.world_navigation.matrix[2] == 0);
  CHECK(frame.world_navigation.matrix[3] == 0x0200);
  CHECK(memcmp(frame.world_navigation.matrix,
               frame.world_navigation.next_matrix,
               sizeof(frame.world_navigation.matrix)) == 0);
  CHECK(frame.world_navigation.rotation == 0);
  CHECK(frame.world_navigation.zoom_current == 0x040A);
  CHECK(frame.world_navigation.zoom_target == 0x040A);
  CHECK(frame.world_navigation.active_location == 1);
  CHECK(frame.underlay_serial != 0);
  CHECK(frame.underlay_origin_tile_x == 0);
  CHECK(frame.underlay_origin_tile_y == 0);
  CHECK(frame.world_navigation_scene.valid);
  CHECK(frame.world_navigation_scene.texture_serial ==
        frame.underlay_serial);
  CHECK(frame.world_navigation_scene.texture_width == 1024);
  CHECK(frame.world_navigation_scene.texture_height == 1024);
  CHECK(frame.world_navigation_scene.tile_width == 128);
  CHECK(frame.world_navigation_scene.tile_height == 128);
  CHECK(frame.world_navigation_scene.ground[0].tile_x == 0);
  CHECK(frame.world_navigation_scene.ground[0].tile_y == 0);
  CHECK(frame.world_navigation_scene.ground[1].tile_x == 128);
  CHECK(frame.world_navigation_scene.ground[1].tile_y == 0);
  CHECK(frame.world_navigation_scene.ground[2].tile_x == 128);
  CHECK(frame.world_navigation_scene.ground[2].tile_y == 128);
  CHECK(frame.world_navigation_scene.ground[3].tile_x == 0);
  CHECK(frame.world_navigation_scene.ground[3].tile_y == 128);
  CHECK(frame.world_navigation_scene.active_location == 1);
  CHECK(frame.world_navigation_scene.active_region_valid);
  CHECK(frame.world_navigation_scene.active_region_x == 640);
  CHECK(frame.world_navigation_scene.active_region_y == 384);
  CHECK(frame.world_navigation_scene.active_region_width == 256);
  CHECK(frame.world_navigation_scene.active_region_height == 256);
  CHECK(SimWorldNavigationScene_LocationHaze(
            &frame.world_navigation_scene, 768.0f, 512.0f, 104.0f) == 0.0f);
  CHECK(SimWorldNavigationScene_LocationHaze(
            &frame.world_navigation_scene, 640.0f, 384.0f, 104.0f) == 0.0f);
  CHECK(SimWorldNavigationScene_LocationHaze(
            &frame.world_navigation_scene, 500.0f, 384.0f, 104.0f) == 1.0f);
  {
    const float halfway = SimWorldNavigationScene_LocationHaze(
        &frame.world_navigation_scene, 588.0f, 512.0f, 104.0f);
    CHECK(halfway > 0.49f && halfway < 0.51f);
  }
  float screen_x = 0.0f, screen_y = 0.0f;
  CHECK(SimWorldNavigationScene_ProjectSource(
      &frame.world_navigation_scene, 768.0f, 512.0f,
      &screen_x, &screen_y));
  CHECK(fabsf(screen_x - 128.0f) < 0.001f);
  CHECK(fabsf(screen_y - 112.0f) < 0.001f);
  /* $0200 is 2.0 source pixels per screen pixel, so the steady visible
   * 256x224 window covers exactly 512x448 source pixels. */
  CHECK(SimWorldNavigationScene_ProjectSource(
      &frame.world_navigation_scene, 512.0f, 288.0f,
      &screen_x, &screen_y));
  CHECK(fabsf(screen_x - 0.0f) < 0.001f);
  CHECK(fabsf(screen_y - 0.0f) < 0.001f);
  CHECK(frame.object_count == 0);
  CHECK(frame.source_count == 0);
  CHECK(!frame.metadata_valid);
  CHECK(frame.effective_features == 0);
}

static void CheckAnimatedWorldNavigation(const uint8 *wram) {
  SimFrameData frame;
  SimRenderMetadata_CaptureFrame(
      &frame, wram, false, true, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.view == kSimView_WorldNavigation);
  CHECK(frame.world_navigation_state_valid);
  CHECK(frame.world_navigation.focus_x == 0x0348);
  CHECK(frame.world_navigation.focus_y == 0x0238);
  CHECK(frame.camera_x == 0x02C8);
  CHECK(frame.camera_y == 0x01C8);
  CHECK(frame.world_navigation.matrix[0] == (int16_t)0x00BC);
  CHECK(frame.world_navigation.matrix[1] == (int16_t)0x026C);
  CHECK(frame.world_navigation.matrix[2] == (int16_t)0xFD93);
  CHECK(frame.world_navigation.matrix[3] == (int16_t)0x00BC);
  CHECK(memcmp(frame.world_navigation.matrix,
               frame.world_navigation.next_matrix,
               sizeof(frame.world_navigation.matrix)) == 0);
  CHECK(frame.world_navigation.rotation == 0x0034);
  CHECK(frame.world_navigation.zoom_current == 0x0516);
  CHECK(frame.world_navigation.zoom_target == 0x040A);
  CHECK(frame.world_navigation.active_location == 1);
  CHECK(frame.world_navigation_scene.valid);
  float screen_x = 0.0f, screen_y = 0.0f;
  CHECK(SimWorldNavigationScene_ProjectSource(
      &frame.world_navigation_scene, 840.0f, 568.0f,
      &screen_x, &screen_y));
  CHECK(fabsf(screen_x - 128.0f) < 0.001f);
  CHECK(fabsf(screen_y - 112.0f) < 0.001f);

  /* Feed one authentic screen point through the captured Mode-7 matrix, then
   * back through the host scene. This pins the sign/order of B and C as well
   * as scale, which a centre-only check cannot do. */
  const float authentic_x = 37.0f, authentic_y = 181.0f;
  const float delta_x = authentic_x - 128.0f;
  const float delta_y = authentic_y - 112.0f;
  const float source_x = 840.0f +
      ((float)(int16_t)0x00BC * delta_x +
       (float)(int16_t)0x026C * delta_y) / 256.0f;
  const float source_y = 568.0f +
      ((float)(int16_t)0xFD93 * delta_x +
       (float)(int16_t)0x00BC * delta_y) / 256.0f;
  CHECK(SimWorldNavigationScene_ProjectSource(
      &frame.world_navigation_scene, source_x, source_y,
      &screen_x, &screen_y));
  CHECK(fabsf(screen_x - authentic_x) < 0.001f);
  CHECK(fabsf(screen_y - authentic_y) < 0.001f);
}

static void PopulateSteadyWorldNavigation(uint8 *wram) {
  memset(wram, 0, kActRaiserWramSize);
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  wram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_WorldMap;
  Write16(wram, kActRaiserWram_Bg1CameraX, 0x0280);
  Write16(wram, kActRaiserWram_Bg1CameraY, 0x0190);
  Write16(wram, kActRaiserWram_WorldFocusX, 0x0300);
  Write16(wram, kActRaiserWram_WorldFocusY, 0x0200);
  Write16(wram, kActRaiserWram_WorldMatrixA, 0x0200);
  Write16(wram, kActRaiserWram_WorldMatrixD, 0x0200);
  Write16(wram, kActRaiserWram_WorldNextMatrixA, 0x0200);
  Write16(wram, kActRaiserWram_WorldNextMatrixD, 0x0200);
  Write16(wram, kActRaiserWram_WorldZoomCurrent, 0x040A);
  Write16(wram, kActRaiserWram_WorldZoomTarget, 0x040A);
  wram[kActRaiserWram_WorldLocation] = 1;
}

static void PopulateAnimatedWorldNavigation(uint8 *wram) {
  PopulateSteadyWorldNavigation(wram);
  Write16(wram, kActRaiserWram_Bg1CameraX, 0x02C8);
  Write16(wram, kActRaiserWram_Bg1CameraY, 0x01C8);
  Write16(wram, kActRaiserWram_WorldFocusX, 0x0348);
  Write16(wram, kActRaiserWram_WorldFocusY, 0x0238);
  const uint16_t matrix[4] = {0x00BC, 0x026C, 0xFD93, 0x00BC};
  for (int i = 0; i < 4; i++) {
    Write16(wram, kActRaiserWram_WorldMatrixA + i * 2, matrix[i]);
    Write16(wram, kActRaiserWram_WorldNextMatrixA + i * 2, matrix[i]);
  }
  Write16(wram, kActRaiserWram_WorldRotation, 0x0034);
  Write16(wram, kActRaiserWram_WorldZoomCurrent, 0x0516);
  Write16(wram, kActRaiserWram_WorldZoomTarget, 0x040A);
}

static void TestWorldNavigationFrameContract(void) {
  uint8 steady[kActRaiserWramSize];
  uint8 animated[kActRaiserWramSize];
  PopulateSteadyWorldNavigation(steady);
  PopulateAnimatedWorldNavigation(animated);
  MakeDevelopedWorldMapAvailable();

  /* The setting is independently off by default. State is still captured for
   * diagnostics, but the authentic renderer remains the selected view. */
  SimFrameData frame;
  SimRenderMetadata_CaptureFrame(
      &frame, steady, true, false, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.view == kSimView_None);
  CHECK(!frame.master_enabled);
  CHECK(frame.world_navigation_state_valid);
  CHECK(!frame.world_navigation_scene.valid);

  CheckSteadyWorldNavigation(steady);
  CheckAnimatedWorldNavigation(animated);
  CHECK(!strcmp(Sim3D_ViewName(kSimView_WorldNavigation),
                "world_navigation"));

  /* The seventh ROM region is Death Heim, outside the six simulation-town
   * origin table. Zero/unknown selector states have no clear-region cutout:
   * presentation keeps the complete world hazed while the scene stays safe. */
  steady[kActRaiserWram_WorldLocation] = 7;
  SimRenderMetadata_CaptureFrame(
      &frame, steady, false, true, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.view == kSimView_WorldNavigation);
  CHECK(frame.world_navigation_scene.active_region_valid);
  CHECK(frame.world_navigation_scene.active_region_x == 640);
  CHECK(frame.world_navigation_scene.active_region_y == 0);
  steady[kActRaiserWram_WorldLocation] = 0;
  SimRenderMetadata_CaptureFrame(
      &frame, steady, false, true, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.view == kSimView_WorldNavigation);
  CHECK(!frame.world_navigation_scene.active_region_valid);
  CHECK(SimWorldNavigationScene_LocationHaze(
            &frame.world_navigation_scene, 768.0f, 512.0f, 104.0f) == 1.0f);
  steady[kActRaiserWram_WorldLocation] = 8;
  SimRenderMetadata_CaptureFrame(
      &frame, steady, false, true, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.view == kSimView_WorldNavigation);
  CHECK(!frame.world_navigation_scene.active_region_valid);
  CHECK(SimWorldNavigationScene_LocationHaze(
            &frame.world_navigation_scene, 0.0f, 0.0f, 104.0f) == 1.0f);
  steady[kActRaiserWram_WorldLocation] = 1;

  /* No complete HLE tilemap means fail closed to authentic Mode 7. */
  SimWorldMap_Shutdown();
  SimRenderMetadata_CaptureFrame(
      &frame, steady, false, true, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.view == kSimView_AuthenticFallback);
  CHECK(frame.underlay_serial == 0);
  CHECK(frame.world_navigation_state_valid);
  CHECK(!frame.world_navigation_scene.valid);

  /* A singular current matrix cannot describe a complete host plane. The
   * dedicated setting remains on, but scene construction fails closed to the
   * authentic renderer for this frame. */
  MakeDevelopedWorldMapAvailable();
  memset(steady + kActRaiserWram_WorldMatrixA, 0, 8);
  SimRenderMetadata_CaptureFrame(
      &frame, steady, false, true, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.view == kSimView_AuthenticFallback);
  CHECK(frame.master_enabled);
  CHECK(!frame.world_navigation_scene.valid);

  steady[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Title;
  SimRenderMetadata_CaptureFrame(
      &frame, steady, false, true, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.view == kSimView_None);
  CHECK(!frame.world_navigation_state_valid);
  CHECK(!frame.world_navigation_scene.valid);
  SimWorldMap_Shutdown();
}

static void TestWorldNavigationCloudCeiling(void) {
  const uint16_t altitude = kSimCloudAltitudeDefaultPx;
  CHECK(SimWorldNavigationScene_CloudVisibility(
            kSimWorldNavigationZoomNear, altitude) == 0.0f);
  CHECK(SimWorldNavigationScene_CloudVisibility(
            kSimWorldNavigationZoomMiddle, altitude) == 1.0f);
  CHECK(SimWorldNavigationScene_CloudVisibility(
            kSimWorldNavigationZoomFar, altitude) == 1.0f);
  CHECK(SimWorldNavigationScene_CloudVisibility(
            kSimWorldNavigationZoomNear, 0) == 1.0f);

  const uint16_t crossing_zoom =
      (uint16_t)(kSimWorldNavigationZoomNear + altitude * 4);
  const float crossing = SimWorldNavigationScene_CloudVisibility(
      crossing_zoom, altitude);
  CHECK(crossing > 0.49f && crossing < 0.51f);
  CHECK(SimWorldNavigationScene_CloudVisibility(
            crossing_zoom - 16, altitude) < crossing);
  CHECK(SimWorldNavigationScene_CloudVisibility(
            crossing_zoom + 16, altitude) > crossing);

  CHECK(SimWorldNavigationScene_MasterFadeAlpha(0) == 255);
  CHECK(SimWorldNavigationScene_MasterFadeAlpha(1) == 238);
  CHECK(SimWorldNavigationScene_MasterFadeAlpha(7) == 136);
  CHECK(SimWorldNavigationScene_MasterFadeAlpha(14) == 17);
  CHECK(SimWorldNavigationScene_MasterFadeAlpha(15) == 0);
  CHECK(SimWorldNavigationScene_MasterFadeAlpha(255) == 0);
}

static void HideAllNavigationOam(uint16_t oam[256]) {
  for (int slot = 0; slot < 128; slot++) {
    oam[slot * 2] = 0xE000;
    oam[slot * 2 + 1] = 0xE000;
  }
}

static void TestWorldNavigationOamClassifier(void) {
  uint16_t oam[256];
  SimWorldNavigationComposition composition;
  HideAllNavigationOam(oam);
  CHECK(SimWorldNavigationScene_ClassifyOam(oam, &composition));
  CHECK(composition.valid);
  CHECK(composition.empty_animation);
  CHECK(!composition.palace.visible);
  CHECK(!composition.ui.visible);

  /* Synthetic copy of gf782's ownership shape: 20 packed priority-3 UI
   * entries, followed by the fixed 3x3 Palace and then hidden OAM. */
  for (int slot = 0; slot < 20; slot++) {
    oam[slot * 2] = (uint16_t)((0x11u << 8) | (uint8_t)(0x20 + slot));
    oam[slot * 2 + 1] = (uint16_t)(0x3000u | (uint8_t)slot);
  }
  static const uint8_t palace_x[9] =
      {104, 120, 136, 104, 120, 136, 104, 120, 136};
  static const uint8_t palace_y[9] =
      {81, 81, 81, 97, 97, 97, 113, 113, 113};
  static const uint8_t palace_tile[9] =
      {0x06, 0x08, 0x0A, 0x0C, 0x0E, 0x26, 0x60, 0x62, 0x64};
  for (int i = 0; i < 9; i++) {
    oam[(20 + i) * 2] =
        (uint16_t)(palace_x[i] | ((uint16_t)palace_y[i] << 8));
    oam[(20 + i) * 2 + 1] =
        (uint16_t)(palace_tile[i] | 0x3200u);
  }
  CHECK(SimWorldNavigationScene_ClassifyOam(oam, &composition));
  CHECK(composition.valid);
  CHECK(!composition.empty_animation);
  CHECK(composition.ui.visible);
  CHECK(composition.ui.oam_first == 0);
  CHECK(composition.ui.oam_count == 20);
  CHECK(composition.palace.visible);
  CHECK(composition.palace.oam_first == 20);
  CHECK(composition.palace.oam_count == 9);

  oam[20 * 2] ^= 1;  /* Palace no longer fills the fixed 3x3 grid. */
  CHECK(!SimWorldNavigationScene_ClassifyOam(oam, &composition));
  oam[20 * 2] ^= 1;
  oam[29 * 2] = 0x1001;  /* Unexpected active OAM after the Palace. */
  CHECK(!SimWorldNavigationScene_ClassifyOam(oam, &composition));
}

static uint8 *ReadWramFixture(const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file) return NULL;
  uint8 *wram = (uint8 *)malloc(kActRaiserWramSize);
  const size_t got = wram ? fread(wram, 1, kActRaiserWramSize, file) : 0;
  const int extra = fgetc(file);
  fclose(file);
  if (!wram || got != kActRaiserWramSize || extra != EOF) {
    free(wram);
    return NULL;
  }
  return wram;
}

static bool ReadOamFixture(const char *path, uint16_t oam[256]) {
  FILE *file = fopen(path, "rb");
  if (!file) return false;
  uint8_t bytes[512];
  const size_t got = fread(bytes, 1, sizeof(bytes), file);
  const int extra = fgetc(file);
  fclose(file);
  if (got != sizeof(bytes) || extra != EOF) return false;
  for (int i = 0; i < 256; i++)
    oam[i] = (uint16_t)(bytes[i * 2] | ((uint16_t)bytes[i * 2 + 1] << 8));
  return true;
}

static void TestCapturedWorldNavigationFixtures(const char *steady_path,
                                                const char *animation_path,
                                                const char *steady_oam_path,
                                                const char *animation_oam_path) {
  uint8 *steady = ReadWramFixture(steady_path);
  uint8 *animation = ReadWramFixture(animation_path);
  CHECK(steady != NULL);
  CHECK(animation != NULL);
  if (!steady || !animation) {
    free(animation);
    free(steady);
    return;
  }
  MakeDevelopedWorldMapAvailable();
  CheckSteadyWorldNavigation(steady);
  CheckAnimatedWorldNavigation(animation);
  if (steady_oam_path && animation_oam_path) {
    uint16_t steady_oam[256], animation_oam[256];
    SimWorldNavigationComposition composition;
    CHECK(ReadOamFixture(steady_oam_path, steady_oam));
    CHECK(ReadOamFixture(animation_oam_path, animation_oam));
    CHECK(SimWorldNavigationScene_ClassifyOam(steady_oam, &composition));
    CHECK(composition.valid && !composition.empty_animation);
    CHECK(composition.ui.oam_first == 0);
    CHECK(composition.ui.oam_count == 20);
    CHECK(composition.palace.oam_first == 20);
    CHECK(composition.palace.oam_count == 9);
    CHECK(SimWorldNavigationScene_ClassifyOam(animation_oam, &composition));
    CHECK(composition.valid && composition.empty_animation);
  }
  SimWorldMap_Shutdown();
  puts("world-navigation fixture metadata: PASS");
  free(animation);
  free(steady);
}

/* The two terrain predicates look alike and answer different questions. They
 * were once one function, and merging them produced two separate regressions:
 * the angel and its arrows snapped upward crossing a peak's cells, and the
 * Napper's pluck and the dragon's building strike -- which dip toward the
 * ground but pass above the roofs they reach over -- were swallowed by
 * buildings. This pins every class in both, so a future edit that
 * re-merges them fails here rather than on screen. */
static void TestTerrainHeightClassPredicates(void) {
  struct { SimHeightClass height_class; bool raised; bool occludable; } cases[] = {
    /* Standing on the ground: terrain raises them AND may hide them. */
    { kSimHeightClass_Grounded,         true,  true  },
    { kSimHeightClass_WaterPlane,       true,  true  },
    /* Dipping toward the ground: terrain raises them, but they are above the
     * roofs they reach over and must stay visible. */
    { kSimHeightClass_GroundEffect,     true,  false },
    { kSimHeightClass_SemiGrounded,     true,  false },
    { kSimHeightClass_GroundStrike,     true,  false },
    /* Absolute altitude above the town: neither raised nor hidden. */
    { kSimHeightClass_Flying,           false, false },
    { kSimHeightClass_FlyingProjectile, false, false },
    { kSimHeightClass_MapPlane,         false, false },
    { kSimHeightClass_None,             false, false },
  };
  bool covered[kSimHeightClass_Count] = {false};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    CHECK(Sim3D_HeightClassStandsOnTerrain(cases[i].height_class) ==
          cases[i].raised);
    CHECK(Sim3D_HeightClassIsOccludable(cases[i].height_class) ==
          cases[i].occludable);
    /* Occludable must imply raised: anything terrain can hide is something
     * terrain also lifts. The converse is the whole point of the split. */
    CHECK(!Sim3D_HeightClassIsOccludable(cases[i].height_class) ||
          Sim3D_HeightClassStandsOnTerrain(cases[i].height_class));
    covered[cases[i].height_class] = true;
  }
  /* A new class must be classified deliberately, not inherit a default. */
  for (int i = 0; i < kSimHeightClass_Count; i++)
    CHECK(covered[i]);
}

int main(int argc, char **argv) {
  TestFeatureDependencies();
  TestLightningMiracleEffectCapture();
  TestTownCreationLightningEffectCapture();
  TestEnemyLightningAndFireEffectCapture();
  TestEffectOverflowFailsClosed();
  TestResolvedPartOverflowFailsClosed();
  TestResolvedPartContractFailsClosed();
  TestRecordPartitionAndClippedReset();
  TestIntegrityFallback();
  TestMapPlaneSelectorTrait();
  TestCloudCoverage();
  TestCullProximity();
  TestCullCornerRounding();
  TestSourceDrawLift();
  TestCullLiftInset();
  TestMaxDrawLift();
  TestOverheadTrait();
  TestSourceCullCover();
  TestVirtualHeightClassification();
  TestTerrainHeightClassPredicates();
  TestGroundStrikeOverride();
  TestHeightSlew();
  TestObjColorMathPartition();
  TestAtlasFailureFallback();
  TestShadowCasterSelection();
  TestWorldNavigationFrameContract();
  TestWorldNavigationCloudCeiling();
  TestWorldNavigationOamClassifier();
  if ((argc == 4 || argc == 6) && strcmp(argv[1], "--fixtures") == 0)
    TestCapturedWorldNavigationFixtures(
        argv[2], argv[3], argc == 6 ? argv[4] : NULL,
        argc == 6 ? argv[5] : NULL);
  else if (argc != 1) {
    fprintf(stderr,
            "usage: %s [--fixtures STEADY_WRAM ANIMATION_WRAM "
            "[STEADY_OAM ANIMATION_OAM]]\n", argv[0]);
    failures++;
  }
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("sim_render_metadata_test: PASS");
  return 0;
}
