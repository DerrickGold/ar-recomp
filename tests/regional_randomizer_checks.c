#include "regional/regional_randomizer.h"
#include "regional/session/regional_session.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static ArRegionalSource HashChoice(void *context,const char *key,ArRegionalProfileGroup group,
    ArRegionalSource current) {
  (void)group;uint64_t *hash=context;
  for(const char *p=key;*p;++p)*hash=(*hash^(uint8_t)*p)*UINT64_C(1099511628211);
  *hash=(*hash^(uint8_t)current)*UINT64_C(1099511628211);return current;
}
void TestRegionalRandomizer(void) {
  ArRegionalSession session;const uint8_t id[16]={7};const ArRegionalCostPolicy costs={{0}};
  assert(ArRegionalSession_NewGame(&session,0,id,&costs));
  for(unsigned t=0;t<6;++t)assert(ArRegionalLairHistory_InitTown(&session.lairs,t));
  assert(ArRegionalLairReloads_Init(&session.reloads));
  const ArRegionalRules base=session.requested;
  uint64_t hash=UINT64_C(14695981039346656037);
  unsigned counts[3]={0};
  for(unsigned seed=0;seed<256;++seed) {
    RandomizerConfig config=RandomizerConfig_Default();config.enabled=true;config.seed=seed;
    ArRegionalRules action,towns,both,repeat;
    config.regional_action=true;
    assert(ArRegionalRandomizer_Choose(&base,&config,&action));
    config.regional_action=false;config.regional_towns=true;
    assert(ArRegionalRandomizer_Choose(&base,&config,&towns));
    config.regional_action=true;
    assert(ArRegionalRandomizer_Choose(&base,&config,&both));
    assert(ArRegionalRandomizer_Choose(&base,&config,&repeat) && !memcmp(&both,&repeat,sizeof(both)));
    assert(ArRegionalRules_PopulationCompatible(&both));
    /* Turning Town rolls on never shifts the independent Action results. */
    assert(!memcmp(&action.actor_stats,&both.actor_stats,sizeof(action.actor_stats)));
    assert(!memcmp(&action.bosses,&both.bosses,sizeof(action.bosses)));
    assert(!memcmp(&towns.support,&both.support,sizeof(towns.support)));
    assert(!memcmp(&towns.story,&both.story,sizeof(towns.story)));
    uint16_t differences;
    assert(ArRegionalProfiles_Changes(&action,&both,&differences));
    const unsigned town_mask=(1u<<kArRegionalProfile_Development)|(1u<<kArRegionalProfile_Population)|
        (1u<<kArRegionalProfile_Lairs)|(1u<<kArRegionalProfile_Resources);
    assert(!(differences&~town_mask));
    assert(ArRegionalProfiles_Changes(&towns,&both,&differences));
    const unsigned action_mask=(1u<<kArRegionalProfile_Stage)|(1u<<kArRegionalProfile_Combat)|
        (1u<<kArRegionalProfile_Magic)|(1u<<kArRegionalProfile_Lives);
    assert(!(differences&~action_mask));
    assert(!memcmp(&both.difficulty,&base.difficulty,sizeof(base.difficulty)) &&
        !memcmp(&both.mode_entry,&base.mode_entry,sizeof(base.mode_entry)));
    session.requested=both;session.effective=base;
    session.effective.support=both.support;session.effective.story=both.story;session.effective.level_goals=both.level_goals;
    /* The normal session codec/activation owner accepts every generated mix. */
    uint8_t encoded[kRandomizerConfigBytes];
    assert(RandomizerConfig_Encode(&config,encoded));
    assert(ArRegionalSession_RequestRules(&session,session.revision,&both));
    assert(ArRegionalProfiles_SelectSources(&both,HashChoice,&hash,&repeat));
    ++counts[both.actor_stats.source[0]];
    config.enabled=false;
    assert(ArRegionalRandomizer_Choose(&base,&config,&repeat) && !memcmp(&base,&repeat,sizeof(base)));
  }
  assert(counts[0] && counts[1] && counts[2]);
  assert(hash==UINT64_C(0x069596619736941c)); /* frozen generator-1 key/source mapping */
  printf("regional generator 1: 256 independent, compatible recipes; fingerprint=%016llx\n",(unsigned long long)hash);
}
