#include "regional/regional_fingerprint.h"
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static bool Fingerprint(const ArRegionalCostPolicy *costs, const ArRegionalCostPolicy *active_costs,
    const ArRegionalTimerPolicy *timers, const ArRegionalTimerPolicy *active_timers,
    uint8_t out[32], bool *baseline) {
  const ArRegionalRules requested = {.costs = *costs, .timers = *timers};
  const ArRegionalRules effective = {.costs = *active_costs, .timers = *active_timers};
  return ArRegionalRules_Fingerprint(&requested, &effective, out, baseline);
}
int main(void) {
  ArRegionalCostPolicy us, eu, jp;
  ArRegionalCosts_Init(&us, kArRegionalSource_US);
  ArRegionalCosts_Init(&eu, kArRegionalSource_Europe);
  ArRegionalCosts_Init(&jp, kArRegionalSource_Japan);
  uint8_t native[32], current[32], pending[32], effective[32];
  bool baseline;
  CHECK(ArRegionalCosts_Fingerprint(&us, &us, native, &baseline) && baseline);
  CHECK(ArRegionalCosts_Fingerprint(&eu, &us, current, &baseline) && baseline);
  CHECK(!memcmp(native, current, 32));
  ArRegionalRules r = {.costs = us}, e = {.costs = eu};
  CHECK(ArRegionalRules_Fingerprint(&r, &e, current, &baseline) && baseline);
  CHECK(!memcmp(native, current, 32));
  r.retry_score = kArRegionalSource_Japan;
  CHECK(ArRegionalRules_Fingerprint(&r, &e, pending, &baseline) && !baseline);
  CHECK(memcmp(native, pending, 32));
  r.retry_score = kArRegionalSource_Europe; e.retry_score = kArRegionalSource_Japan;
  CHECK(ArRegionalRules_Fingerprint(&r, &e, effective, &baseline) && !baseline);
  CHECK(memcmp(pending, effective, 32));
  e.retry_score = kArRegionalSource_US;
  CHECK(ArRegionalRules_Fingerprint(&r, &e, current, &baseline) && baseline);
  CHECK(!memcmp(native, current, 32));
  r.retry_score = kArRegionalSource_Count;
  memset(current, 0xa5, sizeof(current));
  CHECK(!ArRegionalRules_Fingerprint(&r, &e, current, &baseline) && current[0] == 0xa5 && baseline);
  r.retry_score = kArRegionalSource_US;
  for (unsigned p = 0; p < kArRegionalSource_Count; ++p)
    for (unsigned a = 0; a < kArRegionalSource_Count; ++a) {
      r.town_wait = (ArRegionalSource)p; e.town_wait = (ArRegionalSource)a;
      CHECK(ArRegionalRules_Fingerprint(&r, &e, current, &baseline));
      CHECK(baseline == (p != kArRegionalSource_Japan && a != kArRegionalSource_Japan));
      CHECK((memcmp(native, current, 32) == 0) == baseline);
    }
  r.town_wait = kArRegionalSource_Japan; e.town_wait = kArRegionalSource_US;
  CHECK(ArRegionalRules_Fingerprint(&r, &e, pending, &baseline));
  r.town_wait = kArRegionalSource_US; e.town_wait = kArRegionalSource_Japan;
  CHECK(ArRegionalRules_Fingerprint(&r, &e, effective, &baseline));
  CHECK(memcmp(pending,effective,32));
  r.town_wait = kArRegionalSource_Count;
  memset(current,0xa5,sizeof(current)); baseline=true;
  CHECK(!ArRegionalRules_Fingerprint(&r,&e,current,&baseline) && current[0] == 0xa5 && baseline);
  r.town_wait=e.town_wait=kArRegionalSource_US;
  for (unsigned p=0; p<kArRegionalSource_Count; ++p)
    for (unsigned a=0; a<kArRegionalSource_Count; ++a) {
      r.fishing=(ArRegionalSource)p; e.fishing=(ArRegionalSource)a;
      CHECK(ArRegionalRules_Fingerprint(&r,&e,current,&baseline));
      CHECK(baseline == (p!=kArRegionalSource_Japan && a!=kArRegionalSource_Japan));
      CHECK((memcmp(native,current,32)==0) == baseline);
    }
  r.fishing=kArRegionalSource_Japan; e.fishing=kArRegionalSource_US;
  CHECK(ArRegionalRules_Fingerprint(&r,&e,pending,&baseline));
  r.fishing=kArRegionalSource_US; e.fishing=kArRegionalSource_Japan;
  CHECK(ArRegionalRules_Fingerprint(&r,&e,effective,&baseline));
  CHECK(memcmp(pending,effective,32));
  r.fishing=kArRegionalSource_Count;
  memset(current,0xa5,sizeof(current)); baseline=true;
  CHECK(!ArRegionalRules_Fingerprint(&r,&e,current,&baseline) && current[0]==0xa5 && baseline);
  ArRegionalDevelopment_Init(&r.development, kArRegionalSource_US);
  ArRegionalDevelopment_Init(&e.development, kArRegionalSource_US);
  r.fishing = e.fishing = kArRegionalSource_US;
  for (unsigned i = 0; i < kArRegionalRecovery_Count; ++i) {
    ArRegionalRecovery_Init(&r.recovery, kArRegionalSource_US);
    ArRegionalRecovery_Init(&e.recovery, kArRegionalSource_Europe);
    CHECK(ArRegionalRules_Fingerprint(&r, &e, current, &baseline) && baseline && !memcmp(native, current, 32));
    r.recovery.source[i] = kArRegionalSource_Japan;
    CHECK(ArRegionalRules_Fingerprint(&r, &e, pending, &baseline) && !baseline);
    r.recovery.source[i] = kArRegionalSource_US; e.recovery.source[i] = kArRegionalSource_Japan;
    CHECK(ArRegionalRules_Fingerprint(&r, &e, effective, &baseline) && !baseline);
    CHECK(memcmp(pending, effective, 32) && memcmp(native, effective, 32));
  }
  r.recovery.source[0] = kArRegionalSource_Count;
  memset(current, 0xa5, sizeof(current)); baseline = true;
  CHECK(!ArRegionalRules_Fingerprint(&r, &e, current, &baseline) && current[0] == 0xa5 && baseline);
  ArRegionalRecovery_Init(&r.recovery, kArRegionalSource_US);
  ArRegionalRecovery_Init(&e.recovery, kArRegionalSource_US);
  r.fishing=e.fishing=kArRegionalSource_US;
  for(unsigned i=0;i<kArRegionalDevelopmentRule_Count;++i) {
    ArRegionalDevelopment_Init(&r.development,kArRegionalSource_US);
    ArRegionalDevelopment_Init(&e.development,kArRegionalSource_Europe);
    CHECK(ArRegionalRules_Fingerprint(&r,&e,current,&baseline) && baseline && !memcmp(native,current,32));
    r.development.source[i]=kArRegionalSource_Japan;
    CHECK(ArRegionalRules_Fingerprint(&r,&e,pending,&baseline) && !baseline);
    r.development.source[i]=kArRegionalSource_US;e.development.source[i]=kArRegionalSource_Japan;
    CHECK(ArRegionalRules_Fingerprint(&r,&e,effective,&baseline) && !baseline);
    CHECK(memcmp(pending,effective,32) && memcmp(native,effective,32));
  }
  r.development.source[0]=kArRegionalSource_Count;
  memset(current,0xa5,sizeof(current));baseline=true;
  CHECK(!ArRegionalRules_Fingerprint(&r,&e,current,&baseline) && current[0]==0xa5 && baseline);
  ArRegionalDevelopment_Init(&r.development, kArRegionalSource_US);
  ArRegionalDevelopment_Init(&e.development, kArRegionalSource_US);
  for (unsigned i = 0; i < kArRegionalQuake_Count; ++i) {
    ArRegionalQuake_Init(&r.quake, kArRegionalSource_US);
    ArRegionalQuake_Init(&e.quake, kArRegionalSource_Europe);
    CHECK(ArRegionalRules_Fingerprint(&r, &e, current, &baseline) && baseline && !memcmp(native, current, 32));
    r.quake.source[i] = kArRegionalSource_Japan;
    CHECK(ArRegionalRules_Fingerprint(&r, &e, pending, &baseline) && !baseline);
    r.quake.source[i] = kArRegionalSource_US; e.quake.source[i] = kArRegionalSource_Japan;
    CHECK(ArRegionalRules_Fingerprint(&r, &e, effective, &baseline) && !baseline);
    CHECK(memcmp(pending, effective, 32) && memcmp(native, effective, 32));
  }
  r.quake.source[0] = kArRegionalSource_Count;
  memset(current, 0xa5, sizeof(current)); baseline = true;
  CHECK(!ArRegionalRules_Fingerprint(&r, &e, current, &baseline) && current[0] == 0xa5 && baseline);
  ArRegionalQuake_Init(&r.quake, kArRegionalSource_US);
  ArRegionalQuake_Init(&e.quake, kArRegionalSource_US);
  for (unsigned p = 0; p < kArRegionalSource_Count; ++p)
    for (unsigned a = 0; a < kArRegionalSource_Count; ++a) {
      r.score_page = (ArRegionalSource)p; e.score_page = (ArRegionalSource)a;
      CHECK(ArRegionalRules_Fingerprint(&r, &e, current, &baseline));
      CHECK(baseline == (p != kArRegionalSource_Japan && a != kArRegionalSource_Japan));
      CHECK((memcmp(native, current, 32) == 0) == baseline);
    }
  r.score_page = kArRegionalSource_Japan; e.score_page = kArRegionalSource_US;
  CHECK(ArRegionalRules_Fingerprint(&r, &e, pending, &baseline));
  r.score_page = kArRegionalSource_US; e.score_page = kArRegionalSource_Japan;
  CHECK(ArRegionalRules_Fingerprint(&r, &e, effective, &baseline) && memcmp(pending, effective, 32));
  r.score_page = kArRegionalSource_Count;
  memset(current, 0xa5, sizeof(current)); baseline = true;
  CHECK(!ArRegionalRules_Fingerprint(&r, &e, current, &baseline) && current[0] == 0xa5 && baseline);
  r.score_page = e.score_page = kArRegionalSource_US;
  for (unsigned p = 0; p < kArRegionalSource_Count; ++p)
    for (unsigned a = 0; a < kArRegionalSource_Count; ++a) {
      r.menu_return = (ArRegionalSource)p; e.menu_return = (ArRegionalSource)a;
      CHECK(ArRegionalRules_Fingerprint(&r, &e, current, &baseline));
      CHECK(baseline == (p != kArRegionalSource_Japan && a != kArRegionalSource_Japan));
      CHECK((memcmp(native, current, 32) == 0) == baseline);
    }
  r.menu_return = kArRegionalSource_Japan; e.menu_return = kArRegionalSource_US;
  CHECK(ArRegionalRules_Fingerprint(&r, &e, pending, &baseline));
  r.menu_return = kArRegionalSource_US; e.menu_return = kArRegionalSource_Japan;
  CHECK(ArRegionalRules_Fingerprint(&r, &e, effective, &baseline) && memcmp(pending, effective, 32));
  r.menu_return = kArRegionalSource_Count;
  memset(current, 0xa5, sizeof(current)); baseline = true;
  CHECK(!ArRegionalRules_Fingerprint(&r, &e, current, &baseline) && current[0] == 0xa5 && baseline);
  r.menu_return = e.menu_return = kArRegionalSource_US;
  for (unsigned p = 0; p < kArRegionalSource_Count; ++p)
    for (unsigned a = 0; a < kArRegionalSource_Count; ++a) {
      r.speed_range = (ArRegionalSource)p; e.speed_range = (ArRegionalSource)a;
      CHECK(ArRegionalRules_Fingerprint(&r, &e, current, &baseline));
      CHECK(baseline == (p != kArRegionalSource_Japan && a != kArRegionalSource_Japan));
      CHECK((memcmp(native, current, 32) == 0) == baseline);
    }
  r.speed_range = kArRegionalSource_Japan; e.speed_range = kArRegionalSource_US;
  CHECK(ArRegionalRules_Fingerprint(&r, &e, pending, &baseline));
  r.speed_range = kArRegionalSource_US; e.speed_range = kArRegionalSource_Japan;
  CHECK(ArRegionalRules_Fingerprint(&r, &e, effective, &baseline) && memcmp(pending, effective, 32));
  r.speed_range = kArRegionalSource_Count;
  memset(current, 0xa5, sizeof(current)); baseline = true;
  CHECK(!ArRegionalRules_Fingerprint(&r, &e, current, &baseline) && current[0] == 0xa5 && baseline);
  r.speed_range=e.speed_range=kArRegionalSource_US;
  for (unsigned p=0;p<kArRegionalSource_Count;++p) for (unsigned a=0;a<kArRegionalSource_Count;++a) {
    r.magic_gesture=(ArRegionalSource)p; e.magic_gesture=(ArRegionalSource)a;
    CHECK(ArRegionalRules_Fingerprint(&r,&e,current,&baseline));
    CHECK(baseline==(p!=kArRegionalSource_Japan && a!=kArRegionalSource_Japan));
    CHECK((memcmp(native,current,32)==0)==baseline);
  }
  r.magic_gesture=kArRegionalSource_Japan; e.magic_gesture=kArRegionalSource_US;
  CHECK(ArRegionalRules_Fingerprint(&r,&e,pending,&baseline));
  r.magic_gesture=kArRegionalSource_US; e.magic_gesture=kArRegionalSource_Japan;
  CHECK(ArRegionalRules_Fingerprint(&r,&e,effective,&baseline) && memcmp(pending,effective,32));
  r.magic_gesture=kArRegionalSource_Count;
  memset(current,0xa5,sizeof(current)); baseline=true;
  CHECK(!ArRegionalRules_Fingerprint(&r,&e,current,&baseline) && current[0]==0xa5 && baseline);
  CHECK(ArRegionalCosts_Fingerprint(&jp, &us, pending, &baseline) && !baseline);
  CHECK(ArRegionalCosts_Fingerprint(&us, &jp, effective, &baseline) && !baseline);
  CHECK(memcmp(native, pending, 32) && memcmp(native, effective, 32) && memcmp(pending, effective, 32));
  for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i) {
    ArRegionalCostPolicy one = us;
    ArRegionalCosts_SetRule(&one, (ArRegionalCostRule)i, kArRegionalSource_Japan);
    CHECK(ArRegionalCosts_Fingerprint(&one, &us, current, &baseline));
    bool same_price = ArRegionalCosts_Descriptor((ArRegionalCostRule)i)->price[0] ==
                      ArRegionalCosts_Descriptor((ArRegionalCostRule)i)->price[1];
    CHECK(baseline == same_price);
  CHECK((memcmp(native, current, 32) == 0) == same_price);
  }
  ArRegionalTimerPolicy timer_us, timer_eu, timer_jp;
  ArRegionalTimers_Init(&timer_us, kArRegionalSource_US);
  ArRegionalTimers_Init(&timer_eu, kArRegionalSource_Europe);
  ArRegionalTimers_Init(&timer_jp, kArRegionalSource_Japan);
  CHECK(Fingerprint(&jp, &us, &timer_us, &timer_eu, current, &baseline));
  CHECK(!baseline && !memcmp(current, pending, 32));
  CHECK(Fingerprint(&eu, &us, &timer_us, &timer_eu, current, &baseline));
  CHECK(baseline && !memcmp(current, native, 32));
  CHECK(Fingerprint(&us, &eu, &timer_jp, &timer_us, pending, &baseline));
  CHECK(!baseline && memcmp(pending, native, 32));
  CHECK(Fingerprint(&us, &us, &timer_us, &timer_jp, effective, &baseline));
  CHECK(!baseline && memcmp(effective, pending, 32) && memcmp(effective, native, 32));
  for (unsigned i = 0; i < kArRegionalTimerRule_Count; ++i) {
    ArRegionalTimerPolicy one = timer_us;
    CHECK(ArRegionalTimers_SetRule(&one, (ArRegionalTimerRule)i, kArRegionalSource_Japan));
    CHECK(Fingerprint(&us, &us, &one, &timer_us, current, &baseline));
    CHECK(!baseline && memcmp(current, native, 32));
  }
  timer_jp.source[0] = kArRegionalSource_Count;
  memset(current, 0xa5, sizeof(current)); baseline = true;
  CHECK(!Fingerprint(&us, &us, &timer_jp, &timer_us, current, &baseline));
  CHECK(current[0] == 0xa5 && baseline);
  memset(current, 0xa5, sizeof(current)); baseline = true;
  jp.source[0] = kArRegionalSource_Count;
  CHECK(!ArRegionalCosts_Fingerprint(&jp, &us, current, &baseline));
  CHECK(current[0] == 0xa5 && baseline);
  return failures ? 1 : 0;
}
