#include "sim_render_metadata.h"

#include <math.h>
#include <string.h>

#include "actraiser_game.h"
#include "constants.h"
#include "sim_world_map.h"

_Static_assert(kSimMaxSourceRecords ==
                   kActRaiserSimFixedRecordCount +
                       kActRaiserSimWorldRecordCount,
               "effect/source tracker capacity must cover both SIM tiers");
_Static_assert(kSimMaxResolvedParts <= UINT8_MAX,
               "SimRenderObject part counters must hold the part capacity");

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
  uint16_t part_count;
  uint16_t synthetic_part_count;
  uint16_t synthetic_part_overflow_count;
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
  PpuObjPart parts[kSimMaxResolvedParts];
} SimMetadataProducer;

static SimMetadataProducer g_sim_metadata;

typedef struct SimEffectLifetime {
  bool active;
  bool visible;
  uint8_t kind;
  uint8_t phase;
  uint8_t color_family;
  uint32_t generation;
  uint32_t pulse_generation;
  uint32_t last_build_serial;
  uint16_t age_ticks;
  uint16_t phase_ticks;
  uint16_t pulse_ticks;
  uint16_t ticks_since_visible;
} SimEffectLifetime;

/* Fixed and world records are separate address spaces with overlapping local
 * indices. Keep both tiers in the same bounded tracker without letting fixed
 * slot N alias world slot N; current semantic emitters happen to be world
 * records, but fixed-tier effects remain representable without aliasing. */
static SimEffectLifetime g_effect_lifetimes[kSimMaxSourceRecords];
static uint32_t g_next_effect_generation;
static int RecordIndex(uint16_t record_address, bool world_record);
static int EffectLifetimeIndex(const SimSourceRecord *source);

static uint16_t SaturatingTick(uint16_t value) {
  return value == UINT16_MAX ? value : (uint16_t)(value + 1);
}

static uint32_t NextEffectGeneration(void) {
  if (++g_next_effect_generation == 0) ++g_next_effect_generation;
  return g_next_effect_generation;
}

static void ClearEffectLifetimes(void) {
  memset(g_effect_lifetimes, 0, sizeof(g_effect_lifetimes));
}

static void ResetEffectLifetimes(void) {
  ClearEffectLifetimes();
  g_next_effect_generation = 0;
}

/* D3c classification tables.  Every entry is transcribed from the locked
 * height policy in docs/sim-object-catalog.md; the renderer must never
 * rediscover a height from OAM attributes or pixel shape. */
enum {
  /* Record classes with a real 16-entry state table; all are flying. */
  kSimRecordClass_Angel = 0x0C,
  kSimRecordClass_EnemyFirst = 0x12,
  kSimRecordClass_BlueDragon = 0x12,
  kSimRecordClass_RedDemon = 0x14,
  kSimRecordClass_EnemyLast = 0x15,
  /* $01:B9EC state 6 is the Blue Dragon's building strike. The canonical
   * replay holds it for exactly the 33 frames that bracket every
   * $E1BD/$E209/$E255 bolt, and the ROM drops the record onto the target
   * itself, so the body must share the bolt's plane for those frames. */
  kSimRecordState_BlueDragonStrike = 6,
  kSimRecordState_RedDemonAttackFirst = 7,
  kSimRecordState_RedDemonAttackLast = 9,
  kSimComposition_BlueDragonBoltA = 0xE1BD,
  kSimComposition_BlueDragonBoltB = 0xE209,
  kSimComposition_BlueDragonBoltC = 0xE255,
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
  return composition == kSimComposition_BlueDragonBoltA ||
      composition == kSimComposition_BlueDragonBoltB ||
      composition == kSimComposition_BlueDragonBoltC;
}

typedef struct TownCreationLightningPhaseDesc {
  uint16_t composition;
  int16_t strike_y;
  uint8_t phase;
} TownCreationLightningPhaseDesc;

/* Spawn selector $0504 initializes script $01:A8BB. Live run 20260803-133014
 * shows the resulting actors as world-tier process $000E records whose raw
 * +$06 retains $A8BB. Each visible bolt is held for two ticks and followed by
 * two ticks of the deliberately offscreen $E527 sentinel. All four terminate
 * at local (8,80): the first pair end in one 8x8 tile at (4,76), while the
 * last pair end in a 16x16 impact block at (0,72). Their shared centre also
 * keeps particle tails stationary in gaps. */
static const TownCreationLightningPhaseDesc kTownCreationLightningPhases[] = {
  { 0xE9CC, 80, kSimEffectPhase_TownCreationBoltA },
  { 0xE527, 80, kSimEffectPhase_TownCreationGap },
  { 0xEA27, 80, kSimEffectPhase_TownCreationBoltB },
  { 0xEA82, 80, kSimEffectPhase_TownCreationBoltC },
  { 0xEAEC, 80, kSimEffectPhase_TownCreationBoltD },
};

static const TownCreationLightningPhaseDesc *FindTownCreationLightningPhase(
    uint16_t composition) {
  for (size_t i = 0;
       i < sizeof(kTownCreationLightningPhases) /
           sizeof(kTownCreationLightningPhases[0]);
       i++)
    if (kTownCreationLightningPhases[i].composition == composition)
      return &kTownCreationLightningPhases[i];
  return NULL;
}

static bool IsTownCreationLightningComposition(uint16_t composition) {
  const TownCreationLightningPhaseDesc *phase =
      FindTownCreationLightningPhase(composition);
  return phase && phase->phase != kSimEffectPhase_TownCreationGap;
}

