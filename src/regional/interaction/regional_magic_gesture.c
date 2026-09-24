#include "regional/interaction/regional_magic_gesture.h"

static const ArRegionalMagicGestureDescriptor kRule = {"magic_gesture", {0, 1, 0}};
const ArRegionalMagicGestureDescriptor *ArRegionalMagicGesture_Descriptor(void) { return &kRule; }
bool ArRegionalMagicGesture_Resolve(ArRegionalSource source, bool *up_attack) {
  if (!up_attack || (unsigned)source >= kArRegionalSource_Count) return false;
  *up_attack = kRule.up_attack[source] != 0;
  return true;
}
