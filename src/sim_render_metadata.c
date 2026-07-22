#include "sim_render_metadata.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser_game.h"
#include "sim_world_map.h"

typedef struct SimMetadataProducer {
  bool active;
  bool record_active;
  bool world_started;
  uint32_t build_serial;
  uint32_t integrity_flags;
  uint16_t last_record_address;
  uint16_t last_oam_cursor;
  uint16_t emitted_oam_count;
  uint16_t claimed_oam_count;
  uint16_t world_emitted_count;
  uint8_t world_oam_first;
  bool atlas_valid;
  uint16_t atlas_width, atlas_height;
  uint16_t atlas_used_width, atlas_used_height;
  uint8_t source_count;
  uint8_t zero_oam_source_count;
  uint16_t object_count;
  uint8_t current_source;
  uint8_t claimed_oam[128];
  SimSourceRecord sources[kSimMaxSourceRecords];
  SimRenderObject objects[kSimMaxRenderObjects];
} SimMetadataProducer;

static SimMetadataProducer g_sim_metadata;
static FILE *g_sim_d1_trace;
static bool g_sim_d1_trace_env_checked;

/* D3c classification tables.  Every entry is transcribed from the locked
 * height policy in docs/sim-object-catalog.md; the renderer must never
 * rediscover a height from OAM attributes or pixel shape. */
enum {
  /* Record classes with a real 16-entry state table; all are flying. */
  kSimRecordClass_Angel = 0x0C,
  kSimRecordClass_EnemyFirst = 0x12,
  kSimRecordClass_BlueDragon = 0x12,
  kSimRecordClass_EnemyLast = 0x15,
  /* $01:B9EC state 6 is the Blue Dragon's building strike. The canonical
   * replay holds it for exactly the 33 frames that bracket every
   * $E1BD/$E209/$E255 bolt, and the ROM drops the record onto the target
   * itself, so the body must share the bolt's plane for those frames. */
  kSimRecordState_BlueDragonStrike = 6,
  /* The miracle cloud family's own ground shadow ellipse. Inside the
   * $D9E5-$DCD2 range but not part of what hangs in the sky. */
  kSimComposition_MiracleCloudShadow = 0xDA22,
};

static bool CompositionIn(uint16_t composition, uint16_t first,
                          uint16_t last) {
  return composition >= first && composition <= last;
}

/* Position/direction cursors painted onto the selected map square. $D233-$D302
 * are the ROM's cursor family; $D993 is the separate 64x64 hollow square (a
 * 4x4 grid of 16px parts with the centre four omitted, palette 6) used as the
 * path/area selector during Direct the People and targeted miracles. It sits
 * outside the cursor range and is easy to mistake for the neighbouring miracle
 * cloud effects, which start at $D9E5 and use palette 2. */
static bool IsMapPlaneCursorComposition(uint16_t composition) {
  return (composition >= 0xD233 && composition <= 0xD302) ||
      composition == 0xD993;
}

static bool IsAngelArrowComposition(uint16_t composition) {
  return composition == 0xD967 || composition == 0xD972 ||
      composition == 0xD97D || composition == 0xD988;
}

static bool IsBuildingZapComposition(uint16_t composition) {
  return composition == 0xE1BD || composition == 0xE209 ||
      composition == 0xE255;
}

static bool IsNapperPluckComposition(uint16_t composition) {
  return composition == 0xE71B || composition == 0xE73A ||
      composition == 0xE75E;
}

static bool IsGroundFireComposition(uint16_t composition) {
  return composition == 0xE6CA || composition == 0xE6D0 ||
      composition == 0xE6D6;
}

SimObjectClassification Sim3D_ClassifyObject(
    uint8_t tier, uint16_t type, uint16_t semantic_state,
    uint16_t record_address, uint16_t composition) {
  SimObjectClassification result = { kSimHeightClass_None, 0, 0 };

  /* Cursors are painted onto the selected map square in every tier, and stay
   * outside the height system entirely. Class $11 is the town position
   * controller that emits the $D233-$D302 family, but $D993 arrives on a
   * class-$09 record, so the composition remains the sole discriminator. */
  if (IsMapPlaneCursorComposition(composition)) {
    result.height_class = kSimHeightClass_MapPlane;
    result.traits = kSimObjectTrait_MapPlane | kSimObjectTrait_NoShadow;
    return result;
  }
  /* Fixed records are screen-relative UI/effects; they never gain a height,
   * an anchor policy, or a shadow. */
  if (tier != kSimRecordTier_World) return result;

  /* Composition overrides come first because a classified state can leave the
   * record class's default plane (Napper plucking, Blue Dragon zapping). */
  if (record_address == kActRaiserWram_SimAngelArrowRecord ||
      IsAngelArrowComposition(composition)) {
    result.height_class = kSimHeightClass_FlyingProjectile;
    result.virtual_height = kSimVirtualHeight_Flying;
    result.traits = kSimObjectTrait_RecordOriginAnchor |
        kSimObjectTrait_NoShadow;
    return result;
  }
  if (IsBuildingZapComposition(composition) ||
      CompositionIn(composition, 0xE9CC, 0xEAEC)) {
    /* Lightning and struck-ground effects belong to the target tile, not to
     * the flying record that requested them. */
    result.height_class = kSimHeightClass_GroundEffect;
    result.traits = kSimObjectTrait_RecordOriginAnchor |
        kSimObjectTrait_NoShadow;
    return result;
  }
  if (IsGroundFireComposition(composition)) {
    result.height_class = kSimHeightClass_GroundEffect;
    result.traits = kSimObjectTrait_NoShadow;
    return result;
  }
  /* Miracle cloud family. $D9E5 is the cloud alone; $DA4B/$DAA1/$DAF7/$DB5C
   * extend it with a lightning bolt and $DC77/$DBC1/$DC1C/$DCD2 with rain
   * streaks, so one composition's art already spans cloud to ground. $DA22 is
   * the ROM's own shadow ellipse, drawn 40-72px below the shared anchor by a
   * co-located record. Lifting any of them would detach the strike from the
   * ground, and a foot anchor would shift the cloud against its own shadow,
   * so the whole family keeps the ROM's record-origin placement and supplies
   * its own shadow art. */
  if (CompositionIn(composition, 0xD9E5, 0xDCD2)) {
    result.height_class = kSimHeightClass_GroundEffect;
    result.traits = kSimObjectTrait_RecordOriginAnchor |
        kSimObjectTrait_NoShadow;
    /* Overhead for the cloud itself, never for $DA22. That composition is the
     * ROM's own shadow ellipse, drawn 40-72px below the shared anchor: it lies
     * ON the ground and anything standing there should occlude it, which is
     * the exact opposite of what the cloud above it needs. */
    if (composition != kSimComposition_MiracleCloudShadow)
      result.traits |= kSimObjectTrait_Overhead;
    return result;
  }
  if (IsNapperPluckComposition(composition)) {
    result.height_class = kSimHeightClass_SemiGrounded;
    result.virtual_height = kSimVirtualHeight_SemiGrounded;
    return result;
  }
  if (CompositionIn(composition, 0xE99C, 0xE9C6)) {
    result.height_class = kSimHeightClass_WaterPlane;
    result.traits = kSimObjectTrait_WaterPlane | kSimObjectTrait_NoShadow;
    return result;
  }

  /* A classified strike state overrides the record's flight plane, so the
   * body and its own ground-anchored bolt cannot separate. The bolt frames
   * themselves were already claimed by the ground-effect branch above. */
  if (type == kSimRecordClass_BlueDragon &&
      semantic_state == kSimRecordState_BlueDragonStrike) {
    result.height_class = kSimHeightClass_GroundStrike;
    return result;
  }

  /* Record semantics supply the default plane. The angel is identified by its
   * record and class only: the $A627-$A792 pose frames are also borrowed by
   * miracle effect records (observed on a type-$04 record during the kind-5
   * miracle), and those must not inherit the angel's flight plane. */
  if (record_address == kActRaiserWram_SimAngelRecord ||
      type == kSimRecordClass_Angel ||
      (type >= kSimRecordClass_EnemyFirst &&
       type <= kSimRecordClass_EnemyLast)) {
    result.height_class = kSimHeightClass_Flying;
    result.virtual_height = kSimVirtualHeight_Flying;
    return result;
  }
  result.height_class = kSimHeightClass_Grounded;
  return result;
}

