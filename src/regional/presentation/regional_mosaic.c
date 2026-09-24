#include "regional/presentation/regional_mosaic.h"
static const ArRegionalMosaicDescriptor kDescriptor={"aitos_mosaic_pattern",{0,1,2}};
const ArRegionalMosaicDescriptor *ArRegionalMosaic_Descriptor(void) { return &kDescriptor; }
bool ArRegionalMosaic_Resolve(ArRegionalSource source,uint8_t *pattern) {
  if(!pattern || (unsigned)source>=kArRegionalSource_Count)return false;
  *pattern=(uint8_t)kDescriptor.profile[source];return true;
}
