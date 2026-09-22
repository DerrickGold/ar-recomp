#include "regional_fingerprint.h"

#include "byte_order.h"
#include "snesrecomp/support/digest.h"
#include <string.h>

bool ArRegionalCosts_Fingerprint(const ArRegionalCostPolicy *requested,
    const ArRegionalCostPolicy *effective, uint8_t out[32], bool *baseline) {
  ArRegionalCostSnapshot pending, active;
  if (!out || !baseline || !ArRegionalCosts_Resolve(requested, &pending) ||
      !ArRegionalCosts_Resolve(effective, &active)) return false;
  uint8_t bytes[512] = "ARCOST-R1";
  size_t used = 9;
  bool native = true;
  for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i) {
    const ArRegionalCostDescriptor *rule = ArRegionalCosts_Descriptor((ArRegionalCostRule)i);
    size_t length = strlen(rule->key);
    if (length > 255 || length + 5 > sizeof(bytes) - used) return false;
    bytes[used++] = (uint8_t)length;
    memcpy(bytes + used, rule->key, length); used += length;
    ByteOrder_WriteLe16(bytes + used, pending.price[i]);
    ByteOrder_WriteLe16(bytes + used + 2, active.price[i]); used += 4;
    native &= pending.price[i] == rule->price[kArRegionalCost_US] &&
              active.price[i] == rule->price[kArRegionalCost_US];
  }
  if (!sr_support_sha256(bytes, used, out)) return false;
  *baseline = native;
  return true;
}
