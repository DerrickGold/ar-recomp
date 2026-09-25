#include "regional/regional_profiles.h"
#include "regional/regional_families.h"

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
  const unsigned scopes[] = { group,
                              (ArRegionalProfiles_Mask(kArRegionalProfile_Gameplay) & (1u << group))
                                  ? kArRegionalProfile_Gameplay
                                  : kArRegionalProfile_Presentation };
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

/* Profile membership in visit order. The order is behavior, not layout: an
 * expansion or selection that rewrites a field is seen by later members, and a
 * selector sees keys in this order. Keys and values come from the family
 * table's consumer descriptors, never from copies of the regional tables or
 * save-codec ordinals. Two rows are special: costs take their group from each
 * rule (scrolls belong to Magic, miracles to Town resources), and the lairs
 * interleave each lair's seed and reload under shared keys. */
enum { kGroupPerCostRule = 0xff, kMemberLairs = 0xfe };
typedef struct Member {
  uint8_t group;
  uint8_t family;
} Member;
static const Member kMembers[] = {
  /* Action: stage design, combat, magic, lives and scoring. */
  {kArRegionalProfile_Stage, kArRegionalFamily_Terrain},
  {kArRegionalProfile_Stage, kArRegionalFamily_Placements},
  {kArRegionalProfile_Stage, kArRegionalFamily_Timers},
  {kArRegionalProfile_Difficulty, kArRegionalFamily_Difficulty},
  {kArRegionalProfile_Combat, kArRegionalFamily_Hazards},
  {kArRegionalProfile_Combat, kArRegionalFamily_StatueVolley},
  {kArRegionalProfile_Combat, kArRegionalFamily_ActionMotion},
  {kArRegionalProfile_Combat, kArRegionalFamily_Emitters},
  {kArRegionalProfile_Combat, kArRegionalFamily_Bosses},
  {kArRegionalProfile_Combat, kArRegionalFamily_Collision},
  {kArRegionalProfile_Combat, kArRegionalFamily_PlatformSkull},
  {kArRegionalProfile_Combat, kArRegionalFamily_ActorStats},
  {kArRegionalProfile_Combat, kArRegionalFamily_FireEnemy},
  {kGroupPerCostRule, kArRegionalFamily_Costs},
  {kArRegionalProfile_Magic, kArRegionalFamily_SpellInventory},
  {kArRegionalProfile_Magic, kArRegionalFamily_CastHold},
  {kArRegionalProfile_Lives, kArRegionalFamily_RetryScore},
  {kArRegionalProfile_Lives, kArRegionalFamily_ScoreLives},
  {kArRegionalProfile_Lives, kArRegionalFamily_ActionStart},
  {kArRegionalProfile_Lives, kArRegionalFamily_ModeEntry},
  /* Towns: development, population, monsters/lairs and resources. */
  {kArRegionalProfile_Development, kArRegionalFamily_TownWait},
  {kArRegionalProfile_Development, kArRegionalFamily_Fishing},
  {kArRegionalProfile_Development, kArRegionalFamily_Construction},
  {kArRegionalProfile_Development, kArRegionalFamily_Development},
  {kArRegionalProfile_Population, kArRegionalFamily_LevelGoals},
  {kArRegionalProfile_Population, kArRegionalFamily_Support},
  {kArRegionalProfile_Population, kArRegionalFamily_Story},
  {kArRegionalProfile_Population, kArRegionalFamily_TownStatus},
  {kArRegionalProfile_Lairs, kMemberLairs},
  {kArRegionalProfile_Lairs, kArRegionalFamily_SimCombat},
  {kArRegionalProfile_Lairs, kArRegionalFamily_SimAi},
  {kArRegionalProfile_Resources, kArRegionalFamily_HouseCredit},
  {kArRegionalProfile_Resources, kArRegionalFamily_SkullWait},
  {kArRegionalProfile_Resources, kArRegionalFamily_Recovery},
  {kArRegionalProfile_Resources, kArRegionalFamily_Quake},
  {kArRegionalProfile_Resources, kArRegionalFamily_ScoreFeedback},
  {kArRegionalProfile_Resources, kArRegionalFamily_Sources},
  /* Controls & menus, then final island arrival. */
  {kArRegionalProfile_Interaction, kArRegionalFamily_MagicGesture},
  {kArRegionalProfile_Interaction, kArRegionalFamily_LivesDisplay},
  {kArRegionalProfile_Interaction, kArRegionalFamily_ScorePage},
  {kArRegionalProfile_Interaction, kArRegionalFamily_MenuReturn},
  {kArRegionalProfile_Interaction, kArRegionalFamily_SpeedRange},
  {kArRegionalProfile_Arrival, kArRegionalFamily_Arrival},
  /* Presentation: regional artwork and music, independent of gameplay. */
  {kArRegionalProfile_Artwork, kArRegionalFamily_Mosaic},
  {kArRegionalProfile_Artwork, kArRegionalFamily_Poses},
  {kArRegionalProfile_Artwork, kArRegionalFamily_Artwork},
  {kArRegionalProfile_Artwork, kArRegionalFamily_ActorArtwork},
  {kArRegionalProfile_Music, kArRegionalFamily_Music},
  {kArRegionalProfile_Music, kArRegionalFamily_Sequences},
};

