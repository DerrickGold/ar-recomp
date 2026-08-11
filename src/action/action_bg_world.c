#include "action_bg_world.h"

#include <stdlib.h>
#include <string.h>

enum {
  kActionBgPagePixels = 256,
  kActionBgTilePixels = 8,
  kActionBgPageBytes = 256,
  kActionBgTilesPerPage =
      (kActionBgPagePixels / kActionBgTilePixels) *
      (kActionBgPagePixels / kActionBgTilePixels),
  kActionBgMetatileCount = 256,
  kActionBgMetatileBytes = 8,
  kActionBgDefinitionBytes =
      kActionBgMetatileCount * kActionBgMetatileBytes,
  /* At most 512 map pages can fit in the two-bank WRAM input. Each expands to
   * 32x32 tile words, so this is a derived safety bound rather than a guessed
   * level-size cap. */
  kActionBgMaxExpandedTiles =
      (kActionBgMaxWramBytes / kActionBgPageBytes) * kActionBgTilesPerPage,
};

typedef struct ActionBgLayout {
  size_t map_start;
  size_t map_size;
  size_t table_start;
  size_t source_size;
  size_t tile_count;
  unsigned pages_wide;
  unsigned tile_width;
  unsigned tile_height;
} ActionBgLayout;

typedef struct ActionBgPublicationKey {
  size_t map_start;
  size_t map_size;
  size_t table_start;
  uint16_t word_mask;
  uint8_t attributes;
  unsigned tile_width;
  unsigned tile_height;
} ActionBgPublicationKey;

struct ActionBgWorld {
  uint16_t *tiles;
  uint16_t *scratch_tiles;
  size_t tile_capacity;
  size_t scratch_tile_capacity;
  uint8_t *source;
  uint8_t *scratch_source;
  size_t source_capacity;
  size_t scratch_source_capacity;
  size_t source_size;
  ActionBgPublicationKey key;
  uint32_t serial;
  bool has_publication;
  bool valid;
};

_Static_assert(sizeof(uint16_t) == 2, "tilemap words must be 16-bit");
_Static_assert(kActionBgDefinitionBytes == 0x800,
               "256 metatiles must occupy 2 KiB");
_Static_assert(kActionBgMaxExpandedTiles == 512 * 1024,
               "expanded capacity must follow the WRAM page bound");

static void Invalidate(ActionBgWorld *world) {
  if (world) world->valid = false;
}

bool ActionBgMapView_Init(ActionBgMapView *view,
                          const uint8_t *wram, size_t wram_size,
                          uint16_t world_width, uint16_t world_height,
                          uint16_t map_page) {
  if (!view) return false;
  memset(view, 0, sizeof(*view));
  if (!wram || !wram_size || wram_size > kActionBgMaxWramBytes ||
      !world_width || !world_height ||
      world_width % kActionBgPagePixels != 0 ||
      world_height % kActionBgPagePixels != 0)
    return false;

  const size_t pages_wide = world_width / kActionBgPagePixels;
  const size_t pages_high = world_height / kActionBgPagePixels;
  if (!pages_wide || pages_high > SIZE_MAX / pages_wide) return false;
  const size_t page_count = pages_wide * pages_high;
  if (page_count > SIZE_MAX / kActionBgPageBytes) return false;
  const size_t map_size = page_count * kActionBgPageBytes;
  const size_t map_start = map_page & 0xFF00u;
  if (map_start > wram_size || map_size > wram_size - map_start) return false;

  *view = (ActionBgMapView) {
    .map = wram + map_start,
    .map_size = map_size,
    .world_width = world_width,
    .world_height = world_height,
    .pages_wide = (unsigned)pages_wide,
  };
  return true;
}

bool ActionBgMapView_LookupMetatile(const ActionBgMapView *view,
                                    int world_x, int world_y,
                                    uint8_t *metatile) {
  if (!view || !view->map || !metatile || world_x < 0 || world_y < 0 ||
      (unsigned)world_x >= view->world_width ||
      (unsigned)world_y >= view->world_height || !view->pages_wide)
    return false;
  const unsigned x = (unsigned)world_x;
  const unsigned y = (unsigned)world_y;
  const size_t page = (size_t)(y / kActionBgPagePixels) * view->pages_wide +
      x / kActionBgPagePixels;
  const size_t metatile_in_page = (size_t)(y & 0xF0u) +
      (size_t)((x & 0xF0u) / kActionBgMetatilePixels);
  const size_t offset = page * kActionBgPageBytes + metatile_in_page;
  if (offset >= view->map_size) return false;
  *metatile = view->map[offset];
  return true;
}