typedef struct LightningMiraclePhaseDesc {
  uint16_t composition;
  int16_t strike_y;
  uint8_t phase;
} LightningMiraclePhaseDesc;

static const LightningMiraclePhaseDesc kLightningMiraclePhases[] = {
  { 0xD9E5, 64, kSimEffectPhase_LightningCloud },
  { 0xDA4B, 60, kSimEffectPhase_LightningLead },
  { 0xDAA1, 60, kSimEffectPhase_LightningBranch },
  { 0xDAF7, 64, kSimEffectPhase_LightningImpactA },
  { 0xDB5C, 64, kSimEffectPhase_LightningImpactB },
};

static const LightningMiraclePhaseDesc *FindLightningMiraclePhase(
    uint16_t composition) {
  for (size_t i = 0;
       i < sizeof(kLightningMiraclePhases) / sizeof(kLightningMiraclePhases[0]);
       i++)
    if (kLightningMiraclePhases[i].composition == composition)
      return &kLightningMiraclePhases[i];
  return NULL;
}

bool Sim3D_IsLightningMiracleComposition(uint16_t composition) {
  const LightningMiraclePhaseDesc *desc =
      FindLightningMiraclePhase(composition);
  return desc && desc->phase != kSimEffectPhase_LightningCloud;
}

const char *Sim3D_EffectKindName(SimEffectKind kind) {
  switch (kind) {
    case kSimEffect_None: return "none";
    case kSimEffect_LightningMiracle: return "lightning_miracle";
    case kSimEffect_BlueDragonLightning: return "blue_dragon_lightning";
    case kSimEffect_TownCreationLightning:
      return "town_creation_lightning";
    case kSimEffect_RedDemonFire: return "red_demon_fire";
    case kSimEffect_GroundFire: return "ground_fire";
    case kSimEffect_HouseFire: return "house_fire";
  }
  return "unknown";
}

const char *Sim3D_EffectPhaseName(SimEffectPhase phase) {
  switch (phase) {
    case kSimEffectPhase_None: return "none";
    case kSimEffectPhase_LightningCloud: return "lightning_cloud";
    case kSimEffectPhase_LightningLead: return "lightning_lead";
    case kSimEffectPhase_LightningBranch: return "lightning_branch";
    case kSimEffectPhase_LightningImpactA: return "lightning_impact_a";
    case kSimEffectPhase_LightningImpactB: return "lightning_impact_b";
    case kSimEffectPhase_BlueDragonAttack: return "blue_dragon_attack";
    case kSimEffectPhase_BlueDragonBoltA: return "blue_dragon_bolt_a";
    case kSimEffectPhase_BlueDragonBoltB: return "blue_dragon_bolt_b";
    case kSimEffectPhase_BlueDragonBoltC: return "blue_dragon_bolt_c";
    case kSimEffectPhase_TownCreationGap: return "town_creation_gap";
    case kSimEffectPhase_TownCreationBoltA: return "town_creation_bolt_a";
    case kSimEffectPhase_TownCreationBoltB: return "town_creation_bolt_b";
    case kSimEffectPhase_TownCreationBoltC: return "town_creation_bolt_c";
    case kSimEffectPhase_TownCreationBoltD: return "town_creation_bolt_d";
    case kSimEffectPhase_RedDemonAttack: return "red_demon_attack";
    case kSimEffectPhase_RedFireSmall: return "red_fire_small";
    case kSimEffectPhase_RedFireMedium: return "red_fire_medium";
    case kSimEffectPhase_RedFireLarge: return "red_fire_large";
    case kSimEffectPhase_GroundFireA: return "ground_fire_a";
    case kSimEffectPhase_GroundFireB: return "ground_fire_b";
    case kSimEffectPhase_GroundFireC: return "ground_fire_c";
    case kSimEffectPhase_HouseFireA: return "house_fire_a";
    case kSimEffectPhase_HouseFireB: return "house_fire_b";
    case kSimEffectPhase_HouseFireC: return "house_fire_c";
  }
  return "unknown";
}

const char *Sim3D_EffectColorName(SimEffectColorFamily color) {
  switch (color) {
    case kSimEffectColor_None: return "none";
    case kSimEffectColor_LightningBlue: return "lightning_blue";
    case kSimEffectColor_FireRed: return "fire_red";
    case kSimEffectColor_FireBlue: return "fire_blue";
  }
  return "unknown";
}

const char *Sim3D_EffectGeometryName(SimEffectGeometryKind kind) {
  switch (kind) {
    case kSimEffectGeometry_None: return "none";
    case kSimEffectGeometry_Point: return "point";
    case kSimEffectGeometry_Segment: return "segment";
    case kSimEffectGeometry_Area: return "area";
    case kSimEffectGeometry_Scene: return "scene";
  }
  return "unknown";
}

const char *Sim3D_EffectSpaceName(SimEffectGeometrySpace space) {
  switch (space) {
    case kSimEffectSpace_RecordLocal: return "record_local";
    case kSimEffectSpace_WorldLocal: return "world_local";
    case kSimEffectSpace_Screen: return "screen";
  }
  return "unknown";
}

