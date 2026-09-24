#ifndef AR_REGIONAL_PROFILES_H
#define AR_REGIONAL_PROFILES_H

#include "regional_rules.h"

/* Semantic UI groups, independent of save-record order and ROM addresses. */
typedef enum ArRegionalProfileGroup {
  kArRegionalProfile_Stage,
  kArRegionalProfile_Combat,
  kArRegionalProfile_Magic,
  kArRegionalProfile_Lives,
  kArRegionalProfile_Development,
  kArRegionalProfile_Population,
  kArRegionalProfile_Lairs,
  kArRegionalProfile_Resources,
  kArRegionalProfile_Interaction,
  kArRegionalProfile_Arrival,
  kArRegionalProfile_Artwork,
  kArRegionalProfile_Music,
  kArRegionalProfile_Difficulty,
  kArRegionalProfile_GroupCount,
  kArRegionalProfile_Gameplay = kArRegionalProfile_GroupCount,
  kArRegionalProfile_Presentation,
  kArRegionalProfile_Count,
} ArRegionalProfileGroup;

typedef struct ArRegionalProfileSummary {
  /* Source_Count means Custom, not unavailable donor data. Uniform source
   * provenance wins when values match multiple regions; otherwise US first. */
  ArRegionalSource source;
  uint8_t matching_sources;
  unsigned members;
} ArRegionalProfileSummary;

uint16_t ArRegionalProfiles_Mask(ArRegionalProfileGroup group);
const char *ArRegionalProfiles_Key(ArRegionalProfileGroup group);

/* Scope hints for browsing, not a conversion decision. An actual edit must
 * compare resolved support values and the campaign's history on the game side. */
typedef enum ArRegionalTownImpact {
  kArRegionalTownImpact_None,
  kArRegionalTownImpact_Future,
  kArRegionalTownImpact_Redevelopment,
} ArRegionalTownImpact;
ArRegionalTownImpact ArRegionalProfiles_TownImpact(ArRegionalProfileGroup group);

/* Display-only cache. Initialize to zero and refresh through this API; never
 * use it as edit authority. Keys are actual rule values, not just a session
 * revision: debug recovery can restore different values at the same revision.
 * Pending confirmations and donor availability are live UI state, not cached. */
typedef struct ArRegionalProfileCache {
  bool valid;
  ArRegionalRules requested, effective;
  ArRegionalProfileSummary profiles[kArRegionalProfile_Count];
  ArRegionalProfileSummary active_profiles[kArRegionalProfile_Count];
  uint16_t pending_groups;
} ArRegionalProfileCache;
bool ArRegionalProfiles_RefreshCache(ArRegionalProfileCache *cache,
    const ArRegionalRules *requested, const ArRegionalRules *effective);
/* Pure expansion, not activation or permission to redevelop a town. All
 * fallible operations leave outputs unchanged. Gameplay presets include the
 * region's default difficulty; other groups preserve it. Language, physical
 * controls, QoL and donor availability are not inferred here. */
bool ArRegionalProfiles_Expand(const ArRegionalRules *current,
    ArRegionalProfileGroup group,ArRegionalSource source,ArRegionalRules *out);
bool ArRegionalProfiles_Describe(const ArRegionalRules *rules,
    ArRegionalProfileSummary out[kArRegionalProfile_Count]);
/* Resolved behavior differences, not provenance differences. One bit per
 * customization group; useful for pending state and precise edit previews. */
bool ArRegionalProfiles_Changes(const ArRegionalRules *a,const ArRegionalRules *b,
    uint16_t *groups);

#endif