static bool ValidateInput(const ActionBgDecodeInput *input,
                          ActionBgLayout *layout) {
  if (!input || !layout || !input->wram || input->wram_size == 0 ||
      input->wram_size > kActionBgMaxWramBytes)
    return false;
  ActionBgMapView map_view;
  if (!ActionBgMapView_Init(&map_view, input->wram, input->wram_size,
                            input->world_width, input->world_height,
                            input->map_page))
    return false;

  const size_t table_start = input->metatile_table;
  if (table_start > input->wram_size ||
      kActionBgDefinitionBytes > input->wram_size - table_start)
    return false;
  const size_t page_count = map_view.map_size / kActionBgPageBytes;
  if (page_count > SIZE_MAX / kActionBgTilesPerPage) return false;
  const size_t tile_count = page_count * kActionBgTilesPerPage;
  if (tile_count > kActionBgMaxExpandedTiles ||
      map_view.map_size > SIZE_MAX - kActionBgDefinitionBytes)
    return false;

  *layout = (ActionBgLayout) {
    .map_start = (size_t)(map_view.map - input->wram),
    .map_size = map_view.map_size,
    .table_start = table_start,
    .source_size = map_view.map_size + kActionBgDefinitionBytes,
    .tile_count = tile_count,
    .pages_wide = map_view.pages_wide,
    .tile_width = input->world_width / kActionBgTilePixels,
    .tile_height = input->world_height / kActionBgTilePixels,
  };
  return true;
}

static ActionBgPublicationKey MakeKey(const ActionBgDecodeInput *input,
                                      const ActionBgLayout *layout) {
  return (ActionBgPublicationKey) {
    .map_start = layout->map_start,
    .map_size = layout->map_size,
    .table_start = layout->table_start,
    .word_mask = input->word_mask,
    .attributes = input->attributes,
    .tile_width = layout->tile_width,
    .tile_height = layout->tile_height,
  };
}

static bool KeysEqual(const ActionBgPublicationKey *a,
                      const ActionBgPublicationKey *b) {
  return a->map_start == b->map_start && a->map_size == b->map_size &&
      a->table_start == b->table_start && a->word_mask == b->word_mask &&
      a->attributes == b->attributes && a->tile_width == b->tile_width &&
      a->tile_height == b->tile_height;
}

static bool SourcesEqual(const ActionBgWorld *world,
                         const ActionBgDecodeInput *input,
                         const ActionBgLayout *layout,
                         const ActionBgPublicationKey *key) {
  if (!world->has_publication || world->source_size != layout->source_size ||
      !KeysEqual(&world->key, key))
    return false;
  return memcmp(world->source, input->wram + layout->map_start,
                layout->map_size) == 0 &&
      memcmp(world->source + layout->map_size,
             input->wram + layout->table_start,
             kActionBgDefinitionBytes) == 0;
}

static bool Reserve(void **storage, size_t *capacity, size_t count,
                    size_t element_size) {
  if (count <= *capacity) return true;
  if (!element_size || count > SIZE_MAX / element_size) return false;
  void *resized = realloc(*storage, count * element_size);
  if (!resized) return false;
  *storage = resized;
  *capacity = count;
  return true;
}

static bool ReserveScratch(ActionBgWorld *world,
                           const ActionBgLayout *layout) {
  if (!Reserve((void **)&world->scratch_tiles,
               &world->scratch_tile_capacity, layout->tile_count,
               sizeof(*world->scratch_tiles)))
    return false;
  return Reserve((void **)&world->scratch_source,
                 &world->scratch_source_capacity, layout->source_size, 1);
}

static void SnapshotSources(ActionBgWorld *world,
                            const ActionBgDecodeInput *input,
                            const ActionBgLayout *layout) {
  memcpy(world->scratch_source, input->wram + layout->map_start,
         layout->map_size);
  memcpy(world->scratch_source + layout->map_size,
         input->wram + layout->table_start, kActionBgDefinitionBytes);
}

