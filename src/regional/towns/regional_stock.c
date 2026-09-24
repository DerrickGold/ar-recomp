#include "regional/towns/regional_stock.h"

bool ArRegionalStock_Estimate(uint32_t remaining, uint32_t source_seed,
                              uint32_t target_seed, uint32_t storage_max,
                              uint32_t *estimate) {
  if (!estimate || remaining > storage_max || source_seed > storage_max ||
      target_seed > storage_max) return false;
  int64_t value = (int64_t)remaining + target_seed - source_seed;
  if (value < 0) value = 0;
  if (value > storage_max) value = storage_max;
  *estimate = (uint32_t)value;
  return true;
}