const char *Sim3D_HeightClassName(SimHeightClass height_class) {
  switch (height_class) {
    case kSimHeightClass_None: return "none";
    case kSimHeightClass_Grounded: return "grounded";
    case kSimHeightClass_WaterPlane: return "water_plane";
    case kSimHeightClass_GroundEffect: return "ground_effect";
    case kSimHeightClass_SemiGrounded: return "semi_grounded";
    case kSimHeightClass_Flying: return "flying";
    case kSimHeightClass_FlyingProjectile: return "flying_projectile";
    case kSimHeightClass_MapPlane: return "map_plane";
    case kSimHeightClass_GroundStrike: return "ground_strike";
    case kSimHeightClass_Count: break;
  }
  return "invalid";
}

bool Sim3D_HeightClassIsContactExact(SimHeightClass height_class) {
  return height_class == kSimHeightClass_GroundEffect ||
      height_class == kSimHeightClass_GroundStrike;
}

static uint16_t ReadMirror16(const uint8 *wram, uint32_t address) {
  return (uint16_t)(wram[address] | (wram[address + 1] << 8));
}

static int RecordIndex(uint16_t record_address, bool world_record) {
  uint16_t base = world_record ? kActRaiserWram_SimWorldRecords
                               : kActRaiserWram_SimFixedRecords;
  uint16_t stride = world_record ? kActRaiserSimWorldRecordStride
                                 : kActRaiserSimFixedRecordStride;
  uint16_t count = world_record ? kActRaiserSimWorldRecordCount
                                : kActRaiserSimFixedRecordCount;
  if (record_address < base ||
      (uint16_t)(record_address - base) % stride != 0)
    return -1;
  int index = (record_address - base) / stride;
  return index < count ? index : -1;
}

static void BeginBuild(void) {
  uint32_t next_serial = g_sim_metadata.build_serial + 1;
  memset(&g_sim_metadata, 0, sizeof(g_sim_metadata));
  g_sim_metadata.active = true;
  g_sim_metadata.build_serial = next_serial;
}

void SimRenderMetadata_Reset(void) {
  memset(&g_sim_metadata, 0, sizeof(g_sim_metadata));
  SimRenderMetadata_ResetHeightSlew();
}

void SimRenderMetadata_BeginRecord(
    uint16_t record_address, bool world_record, bool alternate_attributes,
    uint16_t composition, uint16_t world_x, uint16_t world_y,
    uint16_t type, uint16_t semantic_state, uint16_t status,
    uint16_t oam_cursor_before) {
  /* $01:ACD9 starts at cursor zero and visits each tier in ascending address
   * order.  <= (not merely <) also recognizes a one-record pass repeated on
   * the next emulated tick.  A clipped record followed by a later record at
   * the same zero cursor does not reset because its address increased. */
  if (!g_sim_metadata.active ||
      (oam_cursor_before == 0 &&
       record_address <= g_sim_metadata.last_record_address))
    BeginBuild();

  if (g_sim_metadata.record_active)
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_CursorMismatch;
  if ((oam_cursor_before & 3) ||
      oam_cursor_before > kActRaiserOamLowTableBytes)
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_CursorMismatch;
  if (g_sim_metadata.source_count &&
      record_address <= g_sim_metadata.last_record_address)
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_RecordOrder;
  if (oam_cursor_before != g_sim_metadata.last_oam_cursor)
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_CursorMismatch;
  if (RecordIndex(record_address, world_record) < 0)
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_InvalidRecord;
  if (!world_record && g_sim_metadata.world_started)
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_RecordOrder |
                                      kSimMetadataIntegrity_WorldSuffix;

  if (g_sim_metadata.source_count >= kSimMaxSourceRecords) {
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_Overflow;
    g_sim_metadata.record_active = false;
    return;
  }

  uint8_t source_index = g_sim_metadata.source_count++;
  SimSourceRecord *source = &g_sim_metadata.sources[source_index];
  *source = (SimSourceRecord){
    .record_address = record_address,
    .composition = composition,
    .world_x = world_x,
    .world_y = world_y,
    .type = type,
    .semantic_state = world_record ? semantic_state : 0,
    .status = status,
    .oam_first = (uint16_t)(oam_cursor_before / 4),
    .fragment_first = g_sim_metadata.object_count,
    .tier = world_record ? kSimRecordTier_World : kSimRecordTier_Fixed,
    .alternate_attributes = alternate_attributes ? 1 : 0,
  };
  g_sim_metadata.current_source = source_index;
  g_sim_metadata.record_active = true;
}

