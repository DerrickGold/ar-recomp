#include "regional/regional_timers.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t profiles[] = {3, 5, 7, 0x24, 0x25, 0x26};
static const uint16_t native_bcd[] = {0x300, 0x200, 0x300, 0x300, 0x200, 0x200};
_Static_assert(kArRegionalTimerRule_Count == 6, "extend the independent timer oracle");

static void CheckPolicies(void) {
  for (unsigned mix = 0; mix < 729; ++mix) {
    ArRegionalTimerPolicy policy;
    unsigned digits = mix;
    assert(ArRegionalTimers_Init(&policy, kArRegionalSource_US));
    for (unsigned i = 0; i < 6; ++i) {
      assert(ArRegionalTimers_SetRule(&policy, (ArRegionalTimerRule)i, (ArRegionalSource)(digits % 3)));
      digits /= 3;
      const ArRegionalTimerDescriptor *desc = ArRegionalTimers_Descriptor((ArRegionalTimerRule)i);
      assert(desc && desc->key[0] && desc->us_profile == profiles[i]);
      for (unsigned j = 0; j < i; ++j)
        assert(strcmp(desc->key, ArRegionalTimers_Descriptor((ArRegionalTimerRule)j)->key));
    }
    for (unsigned profile = 0; profile < 256; ++profile) {
      uint16_t baseline = 0xbeef, expected;
      for (unsigned i = 0; i < 6; ++i)
        if (profiles[i] == profile) baseline = native_bcd[i];
      expected = baseline;
      for (unsigned i = 0; i < 6; ++i)
        if (profiles[i] == profile && policy.source[i] == kArRegionalSource_Japan)
          expected -= 0x100;
      uint16_t actual = 42;
      assert(ArRegionalTimers_Resolve(&policy, (uint8_t)profile, baseline, &actual));
      assert(actual == expected);
      actual = 42;
      assert(ArRegionalTimers_Resolve(&policy, (uint8_t)profile, 0xbeef, &actual) == (expected == baseline));
      assert(actual == (expected == baseline ? 0xbeef : 42));
    }
    ArRegionalSource source;
    const bool grouped = ArRegionalTimers_GroupSource(&policy, &source);
    bool all_native = true, all_jp = true, uniform = true;
    for (unsigned i = 0; i < 6; ++i) {
      all_native &= policy.source[i] != kArRegionalSource_Japan;
      all_jp &= policy.source[i] == kArRegionalSource_Japan;
      uniform &= policy.source[i] == policy.source[0];
    }
    assert(grouped == (all_native || all_jp));
    if (grouped) assert(source == (uniform ? policy.source[0] : kArRegionalSource_US));
  }
  ArRegionalTimerPolicy policy;
  assert(ArRegionalTimers_Init(&policy, kArRegionalSource_US));
  ArRegionalTimerPolicy before = policy;
  assert(!ArRegionalTimers_SetRule(&policy, kArRegionalTimerRule_Count, kArRegionalSource_US));
  assert(!ArRegionalTimers_Init(&policy, kArRegionalSource_Count));
  assert(!memcmp(&policy, &before, sizeof(policy)));
  policy.source[0] = kArRegionalSource_Count;
  uint16_t out = 42;
  assert(!ArRegionalTimers_Resolve(&policy, 3, 0x300, &out) && out == 42);
  assert(!ArRegionalTimers_Resolve(NULL, 3, 0x300, &out));
  assert(!ArRegionalTimers_Resolve(&before, 3, 0x300, NULL));
  assert(!ArRegionalTimers_Descriptor(kArRegionalTimerRule_Count));
}

static void CheckRoms(char **paths) {
  uint8_t tables[5][0x2f * 28];
  const size_t bases[] = {0x1093e, 0x107e7, 0x1093e, 0x1093e, 0x1093e};
  for (unsigned r = 0; r < 5; ++r) {
    FILE *file = fopen(paths[r], "rb"); assert(file);
    assert(!fseek(file, 0, SEEK_END) && ftell(file) == 0x100000);
    assert(!fseek(file, (long)bases[r], SEEK_SET));
    assert(fread(tables[r], 1, sizeof(tables[r]), file) == sizeof(tables[r]));
    assert(!fclose(file));
    ArRegionalTimerPolicy policy;
    assert(ArRegionalTimers_Init(&policy, r == 1 ? kArRegionalSource_Japan :
        r == 0 ? kArRegionalSource_US : kArRegionalSource_Europe));
    for (unsigned profile = 3; profile <= 0x2e; ++profile) {
      if (profile == 8) continue;
      const uint8_t *us = tables[0] + profile * 28;
      const uint8_t *other = tables[r] + profile * 28;
      assert(!memcmp(us, other, 25));
      uint16_t out;
      assert(ArRegionalTimers_Resolve(&policy, (uint8_t)profile, us[25] | us[26] << 8, &out));
      assert(out == (other[25] | other[26] << 8));
    }
  }
}

int main(int argc, char **argv) {
  CheckPolicies();
  if (argc == 6) CheckRoms(argv + 1);
  else assert(argc == 1);
  puts("PASS 729 timer mixes across 256 profiles; native fields preserved outside changed rules");
  return 0;
}
