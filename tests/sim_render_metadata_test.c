#include "sim_render_metadata.h"

#include <stdio.h>

#include "actraiser_game.h"

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
      &frame, wram, true, kSimFeature_All, 0, kSimFeature_All);
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
      &frame, wram, true, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.atlas_valid);
  CHECK(frame.atlas_width == 64 && frame.atlas_height == 64);
  CHECK(frame.atlas_used_width == 28 && frame.atlas_used_height == 9);
  CHECK(frame.objects[2].atlas_valid);
  CHECK(frame.objects[2].local_x0 == -4);

  Write16(wram, kActRaiserWram_SimMapPickerFlag, 0x0100);
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, kSimFeature_All, 0, kSimFeature_All);
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
      &frame, wram, false, kSimFeature_SeparatedComposite, 0, 0);
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
      &frame, wram, true, kSimFeature_All, 0, kSimFeature_All);
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
      &frame, wram, true, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.object_count == 1);
  CHECK(frame.objects[0].traits & kSimObjectTrait_MapPlane);

  SimRenderMetadata_Reset();
  Begin(kActRaiserWram_SimWorldRecords, true, 0xD32B, 0);
  SimRenderMetadata_RecordPart(0, 2u << 12);
  SimRenderMetadata_EndRecord(4);
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, kSimFeature_All, 0, kSimFeature_All);
  CHECK(!(frame.objects[0].traits & kSimObjectTrait_MapPlane));
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
      kSimHeightClass_Flying, kSimVirtualHeight_Flying, 0 },
    { "napper bat", kSimRecordTier_World, 0x13, 5, world, 0xE500,
      kSimHeightClass_Flying, kSimVirtualHeight_Flying, 0 },
    { "red demon", kSimRecordTier_World, 0x14, 1, world, 0xE300,
      kSimHeightClass_Flying, kSimVirtualHeight_Flying, 0 },
    { "skull head", kSimRecordTier_World, 0x15, 1, world, 0xE400,
      kSimHeightClass_Flying, kSimVirtualHeight_Flying, 0 },
    { "angel record", kSimRecordTier_World, 0x0C, 0,
      kActRaiserWram_SimAngelRecord, 0xA627,
      kSimHeightClass_Flying, kSimVirtualHeight_Flying, 0 },
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
    { "struck ground", kSimRecordTier_World, 0x19, 0, world, 0xEA00,
      kSimHeightClass_GroundEffect, 0,
      kSimObjectTrait_RecordOriginAnchor | kSimObjectTrait_NoShadow },
    { "ground fire", kSimRecordTier_World, 0x19, 0, world, 0xE6D0,
      kSimHeightClass_GroundEffect, 0, kSimObjectTrait_NoShadow },
    { "napper pluck", kSimRecordTier_World, 0x13, 11, world, 0xE73A,
      kSimHeightClass_SemiGrounded, kSimVirtualHeight_SemiGrounded, 0 },
    { "map cursor", kSimRecordTier_World, 0x11, 0, world, 0xD2C4,
      kSimHeightClass_MapPlane, 0,
      kSimObjectTrait_MapPlane | kSimObjectTrait_NoShadow },
    /* The 64x64 hollow path-selection square arrives on a class-$09 record
     * and sits between the cursor family and the miracle cloud effects; it
     * must lie on the ground, not billboard. */
    { "path select square", kSimRecordTier_World, 0x09, 1, world, 0xD993,
      kSimHeightClass_MapPlane, 0,
      kSimObjectTrait_MapPlane | kSimObjectTrait_NoShadow },
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
      &frame, wram, true, kSimFeature_All, 0, kSimFeature_All);
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
        &frame, wram, (master), kSimFeature_All, 0,                         \
        kSimFeature_All);                                             \
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
      &frame, wram, true, kSimFeature_All, 0, kSimFeature_All);
  CHECK(frame.object_count == 3);
  CHECK(!frame.objects[0].color_math_eligible);
  CHECK(frame.objects[1].color_math_eligible);
  CHECK(frame.objects[1].oam_count == 2);
  CHECK(!frame.objects[2].color_math_eligible);
}