void SimRenderMetadata_RecordAnchor(int16_t base_x, int16_t base_y) {
  if (!g_sim_metadata.record_active) return;
  SimSourceRecord *source =
      &g_sim_metadata.sources[g_sim_metadata.current_source];
  source->anchor_x = base_x;
  source->anchor_y = base_y;
  source->anchor_valid = 1;
}

void SimRenderMetadata_RecordClippedPart(uint8_t reason) {
  if (!g_sim_metadata.record_active) return;
  SimSourceRecord *source =
      &g_sim_metadata.sources[g_sim_metadata.current_source];
  source->clip_reason |= reason;
  if (source->clipped_parts < 0xFFFF) source->clipped_parts++;
}

void SimRenderMetadata_RecordPart(uint16_t oam_cursor,
                                  uint16_t attributes) {
  if (!g_sim_metadata.record_active) {
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_CursorMismatch;
    return;
  }
  if ((oam_cursor & 3) || oam_cursor >= kActRaiserOamLowTableBytes) {
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_CursorMismatch;
    return;
  }

  uint16_t slot = (uint16_t)(oam_cursor / 4);
  SimSourceRecord *source =
      &g_sim_metadata.sources[g_sim_metadata.current_source];
  uint8_t priority = (uint8_t)((attributes >> 12) & 3);
  uint8_t color_math_eligible = (attributes & 0x0800) != 0;
  if (g_sim_metadata.claimed_oam[slot])
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_Overlap;
  else {
    g_sim_metadata.claimed_oam[slot] = 1;
    g_sim_metadata.claimed_oam_count++;
  }
  g_sim_metadata.emitted_oam_count++;
  source->oam_count++;

  if (source->tier == kSimRecordTier_World) {
    if (!g_sim_metadata.world_started) {
      g_sim_metadata.world_started = true;
      g_sim_metadata.world_oam_first = (uint8_t)slot;
    }
    g_sim_metadata.world_emitted_count++;
  } else if (g_sim_metadata.world_started) {
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_WorldSuffix;
  }

  SimRenderObject *prior = g_sim_metadata.object_count
      ? &g_sim_metadata.objects[g_sim_metadata.object_count - 1] : NULL;
  if (prior && prior->source_index == g_sim_metadata.current_source &&
      prior->priority == priority &&
      prior->color_math_eligible == color_math_eligible &&
      prior->oam_first + prior->oam_count == slot) {
    prior->oam_count++;
    return;
  }

  if (g_sim_metadata.object_count >= kSimMaxRenderObjects) {
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_Overflow;
    return;
  }
  SimObjectClassification classification = Sim3D_ClassifyObject(
      source->tier, source->type, source->semantic_state,
      source->record_address, source->composition);
  SimRenderObject *object =
      &g_sim_metadata.objects[g_sim_metadata.object_count++];
  *object = (SimRenderObject){
    .record_address = source->record_address,
    .composition = source->composition,
    .world_x = source->world_x,
    .world_y = source->world_y,
    .type = source->type,
    .semantic_state = source->semantic_state,
    .oam_first = slot,
    .oam_count = 1,
    .priority = priority,
    .source_index = g_sim_metadata.current_source,
    .tier = source->tier,
    .color_math_eligible = color_math_eligible,
    .traits = classification.traits,
    .height_class = classification.height_class,
    .virtual_height = classification.virtual_height,
    .classified_height = classification.virtual_height,
    .foot_x = (int16_t)source->world_x,
    .foot_y = (int16_t)source->world_y,
    /* Atlas and local bounds are deliberately invalid until the shared PPU
     * rasterizer lands; zero must never be interpreted as a packed rect. */
    .atlas_valid = 0,
  };
}

void SimRenderMetadata_EndRecord(uint16_t oam_cursor_after) {
  if (!g_sim_metadata.record_active) {
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_CursorMismatch;
    return;
  }
  SimSourceRecord *source =
      &g_sim_metadata.sources[g_sim_metadata.current_source];
  uint16_t expected =
      (uint16_t)((source->oam_first + source->oam_count) * 4);
  if ((oam_cursor_after & 3) ||
      oam_cursor_after > kActRaiserOamLowTableBytes ||
      oam_cursor_after != expected)
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_CursorMismatch;

  source->fragment_count =
      (uint16_t)(g_sim_metadata.object_count - source->fragment_first);
  if (!source->oam_count) g_sim_metadata.zero_oam_source_count++;
  g_sim_metadata.last_record_address = source->record_address;
  g_sim_metadata.last_oam_cursor = oam_cursor_after;
  g_sim_metadata.record_active = false;
}

bool SimRenderMetadata_CopyAtlasInput(SimAtlasBuildInput *out) {
  if (!out || !g_sim_metadata.active || g_sim_metadata.record_active)
    return false;
  memset(out, 0, sizeof(*out));
  out->build_serial = g_sim_metadata.build_serial;
  out->object_count = g_sim_metadata.object_count;
  memcpy(out->objects, g_sim_metadata.objects,
         sizeof(SimRenderObject) * out->object_count);
  return true;
}

