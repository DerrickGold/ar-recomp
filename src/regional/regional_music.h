#ifndef AR_REGIONAL_MUSIC_H
#define AR_REGIONAL_MUSIC_H
#include "regional_source.h"
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
#endif