static void TestAtlasDescriptorFallback(void) {
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
  for (uint16_t i = 0; i < atlas.object_count; i++) {
    atlas.objects[i].local_x0 = atlas.objects[i].local_y0 = 0;
    atlas.objects[i].local_x1 = atlas.objects[i].local_y1 = 8;
    atlas.objects[i].atlas_x = atlas.objects[i].atlas_y = 1;
    atlas.objects[i].atlas_w = atlas.objects[i].atlas_h = 8;
    atlas.objects[i].atlas_valid = 1;
  }
  /* Two descriptors claiming the same atlas pixels fail closed. */
  CHECK(SimRenderMetadata_CommitAtlas(
      atlas.build_serial, atlas.objects, atlas.object_count, true,
      64, 64, 9, 9, 0));

  SimFrameData frame;
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, kSimFeature_All, 0, kSimFeature_All);
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
  CHECK(Sim3D_CullProximity(136, 120, ml, mr, lead, sq, li) == 0.0f);
  CHECK(Sim3D_CullProximity((int16_t)x1, 120, ml, mr, lead, sq, li) == 1.0f);
  CHECK(Sim3D_CullProximity((int16_t)-ml, 120, ml, mr, lead, sq, li) == 1.0f);
  /* Past the edge stays saturated rather than overshooting. */
  CHECK(Sim3D_CullProximity((int16_t)(x1 + 400), 120, ml, mr, lead, sq, li)
        == 1.0f);

  /* The ramp lives entirely inside the window: `lead` px short of the edge is
   * where it starts, and it rises monotonically from there. */
  CHECK(Sim3D_CullProximity((int16_t)(x1 - lead), 120, ml, mr, lead, sq, li)
        == 0.0f);
  float near = Sim3D_CullProximity((int16_t)(x1 - 8), 120, ml, mr, lead, sq, li);
  float far = Sim3D_CullProximity((int16_t)(x1 - 40), 120, ml, mr, lead, sq, li);
  CHECK(near > far && far > 0.0f && near < 1.0f);

  /* Vertical is tested too, and never gains the margins -- OAM's Y byte has
   * no ninth bit, so the emitter cannot widen that axis. */
  CHECK(Sim3D_CullProximity(136, kSimSpriteWindowBiasedHeight, ml, mr, lead,
                            sq, li) == 1.0f);
  CHECK(Sim3D_CullProximity(136, -1, ml, mr, lead, sq, li) == 1.0f);

  /* A corner is covered at least as much as either edge meeting there. */
  float corner = Sim3D_CullProximity((int16_t)(x1 - 16), 8, ml, mr, lead, sq, li);
  CHECK(corner >=
        Sim3D_CullProximity((int16_t)(x1 - 16), 120, ml, mr, lead, sq, li));

  /* A degenerate lead is a hard edge, not a division by zero. */
  CHECK(Sim3D_CullProximity(136, 120, ml, mr, 0, sq, li) == 0.0f);
  CHECK(Sim3D_CullProximity((int16_t)x1, 120, ml, mr, 0, sq, li) == 1.0f);

  /* Widening the margins moves the window, so the same record is further
   * from the edge and earns less cover. That is the whole reason the
   * predicate is evaluated against the live margins. */
  float narrow = Sim3D_CullProximity(300, 120, 0, 0, lead, sq, li);
  float wide = Sim3D_CullProximity(300, 120, 64, 64, lead, sq, li);
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
  CHECK(Sim3D_CullProximity((int16_t)x1, 120, ml, mr, lead, radius, li) == 1.0f);
  CHECK(Sim3D_CullProximity((int16_t)-ml, 120, ml, mr, lead, radius, li) == 1.0f);
  CHECK(Sim3D_CullProximity(136, kSimSpriteWindowBiasedHeight, ml, mr, lead,
                            radius, li) == 1.0f);

  /* The centre stays clear whatever the radius says. */
  CHECK(Sim3D_CullProximity(136, 120, ml, mr, lead, radius, li) == 0.0f);

  /* Diagonals gain cover, which is what rounds the corner inward. */
  for (int inset = 0; inset <= 40; inset += 8) {
    float flat = Sim3D_CullProximity((int16_t)(x1 - inset),
                                     (int16_t)(kSimSpriteWindowBiasedHeight -
                                               inset),
                                     ml, mr, lead, 0, li);
    float round = Sim3D_CullProximity((int16_t)(x1 - inset),
                                      (int16_t)(kSimSpriteWindowBiasedHeight -
                                                inset),
                                      ml, mr, lead, radius, li);
    CHECK(round >= flat);
  }

  /* An absurd radius is clamped to the shorter half-extent rather than
   * collapsing the window or reaching past it. */
  CHECK(Sim3D_CullProximity(136, 120, ml, mr, lead, 100000, li) >= 0.0f);
  CHECK(Sim3D_CullProximity((int16_t)x1, 120, ml, mr, lead, 100000, li) == 1.0f);
  /* Negative is treated as no rounding. */
  CHECK(Sim3D_CullProximity((int16_t)(x1 - 16), 8, ml, mr, lead, -50, li) ==
        Sim3D_CullProximity((int16_t)(x1 - 16), 8, ml, mr, lead, 0, li));
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
  CHECK(Sim3D_SourceCullCover(&clipping, ml, mr, lead, sq, li) > 0.9f);

  /* Approaching but not yet clipped still earns cover -- that is the lead. */
  SimSourceRecord approaching = clipping;
  approaching.clipped_parts = 0;
  approaching.clip_reason = 0;
  CHECK(Sim3D_SourceCullCover(&approaching, ml, mr, lead, sq, li) > 0.0f);

  /* No parts and no clipping: the record never asked to be drawn. */
  SimSourceRecord silent = clipping;
  silent.oam_count = 0;
  silent.clipped_parts = 0;
  silent.clip_reason = 0;
  CHECK(Sim3D_SourceCullCover(&silent, ml, mr, lead, sq, li) == 0.0f);

  /* Fixed-tier furniture is screen space and has no town position. */
  SimSourceRecord fixed = clipping;
  fixed.tier = kSimRecordTier_Fixed;
  CHECK(Sim3D_SourceCullCover(&fixed, ml, mr, lead, sq, li) == 0.0f);

  /* A producer that never supplied an anchor must not be read as one at the
   * origin -- every pre-D5a caller drives BeginRecord without one. */
  SimSourceRecord anchorless = clipping;
  anchorless.anchor_valid = 0;
  CHECK(Sim3D_SourceCullCover(&anchorless, ml, mr, lead, sq, li) == 0.0f);

  CHECK(Sim3D_SourceCullCover(NULL, ml, mr, lead, sq, li) == 0.0f);
}

