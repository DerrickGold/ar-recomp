#include "regional/interaction/regional_arrival.h"
static const ArRegionalArrivalDescriptor kRule={"final_island_japanese",{0,1,0}};
const ArRegionalArrivalDescriptor *ArRegionalArrival_Descriptor(void){return &kRule;}
bool ArRegionalArrival_Resolve(ArRegionalSource source,bool *japanese) {
  if(!japanese || (unsigned)source>=kArRegionalSource_Count)return false;
  *japanese=kRule.japanese[source]!=0;return true;
}
