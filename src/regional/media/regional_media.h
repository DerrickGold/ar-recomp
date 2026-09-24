#ifndef AR_REGIONAL_MEDIA_H
#define AR_REGIONAL_MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  kArRegionalMediaVersion = 1,
  kArRegionalMediaMaximumBytes = 2 * 1024 * 1024,
  kArRegionalMediaMaximumEntries = 16,
};
typedef enum ArRegionalMediaRelease {
  kArRegionalMediaRelease_None = 0,
  kArRegionalMediaRelease_US = 1,
  kArRegionalMediaRelease_Japan = 2,
  kArRegionalMediaRelease_Europe = 3,
  kArRegionalMediaRelease_German = 4,
  kArRegionalMediaRelease_French = 5,
  kArRegionalMediaRelease_Count,
} ArRegionalMediaRelease;
typedef enum ArRegionalMediaId {
  kArRegionalMedia_DeathHeimBG2 = 1,
  kArRegionalMedia_ActionHealthPickup = 2,
  kArRegionalMedia_ActionSpellHud = 3,
  kArRegionalMedia_TownFollowerSymbols = 4,
  kArRegionalMedia_TownLairSymbols = 5,
  kArRegionalMedia_TownPyramidDetail = 6,
  kArRegionalMedia_TitleBackground = 7,
  kArRegionalMedia_Sequence09 = 8,
  kArRegionalMedia_Sequence12 = 9,
  kArRegionalMedia_ActorArt = 10,
} ArRegionalMediaId;
typedef struct ArRegionalMediaBytes {
  const uint8_t *data;
  size_t size;
} ArRegionalMediaBytes;
typedef struct ArRegionalMediaEntry {
  uint32_t id;
  ArRegionalMediaBytes bytes;
} ArRegionalMediaEntry;
typedef struct ArRegionalMediaView {
  ArRegionalMediaRelease release;
  size_t count;
  ArRegionalMediaEntry entries[kArRegionalMediaMaximumEntries];
} ArRegionalMediaView;

/* Pure startup/load validation; no file I/O, allocation, renderer or CPU.
 * Successful views borrow input bytes: the owner must retain them immutable
 * for the view's entire lifetime. Failure leaves out unchanged. Never call
 * this hashing/parser path from a frame or native upload loop. */
bool ArRegionalMedia_Parse(const void *data,size_t size,ArRegionalMediaView *out);
ArRegionalMediaBytes ArRegionalMedia_Find(const ArRegionalMediaView *view,uint32_t id);

#endif