static bool EffectPhaseVisible(SimEffectPhase phase) {
  switch (phase) {
    case kSimEffectPhase_LightningLead:
    case kSimEffectPhase_LightningBranch:
    case kSimEffectPhase_LightningImpactA:
    case kSimEffectPhase_LightningImpactB:
    case kSimEffectPhase_BlueDragonBoltA:
    case kSimEffectPhase_BlueDragonBoltB:
    case kSimEffectPhase_BlueDragonBoltC:
    case kSimEffectPhase_TownCreationBoltA:
    case kSimEffectPhase_TownCreationBoltB:
    case kSimEffectPhase_TownCreationBoltC:
    case kSimEffectPhase_TownCreationBoltD:
    case kSimEffectPhase_RedFireSmall:
    case kSimEffectPhase_RedFireMedium:
    case kSimEffectPhase_RedFireLarge:
    case kSimEffectPhase_GroundFireA:
    case kSimEffectPhase_GroundFireB:
    case kSimEffectPhase_GroundFireC:
    case kSimEffectPhase_HouseFireA:
    case kSimEffectPhase_HouseFireB:
    case kSimEffectPhase_HouseFireC:
      return true;
    case kSimEffectPhase_None:
    case kSimEffectPhase_LightningCloud:
    case kSimEffectPhase_BlueDragonAttack:
    case kSimEffectPhase_TownCreationGap:
    case kSimEffectPhase_RedDemonAttack:
      return false;
  }
  return false;
}

static void UpdateEffectLifetime(SimEffectLifetime *lifetime,
                                 uint32_t build_serial,
                                 SimEffectKind kind,
                                 SimEffectPhase phase,
                                 SimEffectColorFamily color_family,
                                 bool visible) {
  /* CaptureFrame may be asked for the same immutable producer build by a
   * screenshot or paused redraw. Never turn that into another effect tick. */
  if (lifetime->active && lifetime->last_build_serial == build_serial &&
      lifetime->kind == kind)
    return;

  bool continuous = lifetime->active && lifetime->kind == kind &&
      lifetime->last_build_serial + 1 == build_serial;
  if (!continuous) {
    *lifetime = (SimEffectLifetime){
      .active = true,
      .visible = visible,
      .kind = kind,
      .phase = phase,
      .color_family = color_family,
      .generation = NextEffectGeneration(),
      .pulse_generation = visible ? 1u : 0u,
      .last_build_serial = build_serial,
      .ticks_since_visible = visible ? 0 : UINT16_MAX,
    };
    return;
  }

  lifetime->age_ticks = SaturatingTick(lifetime->age_ticks);
  lifetime->phase_ticks = lifetime->phase == phase
      ? SaturatingTick(lifetime->phase_ticks) : 0;
  if (visible) {
    if (lifetime->visible) {
      lifetime->pulse_ticks = SaturatingTick(lifetime->pulse_ticks);
    } else {
      if (++lifetime->pulse_generation == 0)
        ++lifetime->pulse_generation;
      lifetime->pulse_ticks = 0;
    }
    lifetime->ticks_since_visible = 0;
  } else if (lifetime->pulse_generation) {
    lifetime->pulse_ticks = SaturatingTick(lifetime->pulse_ticks);
    lifetime->ticks_since_visible = lifetime->visible ? 1 :
        SaturatingTick(lifetime->ticks_since_visible);
  } else {
    lifetime->ticks_since_visible = UINT16_MAX;
  }
  lifetime->visible = visible;
  lifetime->phase = phase;
  if (color_family != kSimEffectColor_None)
    lifetime->color_family = color_family;
  lifetime->last_build_serial = build_serial;
}

typedef struct SimEffectCaptureDesc {
  SimEffectKind kind;
  SimEffectPhase phase;
  SimEffectColorFamily color_family;
  SimEffectGeometry geometry;
  uint8_t flags;
} SimEffectCaptureDesc;

static SimEffectGeometry EffectPointGeometry(int16_t x, int16_t y,
                                             int16_t height) {
  return (SimEffectGeometry){
    .kind = kSimEffectGeometry_Point,
    .space = kSimEffectSpace_RecordLocal,
    .data.point = { .x = x, .y = y, .height = height },
  };
}

static SimEffectPhase BlueDragonPhase(uint16_t composition, int16_t *strike_y) {
  *strike_y = 56;
  switch (composition) {
    case kSimComposition_BlueDragonBoltA:
      *strike_y = 52;
      return kSimEffectPhase_BlueDragonBoltA;
    case kSimComposition_BlueDragonBoltB:
      *strike_y = 52;
      return kSimEffectPhase_BlueDragonBoltB;
    case kSimComposition_BlueDragonBoltC:
      return kSimEffectPhase_BlueDragonBoltC;
  }
  return kSimEffectPhase_BlueDragonAttack;
}

static SimEffectPhase RedDemonPhase(uint16_t composition, int16_t *flame_y) {
  *flame_y = 20;
  switch (composition) {
    case 0xE340:
      *flame_y = 18;
      return kSimEffectPhase_RedFireSmall;
    case 0xE35A:
      return kSimEffectPhase_RedFireMedium;
    case 0xE383:
      *flame_y = 22;
      return kSimEffectPhase_RedFireLarge;
  }
  return kSimEffectPhase_RedDemonAttack;
}

static SimEffectPhase GroundFirePhase(uint16_t composition) {
  switch (composition) {
    case 0xE6CA: return kSimEffectPhase_GroundFireA;
    case 0xE6D0: return kSimEffectPhase_GroundFireB;
    case 0xE6D6: return kSimEffectPhase_GroundFireC;
  }
  return kSimEffectPhase_None;
}

static SimEffectPhase HouseFirePhase(uint16_t composition) {
  /* Script $01:A838. These are three complete 16x16 compositions, not a
   * pointer range: the six bytes between each address are part records. */
  switch (composition) {
    case 0xDD2D: return kSimEffectPhase_HouseFireA;
    case 0xDD33: return kSimEffectPhase_HouseFireB;
    case 0xDD39: return kSimEffectPhase_HouseFireC;
  }
  return kSimEffectPhase_None;
}

