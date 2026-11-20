#ifndef AR_DIORAMA_VOXEL_SOURCE_CACHE_H
#define AR_DIORAMA_VOXEL_SOURCE_CACHE_H

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "present/owned_bg_packet.h"
#include "snesrecomp/runner/ppu_bg_packet.h"

/* Identity of the split's immutable geometry and its world/color inputs.
 * Packet equality below proves the native source; neither producer pointers
 * nor palette/arena offsets are content revisions. */
typedef struct DioramaVoxelSourceKey {
  const void *geometry;
  uint64_t revision;
  int world_x, world_y, width, height;
  size_t output_pitch;
  unsigned brightness, cgwsel, cgadsub, fixed_color;
} DioramaVoxelSourceKey;

typedef struct DioramaVoxelSourceCache {
  const SrPpuBgPacket *packet;
  SrPpuBgPacket *private_packet;
  OwnedBgPacket *owner;
  DioramaVoxelSourceKey key;
  bool valid;
} DioramaVoxelSourceCache;

typedef struct DioramaVoxelSourceRowPlan {
  unsigned count;
  uint8_t changed[SR_PPU_BG_PACKET_HEIGHT];
} DioramaVoxelSourceRowPlan;

typedef struct DioramaVoxelSourcePalettePair {
  unsigned a, b, words;
  bool equal;
} DioramaVoxelSourcePalettePair;

static inline bool DioramaVoxelSourceWordsEqual(const uint32_t *a,
                                               const uint32_t *b, unsigned count) {
  /* Word reductions remain vectorizable on cross-toolchain C runtimes whose
   * variable-size memcmp is bytewise. No hashes or overreads are involved. */
  while (count >= 16) {
    uint32_t difference = 0;
    for (unsigned i = 0; i < 16; ++i) difference |= a[i] ^ b[i];
    if (difference) return false;
    a += 16; b += 16; count -= 16;
  }
  for (unsigned i = 0; i < count; ++i) if (a[i] != b[i]) return false;
  return true;
}

static inline bool DioramaVoxelSourceRowEqual(const SrPpuBgPacket *a,
    const SrPpuBgPacket *b, unsigned source, unsigned y,
    DioramaVoxelSourcePalettePair *palette) {
  const unsigned index = SR_PPU_BG_PACKET_HEADER_WORDS +
      SrPpuBgPacket_Row(source, y) * SR_PPU_BG_PACKET_ROW_WORDS;
  const uint32_t *ar = a->words + index, *br = b->words + index;
  if (!DioramaVoxelSourceWordsEqual(ar, br, 5)) return false;
  if (!ar[0]) return true;
  /* Compact rows and edit cells address three palette bits and four color
   * bits: only entries 0..127. Format 1 can address all 256. */
  const unsigned words = ar[0] >= 2 ? 128u : 256u;
  if (ar[6] != palette->a || br[6] != palette->b || words != palette->words) {
    palette->a = ar[6]; palette->b = br[6]; palette->words = words;
    palette->equal = DioramaVoxelSourceWordsEqual(a->words + ar[6], b->words + br[6], words);
  }
  /* A differing shared palette dirties every referencing row, including
   * later rows whose offsets are identical to the first comparison. */
  if (!palette->equal) return false;
  if (!!ar[7] != !!br[7] || (ar[7] &&
      !DioramaVoxelSourceWordsEqual(a->words + ar[7], b->words + br[7],
                                  SrPpuBgPacket_RowWords(ar, source)))) return false;
  if (ar[0] == 4 && (!!ar[5] != !!br[5] || (ar[5] &&
      !DioramaVoxelSourceWordsEqual(a->words + ar[5], b->words + br[5],
                                  SR_PPU_BG_PACKET_PIXEL_STRIDE)))) return false;
  return true;
}

static inline bool DioramaVoxelSourceWinnerRowEqual(const SrPpuBgPacket *a,
    const SrPpuBgPacket *b, unsigned y) {
  const unsigned ar = a->words[a->words[7] + y];
  const unsigned br = b->words[b->words[7] + y];
  return !!ar == !!br && (!ar || DioramaVoxelSourceWordsEqual(
      a->words + ar, b->words + br, 4 + a->words[0]));
}