bool SimRenderMetadata_AtlasReady(void) {
  return g_sim_metadata.active && !g_sim_metadata.record_active &&
      g_sim_metadata.atlas_valid && !g_sim_metadata.integrity_flags;
}

bool SimRenderMetadata_CommitAtlas(
    uint32_t build_serial, const SimRenderObject *objects,
    uint16_t object_count, bool atlas_valid,
    uint16_t atlas_width, uint16_t atlas_height,
    uint16_t atlas_used_width, uint16_t atlas_used_height,
    uint32_t integrity_flags) {
  if (!g_sim_metadata.active || g_sim_metadata.record_active ||
      build_serial != g_sim_metadata.build_serial ||
      object_count != g_sim_metadata.object_count ||
      (object_count && !objects))
    return false;

  const uint32_t atlas_failures =
      kSimMetadataIntegrity_AtlasOverflow |
      kSimMetadataIntegrity_AtlasRasterFailure;
  g_sim_metadata.integrity_flags |= integrity_flags & atlas_failures;
  bool descriptors_valid = atlas_valid && atlas_width && atlas_height &&
      atlas_used_width <= atlas_width && atlas_used_height <= atlas_height;
  for (uint16_t i = 0; descriptors_valid && i < object_count; i++) {
    const SimRenderObject *object = &objects[i];
    /* A purged object carries no descriptor to validate. The builder drops
     * one it cannot pack rather than discarding the atlas, so an absent
     * descriptor is an expected outcome here, not a contract violation --
     * the D1 census counts it through the object's own atlas_valid. */
    if (!object->atlas_valid) continue;
    descriptors_valid = object->atlas_w &&
        object->atlas_h && object->atlas_x + object->atlas_w <= atlas_width &&
        object->atlas_y + object->atlas_h <= atlas_height &&
        object->local_x1 - object->local_x0 == object->atlas_w &&
        object->local_y1 - object->local_y0 == object->atlas_h;
    for (uint16_t j = 0; descriptors_valid && j < i; j++) {
      const SimRenderObject *prior = &objects[j];
      if (!prior->atlas_valid) continue;
      bool overlap = object->atlas_x < prior->atlas_x + prior->atlas_w &&
          object->atlas_x + object->atlas_w > prior->atlas_x &&
          object->atlas_y < prior->atlas_y + prior->atlas_h &&
          object->atlas_y + object->atlas_h > prior->atlas_y;
      if (overlap) descriptors_valid = false;
    }
  }
  if (!descriptors_valid && !(g_sim_metadata.integrity_flags & atlas_failures))
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_AtlasRasterFailure;

  g_sim_metadata.atlas_valid = descriptors_valid &&
      !(g_sim_metadata.integrity_flags & atlas_failures);
  g_sim_metadata.atlas_width = g_sim_metadata.atlas_valid ? atlas_width : 0;
  g_sim_metadata.atlas_height = g_sim_metadata.atlas_valid ? atlas_height : 0;
  g_sim_metadata.atlas_used_width =
      g_sim_metadata.atlas_valid ? atlas_used_width : 0;
  g_sim_metadata.atlas_used_height =
      g_sim_metadata.atlas_valid ? atlas_used_height : 0;

  for (uint16_t i = 0; i < object_count; i++) {
    SimRenderObject *dst = &g_sim_metadata.objects[i];
    if (g_sim_metadata.atlas_valid) {
      dst->foot_x = objects[i].foot_x;
      dst->foot_y = objects[i].foot_y;
      dst->local_x0 = objects[i].local_x0;
      dst->local_y0 = objects[i].local_y0;
      dst->local_x1 = objects[i].local_x1;
      dst->local_y1 = objects[i].local_y1;
      dst->atlas_x = objects[i].atlas_x;
      dst->atlas_y = objects[i].atlas_y;
      dst->atlas_w = objects[i].atlas_w;
      dst->atlas_h = objects[i].atlas_h;
      dst->atlas_valid = objects[i].atlas_valid;
    } else {
      dst->local_x0 = dst->local_y0 = 0;
      dst->local_x1 = dst->local_y1 = 0;
      dst->atlas_x = dst->atlas_y = 0;
      dst->atlas_w = dst->atlas_h = 0;
      dst->atlas_valid = 0;
    }
  }
  return true;
}

/* Per-world-record presentation-height animation. This is host-only easing
 * state on the game thread: it is folded into the immutable per-frame value
 * copy, so the present thread still reads one self-contained snapshot. */
typedef struct SimHeightSlew {
  int16_t height;
  uint32_t last_serial;
  bool active;
} SimHeightSlew;

static SimHeightSlew g_height_slew[kActRaiserSimWorldRecordCount];

void SimRenderMetadata_ResetHeightSlew(void) {
  memset(g_height_slew, 0, sizeof(g_height_slew));
}

static void ApplyHeightSlew(SimFrameData *dst, bool enabled) {
  /* The easing exists only for the projected view. With the SIM 3D master
   * off, in a picker, or on a fallback frame, the table is cleared so a later
   * enable starts every actor on its classified plane instead of replaying a
   * stale ramp. */
  if (!enabled || dst->view != kSimView_Enhanced) {
    memset(g_height_slew, 0, sizeof(g_height_slew));
    return;
  }

  for (uint16_t i = 0; i < dst->object_count; i++) {
    SimRenderObject *object = &dst->objects[i];
    if (object->tier != kSimRecordTier_World) continue;
    int index = RecordIndex(object->record_address, true);
    if (index < 0) continue;
    SimHeightSlew *slew = &g_height_slew[index];

    /* Every fragment of one record shares its source's classification, so the
     * record steps once per build and later fragments only read the result. */
    if (slew->active && slew->last_serial == dst->build_serial) {
      object->virtual_height = slew->height;
      continue;
    }

    int16_t target = object->virtual_height;
    /* Ramp only a record that was present in the immediately preceding build.
     * A recycled slot is a different actor and must not inherit the previous
     * occupant's plane. Contact-critical classes are positioned by the ROM and
     * must land exactly on their first frame. */
    bool continuous = slew->active &&
        slew->last_serial + 1 == dst->build_serial;
    if (!continuous ||
        Sim3D_HeightClassIsContactExact(
            (SimHeightClass)object->height_class)) {
      slew->height = target;
    } else if (slew->height < target) {
      slew->height = (int16_t)(slew->height + kSimHeightSlewStep < target
          ? slew->height + kSimHeightSlewStep : target);
    } else if (slew->height > target) {
      slew->height = (int16_t)(slew->height - kSimHeightSlewStep > target
          ? slew->height - kSimHeightSlewStep : target);
    }
    slew->active = true;
    slew->last_serial = dst->build_serial;
    object->virtual_height = slew->height;
  }
}

