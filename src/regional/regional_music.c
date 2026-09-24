#include "regional_music.h"
#include <stddef.h>
static const ArRegionalMusicDescriptor kDescriptor = {"scene_music_route",{0,1,0}};
const ArRegionalMusicDescriptor *ArRegionalMusic_Descriptor(void) { return &kDescriptor; }
bool ArRegionalMusic_Resolve(ArRegionalSource source,uint8_t *profile) {
  if(!profile || (unsigned)source>=kArRegionalSource_Count)return false;
  *profile=(uint8_t)kDescriptor.profile[source];return true;
}
bool ArRegionalMusic_UseFillmore(uint8_t profile,uint16_t scene) {
  return profile==1 && (scene==0x0201 || scene==0x0301);
}
static const ArRegionalMusicDescriptor kSequences[]={
  {"song_09_sequence",{0,1,0}},
  {"song_12_sequence",{0,1,0}},
};
const ArRegionalMusicDescriptor *ArRegionalSequences_Descriptor(unsigned rule) {
  return rule<kArRegionalSequence_Count?&kSequences[rule]:NULL;
}
bool ArRegionalSequences_Resolve(const ArRegionalSequencePolicy *policy,uint8_t *mask) {
  if(!policy || !mask)return false;
  uint8_t value=0;
  for(unsigned i=0;i<kArRegionalSequence_Count;++i) {
    if((unsigned)policy->source[i]>=kArRegionalSource_Count)return false;
    value|=(uint8_t)(kSequences[i].profile[policy->source[i]]<<i);
  }
  *mask=value;return true;
}
