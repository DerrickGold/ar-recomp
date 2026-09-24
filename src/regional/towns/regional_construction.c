#include "regional/towns/regional_construction.h"

static const ArRegionalConstructionDescriptor kRule = {"construction_price_japanese", {0, 1, 0}};
const ArRegionalConstructionDescriptor *ArRegionalConstruction_Descriptor(void) { return &kRule; }
bool ArRegionalConstruction_Resolve(ArRegionalSource source, bool *japanese) {
  if (!japanese || (unsigned)source >= kArRegionalSource_Count) return false;
  *japanese = kRule.japanese[source] != 0;
  return true;
}
bool ArRegionalConstruction_Price(bool japanese, unsigned civilization, uint16_t *price) {
  if (!price || civilization < 1 || civilization > 3) return false;
  *price = japanese ? 4 : (uint16_t)(2 * civilization + 2);
  return true;
}
