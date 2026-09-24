#include "regional_randomizer.h"
#include <string.h>

/* Generator 1: domain-separated FNV-1a key hash, then SplitMix64 finalization.
 * Keep these constants and keys stable; changing the mapping needs a new
 * generator version. No native RNG state or campaign UUID participates. */
static ArRegionalSource Pick(uint32_t seed,const char *key) {
  uint64_t value=UINT64_C(14695981039346656037);
  const char *domain="actraiser-regional-v1";
  for(const char *p=domain;*p;++p)value=(value^(uint8_t)*p)*UINT64_C(1099511628211);
  for(unsigned i=0;i<4;++i)value=(value^(uint8_t)(seed>>(8*i)))*UINT64_C(1099511628211);
  for(const char *p=key;*p;++p)value=(value^(uint8_t)*p)*UINT64_C(1099511628211);
  value=(value^(value>>30))*UINT64_C(0xbf58476d1ce4e5b9);
  value=(value^(value>>27))*UINT64_C(0x94d049bb133111eb);
  return (ArRegionalSource)((value^(value>>31))%kArRegionalSource_Count);
}
static ArRegionalSource Select(void *context,const char *key,ArRegionalProfileGroup group,
    ArRegionalSource current) {
  const RandomizerConfig *c=context;
  const bool action=group==kArRegionalProfile_Stage || group==kArRegionalProfile_Combat ||
      group==kArRegionalProfile_Magic || group==kArRegionalProfile_Lives;
  const bool town=group==kArRegionalProfile_Development || group==kArRegionalProfile_Population ||
      group==kArRegionalProfile_Lairs || group==kArRegionalProfile_Resources;
  /* Title access/return routing and the player's chosen difficulty are not
   * surprise gameplay rolls. Presentation/controls stay manually selected. */
  for(unsigned i=0;i<kArRegionalMode_Count;++i)
    if(!strcmp(key,ArRegionalMode_Descriptor(i)->key))return current;
  if((action && c->regional_action) || (town && c->regional_towns))return Pick(c->seed,key);
  return current;
}
bool ArRegionalRandomizer_Choose(const ArRegionalRules *base,const RandomizerConfig *config,
    ArRegionalRules *out) {
  if(!base || !out || !RandomizerConfig_Valid(config))return false;
  if(!config->enabled) {*out=*base;return true;}
  ArRegionalRules next;
  RandomizerConfig local=*config;
  if(!ArRegionalProfiles_SelectSources(base,Select,&local,&next))return false;
  if(config->regional_towns && !ArRegionalRules_PopulationCompatible(&next)) {
    /* Reduced support cannot be paired with unreachable Western goals. The
     * unrelated Compass prerequisite and growth-status leaves stay free. */
    next.level_goals=kArRegionalSource_Japan;
    next.story.source[kArRegionalStory_FillmoreHint]=kArRegionalSource_Japan;
    next.story.source[kArRegionalStory_KasandoraTablet]=kArRegionalSource_Japan;
  }
  if(!ArRegionalRules_PopulationCompatible(&next))return false;
  *out=next;return true;
}
