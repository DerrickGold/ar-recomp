#include "regional_music.h"
static const ArRegionalMusicDescriptor kDescriptor = {"scene_music_route",{0,1,0}};
const ArRegionalMusicDescriptor *ArRegionalMusic_Descriptor(void) { return &kDescriptor; }
bool ArRegionalMusic_Resolve(ArRegionalSource source,uint8_t *profile) {
  if(!profile || (unsigned)source>=kArRegionalSource_Count)return false;
  *profile=(uint8_t)kDescriptor.profile[source];return true;
}
bool ArRegionalMusic_UseFillmore(uint8_t profile,uint16_t scene) {
  return profile==1 && (scene==0x0201 || scene==0x0301);
}
