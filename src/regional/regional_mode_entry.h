#ifndef AR_REGIONAL_MODE_ENTRY_H
#define AR_REGIONAL_MODE_ENTRY_H
#include "regional_source.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum ArRegionalModeRule {
  kArRegionalMode_Unlocked,
  kArRegionalMode_GameOverTitle,
  kArRegionalMode_Count,
} ArRegionalModeRule;
typedef struct ArRegionalModePolicy {
  ArRegionalSource source[kArRegionalMode_Count];
} ArRegionalModePolicy;
typedef struct ArRegionalModeDescriptor {
  const char *key;
  uint16_t value[kArRegionalSource_Count];
} ArRegionalModeDescriptor;
const ArRegionalModeDescriptor *ArRegionalMode_Descriptor(unsigned rule);
bool ArRegionalMode_Init(ArRegionalModePolicy *policy,ArRegionalSource source);
/* Bit0: Action available without completion; bit1: Game Over returns to title. */
bool ArRegionalMode_Resolve(const ArRegionalModePolicy *policy,uint8_t *snapshot);
bool ArRegionalMode_GroupSource(const ArRegionalModePolicy *policy,ArRegionalSource *source);
/* Native title cycles 1 -> 0 -> 2 -> 1. Never offers Continue without a save. */
uint8_t ArRegionalMode_NextChoice(uint8_t choice,bool can_continue);
#endif
