#include "actraiser/actraiser_event_bugfixes.h"

#include <stdio.h>
#include <string.h>

enum {
  kWramSize = 0x20000,
  kCurrentTownIndex = 0x17BFB,
  kEventPrerequisiteAitos = 0x19113,
  kEventFiredAitos = 0x1912B,
  kEventDispatchedAitos = 0x19143,
  kPendingEventLatch = 0x1920E,
  kAllMonstersMask = 0x40,
  kMountainMask = 0x08,
};

static uint8_t wram[kWramSize];
static int failures;

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      failures++;                                                          \
    }                                                                      \
  } while (0)

static void SeedCollision(void) {
  memset(wram, 0, sizeof(wram));
  wram[kCurrentTownIndex] = 6;
  wram[kCurrentTownIndex + 1] = 0;
  wram[kPendingEventLatch] = 0x81;
  wram[kEventFiredAitos] = kAllMonstersMask;
  wram[kEventPrerequisiteAitos] = kMountainMask;
  wram[kEventDispatchedAitos] = kMountainMask;
}

static void TestExactCollisionIsRepaired(void) {
  SeedCollision();
  CHECK(ActRaiser_RepairAitosEventQueueCollision(wram, sizeof(wram)));
  CHECK(wram[kPendingEventLatch] == 0);
  CHECK(wram[kEventFiredAitos] == kAllMonstersMask);
  CHECK(wram[kEventPrerequisiteAitos] == kMountainMask);
  CHECK(wram[kEventDispatchedAitos] == kMountainMask);
}

static void CheckNearMissIsUntouched(size_t offset, uint8_t value) {
  SeedCollision();
  wram[offset] = value;
  CHECK(!ActRaiser_RepairAitosEventQueueCollision(wram, sizeof(wram)));
  CHECK(wram[kPendingEventLatch] ==
        (offset == kPendingEventLatch ? value : 0x81));
}

static void TestUnrelatedStatesRemainAuthentic(void) {
  CheckNearMissIsUntouched(kCurrentTownIndex, 4); /* Kasandora */
  CheckNearMissIsUntouched(kPendingEventLatch, 0x84);
  CheckNearMissIsUntouched(kEventFiredAitos, 0);
  CheckNearMissIsUntouched(kEventPrerequisiteAitos, 0);
  CheckNearMissIsUntouched(kEventDispatchedAitos, 0);
  CheckNearMissIsUntouched(kEventFiredAitos,
                           kAllMonstersMask | kMountainMask);

  SeedCollision();
  CHECK(!ActRaiser_RepairAitosEventQueueCollision(
      wram, kPendingEventLatch));
  CHECK(wram[kPendingEventLatch] == 0x81);
  CHECK(!ActRaiser_RepairAitosEventQueueCollision(NULL, sizeof(wram)));
}

int main(void) {
  TestExactCollisionIsRepaired();
  TestUnrelatedStatesRemainAuthentic();
  if (failures) return 1;
  puts("ActRaiser event bugfix checks passed");
  return 0;
}