static inline bool DioramaVoxelSourcePacketUsable(const SrPpuBgPacket *p,
                                                 const DioramaVoxelSourceKey *key) {
  if (!p || !key->geometry || !key->revision || !(p->owned_sources & 1u) ||
      !(p->words[2] & 1u) || key->width <= 0 || key->height <= 0 ||
      p->words[0] != (unsigned)key->width || p->words[1] != (unsigned)key->height ||
      key->output_pitch < (size_t)key->width || !SrPpuBgPacket_Valid(p)) return false;
  for (unsigned y = 0; y < p->words[1]; ++y)
    if (!p->words[SR_PPU_BG_PACKET_HEADER_WORDS +
                  SrPpuBgPacket_Row(0, y) * SR_PPU_BG_PACKET_ROW_WORDS]) return false;
  return true;
}

static inline bool DioramaVoxelSourceCache_ContextMatches(const DioramaVoxelSourceCache *cache,
    const SrPpuBgPacket *p, const DioramaVoxelSourceKey *key) {
  if (!cache->valid || !cache->packet || !DioramaVoxelSourcePacketUsable(p, key)) return false;
  const DioramaVoxelSourceKey *old = &cache->key;
  if (key->geometry != old->geometry || key->revision != old->revision ||
      key->world_x != old->world_x || key->world_y != old->world_y ||
      key->width != old->width || key->height != old->height ||
      key->output_pitch != old->output_pitch || key->brightness != old->brightness ||
      key->cgwsel != old->cgwsel || key->cgadsub != old->cgadsub ||
      key->fixed_color != old->fixed_color) return false;
  return true;
}

/* BG1 decoding and the Voxel split have no dependency on adjacent capture
 * rows: sparse backing donors, columns and masks are immutable geometry,
 * indexed by world coordinates rather than neighboring captured pixels.
 * Retain unchanged output rows when artwork changes only part of the capture.
 * Ordinary rows depend only on BG1; full-add also depends on BG2 and native
 * winners. Compare all palette entries addressable by each row format.
 * Geometry, camera, layout and color changes still take the complete path.
 * Both retained split buffers must exist before the caller uses this plan. */
static inline bool DioramaVoxelSourceCache_PlanRows(const DioramaVoxelSourceCache *cache,
    const SrPpuBgPacket *p, const DioramaVoxelSourceKey *key,
    DioramaVoxelSourceRowPlan *plan) {
  memset(plan, 0, sizeof(*plan));
  if (!DioramaVoxelSourceCache_ContextMatches(cache, p, key) ||
      !!p->words[7] != !!cache->packet->words[7]) return false;
  const bool full_add = p->words[7] != 0;
  if ((p->words[2] ^ cache->packet->words[2]) & (full_add ? 3u : 1u)) return false;
  DioramaVoxelSourcePalettePair palette[2] = {{0}, {0}};
  for (unsigned y = 0; y < p->words[1]; ++y) {
    const bool bg1 = DioramaVoxelSourceRowEqual(p, cache->packet, 0, y, &palette[0]);
    const bool bg2 = !full_add || DioramaVoxelSourceRowEqual(p, cache->packet, 1, y, &palette[1]);
    const bool winner = !full_add || DioramaVoxelSourceWinnerRowEqual(p, cache->packet, y);
    plan->changed[y] = !bg1 || !bg2 || !winner;
    plan->count += plan->changed[y];
  }
  return true;
}

/* Call only after a successful materialization and split into the retained
 * surfaces. Allocation failure merely disables this optional reuse. */
static inline void DioramaVoxelSourceCache_Retain(DioramaVoxelSourceCache *cache,
    const SrPpuBgPacket *p, const DioramaVoxelSourceKey *key, OwnedBgPacket *owner) {
  cache->valid = false;
  if (!DioramaVoxelSourcePacketUsable(p, key)) return;
  if (owner && p == &owner->packet) {
    OwnedBgPacket_Retain(owner);
  } else {
    owner = NULL;
    if (!cache->private_packet) cache->private_packet = malloc(sizeof(*cache->private_packet));
    if (!cache->private_packet) return;
    memcpy(cache->private_packet, p, SrPpuBgPacket_Size(p));
  }
  OwnedBgPacket_Release(cache->owner);
  cache->owner = owner;
  cache->packet = owner ? &owner->packet : cache->private_packet;
  cache->key = *key;
  cache->valid = true;
}

static inline void DioramaVoxelSourceCache_Reset(DioramaVoxelSourceCache *cache) {
  OwnedBgPacket_Release(cache->owner);
  free(cache->private_packet);
  *cache = (DioramaVoxelSourceCache){0};
}

#endif
