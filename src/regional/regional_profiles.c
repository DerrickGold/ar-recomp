#include "regional_profiles.h"

#include <string.h>

/* Semantic groups are shared with the overlay, but this inventory owns only
 * rule membership. Activation and native adapters stay in their own layers. */

static const char *const kKeys[] = {
    [kArRegionalProfile_Stage] = "stage",
    [kArRegionalProfile_Combat] = "combat",
    [kArRegionalProfile_Magic] = "magic",
    [kArRegionalProfile_Lives] = "lives",
    [kArRegionalProfile_Development] = "development",
    [kArRegionalProfile_Population] = "population",
    [kArRegionalProfile_Lairs] = "lairs",
    [kArRegionalProfile_Resources] = "resources",
    [kArRegionalProfile_Interaction] = "interaction",
    [kArRegionalProfile_Arrival] = "arrival",
    [kArRegionalProfile_Artwork] = "artwork",
    [kArRegionalProfile_Music] = "music",
    [kArRegionalProfile_Difficulty] = "difficulty",
    [kArRegionalProfile_Gameplay] = "gameplay",
    [kArRegionalProfile_Presentation] = "presentation",
};
_Static_assert(sizeof(kKeys) / sizeof(kKeys[0]) == kArRegionalProfile_Count, "profile keys");
uint16_t ArRegionalProfiles_Mask(ArRegionalProfileGroup group) {
  if ((unsigned)group < kArRegionalProfile_GroupCount) return (uint16_t)(1u << group);
  if (group == kArRegionalProfile_Gameplay)
    return ((1u << kArRegionalProfile_Artwork) - 1) | (1u << kArRegionalProfile_Difficulty);
  if (group == kArRegionalProfile_Presentation)
    return (1u << kArRegionalProfile_Artwork) | (1u << kArRegionalProfile_Music);
  return 0;
}
const char *ArRegionalProfiles_Key(ArRegionalProfileGroup group) {
  return (unsigned)group < kArRegionalProfile_Count ? kKeys[group] : "";
}

ArRegionalTownImpact ArRegionalProfiles_TownImpact(ArRegionalProfileGroup group) {
  switch (group) {
    case kArRegionalProfile_Population:
    case kArRegionalProfile_Gameplay: return kArRegionalTownImpact_Redevelopment;
    case kArRegionalProfile_Development:
    case kArRegionalProfile_Lairs:
    case kArRegionalProfile_Resources: return kArRegionalTownImpact_Future;
    default: return kArRegionalTownImpact_None;
  }
}

bool ArRegionalProfiles_RefreshCache(ArRegionalProfileCache *cache,
                                     const ArRegionalRules *requested,
                                     const ArRegionalRules *effective) {
  if (!cache || !requested || !effective) return false;
  if (cache->valid && !memcmp(&cache->requested, requested, sizeof(*requested)) &&
      !memcmp(&cache->effective, effective, sizeof(*effective)))
    return true;

  ArRegionalProfileCache next = {0};
  if (!ArRegionalProfiles_Describe(requested, next.profiles) ||
      !ArRegionalProfiles_Describe(effective, next.active_profiles) ||
      !ArRegionalProfiles_Changes(requested, effective, &next.pending_groups))
    return false;
  next.requested = *requested;
  next.effective = *effective;
  next.valid = true;
  *cache = next;
  return true;
}

typedef struct Visit {
  uint16_t expand, changes;
  ArRegionalSource source;
  const ArRegionalRules *other;
  ArRegionalProfileSummary summary[kArRegionalProfile_Count];
  ArRegionalSource uniform[kArRegionalProfile_Count];
  ArRegionalSourceSelector select;
  void *context;
} Visit;

static bool VisitRegionalSource(Visit *visit, ArRegionalSource *field, ArRegionalSource other,
                                ArRegionalProfileGroup group,
                                const uint16_t values[kArRegionalSource_Count],const char *key) {
  if ((unsigned)*field >= kArRegionalSource_Count ||
      (visit->other && (unsigned)other >= kArRegionalSource_Count))
    return false;
  const unsigned scopes[] = {group, (ArRegionalProfiles_Mask(kArRegionalProfile_Gameplay) & (1u << group))
                                        ? kArRegionalProfile_Gameplay
                                        : kArRegionalProfile_Presentation};
  for (unsigned i = 0; i < 2; ++i) {
    ArRegionalProfileSummary *s = &visit->summary[scopes[i]];
    if (!s->members) {
      s->matching_sources = 7;
      visit->uniform[scopes[i]] = *field;
    } else if (visit->uniform[scopes[i]] != *field)
      visit->uniform[scopes[i]] = kArRegionalSource_Count;
    ++s->members;
    for (unsigned source = 0; source < kArRegionalSource_Count; ++source)
      if (values[*field] != values[source]) s->matching_sources &= (uint8_t)~(1u << source);
  }
  if (visit->other && values[*field] != values[other]) visit->changes |= (uint16_t)(1u << group);
  if (visit->expand & (1u << group)) *field = visit->source;
  if(visit->select) {
    *field=visit->select(visit->context,key,group,*field);
    if((unsigned)*field>=kArRegionalSource_Count)return false;
  }
  return true;
}