static uint16_t ReadWord(const uint8_t *bytes) {
  return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static void DecodeSnapshot(ActionBgWorld *world,
                           const ActionBgDecodeInput *input,
                           const ActionBgLayout *layout) {
  const uint8_t *map = world->scratch_source;
  const uint8_t *definitions = map + layout->map_size;
  const uint16_t attributes =
      (uint16_t)((uint16_t)input->attributes << 8);

  for (unsigned tile_y = 0; tile_y < layout->tile_height; tile_y++) {
    const size_t page_row = (size_t)(tile_y >> 5) * layout->pages_wide;
    const size_t metatile_row = ((tile_y >> 1) & 15u) << 4;
    const unsigned quadrant_row = (tile_y & 1u) << 1;
    for (unsigned tile_x = 0; tile_x < layout->tile_width; tile_x++) {
      const size_t page = page_row + (tile_x >> 5);
      const size_t metatile =
          metatile_row + ((tile_x >> 1) & 15u);
      const uint8_t id = map[page * kActionBgPageBytes + metatile];
      const unsigned quadrant = quadrant_row | (tile_x & 1u);
      const uint8_t *definition =
          definitions + (size_t)id * kActionBgMetatileBytes + quadrant * 2;
      world->scratch_tiles[(size_t)tile_y * layout->tile_width + tile_x] =
          (ReadWord(definition) & input->word_mask) | attributes;
    }
  }
}

static void Publish(ActionBgWorld *world, const ActionBgLayout *layout,
                    const ActionBgPublicationKey *key) {
  uint16_t *tiles = world->tiles;
  world->tiles = world->scratch_tiles;
  world->scratch_tiles = tiles;
  size_t capacity = world->tile_capacity;
  world->tile_capacity = world->scratch_tile_capacity;
  world->scratch_tile_capacity = capacity;

  uint8_t *source = world->source;
  world->source = world->scratch_source;
  world->scratch_source = source;
  capacity = world->source_capacity;
  world->source_capacity = world->scratch_source_capacity;
  world->scratch_source_capacity = capacity;

  world->source_size = layout->source_size;
  world->key = *key;
  world->has_publication = true;
  world->valid = true;
  if (++world->serial == 0) world->serial = 1;
}

ActionBgWorld *ActionBgWorld_Create(void) {
  return calloc(1, sizeof(ActionBgWorld));
}

void ActionBgWorld_Destroy(ActionBgWorld *world) {
  if (!world) return;
  free(world->tiles);
  free(world->scratch_tiles);
  free(world->source);
  free(world->scratch_source);
  free(world);
}

void ActionBgWorld_Reset(ActionBgWorld *world) {
  if (!world) return;
  world->source_size = 0;
  memset(&world->key, 0, sizeof(world->key));
  world->serial = 0;
  world->has_publication = false;
  world->valid = false;
}

bool ActionBgWorld_Update(ActionBgWorld *world,
                          const ActionBgDecodeInput *input) {
  ActionBgLayout layout;
  if (!world || !ValidateInput(input, &layout)) {
    Invalidate(world);
    return false;
  }
  const ActionBgPublicationKey key = MakeKey(input, &layout);
  if (SourcesEqual(world, input, &layout, &key)) {
    world->valid = true;
    return true;
  }
  if (!ReserveScratch(world, &layout)) {
    Invalidate(world);
    return false;
  }
  SnapshotSources(world, input, &layout);
  DecodeSnapshot(world, input, &layout);
  Publish(world, &layout, &key);
  return true;
}

ActionBgLookupResult ActionBgWorld_Lookup(const ActionBgWorld *world,
                                           int tile_x, int tile_y,
                                           uint16_t *entry) {
  if (!world || !world->valid || !entry)
    return kActionBgLookup_ProviderFailure;
  if (tile_x < 0 || tile_y < 0 ||
      (unsigned)tile_x >= world->key.tile_width ||
      (unsigned)tile_y >= world->key.tile_height)
    return kActionBgLookup_OutsideWorld;
  *entry = world->tiles[(size_t)(unsigned)tile_y * world->key.tile_width +
                        (unsigned)tile_x];
  return kActionBgLookup_Tile;
}

bool ActionBgWorld_IsValid(const ActionBgWorld *world) {
  return world && world->valid;
}

uint32_t ActionBgWorld_Serial(const ActionBgWorld *world) {
  return world && world->valid ? world->serial : 0;
}

unsigned ActionBgWorld_TileWidth(const ActionBgWorld *world) {
  return world && world->valid ? world->key.tile_width : 0;
}

unsigned ActionBgWorld_TileHeight(const ActionBgWorld *world) {
  return world && world->valid ? world->key.tile_height : 0;
}

size_t ActionBgWorld_TileCount(const ActionBgWorld *world) {
  if (!world || !world->valid) return 0;
  return (size_t)world->key.tile_width * world->key.tile_height;
}