static SimEffectColorFamily GroundFireColor(
    const SimSourceRecord *source) {
  if (!source) return kSimEffectColor_None;
  /* These are exact live observations, not a palette-colour heuristic. CGRAM
   * is shared and contains both ramps in both captures; it is the palette
   * selected by each emitted OAM part that distinguishes the runtime art. A
   * mixed or absent mask is intentionally unsupported so an ambiguous record
   * cannot receive confidently wrong lighting. */
  if (source->obj_palette_mask == (1u << 1))
    return kSimEffectColor_FireRed;
  if (source->obj_palette_mask == (1u << 2))
    return kSimEffectColor_FireBlue;
  return kSimEffectColor_None;
}

static bool ClassifyEffectSource(const SimFrameData *frame,
                                 const SimSourceRecord *source,
                                 SimEffectCaptureDesc *desc) {
  if (!frame || !source || !desc) return false;

  /* Run 20260803-133014 proves the two creation bolts are world-tier process
   * records: +$0E is $000E and polymorphic +$06 retains script base $A8BB.
   * Requiring both fields lets $E527 keep one lifecycle through authored gaps
   * without turning the same sentinel in cursor lists 40-48 into lightning. */
  if (source->tier == kSimRecordTier_World && source->type == 0x000E &&
      source->record_word06 == 0xA8BB) {
    const TownCreationLightningPhaseDesc *phase =
        FindTownCreationLightningPhase(source->composition);
    if (!phase) return false;
    *desc = (SimEffectCaptureDesc){
      .kind = kSimEffect_TownCreationLightning,
      .phase = (SimEffectPhase)phase->phase,
      .color_family = kSimEffectColor_LightningBlue,
      .geometry = EffectPointGeometry(8, phase->strike_y, 0),
      .flags = kSimEffectFlag_RecordLifecycle,
    };
    return true;
  }

  if (source->tier != kSimRecordTier_World) return false;

  /* The scripted burning-house actors use class byte $01 and retain spawn
   * list $0A in the adjacent high byte, which the captured +$0E identity
   * publishes as $0A01. Requiring that packed identity as well as one of the
   * three exact $A838 frames avoids classifying unrelated list-10 setup art
   * or another class-$01 town actor as fire. */
  SimEffectPhase house_fire_phase = HouseFirePhase(source->composition);
  if (source->type == 0x0A01 &&
      house_fire_phase != kSimEffectPhase_None) {
    *desc = (SimEffectCaptureDesc){
      .kind = kSimEffect_HouseFire,
      .phase = house_fire_phase,
      .color_family = kSimEffectColor_FireRed,
      .geometry = EffectPointGeometry(8, 16, 0),
      .flags = kSimEffectFlag_RecordLifecycle,
    };
    return true;
  }

  bool miracle_lifecycle = frame->miracle_kind == 1 &&
      (frame->miracle_user_active || frame->miracle_posted_active);
  if (source->type == 0x02 && miracle_lifecycle) {
    const LightningMiraclePhaseDesc *phase =
        FindLightningMiraclePhase(source->composition);
    if (!phase) return false;
    *desc = (SimEffectCaptureDesc){
      .kind = kSimEffect_LightningMiracle,
      .phase = (SimEffectPhase)phase->phase,
      .color_family = kSimEffectColor_LightningBlue,
      .geometry = EffectPointGeometry(8, phase->strike_y, 0),
      .flags =
          (frame->miracle_user_active ? kSimEffectFlag_UserLifecycle : 0) |
          (frame->miracle_posted_active ?
              kSimEffectFlag_PostedLifecycle : 0) |
          (frame->miracle_visual_complete ?
              kSimEffectFlag_VisualComplete : 0) |
          (frame->miracle_actor_done ? kSimEffectFlag_ActorDone : 0),
    };
    return true;
  }

  if (source->type == kSimRecordClass_BlueDragon &&
      source->semantic_state == kSimRecordState_BlueDragonStrike) {
    int16_t strike_y;
    SimEffectPhase phase = BlueDragonPhase(source->composition, &strike_y);
    *desc = (SimEffectCaptureDesc){
      .kind = kSimEffect_BlueDragonLightning,
      .phase = phase,
      .color_family = kSimEffectColor_LightningBlue,
      .geometry = EffectPointGeometry(8, strike_y, 0),
      .flags = kSimEffectFlag_RecordLifecycle,
    };
    return true;
  }

  if (source->type == kSimRecordClass_RedDemon &&
      source->semantic_state >= kSimRecordState_RedDemonAttackFirst &&
      source->semantic_state <= kSimRecordState_RedDemonAttackLast) {
    int16_t flame_y;
    SimEffectPhase phase = RedDemonPhase(source->composition, &flame_y);
    *desc = (SimEffectCaptureDesc){
      .kind = kSimEffect_RedDemonFire,
      .phase = phase,
      .color_family = kSimEffectColor_FireRed,
      /* Red Demon attack art is attached to the class's proven 24px flight
       * plane. Renderer height scaling is applied later from the FrameSlot. */
      .geometry = EffectPointGeometry(
          8, flame_y, kSimVirtualHeight_Flying),
      .flags = kSimEffectFlag_RecordLifecycle,
    };
    return true;
  }

  SimEffectPhase fire_phase = GroundFirePhase(source->composition);
  if (fire_phase != kSimEffectPhase_None) {
    *desc = (SimEffectCaptureDesc){
      .kind = kSimEffect_GroundFire,
      .phase = fire_phase,
      .color_family = GroundFireColor(source),
      .geometry = EffectPointGeometry(8, 8, 0),
      .flags = kSimEffectFlag_RecordLifecycle,
    };
    return true;
  }
  return false;
}

