#ifndef AR_REGIONAL_EMITTERS_H
#define AR_REGIONAL_EMITTERS_H
#include "regional/regional_source.h"
#include <stdbool.h>
#include <stdint.h>
enum { kArRegionalEmitter_Cadence, kArRegionalEmitter_Position, kArRegionalEmitter_Count };
typedef struct ArRegionalEmitterPolicy { ArRegionalSource source[kArRegionalEmitter_Count]; } ArRegionalEmitterPolicy;
/* Cadence is 0/1/2 (360/180/255 updates); bit2 selects PAL's launch offset.
 * Numeric room snapshot only, no native addresses or actor lifetime state. */
typedef uint8_t ArRegionalEmitterSnapshot;
typedef struct ArRegionalEmitterDescriptor { const char *key; uint16_t value[kArRegionalSource_Count]; } ArRegionalEmitterDescriptor;
const ArRegionalEmitterDescriptor *ArRegionalEmitter_Descriptor(unsigned rule);
bool ArRegionalEmitter_Init(ArRegionalEmitterPolicy *policy,ArRegionalSource source);
bool ArRegionalEmitter_Resolve(const ArRegionalEmitterPolicy *policy,ArRegionalEmitterSnapshot *out);
bool ArRegionalEmitter_GroupSource(const ArRegionalEmitterPolicy *policy,ArRegionalSource *source);
bool ArRegionalEmitter_Row(ArRegionalEmitterSnapshot snapshot,unsigned state,unsigned row,uint16_t *duration,int16_t dx);
#endif