/* One typed ownership inventory serves expansion, Custom detection and
 * pending-state comparison. Values come from the consumer descriptors, not
 * copies of the regional tables or save-codec ordinals. */
static bool VisitProfileMembers(ArRegionalRules *rules, Visit *visit) {
  if ((unsigned)rules->difficulty.level >= kArRegionalDifficulty_Count ||
      (visit->other && (unsigned)visit->other->difficulty.level >= kArRegionalDifficulty_Count))
    return false;
#define FIELD(group, member, values, key)                                                       \
  do {                                                                                          \
    if (!VisitRegionalSource(visit, &rules->member,                                             \
                             visit->other ? visit->other->member : kArRegionalSource_US, group, \
                             values,key))                                                       \
      return false;                                                                             \
  } while (0)
#define FAMILY(group, member, count, type, getter, values) \
  do {                                                     \
    for (unsigned i = 0; i < count; ++i) {                 \
      const type *d = getter(i);                           \
      if (!d) return false;                                \
      FIELD(group, member.source[i], d->values, d->key);   \
    }                                                      \
  } while (0)
#define SINGLE(group, member, getter, values) FIELD(group, member, getter()->values,getter()->key)
  /* Action: stage design, combat, magic, lives and scoring. */
  SINGLE(kArRegionalProfile_Stage, terrain, ArRegionalTerrain_Descriptor, profile);
  FIELD(kArRegionalProfile_Stage, placements.enemies,
        ArRegionalPlacements_Descriptor(kArRegionalPlacement_Enemies)->profile,
        ArRegionalPlacements_Descriptor(kArRegionalPlacement_Enemies)->key);
  FIELD(kArRegionalProfile_Stage, placements.pickups,
        ArRegionalPlacements_Descriptor(kArRegionalPlacement_Pickups)->profile,
        ArRegionalPlacements_Descriptor(kArRegionalPlacement_Pickups)->key);
  FAMILY(kArRegionalProfile_Stage, timers, kArRegionalTimerRule_Count, ArRegionalTimerDescriptor,
         ArRegionalTimers_Descriptor, bcd);
  for (unsigned i = 0; i < kArRegionalDifficultyRule_Count; ++i) {
    const ArRegionalDifficultyDescriptor *d = ArRegionalDifficulty_Descriptor(i);
    FIELD(kArRegionalProfile_Difficulty, difficulty.source[i], d->value,d->key);
  }
  SINGLE(kArRegionalProfile_Combat, hazards, ArRegionalHazards_Descriptor, profile);
  SINGLE(kArRegionalProfile_Combat, statue_volley, ArRegionalVolley_Descriptor, shots);
  FAMILY(kArRegionalProfile_Combat, action_motion, kArRegionalActionMotion_Count,
         ArRegionalActionMotionDescriptor, ArRegionalActionMotion_Descriptor, value);
  FAMILY(kArRegionalProfile_Combat, emitters, kArRegionalEmitter_Count, ArRegionalEmitterDescriptor,
         ArRegionalEmitter_Descriptor, value);
  FAMILY(kArRegionalProfile_Combat, bosses, kArRegionalBoss_Count, ArRegionalBossDescriptor,
         ArRegionalBoss_Descriptor, value);
  FAMILY(kArRegionalProfile_Combat, collision, kArRegionalCollision_Count,
         ArRegionalCollisionDescriptor, ArRegionalCollision_Descriptor, japanese);
  FAMILY(kArRegionalProfile_Combat, platform_skull, kArRegionalPlatformSkull_Count,
         ArRegionalPlatformSkullDescriptor, ArRegionalPlatformSkull_Descriptor, value);
  FAMILY(kArRegionalProfile_Combat, actor_stats, kArRegionalActorStat_Count,
         ArRegionalActorStatDescriptor, ArRegionalActorStats_Descriptor, value);
  FAMILY(kArRegionalProfile_Combat, fire_enemy, kArRegionalFire_Count, ArRegionalFireDescriptor,
         ArRegionalFire_Descriptor, value);
  /* Shared prices: scrolls belong to Magic; miracles to Town resources. */
  for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i) {
    const ArRegionalCostDescriptor *d = ArRegionalCosts_Descriptor(i);
    FIELD(d->group == kArRegionalCostGroup_Scrolls ? kArRegionalProfile_Magic
                                                   : kArRegionalProfile_Resources,
          costs.source[i], d->price,d->key);
  }
  SINGLE(kArRegionalProfile_Magic, spell_inventory, ArRegionalInventory_Descriptor, enabled);
  FAMILY(kArRegionalProfile_Magic, cast_hold, kArRegionalCastHold_Count,
         ArRegionalCastHoldDescriptor, ArRegionalCastHold_Descriptor, enabled);
  SINGLE(kArRegionalProfile_Lives, retry_score, ArRegionalRetry_Descriptor, clear_score);
  SINGLE(kArRegionalProfile_Lives, score_lives, ArRegionalScoreLives_Descriptor, enabled);
  FAMILY(kArRegionalProfile_Lives, action_start, kArRegionalActionStart_Count,
         ArRegionalActionStartDescriptor, ArRegionalActionStart_Descriptor, value);
  FAMILY(kArRegionalProfile_Lives, mode_entry, kArRegionalMode_Count, ArRegionalModeDescriptor,
         ArRegionalMode_Descriptor, value);
  /* Towns: development, population, monsters/lairs and resources. */
  SINGLE(kArRegionalProfile_Development, town_wait, ArRegionalTownWait_Descriptor, updates);
  SINGLE(kArRegionalProfile_Development, fishing, ArRegionalFishing_Descriptor, updates);
  SINGLE(kArRegionalProfile_Development, construction, ArRegionalConstruction_Descriptor, japanese);
  FAMILY(kArRegionalProfile_Development, development, kArRegionalDevelopmentRule_Count,
         ArRegionalDevelopmentDescriptor, ArRegionalDevelopment_Descriptor, updates);
  SINGLE(kArRegionalProfile_Population, level_goals, ArRegionalLevelGoals_Descriptor, japanese);
  FAMILY(kArRegionalProfile_Population, support, kArRegionalSupport_Count,
         ArRegionalSupportDescriptor, ArRegionalSupport_Descriptor, amount);
  FAMILY(kArRegionalProfile_Population, story, kArRegionalStory_Count, ArRegionalStoryDescriptor,
         ArRegionalStory_Descriptor, value);
  FAMILY(kArRegionalProfile_Population, town_status, kArRegionalTownStatus_Count,
         ArRegionalTownStatusDescriptor, ArRegionalTownStatus_Descriptor, japanese);
  for (unsigned i = 0; i < kArRegionalLairCount; ++i) {
    FIELD(kArRegionalProfile_Lairs, lair_seeds, ArRegionalLair_SeedDescriptor(i)->stock,"lair_stock");
    uint16_t reload[kArRegionalSource_Count];
    for (unsigned source = 0; source < kArRegionalSource_Count; ++source)
      if (!ArRegionalLair_Reload(source, i, &reload[source])) return false;
    FIELD(kArRegionalProfile_Lairs, lair_reloads, reload,"lair_reload_japanese");
  }
  FAMILY(kArRegionalProfile_Lairs, sim_combat, kArRegionalSimCombat_Count,
         ArRegionalSimCombatDescriptor, ArRegionalSimCombat_Descriptor, value);
  FAMILY(kArRegionalProfile_Lairs, sim_ai, kArRegionalSimAi_Count, ArRegionalSimAiDescriptor,
         ArRegionalSimAi_Descriptor, value);
  SINGLE(kArRegionalProfile_Resources, house_credit, ArRegionalHouseCredit_Descriptor, tiered);
  SINGLE(kArRegionalProfile_Resources, skull_wait, ArRegionalSkullWait_Descriptor, frames);
  FAMILY(kArRegionalProfile_Resources, recovery, kArRegionalRecovery_Count,
         ArRegionalRecoveryDescriptor, ArRegionalRecovery_Descriptor, value);
  FAMILY(kArRegionalProfile_Resources, quake, kArRegionalQuake_Count, ArRegionalQuakeDescriptor,
         ArRegionalQuake_Descriptor, random);
  FAMILY(kArRegionalProfile_Resources, score_feedback, kArRegionalScore_Count,
         ArRegionalScoreDescriptor, ArRegionalScore_Descriptor, japanese);
  FAMILY(kArRegionalProfile_Resources, sources, kArRegionalSourceItem_Count,
         ArRegionalSourcesDescriptor, ArRegionalSources_Descriptor, automatic);
  /* Controls & menus, then final island arrival. */
  SINGLE(kArRegionalProfile_Interaction, magic_gesture, ArRegionalMagicGesture_Descriptor,
         up_attack);
  SINGLE(kArRegionalProfile_Interaction, lives_display, ArRegionalLivesDisplay_Descriptor,
         zero_based);
  SINGLE(kArRegionalProfile_Interaction, score_page, ArRegionalScorePage_Descriptor, enabled);
  SINGLE(kArRegionalProfile_Interaction, menu_return, ArRegionalMenuReturn_Descriptor, keep_open);
  SINGLE(kArRegionalProfile_Interaction, speed_range, ArRegionalSpeedRange_Descriptor, maximum);
  SINGLE(kArRegionalProfile_Arrival, arrival, ArRegionalArrival_Descriptor, japanese);
  /* Presentation: regional artwork and music, independent of gameplay. */
  SINGLE(kArRegionalProfile_Artwork, mosaic, ArRegionalMosaic_Descriptor, profile);
  FAMILY(kArRegionalProfile_Artwork, poses, kArRegionalPose_Count, ArRegionalPoseDescriptor,
         ArRegionalPoses_Descriptor, enabled);
  FAMILY(kArRegionalProfile_Artwork, artwork, kArRegionalArtwork_Count, ArRegionalArtworkDescriptor,
         ArRegionalArtwork_Descriptor, enabled);
  FAMILY(kArRegionalProfile_Artwork, actor_artwork, kArRegionalActorArtwork_Count,
         ArRegionalArtworkDescriptor, ArRegionalActorArtwork_Descriptor, enabled);
  SINGLE(kArRegionalProfile_Music, music, ArRegionalMusic_Descriptor, profile);
  FAMILY(kArRegionalProfile_Music, sequences, kArRegionalSequence_Count, ArRegionalMusicDescriptor,
         ArRegionalSequences_Descriptor, profile);