float Sim3D_CloudCoverage(float x, float y, float clear_x0, float clear_x1,
                          float clear_y0, float clear_y1, float inset,
                          float falloff) {
  /* Signed distance outside the rectangle: negative inside, zero on the edge.
   * The larger axis wins rather than the diagonal, so a corner is never
   * thinner than the edges meeting there. */
  float dx = clear_x0 - x;
  float dx1 = x - clear_x1;
  if (dx1 > dx) dx = dx1;
  float dy = clear_y0 - y;
  float dy1 = y - clear_y1;
  if (dy1 > dy) dy = dy1;
  float distance = dx > dy ? dx : dy;

  /* The inset is in pixels but the rectangle's two axes are very different
   * sizes -- roughly 446 wide against 224 tall -- so an inset the horizontal
   * axis shrugs off can swallow the vertical one from both sides and veil the
   * middle of the screen. Cap it at a quarter of the shorter half-extent so
   * the playable centre stays clear whatever the setting says. */
  float half_x = (clear_x1 - clear_x0) * 0.5f;
  float half_y = (clear_y1 - clear_y0) * 0.5f;
  float smallest = half_x < half_y ? half_x : half_y;
  float limit = smallest * 0.5f;
  if (inset > limit) inset = limit;
  if (inset < 0.0f) inset = 0.0f;

  float width = inset + falloff;
  if (width <= 0.0f) return distance >= 0.0f ? 1.0f : 0.0f;
  float coverage = (distance + inset) / width;
  return coverage < 0.0f ? 0.0f : coverage > 1.0f ? 1.0f : coverage;
}

float Sim3D_CullProximity(int16_t anchor_x, int16_t anchor_y,
                          int margin_left, int margin_right, int lead,
                          int corner, int lift_inset) {
  if (lead <= 0) lead = 1;

  /* The window the emitter actually tests against. Horizontal gains the live
   * widescreen margins; vertical never does -- OAM's Y byte has no ninth bit,
   * so the emitter cannot widen it and neither may this. */
  float x0 = (float)(-margin_left);
  float x1 = (float)(kSimSpriteWindowBiasedWidth + margin_right);
  float y0 = 0.0f;
  float y1 = (float)kSimSpriteWindowBiasedHeight;
  /* Bottom only; see the header. Clamped so an absurd inset cannot invert the
   * window or collapse it onto a line. */
  if (lift_inset > 0) {
    float limit = (y1 - y0) * 0.5f;
    float inset = (float)lift_inset;
    if (inset > limit) inset = limit;
    y1 -= inset;
  }

  float half_x = (x1 - x0) * 0.5f;
  float half_y = (y1 - y0) * 0.5f;
  float centre_x = (x0 + x1) * 0.5f;
  float centre_y = (y0 + y1) * 0.5f;

  /* Corner radius, clamped so it can never exceed the shorter half-extent --
   * beyond that the "rectangle" is just a capsule and the window stops
   * describing the emitter's predicate at all. */
  float radius = (float)corner;
  float smallest = half_x < half_y ? half_x : half_y;
  if (radius > smallest) radius = smallest;
  if (radius < 0.0f) radius = 0.0f;

  /* Signed distance to a rounded rectangle: negative inside, zero on the
   * edge. The previous form took the larger axis, which is a Chebyshev
   * distance and therefore an axis-aligned box with hard corners -- the
   * visible squareness of the lit region was that choice showing through.
   *
   * Rounding moves cover in the safe direction. At a corner this distance is
   * radius*(sqrt(2)-1) GREATER than the flat-edge case, so a record cutting
   * the diagonal is covered sooner than one approaching the edges meeting
   * there, never later. */
  float qx = (float)anchor_x - centre_x;
  float qy = (float)anchor_y - centre_y;
  qx = (qx < 0.0f ? -qx : qx) - (half_x - radius);
  qy = (qy < 0.0f ? -qy : qy) - (half_y - radius);
  float ox = qx > 0.0f ? qx : 0.0f;
  float oy = qy > 0.0f ? qy : 0.0f;
  float outside = sqrtf(ox * ox + oy * oy);
  float longest = qx > qy ? qx : qy;
  float inside = longest < 0.0f ? longest : 0.0f;
  float distance = outside + inside - radius;

  float ramp = (distance + (float)lead) / (float)lead;
  if (ramp <= 0.0f) return 0.0f;
  if (ramp >= 1.0f) return 1.0f;
  /* Feathered rather than linear. A linear ramp has a discontinuous slope at
   * both ends, and the eye finds those two creases and reads them as edges --
   * which is the whole thing the ramp exists to avoid. */
  return ramp * ramp * (3.0f - 2.0f * ramp);
}

float Sim3D_SourceCullCover(const SimSourceRecord *source,
                            int margin_left, int margin_right, int lead,
                            int corner, int lift_inset) {
  if (!source || !source->anchor_valid) return 0.0f;

  /* World-tier records only. Fixed-tier records are HUD and cursor furniture
   * that lives in screen space; it does not belong to the town and a cloud
   * over it would be nonsense. */
  if (source->tier != kSimRecordTier_World) return 0.0f;

  /* A record with no parts at all never asked to be drawn. Only the sprite
   * window may create cover, so a record that emitted nothing AND was never
   * clipped is the game's own decision, not ours to hide. */
  if (!source->oam_count && !source->clipped_parts) return 0.0f;

  /* Deliberately the record's own anchor, NOT where the renderer draws it.
   * See Sim3D_SourceDrawLift: when the cover arrives and where it goes are
   * two different questions, and this is the first one. */
  return Sim3D_CullProximity(source->anchor_x, source->anchor_y,
                             margin_left, margin_right, lead, corner,
                             lift_inset);
}

