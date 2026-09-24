#include "regional_artwork.h"
#include <stddef.h>
static const ArRegionalArtworkDescriptor kDescriptors[]={
  {"death_heim_art",{0,1,0}},
  {"action_item_art",{0,0,1}},
  {"follower_symbols",{0,1,0}},
  {"lair_symbols",{0,1,0}},
  {"pyramid_detail",{0,1,0}},
  {"title_background",{0,1,0}},
};
_Static_assert(sizeof(kDescriptors)/sizeof(kDescriptors[0])==kArRegionalArtwork_Count,"artwork descriptors");
_Static_assert(kArRegionalArtwork_Count<=8,"artwork snapshot mask capacity");
const ArRegionalArtworkDescriptor *ArRegionalArtwork_Descriptor(unsigned rule) {
  return rule<kArRegionalArtwork_Count?&kDescriptors[rule]:NULL;
}
bool ArRegionalArtwork_Resolve(const ArRegionalArtworkPolicy *policy,uint8_t *mask) {
  if(!policy || !mask)return false;
  uint8_t bits=0;
  for(unsigned i=0;i<kArRegionalArtwork_Count;++i) {
    if((unsigned)policy->source[i]>=kArRegionalSource_Count)return false;
    bits|=(uint8_t)(kDescriptors[i].enabled[policy->source[i]]<<i);
  }
  *mask=bits;return true;
}
static const ArRegionalArtworkDescriptor kActorDescriptors[]={
  {"fillmore_actor_art",{0,1,0}},
  {"bloodpool_actor_art",{0,1,0}},
  {"kasandora_actor_art",{0,1,0}},
  {"aitos_actor_art",{0,1,0}},
  {"marahna_actor_art",{0,1,0}},
  {"northwall_actor_art",{0,1,0}},
  {"death_heim_actor_art",{0,1,0}},
};
_Static_assert(sizeof(kActorDescriptors)/sizeof(kActorDescriptors[0])==kArRegionalActorArtwork_Count,"actor artwork descriptors");
_Static_assert(kArRegionalActorArtwork_Count<=8,"actor artwork mask capacity");
const ArRegionalArtworkDescriptor *ArRegionalActorArtwork_Descriptor(unsigned area) {
  return area<kArRegionalActorArtwork_Count?&kActorDescriptors[area]:NULL;
}
bool ArRegionalActorArtwork_Resolve(const ArRegionalActorArtworkPolicy *policy,uint8_t *mask) {
  if(!policy || !mask)return false;
  uint8_t bits=0;
  for(unsigned i=0;i<kArRegionalActorArtwork_Count;++i) {
    if((unsigned)policy->source[i]>=kArRegionalSource_Count)return false;
    bits|=(uint8_t)(kActorDescriptors[i].enabled[policy->source[i]]<<i);
  }
  *mask=bits;return true;
}
