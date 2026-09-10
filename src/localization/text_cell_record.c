#include "localization/text_cell_record.h"

#include <stddef.h>
#include <string.h>

bool ArTextCellDestinationsEqual(ArTextCellDestination a,
                                 ArTextCellDestination b) {
  return a.background == b.background && a.screen == b.screen &&
      a.tilemap_base_words == b.tilemap_base_words;
}

bool ArTextCellRegionsIntersect(ArTextCellRegion a, ArTextCellRegion b) {
  if (!a.columns || !a.rows || !b.columns || !b.rows) return false;
  const unsigned a_right = (unsigned)a.column + a.columns;
  const unsigned b_right = (unsigned)b.column + b.columns;
  const unsigned a_bottom = (unsigned)a.row + a.rows;
  const unsigned b_bottom = (unsigned)b.row + b.rows;
  return a.column < b_right && b.column < a_right &&
      a.row < b_bottom && b.row < a_bottom;
}

void ArTextCellRecordSet_Reset(ArTextCellRecordSet *set) {
  if (!set) return;
  memset(set, 0, sizeof(*set));
  set->next_serial = 1u;
}

static void RemoveAt(ArTextCellRecordSet *set, uint8_t index) {
  if (index + 1u < set->count) {
    memmove(&set->records[index], &set->records[index + 1u],
            (size_t)(set->count - index - 1u) * sizeof(set->records[0]));
  }
  --set->count;
  memset(&set->records[set->count], 0, sizeof(set->records[0]));
}

static bool SupersededBy(const ArTextCellRecord *record,
                         uint32_t surface_id,
                         ArTextCellDestination destination,
                         ArTextCellRegion region) {
  return record->surface_id == surface_id ||
      (ArTextCellDestinationsEqual(record->destination, destination) &&
       ArTextCellRegionsIntersect(record->region, region));
}

bool ArTextCellRecordSet_Claim(ArTextCellRecordSet *set,
                               uint32_t surface_id,
                               ArTextCellDestination destination,
                               ArTextCellRegion region,
                               int8_t snapshot_slot) {
  if (!set || !surface_id || destination.background < 1u ||
      destination.background > 4u ||
      destination.screen > kArTextCellScreen_Composited ||
      !region.columns || !region.rows ||
      (unsigned)region.column + region.columns > 64u ||
      (unsigned)region.row + region.rows > 64u)
    return false;
  if (!set->next_serial) ArTextCellRecordSet_Reset(set);

  /* Decide before mutating. In particular, a failed full-set claim must not
   * evict an old region and then discover that it cannot install the new one. */
  unsigned superseded = 0;
  for (uint8_t index = 0; index < set->count; ++index) {
    if (SupersededBy(&set->records[index], surface_id, destination, region))
      ++superseded;
  }
  if ((unsigned)set->count - superseded + 1u > kArTextCellRecordCapacity)
    return false;

  for (uint8_t index = set->count; index-- > 0;) {
    if (SupersededBy(&set->records[index], surface_id, destination, region))
      RemoveAt(set, index);
  }
  uint64_t serial = set->next_serial++;
  if (!serial) {
    serial = 1u;
    set->next_serial = 2u;
  }
  set->records[set->count++] = (ArTextCellRecord){
    .surface_id = surface_id,
    .destination = destination,
    .region = region,
    .snapshot_slot = snapshot_slot,
    .serial = serial,
  };
  return true;
}

bool ArTextCellRecordSet_Release(ArTextCellRecordSet *set,
                                 uint32_t surface_id) {
  if (!set || !surface_id) return false;
  for (uint8_t index = 0; index < set->count; ++index) {
    if (set->records[index].surface_id != surface_id) continue;
    RemoveAt(set, index);
    return true;
  }
  return false;
}

const ArTextCellRecord *ArTextCellRecordSet_Find(
    const ArTextCellRecordSet *set, uint32_t surface_id) {
  if (!set || !surface_id) return NULL;
  for (uint8_t index = 0; index < set->count; ++index) {
    if (set->records[index].surface_id == surface_id)
      return &set->records[index];
  }
  return NULL;
}

const ArTextCellRecord *ArTextCellRecordSet_OwnerOfCell(
    const ArTextCellRecordSet *set, ArTextCellDestination destination,
    unsigned column, unsigned row) {
  if (!set) return NULL;
  for (uint8_t index = 0; index < set->count; ++index) {
    const ArTextCellRecord *record = &set->records[index];
    const ArTextCellRegion *region = &record->region;
    if (ArTextCellDestinationsEqual(record->destination, destination) &&
        column >= region->column &&
        column < (unsigned)region->column + region->columns &&
        row >= region->row && row < (unsigned)region->row + region->rows)
      return record;
  }
  return NULL;
}