static void CaptureEffectInstances(SimFrameData *dst) {
  dst->effect_metadata_valid = dst->metadata_valid;
  dst->effect_count = 0;
  dst->effect_visible_count = 0;
  dst->effect_overflow_count = 0;

  if (!dst->metadata_valid) {
    /* Source identity is no longer trustworthy. Retire every tracker rather
     * than accidentally joining a later valid record to an old generation. */
    ClearEffectLifetimes();
    return;
  }
  for (uint8_t i = 0; i < dst->source_count; i++) {
    const SimSourceRecord *source = &dst->sources[i];
    SimEffectCaptureDesc desc;
    if (!ClassifyEffectSource(dst, source, &desc)) continue;

    int record_index = EffectLifetimeIndex(source);
    if (record_index < 0) continue;
    bool visible = EffectPhaseVisible(desc.phase);
    SimEffectLifetime *lifetime = &g_effect_lifetimes[record_index];
    UpdateEffectLifetime(lifetime, dst->build_serial, desc.kind,
                         desc.phase, desc.color_family, visible);

    SimEffectInstance effect = {
      .generation = lifetime->generation,
      .pulse_generation = lifetime->pulse_generation,
      .record_address = source->record_address,
      .composition = source->composition,
      .world_x = source->world_x,
      .world_y = source->world_y,
      .age_ticks = lifetime->age_ticks,
      .phase_ticks = lifetime->phase_ticks,
      .pulse_ticks = lifetime->pulse_ticks,
      .ticks_since_visible = lifetime->ticks_since_visible,
      .geometry = desc.geometry,
      .source_index = i,
      .kind = desc.kind,
      .phase = desc.phase,
      .color_family = lifetime->color_family,
      .flags = desc.flags | (visible ? kSimEffectFlag_Visible : 0),
    };
    if (visible && dst->effect_visible_count != UINT8_MAX)
      dst->effect_visible_count++;
    if (dst->effect_count < kSimMaxEffectInstances) {
      dst->effects[dst->effect_count++] = effect;
    } else {
      if (dst->effect_overflow_count != UINT8_MAX)
        dst->effect_overflow_count++;
      dst->effect_metadata_valid = false;
    }
  }

  /* Outer lifecycle words and enemy states can outlast their visible actors.
   * Retire every slot absent from this immutable producer build, so immediate
   * record reuse cannot inherit an old kind or generation. */
  for (size_t i = 0;
       i < sizeof(g_effect_lifetimes) / sizeof(g_effect_lifetimes[0]); i++) {
    SimEffectLifetime *lifetime = &g_effect_lifetimes[i];
    if (lifetime->active &&
        lifetime->last_build_serial != dst->build_serial)
      memset(lifetime, 0, sizeof(*lifetime));
  }
}

