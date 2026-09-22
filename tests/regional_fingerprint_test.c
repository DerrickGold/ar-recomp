#include "regional/regional_fingerprint.h"
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
int main(void) {
  ArRegionalCostPolicy us, eu, jp;
  ArRegionalCosts_Init(&us, kArRegionalCost_US);
  ArRegionalCosts_Init(&eu, kArRegionalCost_Europe);
  ArRegionalCosts_Init(&jp, kArRegionalCost_Japan);
  uint8_t native[32], current[32], pending[32], effective[32];
  bool baseline;
  CHECK(ArRegionalCosts_Fingerprint(&us, &us, native, &baseline) && baseline);
  CHECK(ArRegionalCosts_Fingerprint(&eu, &us, current, &baseline) && baseline);
  CHECK(!memcmp(native, current, 32));
  CHECK(ArRegionalCosts_Fingerprint(&jp, &us, pending, &baseline) && !baseline);
  CHECK(ArRegionalCosts_Fingerprint(&us, &jp, effective, &baseline) && !baseline);
  CHECK(memcmp(native, pending, 32) && memcmp(native, effective, 32) && memcmp(pending, effective, 32));
  for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i) {
    ArRegionalCostPolicy one = us;
    ArRegionalCosts_SetRule(&one, (ArRegionalCostRule)i, kArRegionalCost_Japan);
    CHECK(ArRegionalCosts_Fingerprint(&one, &us, current, &baseline));
    bool same_price = ArRegionalCosts_Descriptor((ArRegionalCostRule)i)->price[0] ==
                      ArRegionalCosts_Descriptor((ArRegionalCostRule)i)->price[1];
    CHECK(baseline == same_price);
    CHECK((memcmp(native, current, 32) == 0) == same_price);
  }
  memset(current, 0xa5, sizeof(current)); baseline = true;
  jp.source[0] = kArRegionalCostSource_Count;
  CHECK(!ArRegionalCosts_Fingerprint(&jp, &us, current, &baseline));
  CHECK(current[0] == 0xa5 && baseline);
  return failures ? 1 : 0;
}
