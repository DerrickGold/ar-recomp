#ifndef AR_REGIONAL_ARTWORK_H
#define AR_REGIONAL_ARTWORK_H
#include "regional_source.h"
#include <stdbool.h>
#include <stdint.h>
typedef enum ArRegionalArtworkRule {
  kArRegionalArtwork_DeathHeim,
  kArRegionalArtwork_ActionItems,
  kArRegionalArtwork_FollowerSymbols,
  kArRegionalArtwork_LairSymbols,
  kArRegionalArtwork_PyramidDetail,
  kArRegionalArtwork_TitleBackground,
  kArRegionalArtwork_Count,
} ArRegionalArtworkRule;
enum {
  kArRegionalArtwork_ActionMask = (1u<<kArRegionalArtwork_DeathHeim) | (1u<<kArRegionalArtwork_ActionItems),
  kArRegionalArtwork_TownMask = (1u<<kArRegionalArtwork_FollowerSymbols) |
      (1u<<kArRegionalArtwork_LairSymbols) | (1u<<kArRegionalArtwork_PyramidDetail),
  kArRegionalArtwork_TitleMask = 1u<<kArRegionalArtwork_TitleBackground,
};
typedef struct ArRegionalArtworkPolicy {
  ArRegionalSource source[kArRegionalArtwork_Count];
} ArRegionalArtworkPolicy;
typedef struct ArRegionalArtworkDescriptor {
  const char *key;
  uint16_t enabled[kArRegionalSource_Count];
} ArRegionalArtworkDescriptor;
const ArRegionalArtworkDescriptor *ArRegionalArtwork_Descriptor(unsigned rule);
bool ArRegionalArtwork_Resolve(const ArRegionalArtworkPolicy *policy,uint8_t *mask);

/* Enemy/boss pictures share atlases and palettes within each action area.
 * These seven independent leaves are therefore coherent area choices, not
 * per-enemy switches that could produce mixed character/palette ownership. */
enum { kArRegionalActorArtwork_Count = 7 };
typedef struct ArRegionalActorArtworkPolicy {
  ArRegionalSource source[kArRegionalActorArtwork_Count];
} ArRegionalActorArtworkPolicy;
const ArRegionalArtworkDescriptor *ArRegionalActorArtwork_Descriptor(unsigned area);
bool ArRegionalActorArtwork_Resolve(const ArRegionalActorArtworkPolicy *policy,uint8_t *mask);
#endif