int16_t Sim3D_MaxDrawLift(unsigned height_scale_x100) {
  long lift = (long)kSimVirtualHeight_Flying * (long)height_scale_x100 / 100;
  if (lift < 0) lift = 0;
  if (lift > 0x7FFF) lift = 0x7FFF;
  return (int16_t)lift;
}

int16_t Sim3D_SourceDrawLift(const SimSourceRecord *source,
                             unsigned height_scale_x100) {
  if (!source || source->tier != kSimRecordTier_World) return 0;
  /* The same pure classifier the object pass uses, run from the source
   * record's own fields. It has to be reachable this way: a record the sprite
   * window took away entirely emitted no parts, so it has no entry in
   * objects[] to read a height from -- and that is exactly the record whose
   * cover placement matters most. */
  SimObjectClassification classification = Sim3D_ClassifyObject(
      source->tier, source->type, source->semantic_state,
      source->record_address, source->composition);
  long lift = (long)classification.virtual_height *
      (long)height_scale_x100 / 100;
  if (lift < 0) lift = 0;
  if (lift > 0x7FFF) lift = 0x7FFF;
  return (int16_t)lift;
}

bool Sim3D_ObjectCastsShadow(const SimRenderObject *object) {
  if (!object || !object->atlas_valid) return false;
  if (object->tier != kSimRecordTier_World) return false;
  if (object->traits & (kSimObjectTrait_MapPlane | kSimObjectTrait_NoShadow))
    return false;
  return object->atlas_w > 0 && object->atlas_h > 0 &&
      object->local_x1 > object->local_x0 &&
      object->local_y1 > object->local_y0;
}

SimRenderFeatureMask Sim3D_ResolveFeatureMask(
    SimRenderFeatureMask requested_features,
    SimRenderFeatureMask implemented_features,
    SimViewKind view, bool master_enabled, bool metadata_valid) {
  if (!master_enabled || view != kSimView_Enhanced)
    return 0;

  SimRenderFeatureMask features =
      requested_features & implemented_features & kSimFeature_All;
  if (!(features & kSimFeature_SeparatedComposite)) return 0;

  /* Object metadata is a sprite-only dependency. The separated composite, the
   * ground projection and the world underlay are built from captured plane
   * pixels and the camera, none of which the semantic record pass supplies,
   * so an unusable object list costs the sprites and nothing else. */
  if (!metadata_valid)
    features &= ~(kSimFeature_ObjectBillboards | kSimFeature_VirtualHeight |
                  kSimFeature_Shadows | kSimFeature_SoftShadows |
                  kSimFeature_RimLight);

  if (!(features & kSimFeature_ObjectBillboards))
    features &= ~(kSimFeature_VirtualHeight | kSimFeature_Shadows |
                  kSimFeature_SoftShadows | kSimFeature_RimLight);
  /* SoftShadows deliberately does NOT depend on shader availability: D4b's
   * blur is ordinary blended draws over the mask target, so it works on every
   * renderer backend. A missing blur target degrades to the hard silhouette at
   * draw time rather than clearing the bit here. */
  if (!(features & kSimFeature_Shadows))
    features &= ~kSimFeature_SoftShadows;
  /* The underlay is drawn in the ground plane, positioned by the same
   * view/projection transform as the town's own ground mesh. With the flat
   * view there is no plane to extend and no transform to share. */
  if (!(features & kSimFeature_GroundProjection))
    features &= ~kSimFeature_WorldUnderlay;
  /* Both cull cues describe the same boundary on the extended ground. Without
   * the extension there is no out-of-range ground to mark, and the finite
   * town's own edge already says everything there is to say. */
  if (!(features & kSimFeature_WorldUnderlay))
    features &= ~(kSimFeature_CloudShroud | kSimFeature_CullHaze);
  /* The backdrop is a gradient anchored to the projected horizon. The flat
   * view has no horizon to anchor to, and the flat clear it would replace is
   * already correct there. */
  if (!(features & kSimFeature_GroundProjection))
    features &= ~kSimFeature_Backdrop;
  return features;
}

