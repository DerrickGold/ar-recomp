#include "regional_volley.h"
static const ArRegionalVolleyDescriptor kRule = {"bloodpool_statue_shots", {1, 2, 2}};
const ArRegionalVolleyDescriptor *ArRegionalVolley_Descriptor(void) { return &kRule; }
bool ArRegionalVolley_Resolve(ArRegionalSource source, bool *double_shot) {
  if (!double_shot || (unsigned)source >= kArRegionalSource_Count) return false;
  *double_shot = kRule.shots[source] == 2;
  return true;
}