static ArRegionalSource OtherSource(const Visit *visit, ArRegionalFamilyId family,
                                    unsigned rule) {
  return visit->other ? ArRegionalFamilies_Source(visit->other, family, rule)
                      : kArRegionalSource_US;
}

static bool VisitFamily(ArRegionalRules *rules, Visit *visit, unsigned group,
                        ArRegionalFamilyId family) {
  const ArRegionalFamily *row = ArRegionalFamilies_Get(family);
  for (unsigned rule = 0; rule < row->rules; ++rule) {
    const ArRegionalRuleInfo info = row->info(rule);
    unsigned member_group = group;
    if (group == kGroupPerCostRule)
      member_group = ArRegionalCosts_Descriptor((ArRegionalCostRule)rule)->group ==
                             kArRegionalCostGroup_Scrolls
                         ? kArRegionalProfile_Magic
                         : kArRegionalProfile_Resources;
    if (!VisitRegionalSource(visit, ArRegionalFamilies_Field(rules, family, rule),
                             OtherSource(visit, family, rule),
                             (ArRegionalProfileGroup)member_group, info.values, info.key))
      return false;
  }
  return true;
}

/* Each lair's seed stock, then its reload rule, under one key per kind. */
static bool VisitLairs(ArRegionalRules *rules, Visit *visit) {
  for (unsigned i = 0; i < kArRegionalLairCount; ++i) {
    if (!VisitRegionalSource(visit, &rules->lair_seeds,
                             OtherSource(visit, kArRegionalFamily_LairSeeds, 0),
                             kArRegionalProfile_Lairs, ArRegionalLair_SeedDescriptor(i)->stock,
                             "lair_stock"))
      return false;
    uint16_t reload[kArRegionalSource_Count];
    for (unsigned source = 0; source < kArRegionalSource_Count; ++source)
      if (!ArRegionalLair_Reload(source, i, &reload[source])) return false;
    if (!VisitRegionalSource(visit, &rules->lair_reloads,
                             OtherSource(visit, kArRegionalFamily_LairReloads, 0),
                             kArRegionalProfile_Lairs, reload, "lair_reload_japanese"))
      return false;
  }
  return true;
}

/* One typed ownership inventory serves expansion, Custom detection and
 * pending-state comparison. */
static bool VisitProfileMembers(ArRegionalRules *rules, Visit *visit) {
  if ((unsigned)rules->difficulty.level >= kArRegionalDifficulty_Count ||
      (visit->other && (unsigned)visit->other->difficulty.level >= kArRegionalDifficulty_Count))
    return false;
  for (unsigned i = 0; i < sizeof(kMembers) / sizeof(kMembers[0]); ++i) {
    const bool ok = kMembers[i].family == kMemberLairs
                        ? VisitLairs(rules, visit)
                        : VisitFamily(rules, visit, kMembers[i].group,
                                      (ArRegionalFamilyId)kMembers[i].family);
    if (!ok) return false;
  }
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
  if (x.timer_reload != y.timer_reload || x.spawn_hp != y.spawn_hp ||
      x.contact_extra != y.contact_extra || x.skip_dragon_attack != y.skip_dragon_attack ||
      x.single_tendril_bob != y.single_tendril_bob)
    visit.changes |= 1u << kArRegionalProfile_Difficulty;
  /* EU placement markers also consume the level, including later waves. */
  if (a->difficulty.level != b->difficulty.level &&
      (a->placements.enemies == kArRegionalSource_Europe ||
       b->placements.enemies == kArRegionalSource_Europe))
    visit.changes |= (1u << kArRegionalProfile_Difficulty) | (1u << kArRegionalProfile_Stage);
  *groups = visit.changes;
  return true;
}
