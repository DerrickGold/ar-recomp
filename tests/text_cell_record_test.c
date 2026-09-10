#include "localization/text_cell_record.h"

#include <stdio.h>

static int s_failures;
#define CHECK(expression) do {                                             \
  if (!(expression)) {                                                     \
    fprintf(stderr, "%s:%d: check failed: %s\n",                          \
            __FILE__, __LINE__, #expression);                              \
    ++s_failures;                                                          \
  }                                                                        \
} while (0)

static const ArTextCellDestination kMainBg3 = {
  .background = 3,
  .screen = kArTextCellScreen_Composited,
  .tilemap_base_words = 0x3800,
};

static void TestDestinationAwareIntersection(void) {
  const ArTextCellRegion a = {4, 4, 4, 4};
  const ArTextCellRegion touching = {8, 4, 4, 4};
  const ArTextCellRegion overlap = {7, 4, 4, 4};
  CHECK(!ArTextCellRegionsIntersect(a, touching));
  CHECK(ArTextCellRegionsIntersect(a, overlap));
  CHECK(ArTextCellRegionsIntersect(overlap, a));

  ArTextCellRecordSet set;
  ArTextCellRecordSet_Reset(&set);
  CHECK(ArTextCellRecordSet_Claim(&set, 1, kMainBg3, a, 0));
  ArTextCellDestination subscreen = kMainBg3;
  subscreen.screen = kArTextCellScreen_Sub;
  CHECK(ArTextCellRecordSet_Claim(&set, 2, subscreen, overlap, 1));
  CHECK(set.count == 2);
  CHECK(ArTextCellRecordSet_Claim(&set, 3, kMainBg3, overlap, 2));
  CHECK(set.count == 2);
  CHECK(ArTextCellRecordSet_Find(&set, 1) == NULL);
  CHECK(ArTextCellRecordSet_Find(&set, 2) != NULL);
}

static void TestWholeRegionOwnershipAndReclaim(void) {
  ArTextCellRecordSet set;
  ArTextCellRecordSet_Reset(&set);
  const ArTextCellRegion dialogue = {3, 20, 26, 6};
  CHECK(ArTextCellRecordSet_Claim(&set, 10, kMainBg3, dialogue, 0));
  const ArTextCellRecord *first = ArTextCellRecordSet_Find(&set, 10);
  CHECK(first != NULL);
  const uint64_t first_serial = first ? first->serial : 0;
  CHECK(ArTextCellRecordSet_OwnerOfCell(&set, kMainBg3, 3, 20) == first);
  CHECK(ArTextCellRecordSet_OwnerOfCell(&set, kMainBg3, 28, 25) == first);
  CHECK(ArTextCellRecordSet_OwnerOfCell(&set, kMainBg3, 29, 25) == NULL);

  const ArTextCellRegion shorter = {3, 20, 18, 3};
  CHECK(ArTextCellRecordSet_Claim(&set, 10, kMainBg3, shorter, 1));
  const ArTextCellRecord *second = ArTextCellRecordSet_Find(&set, 10);
  CHECK(set.count == 1);
  CHECK(second && second->serial > first_serial);
  CHECK(second && second->snapshot_slot == 1);
  CHECK(ArTextCellRecordSet_OwnerOfCell(&set, kMainBg3, 22, 24) == NULL);
}

static void TestCapacityFailureIsAtomic(void) {
  ArTextCellRecordSet set;
  ArTextCellRecordSet_Reset(&set);
  for (unsigned i = 0; i < kArTextCellRecordCapacity; ++i) {
    const ArTextCellRegion cell = {(uint8_t)i, 0, 1, 1};
    CHECK(ArTextCellRecordSet_Claim(
        &set, i + 1u, kMainBg3, cell, kArTextCellRecordNoSnapshot));
  }
  const uint64_t first_serial = set.records[0].serial;
  CHECK(!ArTextCellRecordSet_Claim(
      &set, 99, kMainBg3, (ArTextCellRegion){31, 31, 1, 1}, 0));
  CHECK(set.count == kArTextCellRecordCapacity);
  CHECK(set.records[0].surface_id == 1);
  CHECK(set.records[0].serial == first_serial);

  CHECK(ArTextCellRecordSet_Claim(
      &set, 1, kMainBg3, (ArTextCellRegion){31, 31, 1, 1}, 3));
  CHECK(set.count == kArTextCellRecordCapacity);
  CHECK(ArTextCellRecordSet_Find(&set, 1)->region.column == 31);
}

static void TestValidationAndRelease(void) {
  ArTextCellRecordSet set = {0};
  CHECK(!ArTextCellRecordSet_Claim(
      &set, 0, kMainBg3, (ArTextCellRegion){1, 1, 1, 1}, 0));
  CHECK(!ArTextCellRecordSet_Claim(
      &set, 1, (ArTextCellDestination){0},
      (ArTextCellRegion){1, 1, 1, 1}, 0));
  CHECK(!ArTextCellRecordSet_Claim(
      &set, 1, kMainBg3, (ArTextCellRegion){63, 63, 2, 2}, 0));
  CHECK(ArTextCellRecordSet_Claim(
      &set, 1, kMainBg3, (ArTextCellRegion){1, 1, 1, 1}, 0));
  CHECK(set.next_serial > 1);
  CHECK(ArTextCellRecordSet_Release(&set, 1));
  CHECK(!ArTextCellRecordSet_Release(&set, 1));
}

int main(void) {
  TestDestinationAwareIntersection();
  TestWholeRegionOwnershipAndReclaim();
  TestCapacityFailureIsAtomic();
  TestValidationAndRelease();
  if (s_failures) return 1;
  puts("text cell record checks passed");
  return 0;
}