void SimRenderMetadata_CaptureFrame(
    SimFrameData *dst, const uint8 *wram, bool master_enabled,
    SimRenderFeatureMask requested_features,
    uint32_t diagnostic_layer_mask,
    SimRenderFeatureMask implemented_features) {
  if (!dst) return;
  memset(dst, 0, sizeof(*dst));
  /* Zero is a deliberate "ground everything" tuning value, so a frame that is
   * never annotated must still default to the catalogue heights. */
  dst->height_scale_x100 = 100;
  dst->master_enabled = master_enabled;
  dst->requested_features = requested_features & kSimFeature_All;
  dst->diagnostic_layer_mask = diagnostic_layer_mask;
  if (!wram) return;

  uint8_t map_group = wram[kActRaiserWram_MapGroup];
  uint8_t map_number = wram[kActRaiserWram_CurrentMap];
  bool town = ActRaiser_IsSimulationTown(map_group, map_number);
  dst->town = town ? map_number : 0;
  /* Adopt the live Mode-7 shadow every frame the current map owns it, so the
   * underlay shows neighbouring towns at their present development state
   * rather than the ROM baseline. Cheap: a memcmp that usually matches. */
  SimWorldMap_Refresh(wram, map_group, map_number);
  int underlay_x = 0, underlay_y = 0;
  if (town && SimWorldMap_OriginForTown(map_number, &underlay_x,
                                        &underlay_y)) {
    dst->underlay_serial = SimWorldMap_Serial();
    dst->underlay_origin_tile_x = (uint8_t)underlay_x;
    dst->underlay_origin_tile_y = (uint8_t)underlay_y;
  }
  dst->game_frame = ReadMirror16(wram, kActRaiserWram_GameFrame);
  dst->camera_x = ReadMirror16(wram, kActRaiserWram_Bg1CameraX);
  dst->camera_y = ReadMirror16(wram, kActRaiserWram_Bg1CameraY);
  dst->picker_flag =
      ReadMirror16(wram, kActRaiserWram_SimMapPickerFlag);

  dst->build_serial = g_sim_metadata.build_serial;
  dst->integrity_flags = g_sim_metadata.integrity_flags;
  dst->atlas_valid = g_sim_metadata.atlas_valid;
  dst->atlas_width = g_sim_metadata.atlas_width;
  dst->atlas_height = g_sim_metadata.atlas_height;
  dst->atlas_used_width = g_sim_metadata.atlas_used_width;
  dst->atlas_used_height = g_sim_metadata.atlas_used_height;
  if (g_sim_metadata.record_active)
    dst->integrity_flags |= kSimMetadataIntegrity_CursorMismatch;
  dst->metadata_valid = g_sim_metadata.active && !dst->integrity_flags;
  if (!town)
    dst->view = kSimView_None;
#if AR_SIM3D_PICKER_TOPDOWN
  /* An active position picker owns the frame: the authentic flat view is
   * pixel- and input-identical to the original game by construction. */
  else if (dst->picker_flag)
    dst->view = kSimView_AuthenticPicker;
#endif
  else
    /* Broken object metadata deliberately does NOT drop the view. The ground,
     * the projection and the camera come from the separated plane capture,
     * which is gated separately; only the sprites depend on the metadata. A
     * whole-screen perspective flash is a far worse artifact than a missing
     * sprite for one frame, so the resolver clears the object stages instead
     * and the view stays put. */
    dst->view = kSimView_Enhanced;

  dst->emitted_oam_count = g_sim_metadata.emitted_oam_count;
  dst->claimed_oam_count = g_sim_metadata.claimed_oam_count;
  dst->source_count = g_sim_metadata.source_count;
  dst->zero_oam_source_count = g_sim_metadata.zero_oam_source_count;
  dst->object_count = g_sim_metadata.object_count;
  if (g_sim_metadata.world_started) {
    uint16_t world_end = g_sim_metadata.last_oam_cursor / 4;
    dst->world_oam_first = g_sim_metadata.world_oam_first;
    if (world_end >= dst->world_oam_first)
      dst->world_oam_count =
          (uint8_t)(world_end - dst->world_oam_first);
    if (dst->world_oam_count != g_sim_metadata.world_emitted_count) {
      dst->integrity_flags |= kSimMetadataIntegrity_WorldSuffix;
      dst->metadata_valid = false;
    }
  }
  memcpy(dst->sources, g_sim_metadata.sources,
         sizeof(SimSourceRecord) * dst->source_count);
  memcpy(dst->objects, g_sim_metadata.objects,
         sizeof(SimRenderObject) * dst->object_count);
  ApplyHeightSlew(dst, master_enabled);

  dst->effective_features = Sim3D_ResolveFeatureMask(
      dst->requested_features, implemented_features, dst->view,
      master_enabled, dst->metadata_valid);
}

const char *Sim3D_ViewName(SimViewKind view) {
  switch (view) {
    case kSimView_None: return "none";
    case kSimView_Enhanced: return "enhanced";
    case kSimView_AuthenticPicker: return "authentic_picker";
    case kSimView_AuthenticFallback: return "authentic_fallback";
  }
  return "unknown";
}

const char *Sim3D_CaptureStatusName(Sim3DCaptureStatus status) {
  switch (status) {
    case kSim3DCapture_Inactive: return "inactive";
    case kSim3DCapture_MasterOff: return "master_off";
    case kSim3DCapture_NotRequested: return "not_requested";
    case kSim3DCapture_Picker: return "picker";
    case kSim3DCapture_NoRenderer: return "no_renderer";
    case kSim3DCapture_OverlayConflict: return "overlay_conflict";
    case kSim3DCapture_UnsupportedPpu: return "unsupported_ppu";
    case kSim3DCapture_UnsupportedColorMath:
      return "unsupported_color_math";
    case kSim3DCapture_AllocationFailure: return "allocation_failure";
    case kSim3DCapture_Capturing: return "capturing";
    case kSim3DCapture_AtlasInvalid: return "atlas_invalid";
    case kSim3DCapture_PixelMismatch: return "pixel_mismatch";
    case kSim3DCapture_Ready: return "ready";
  }
  return "unknown";
}

static uint64_t FrameHash(const uint8_t *rgba, int width, int height,
                          int pitch) {
  uint64_t hash = UINT64_C(1469598103934665603);
  if (!rgba || width <= 0 || height <= 0 || pitch < width * 4) return 0;
  for (int y = 0; y < height; y++) {
    const uint8_t *row = rgba + (size_t)y * pitch;
    for (int x = 0; x < width * 4; x++) {
      hash ^= row[x];
      hash *= UINT64_C(1099511628211);
    }
  }
  return hash;
}

static void TraceInitFromEnvironment(void) {
  if (g_sim_d1_trace_env_checked) return;
  g_sim_d1_trace_env_checked = true;
  const char *path = getenv("AR_SIM3D_D1_TRACE");
  if (!path || !path[0]) return;
  g_sim_d1_trace = fopen(path, "w");
  if (!g_sim_d1_trace) {
    fprintf(stderr, "[sim3d-d1] cannot open %s\n", path);
    return;
  }
  fprintf(stderr, "[sim3d-d1] metadata trace -> %s\n", path);
}