static bool IsNapperPluckComposition(uint16_t composition) {
  return composition == 0xE71B || composition == 0xE73A ||
      composition == 0xE75E;
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
    result.traits = kSimObjectTrait_MapPlane | kSimObjectTrait_NoShadow |
        kSimObjectTrait_SelectionOverlay;
    return result;
  }
  /* The $0504 initializer creates world process-$000E town-creation bolts.
   * Their coordinates and art are authored against the town ground just like
   * the building-zap family. Exact composition starts only: the bytes between
   * $E9CC and $EAEC are part records, not additional identities. The effect
   * classifier separately requires raw +$06 == $A8BB for lifecycle identity. */
  if (IsTownCreationLightningComposition(composition)) {
    result.height_class = kSimHeightClass_GroundEffect;
    result.traits = kSimObjectTrait_RecordOriginAnchor |
        kSimObjectTrait_NoShadow;
    return result;
  }
  /* Fixed records are screen-relative UI/effects; they never gain a height,
   * an anchor policy, or a shadow unless an exact ground-owned family above
   * says otherwise. */
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
  if (IsBuildingZapComposition(composition)) {
    /* Lightning and struck-ground effects belong to the target tile, not to
     * the flying record that requested them. */
    result.height_class = kSimHeightClass_GroundEffect;
    result.traits = kSimObjectTrait_RecordOriginAnchor |
        kSimObjectTrait_NoShadow;
    return result;
  }
  if (GroundFirePhase(composition) != kSimEffectPhase_None) {
    result.height_class = kSimHeightClass_GroundEffect;
    result.traits = kSimObjectTrait_NoShadow;
    return result;
  }
  if (HouseFirePhase(composition) != kSimEffectPhase_None) {
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
  bool flying_actor = record_address == kActRaiserWram_SimAngelRecord ||
      type == kSimRecordClass_Angel ||
      (type >= kSimRecordClass_EnemyFirst &&
       type <= kSimRecordClass_EnemyLast);
  if (flying_actor) {
    result.height_class = kSimHeightClass_Flying;
    result.virtual_height = kSimVirtualHeight_Flying;
    /* This class is physically above the town, so terrain may never occlude
     * it. Classified contact states returned earlier remain in the terrain
     * depth sort while they are on or striking the ground. */
    result.traits |= kSimObjectTrait_Overhead;
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

static int EffectLifetimeIndex(const SimSourceRecord *source) {
  if (!source) return -1;
  bool world = source->tier == kSimRecordTier_World;
  int local = RecordIndex(source->record_address, world);
  if (local < 0) return -1;
  return world ? kActRaiserSimFixedRecordCount + local : local;
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
  ResetEffectLifetimes();
}

bool SimRenderMetadata_BeginRecord(
    uint16_t record_address, bool world_record, bool alternate_attributes,
    uint16_t composition, uint16_t world_x, uint16_t world_y,
    uint16_t type, uint16_t semantic_state, uint16_t status,
    uint16_t oam_cursor_before) {
  /* $01:ACD9 starts at cursor zero and visits each tier in ascending address
   * order.  <= (not merely <) also recognizes a one-record pass repeated on
   * the next emulated tick.  A clipped record followed by a later record at
   * the same zero cursor does not reset because its address increased. */
  bool began_build = !g_sim_metadata.active ||
      (oam_cursor_before == 0 &&
       record_address <= g_sim_metadata.last_record_address);
  if (began_build)
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
    return began_build;
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
  return began_build;
}

void SimRenderMetadata_RecordAnchor(int16_t base_x, int16_t base_y) {
  if (!g_sim_metadata.record_active) return;
  SimSourceRecord *source =
      &g_sim_metadata.sources[g_sim_metadata.current_source];
  source->anchor_x = base_x;
  source->anchor_y = base_y;
  source->anchor_valid = 1;
}

void SimRenderMetadata_RecordWord06(uint16_t value) {
  if (!g_sim_metadata.record_active) return;
  g_sim_metadata.sources[g_sim_metadata.current_source].record_word06 = value;
}

void SimRenderMetadata_RecordClippedPart(uint8_t reason) {
  if (!g_sim_metadata.record_active) return;
  SimSourceRecord *source =
      &g_sim_metadata.sources[g_sim_metadata.current_source];
  source->clip_reason |= reason;
  if (source->clipped_parts < 0xFFFF) source->clipped_parts++;
}

static SimRenderObject *RecordObjectForPart(uint16_t slot,
                                            uint16_t attributes,
                                            bool oam_backed) {
  SimSourceRecord *source =
      &g_sim_metadata.sources[g_sim_metadata.current_source];
  uint8_t priority = (uint8_t)((attributes >> 12) & 3);
  uint8_t color_math_eligible = (attributes & 0x0800) != 0;
  SimRenderObject *prior = g_sim_metadata.object_count
      ? &g_sim_metadata.objects[g_sim_metadata.object_count - 1] : NULL;
  if (prior && prior->source_index == g_sim_metadata.current_source &&
      prior->priority == priority &&
      prior->color_math_eligible == color_math_eligible &&
      (!oam_backed || prior->oam_first + prior->oam_count == slot))
    return prior;

  if (g_sim_metadata.object_count >= kSimMaxRenderObjects) {
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_Overflow;
    return NULL;
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
    .part_first = g_sim_metadata.part_count,
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
  return object;
}

static bool AppendExactPart(SimRenderObject *object,
                            const PpuObjPart *part, bool synthetic) {
  if (!object || !part || !part->size ||
      object->part_first + object->part_count != g_sim_metadata.part_count) {
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_PartContract;
    return false;
  }
  if (g_sim_metadata.part_count >= kSimMaxResolvedParts) {
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_Overflow;
    if (synthetic && g_sim_metadata.synthetic_part_overflow_count < UINT16_MAX)
      g_sim_metadata.synthetic_part_overflow_count++;
    return false;
  }
  g_sim_metadata.parts[g_sim_metadata.part_count++] = *part;
  object->part_count++;
  if (synthetic) object->synthetic_part_count++;
  return true;
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
  uint8_t obj_palette = (uint8_t)((attributes >> 9) & 7);
  if (g_sim_metadata.claimed_oam[slot])
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_Overlap;
  else {
    g_sim_metadata.claimed_oam[slot] = 1;
    g_sim_metadata.claimed_oam_count++;
  }
  g_sim_metadata.emitted_oam_count++;
  source->oam_count++;
  source->obj_palette_mask |= (uint8_t)(1u << obj_palette);

  if (source->tier == kSimRecordTier_World) {
    if (!g_sim_metadata.world_started) {
      g_sim_metadata.world_started = true;
      g_sim_metadata.world_oam_first = (uint8_t)slot;
    }
    g_sim_metadata.world_emitted_count++;
  } else if (g_sim_metadata.world_started) {
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_WorldSuffix;
  }

  SimRenderObject *object = RecordObjectForPart(slot, attributes, true);
  if (object) object->oam_count++;
}

void SimRenderMetadata_RecordExactOamPart(const PpuObjPart *part) {
  if (!g_sim_metadata.record_active || !g_sim_metadata.object_count || !part) {
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_PartContract;
    return;
  }
  SimRenderObject *object =
      &g_sim_metadata.objects[g_sim_metadata.object_count - 1];
  if (object->source_index != g_sim_metadata.current_source ||
      object->priority != (uint8_t)((part->tile_attr >> 12) & 3) ||
      object->color_math_eligible != ((part->tile_attr & 0x0800) != 0)) {
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_PartContract;
    return;
  }
  AppendExactPart(object, part, false);
}

void SimRenderMetadata_RecordSyntheticPart(uint16_t oam_cursor,
                                           const PpuObjPart *part) {
  if (!g_sim_metadata.record_active || !part || !part->size ||
      (oam_cursor & 3) || oam_cursor > kActRaiserOamLowTableBytes) {
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_PartContract;
    return;
  }
  if (g_sim_metadata.part_count >= kSimMaxResolvedParts) {
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_Overflow;
    if (g_sim_metadata.synthetic_part_overflow_count < UINT16_MAX)
      g_sim_metadata.synthetic_part_overflow_count++;
    return;
  }
  SimSourceRecord *source =
      &g_sim_metadata.sources[g_sim_metadata.current_source];
  SimRenderObject *object = RecordObjectForPart(
      (uint16_t)(oam_cursor / 4), part->tile_attr, false);
  if (!object) return;
  if (!AppendExactPart(object, part, true)) return;
  uint8_t obj_palette = (uint8_t)((part->tile_attr >> 9) & 7);
  source->obj_palette_mask |= (uint8_t)(1u << obj_palette);
  if (source->synthetic_parts < UINT16_MAX) source->synthetic_parts++;
  if (g_sim_metadata.synthetic_part_count < UINT16_MAX)
    g_sim_metadata.synthetic_part_count++;
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
  out->part_count = g_sim_metadata.part_count;
  memcpy(out->objects, g_sim_metadata.objects,
         sizeof(SimRenderObject) * out->object_count);
  memcpy(out->parts, g_sim_metadata.parts,
         sizeof(PpuObjPart) * out->part_count);
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
  /* SimRenderAtlas_Build owns packing, descriptor bounds and overlap. Commit
   * only closes the matching build transaction and publishes its result; a
   * second O(n^2) verification here used to re-prove the builder's output. */
  bool dimensions_valid = atlas_valid && atlas_width && atlas_height &&
      atlas_used_width <= atlas_width && atlas_used_height <= atlas_height;
  if (!dimensions_valid &&
      !(g_sim_metadata.integrity_flags & atlas_failures))
    g_sim_metadata.integrity_flags |= kSimMetadataIntegrity_AtlasRasterFailure;

  g_sim_metadata.atlas_valid = dimensions_valid &&
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
 * copy, so the presentation path still reads one self-contained snapshot. */
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
   * sizes -- roughly 496 wide against 224 tall -- so an inset the horizontal
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
                          int margin_left, int margin_right,
                          int margin_top, int margin_bottom,
                          int lead, int corner, int lift_inset) {
  if (lead <= 0) lead = 1;

  /* The complete host-renderable window. Real OAM remains vertically
   * authentic, but exact synthetic parts provide the explicit top/bottom
   * reach, so the cues must follow those margins rather than the byte decode. */
  float x0 = (float)(-margin_left);
  float x1 = (float)(kSimSpriteWindowBiasedWidth + margin_right);
  float y0 = (float)(-margin_top);
  float y1 = (float)(kSimSpriteWindowBiasedHeight + margin_bottom);
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
                            int margin_left, int margin_right,
                            int margin_top, int margin_bottom,
                            int lead, int corner, int lift_inset) {
  if (!source || !source->anchor_valid) return 0.0f;

  /* World-tier records only. Fixed-tier records are HUD and cursor furniture
   * that lives in screen space; it does not belong to the town and a cloud
   * over it would be nonsense. */
  if (source->tier != kSimRecordTier_World) return 0.0f;

  /* A record with no parts at all never asked to be drawn. Only the sprite
   * window may create cover, so a record that emitted nothing AND was never
   * clipped is the game's own decision, not ours to hide. */
  if (!source->oam_count && !source->synthetic_parts &&
      !source->clipped_parts)
    return 0.0f;

  /* Deliberately the record's own anchor, NOT where the renderer draws it.
   * See Sim3D_SourceDrawLift: when the cover arrives and where it goes are
   * two different questions, and this is the first one. */
  return Sim3D_CullProximity(source->anchor_x, source->anchor_y,
                             margin_left, margin_right,
                             margin_top, margin_bottom,
                             lead, corner, lift_inset);
}

int16_t Sim3D_MaxDrawLift(unsigned height_scale_x100) {
  long lift = (long)kSimVirtualHeight_Flying * (long)height_scale_x100 /
      kPercentScale;
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
      (long)height_scale_x100 / kPercentScale;
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
                  kSimFeature_RimLight | kSimFeature_EffectLighting |
                  kSimFeature_Particles);

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
    features &= ~(kSimFeature_WorldUnderlay | kSimFeature_EffectLighting |
                  kSimFeature_Particles);
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

static void CaptureWorldNavigationState(SimFrameData *dst,
                                        const uint8 *wram) {
  dst->world_navigation_state_valid = true;
  SimWorldNavigationFrame *navigation = &dst->world_navigation;
  navigation->focus_x =
      ReadMirror16(wram, kActRaiserWram_WorldFocusX);
  navigation->focus_y =
      ReadMirror16(wram, kActRaiserWram_WorldFocusY);
  for (int i = 0; i < 4; i++) {
    navigation->matrix[i] = (int16_t)ReadMirror16(
        wram, kActRaiserWram_WorldMatrixA + i * 2);
    navigation->next_matrix[i] = (int16_t)ReadMirror16(
        wram, kActRaiserWram_WorldNextMatrixA + i * 2);
  }
  navigation->rotation =
      ReadMirror16(wram, kActRaiserWram_WorldRotation);
  navigation->zoom_current =
      ReadMirror16(wram, kActRaiserWram_WorldZoomCurrent);
  navigation->zoom_target =
      ReadMirror16(wram, kActRaiserWram_WorldZoomTarget);
  navigation->active_location = wram[kActRaiserWram_WorldLocation];
}

void SimRenderMetadata_CaptureFrame(
    SimFrameData *dst, const uint8 *wram, bool town_master_enabled,
    bool world_navigation_enabled,
    SimRenderFeatureMask requested_features,
    uint32_t diagnostic_layer_mask,
    SimRenderFeatureMask implemented_features) {
  if (!dst) return;
  memset(dst, 0, sizeof(*dst));
  /* Zero is a deliberate "ground everything" tuning value, so a frame that is
   * never annotated must still default to the catalogue heights. */
  dst->height_scale_x100 = kPercentScale;
  dst->requested_features = requested_features & kSimFeature_All;
  dst->diagnostic_layer_mask = diagnostic_layer_mask;
  if (!wram) return;

  uint8_t map_group = wram[kActRaiserWram_MapGroup];
  uint8_t map_number = wram[kActRaiserWram_CurrentMap];
  bool town = ActRaiser_IsSimulationTown(map_group, map_number);
  bool world_navigation =
      map_group == kActRaiserMapGroup_NonAction &&
      map_number == kActRaiserNonActionMap_WorldMap;
  dst->master_enabled = town ? town_master_enabled
      : world_navigation ? world_navigation_enabled : false;
  dst->town = town ? map_number : 0;
  int underlay_x = 0, underlay_y = 0;
  if (town && SimWorldMap_DevelopedAvailable() &&
      SimWorldMap_OriginForTown(map_number, &underlay_x, &underlay_y)) {
    dst->underlay_serial = SimWorldMap_Serial();
    dst->underlay_origin_tile_x = (uint8_t)underlay_x;
    dst->underlay_origin_tile_y = (uint8_t)underlay_y;
  } else if (world_navigation && SimWorldMap_DevelopedAvailable()) {
    /* The navigation scene consumes the complete map, so its origin is zero. */
    dst->underlay_serial = SimWorldMap_Serial();
  }
  dst->game_frame = ReadMirror16(wram, kActRaiserWram_GameFrame);
  dst->camera_x = ReadMirror16(wram, kActRaiserWram_Bg1CameraX);
  dst->camera_y = ReadMirror16(wram, kActRaiserWram_Bg1CameraY);
  dst->picker_flag =
      ReadMirror16(wram, kActRaiserWram_SimMapPickerFlag);
  dst->miracle_kind =
      ReadMirror16(wram, kActRaiserWram_SimMiracleKind);
  dst->miracle_user_active =
      ReadMirror16(wram, kActRaiserWram_SimUserMiracleActive);
  dst->miracle_posted_active =
      ReadMirror16(wram, kActRaiserWram_SimPostedMiracleActive);
  dst->miracle_visual_complete =
      ReadMirror16(wram, kActRaiserWram_SimMiracleVisualComplete);
  dst->miracle_actor_done =
      ReadMirror16(wram, kActRaiserWram_SimMiracleActorDone);
  if (world_navigation) CaptureWorldNavigationState(dst, wram);

  if (town) {
    for (int i = 0; i < kActRaiserSimWorldRecordCount; i++) {
      uint32_t address = kActRaiserWram_SimWorldRecords +
          i * kActRaiserSimWorldRecordStride;
      if (ReadMirror16(wram, address) != 0)
        dst->world_record_occupancy++;
    }
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
#if AR_SIM3D_PICKER_TOPDOWN
    /* An active position picker owns the frame: the authentic flat view is
     * pixel- and input-identical to the original game by construction. */
    if (dst->picker_flag)
      dst->view = kSimView_AuthenticPicker;
    else
#endif
      /* Broken object metadata deliberately does NOT drop the view. The
       * ground, projection and camera have separate capture gates; only the
       * sprites depend on this metadata. */
      dst->view = kSimView_Enhanced;
  } else if (!world_navigation || !world_navigation_enabled) {
    dst->view = kSimView_None;
  } else if (!SimWorldMap_DevelopedAvailable()) {
    dst->view = kSimView_AuthenticFallback;
  } else if (!SimWorldNavigationScene_Build(
                 &dst->world_navigation_scene, &dst->world_navigation,
                 dst->underlay_serial)) {
    /* A singular camera transform cannot produce a complete host plane.
     * Never expose a half-configured scene; keep the authentic renderer for
     * this frame and let a later valid matrix recover automatically. */
    dst->view = kSimView_AuthenticFallback;
  } else {
    dst->view = kSimView_WorldNavigation;
  }

  /* Leaving town retires every live emitter, but generation stays monotonic so
   * an old queued FrameSlot can never alias a newly allocated record on re-entry. */
  if (!town) ClearEffectLifetimes();

  /* The semantic record producer describes simulation-town records only.
   * Never leak its last town build into a $09 frame: navigation OAM has a
   * separate Palace/UI contract and will be captured explicitly in Step 4. */
  if (town) {
    dst->emitted_oam_count = g_sim_metadata.emitted_oam_count;
    dst->claimed_oam_count = g_sim_metadata.claimed_oam_count;
    dst->synthetic_part_count = g_sim_metadata.synthetic_part_count;
    dst->synthetic_part_overflow_count =
        g_sim_metadata.synthetic_part_overflow_count;
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
    CaptureEffectInstances(dst);
  }
  ApplyHeightSlew(dst, dst->master_enabled);

  dst->effective_features = Sim3D_ResolveFeatureMask(
      dst->requested_features, implemented_features, dst->view,
      dst->master_enabled, dst->metadata_valid);
  if (!dst->effect_metadata_valid)
    dst->effective_features &= ~(kSimFeature_EffectLighting |
                                 kSimFeature_Particles);
}

const char *Sim3D_ViewName(SimViewKind view) {
  switch (view) {
    case kSimView_None: return "none";
    case kSimView_Enhanced: return "enhanced";
    case kSimView_WorldNavigation: return "world_navigation";
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
