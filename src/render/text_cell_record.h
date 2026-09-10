#ifndef AR_RENDER_TEXT_CELL_RECORD_H
#define AR_RENDER_TEXT_CELL_RECORD_H

#include <stdbool.h>
#include <stdint.h>

#define AR_TEXT_CELL_RECORD_ABI_VERSION UINT32_C(1)

enum {
  kArTextCellRecordCapacity = 16,
  kArTextCellRecordNoSnapshot = -1,
};

typedef enum ArTextCellScreen {
  kArTextCellScreen_Main = 0,
  kArTextCellScreen_Sub,
  kArTextCellScreen_Composited,
} ArTextCellScreen;

/* A record owns cells only in this exact tilemap destination. Keeping the
 * BG layer, screen and tilemap base explicit prevents visually overlapping
 * coordinates from contaminating unrelated native planes. */
typedef struct ArTextCellDestination {
  uint8_t background;       /* SNES BG number, 1..4. */
  uint8_t screen;           /* ArTextCellScreen. */
  uint16_t tilemap_base_words;
} ArTextCellDestination;

typedef struct ArTextCellRegion {
  uint8_t column;
  uint8_t row;
  uint8_t columns;
  uint8_t rows;
} ArTextCellRegion;

typedef struct ArTextCellRecord {
  uint32_t surface_id;
  ArTextCellDestination destination;
  ArTextCellRegion region;
  int8_t snapshot_slot;
  uint64_t serial;
} ArTextCellRecord;

typedef struct ArTextCellRecordSet {
  ArTextCellRecord records[kArTextCellRecordCapacity];
  uint8_t count;
  uint64_t next_serial;
} ArTextCellRecordSet;

void ArTextCellRecordSet_Reset(ArTextCellRecordSet *set);
bool ArTextCellRecordSet_Claim(ArTextCellRecordSet *set,
                               uint32_t surface_id,
                               ArTextCellDestination destination,
                               ArTextCellRegion region,
                               int8_t snapshot_slot);
bool ArTextCellRecordSet_Release(ArTextCellRecordSet *set,
                                 uint32_t surface_id);
const ArTextCellRecord *ArTextCellRecordSet_Find(
    const ArTextCellRecordSet *set, uint32_t surface_id);
const ArTextCellRecord *ArTextCellRecordSet_OwnerOfCell(
    const ArTextCellRecordSet *set, ArTextCellDestination destination,
    unsigned column, unsigned row);
bool ArTextCellDestinationsEqual(ArTextCellDestination a,
                                 ArTextCellDestination b);
bool ArTextCellRegionsIntersect(ArTextCellRegion a, ArTextCellRegion b);

#endif /* AR_RENDER_TEXT_CELL_RECORD_H */
