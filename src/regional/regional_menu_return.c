#include "regional_menu_return.h"

static const ArRegionalMenuReturnDescriptor kRule = {"town_menu_return", {0, 1, 0}};
const ArRegionalMenuReturnDescriptor *ArRegionalMenuReturn_Descriptor(void) { return &kRule; }
bool ArRegionalMenuReturn_Resolve(ArRegionalSource source, bool *keep_open) {
  if (!keep_open || (unsigned)source >= kArRegionalSource_Count) return false;
  *keep_open = kRule.keep_open[source] != 0;
  return true;
}
