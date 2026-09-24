#ifndef AR_REGIONAL_MUSIC_H
#define AR_REGIONAL_MUSIC_H
#include "regional/regional_source.h"
#include <stdbool.h>
#include <stdint.h>
typedef struct ArRegionalMusicDescriptor {
  const char *key;
  uint16_t profile[kArRegionalSource_Count];
} ArRegionalMusicDescriptor;
const ArRegionalMusicDescriptor *ArRegionalMusic_Descriptor(void);
bool ArRegionalMusic_Resolve(ArRegionalSource source,uint8_t *profile);
/* Scene-routing choice only. Track payload/sequence variants are separate. */
bool ArRegionalMusic_UseFillmore(uint8_t profile,uint16_t scene);
enum { kArRegionalSequence_Count = 2 };
typedef struct ArRegionalSequencePolicy {
  ArRegionalSource source[kArRegionalSequence_Count];
} ArRegionalSequencePolicy;
/* Note programs only; no SPC firmware, sample, routing or clock ownership. */
const ArRegionalMusicDescriptor *ArRegionalSequences_Descriptor(unsigned rule);
bool ArRegionalSequences_Resolve(const ArRegionalSequencePolicy *policy,uint8_t *mask);
#endif