/* The lit window's bottom inset. It exists so the ground-painted boundary is
 * true for lifted records too, and it must move only that edge. */
static void TestCullLiftInset(void) {
  const int lead = 48, ml = 64, mr = 64, sq = 0;
  const int inset = 24;
  const int bottom = kSimSpriteWindowBiasedHeight;

  /* The bottom edge moves up by the inset: what used to be the boundary is
   * now well past it, and the new boundary sits `inset` rows higher. */
  CHECK(Sim3D_CullProximity(136, (int16_t)(bottom - inset), ml, mr, lead, sq,
                            inset) == 1.0f);
  /* ...and a record that far in was still clear without the inset. */
  CHECK(Sim3D_CullProximity(136, (int16_t)(bottom - inset), ml, mr, lead, sq,
                            0) < 1.0f);

  /* A lifted record culls on its unlifted anchor, so the test that matters is
   * that the anchor's cull row is already fully covered. */
  CHECK(Sim3D_CullProximity(136, (int16_t)bottom, ml, mr, lead, sq, inset)
        == 1.0f);

  /* The TOP edge is untouched -- lift is toward negative y, so that side is
   * already conservative and insetting it would only cost bright area. */
  CHECK(Sim3D_CullProximity(136, 0, ml, mr, lead, sq, inset) ==
        Sim3D_CullProximity(136, 0, ml, mr, lead, sq, 0));
  CHECK(Sim3D_CullProximity(136, (int16_t)(0 + inset), ml, mr, lead, sq,
                            inset) ==
        Sim3D_CullProximity(136, (int16_t)(0 + inset), ml, mr, lead, sq, 0));

  /* Horizontal is untouched too: the lift is vertical. */
  CHECK(Sim3D_CullProximity((int16_t)(kSimSpriteWindowBiasedWidth + mr), 120,
                            ml, mr, lead, sq, inset) == 1.0f);
  CHECK(Sim3D_CullProximity(136, 120, ml, mr, lead, sq, inset) ==
        Sim3D_CullProximity(136, 120, ml, mr, lead, sq, 0));

  /* An absurd inset cannot invert the window or collapse it onto a line. */
  CHECK(Sim3D_CullProximity(136, 120, ml, mr, lead, sq, 100000) >= 0.0f);
  CHECK(Sim3D_CullProximity(136, 120, ml, mr, lead, sq, 100000) <= 1.0f);
  /* Negative is treated as none. */
  CHECK(Sim3D_CullProximity(136, (int16_t)(bottom - 8), ml, mr, lead, sq, -9)
        == Sim3D_CullProximity(136, (int16_t)(bottom - 8), ml, mr, lead, sq,
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
 * NOT change. It must not become a height (the family's ground contact is the
 * ROM's), must not resurrect a shadow, and must not spread to the shadow
 * ellipse that shares its composition range. */
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

  /* Nothing else in the catalogue claims it -- an ordinary grounded record and
   * a flying one both stay in the row sort. */
  SimObjectClassification grounded = Sim3D_ClassifyObject(
      kSimRecordTier_World, 0x02, 0, kActRaiserWram_SimWorldRecords, 0xE000);
  CHECK(!(grounded.traits & kSimObjectTrait_Overhead));
  SimObjectClassification flying = Sim3D_ClassifyObject(
      kSimRecordTier_World, 0x0C, 0, kActRaiserWram_SimAngelRecord, 0xA627);
  CHECK(!(flying.traits & kSimObjectTrait_Overhead));
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

int main(void) {
  TestFeatureDependencies();
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
  TestGroundStrikeOverride();
  TestHeightSlew();
  TestObjColorMathPartition();
  TestAtlasDescriptorFallback();
  TestShadowCasterSelection();
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("sim_render_metadata_test: PASS");
  return 0;
}
