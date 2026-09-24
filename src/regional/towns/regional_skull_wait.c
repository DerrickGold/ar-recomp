#include "regional/towns/regional_skull_wait.h"

static const ArRegionalSkullWaitDescriptor kRule={"skull_post_effect_frames", {90,0,90}};
const ArRegionalSkullWaitDescriptor *ArRegionalSkullWait_Descriptor(void) { return &kRule; }
bool ArRegionalSkullWait_Resolve(ArRegionalSource source, uint16_t *frames) {
  if (!frames || (unsigned)source>=kArRegionalSource_Count) return false;
  *frames=kRule.frames[source];
  return true;
}