#undef SINGLE
#undef FAMILY
#undef FIELD
  return true;
}

bool ArRegionalProfiles_Expand(const ArRegionalRules *current, ArRegionalProfileGroup group,
                               ArRegionalSource source, ArRegionalRules *out) {
  if (!current || !out || (unsigned)source >= kArRegionalSource_Count) return false;
  Visit visit = {.expand = ArRegionalProfiles_Mask(group), .source = source};
  if (!visit.expand) return false;
  ArRegionalRules candidate = *current;
  if (!VisitProfileMembers(&candidate, &visit)) return false;
  if (group == kArRegionalProfile_Gameplay)
    candidate.difficulty.level = kArRegionalDifficulty_Normal;
  *out = candidate;
  return true;
}
bool ArRegionalProfiles_SelectSources(const ArRegionalRules *current,
    ArRegionalSourceSelector select,void *context,ArRegionalRules *out) {
  if(!current || !select || !out)return false;
  ArRegionalRules candidate=*current;
  Visit visit={.select=select,.context=context};
  if(!VisitProfileMembers(&candidate,&visit))return false;
  *out=candidate;return true;
}
bool ArRegionalProfiles_Describe(const ArRegionalRules *rules,
                                 ArRegionalProfileSummary out[kArRegionalProfile_Count]) {
  if (!rules || !out) return false;
  Visit visit = {0};
  ArRegionalRules copy = *rules;
  if (!VisitProfileMembers(&copy, &visit)) return false;
  for (unsigned i = 0; i < kArRegionalProfile_Count; ++i) {
    ArRegionalProfileSummary *s = &visit.summary[i];
    s->source = kArRegionalSource_Count;
    if ((unsigned)visit.uniform[i] < kArRegionalSource_Count)
      s->source = visit.uniform[i];
    else
      for (unsigned source = 0; source < kArRegionalSource_Count; ++source)
        if (s->matching_sources & (1u << source)) {
          s->source = source;
          break;
        }
  }
  memcpy(out, visit.summary, sizeof(visit.summary));
  return true;
}
bool ArRegionalProfiles_Changes(const ArRegionalRules *a, const ArRegionalRules *b,
                                uint16_t *groups) {
  if (!a || !b || !groups) return false;
  Visit visit = {.other = b};
  ArRegionalRules copy = *a;
  if (!VisitProfileMembers(&copy, &visit)) return false;
  ArRegionalDifficultySnapshot x, y;
  if (!ArRegionalDifficulty_Resolve(&a->difficulty, &x) ||
      !ArRegionalDifficulty_Resolve(&b->difficulty, &y))
    return false;
  if (x.timer_reload != y.timer_reload || x.spawn_hp != y.spawn_hp || x.contact_extra != y.contact_extra ||
      x.skip_dragon_attack != y.skip_dragon_attack || x.single_tendril_bob != y.single_tendril_bob)
    visit.changes |= 1u << kArRegionalProfile_Difficulty;
  /* EU placement markers also consume the level, including later waves. */
  if (a->difficulty.level != b->difficulty.level &&
      (a->placements.enemies == kArRegionalSource_Europe || b->placements.enemies == kArRegionalSource_Europe))
    visit.changes |= (1u << kArRegionalProfile_Difficulty) | (1u << kArRegionalProfile_Stage);
  *groups = visit.changes;
  return true;
}