void SimRenderMetadata_TraceFrame(uint32_t host_frame,
                                  const SimFrameData *frame,
                                  const uint8_t *rgba, int width, int height,
                                  int pitch) {
  TraceInitFromEnvironment();
  if (!g_sim_d1_trace || !frame || !frame->town) return;

  fprintf(g_sim_d1_trace,
          "{\"host_frame\":%u,\"game_frame\":%u,\"town\":%u,"
          "\"view\":\"%s\",\"picker_flag\":%u,"
          "\"build_serial\":%u,\"master_enabled\":%s,"
          "\"requested\":%u,\"effective\":%u,"
          "\"metadata_valid\":%s,\"integrity_flags\":%u,"
          "\"atlas_valid\":%s,\"atlas_size\":[%u,%u],"
          "\"atlas_used\":[%u,%u],"
          "\"separated_valid\":%s,\"separated_status\":%u,"
          "\"separated_mismatch_pixels\":%u,"
          "\"separated_hash\":\"%016llx\","
          "\"projection_camera\":[%d,%d,%u],"
          "\"height_scale_x100\":%u,\"shadow_opacity_pct\":%u,"
          "\"shadow_softness_pct\":%u,\"light\":[%u,%u],"
          "\"picker_topdown\":%s,"
          "\"backdrop_argb\":%u,\"object_half_add\":%s,"
          "\"source_count\":%u,\"zero_oam_sources\":%u,"
          "\"object_count\":%u,\"emitted_oam_count\":%u,"
          "\"claimed_oam_count\":%u,\"world_oam_first\":%u,"
          "\"world_oam_count\":%u,\"framebuffer_hash\":\"%016llx\","
          "\"sources\":[",
          (unsigned)host_frame, (unsigned)frame->game_frame,
          (unsigned)frame->town,
          Sim3D_ViewName(frame->view), (unsigned)frame->picker_flag,
          (unsigned)frame->build_serial,
          frame->master_enabled ? "true" : "false",
          (unsigned)frame->requested_features,
          (unsigned)frame->effective_features,
          frame->metadata_valid ? "true" : "false",
          (unsigned)frame->integrity_flags,
          frame->atlas_valid ? "true" : "false",
          (unsigned)frame->atlas_width, (unsigned)frame->atlas_height,
          (unsigned)frame->atlas_used_width,
          (unsigned)frame->atlas_used_height,
          frame->separated_valid ? "true" : "false",
          (unsigned)frame->separated_status,
          (unsigned)frame->separated_mismatch_pixels,
          (unsigned long long)frame->separated_hash,
          (int)frame->projection_pitch_mrad,
          (int)frame->projection_yaw_mrad,
          (unsigned)frame->projection_distance_x100,
          (unsigned)frame->height_scale_x100,
          (unsigned)frame->shadow_opacity_pct,
          (unsigned)frame->shadow_softness_pct,
          (unsigned)frame->light_azimuth_deg,
          (unsigned)frame->light_elevation_deg,
          AR_SIM3D_PICKER_TOPDOWN ? "true" : "false",
          (unsigned)frame->separated_backdrop_argb,
          frame->object_half_add ? "true" : "false",
          (unsigned)frame->source_count,
          (unsigned)frame->zero_oam_source_count,
          (unsigned)frame->object_count,
          (unsigned)frame->emitted_oam_count,
          (unsigned)frame->claimed_oam_count,
          (unsigned)frame->world_oam_first,
          (unsigned)frame->world_oam_count,
          (unsigned long long)FrameHash(rgba, width, height, pitch));
  for (unsigned i = 0; i < frame->source_count; i++) {
    const SimSourceRecord *source = &frame->sources[i];
    if (i) fputc(',', g_sim_d1_trace);
    fprintf(g_sim_d1_trace,
            "{\"record\":%u,\"tier\":%u,\"composition\":%u,"
            "\"type\":%u,\"state\":%u,\"x\":%u,\"y\":%u,"
            "\"oam_first\":%u,\"oam_count\":%u,"
            "\"fragment_first\":%u,\"fragment_count\":%u}",
            (unsigned)source->record_address, (unsigned)source->tier,
            (unsigned)source->composition, (unsigned)source->type,
            (unsigned)source->semantic_state, (unsigned)source->world_x,
            (unsigned)source->world_y, (unsigned)source->oam_first,
            (unsigned)source->oam_count,
            (unsigned)source->fragment_first,
            (unsigned)source->fragment_count);
  }
  fputs("],\"objects\":[", g_sim_d1_trace);
  for (unsigned i = 0; i < frame->object_count; i++) {
    const SimRenderObject *object = &frame->objects[i];
    if (i) fputc(',', g_sim_d1_trace);
    fprintf(g_sim_d1_trace,
            "{\"record\":%u,\"source_index\":%u,\"tier\":%u,"
            "\"composition\":%u,\"priority\":%u,\"traits\":%u,"
            "\"height_class\":%u,\"height_class_name\":\"%s\","
            "\"virtual_height\":%d,\"classified_height\":%d,"
            "\"casts_shadow\":%s,\"color_math_eligible\":%s,"
            "\"oam_first\":%u,\"oam_count\":%u,"
            "\"foot_x\":%d,\"foot_y\":%d,\"atlas_valid\":%s,"
            "\"local_bounds\":[%d,%d,%d,%d],"
            "\"atlas\":[%u,%u,%u,%u]}",
            (unsigned)object->record_address,
            (unsigned)object->source_index, (unsigned)object->tier,
            (unsigned)object->composition, (unsigned)object->priority,
            (unsigned)object->traits,
            (unsigned)object->height_class,
            Sim3D_HeightClassName((SimHeightClass)object->height_class),
            (int)object->virtual_height, (int)object->classified_height,
            Sim3D_ObjectCastsShadow(object) ? "true" : "false",
            object->color_math_eligible ? "true" : "false",
            (unsigned)object->oam_first,
            (unsigned)object->oam_count, object->foot_x, object->foot_y,
            object->atlas_valid ? "true" : "false",
            object->local_x0, object->local_y0,
            object->local_x1, object->local_y1,
            (unsigned)object->atlas_x, (unsigned)object->atlas_y,
            (unsigned)object->atlas_w, (unsigned)object->atlas_h);
  }
  fputs("]}\n", g_sim_d1_trace);
  fflush(g_sim_d1_trace);
}

void SimRenderMetadata_TraceClose(void) {
  if (g_sim_d1_trace) fclose(g_sim_d1_trace);
  g_sim_d1_trace = NULL;
}
