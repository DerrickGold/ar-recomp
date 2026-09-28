#ifndef AR_SAVE_SNAPSHOT_H
#define AR_SAVE_SNAPSHOT_H
#include "save/save_name.h"

/* One immutable persisted campaign. Feature bytes remain owned by their
 * codec; storage owns their publication together with SRAM and the name. */
typedef struct SaveSnapshot {
  uint8_t image[kActRaiserSramSize];
  uint8_t payload[kSaveCampaignPayloadCapacity];
  size_t payload_size;
  char name[kSaveNameCapacity];
} SaveSnapshot;
#endif
