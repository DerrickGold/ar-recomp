#include "actraiser/actraiser_localization_routes.h"

#include <stddef.h>

#include "actraiser_game.h"

enum {
  kDialogueSurface = 1,
  kSkyPalaceMapGroup = 0,
  kFirstSimulationTown = 1,
  kLastSimulationTown = 6,
};

/* Page-unit totals count calls into the native top-level byte decoder,
 * including the page/end control which completes that page. They are only a
 * timing ratio: external text is always indexed by complete grapheme cluster.
 * The address-only table is generated from the complete private USA
 * extraction; it contains no retail wording or decoded operation streams. */
#include "actraiser/actraiser_localization_route_data.inc"

_Static_assert(sizeof(kDialogueRoutes) / sizeof(kDialogueRoutes[0]) == 365,
               "USA scoped dialogue route census changed");

static bool InCurrentRuntimeScope(uint8_t map_group, uint8_t map_number) {
  return map_group == kSkyPalaceMapGroup &&
      (map_number == kActRaiserNonActionMap_SkyPalace ||
       (map_number >= kFirstSimulationTown &&
        map_number <= kLastSimulationTown));
}

const ActRaiserLocalizationRoute *ActRaiserLocalizationRoute_ResolveDialogue(
    const ActRaiserLocalizationTextObservation *observation) {
  if (!observation ||
      observation->abi_version !=
          ACTRAISER_LOCALIZATION_TEXT_OBSERVATION_ABI_VERSION ||
      !InCurrentRuntimeScope(observation->map_group, observation->map_number))
    return NULL;
  size_t low = 0;
  size_t high = sizeof(kDialogueRoutes) / sizeof(kDialogueRoutes[0]);
  while (low < high) {
    const size_t middle = low + (high - low) / 2u;
    if (kDialogueRoutes[middle].source_pc24 < observation->source_pc24)
      low = middle + 1u;
    else
      high = middle;
  }
  for (size_t index = low;
       index < sizeof(kDialogueRoutes) / sizeof(kDialogueRoutes[0]) &&
       kDialogueRoutes[index].source_pc24 == observation->source_pc24;
       ++index) {
    const ActRaiserLocalizationRoute *route = &kDialogueRoutes[index];
    if ((route->match_flags & kActRaiserLocalizationRouteMatch_Caller) &&
        route->caller_pc24 != observation->caller_pc24)
      continue;
    if ((route->match_flags & kActRaiserLocalizationRouteMatch_Context) &&
        route->context_pc24 != observation->context_pc24)
      continue;
    if ((route->match_flags & kActRaiserLocalizationRouteMatch_SelectorX) &&
        route->selector_x != observation->selector_x)
      continue;
    if ((route->match_flags & kActRaiserLocalizationRouteMatch_MapNumber) &&
        route->map_number != observation->map_number)
      continue;
    return route;
  }
  return NULL;
}

uint16_t ActRaiserLocalizationRoute_PageUnitCount(
    const ActRaiserLocalizationRoute *route, uint32_t native_page_index) {
  if (!route || !route->native_page_count) return 0;
  if (native_page_index >= route->native_page_count)
    native_page_index = route->native_page_count - 1u;
  return route->native_page_units[native_page_index];
}

static size_t TilemapWordAddress(uint16_t base, unsigned width,
                                 unsigned column, unsigned row) {
  const unsigned screens_across = width / 32u;
  const unsigned screen = (row / 32u) * screens_across + column / 32u;
  return (size_t)base + screen * 1024u +
      (row % 32u) * 32u + column % 32u;
}

size_t ActRaiserLocalizationRoute_FindNativePreserves(
    const ActRaiserLocalizationRoute *route,
    uint16_t tilemap_base_words,
    unsigned tilemap_width_tiles,
    unsigned tilemap_height_tiles,
    const uint16_t *vram_words,
    size_t vram_word_count,
    ArTextCellRegion *preserves,
    size_t preserve_capacity) {
  if (!route || !vram_words || !preserves ||
      (tilemap_width_tiles != 32u && tilemap_width_tiles != 64u) ||
      (tilemap_height_tiles != 32u && tilemap_height_tiles != 64u) ||
      route->preserved_tile_word_count >
          kActRaiserLocalizationMaximumPreservedTileWords ||
      (unsigned)route->region.column + route->region.columns >
          tilemap_width_tiles ||
      (unsigned)route->region.row + route->region.rows >
          tilemap_height_tiles)
    return SIZE_MAX;
  size_t count = 0;
  for (unsigned row = route->region.row;
       row < (unsigned)route->region.row + route->region.rows; ++row) {
    for (unsigned column = route->region.column;
         column < (unsigned)route->region.column + route->region.columns;
         ++column) {
      const size_t address = TilemapWordAddress(
          tilemap_base_words, tilemap_width_tiles, column, row);
      if (address >= vram_word_count) return SIZE_MAX;
      const uint16_t identity = vram_words[address] & UINT16_C(0x3FFF);
      bool preserved = false;
      for (uint8_t index = 0;
           index < route->preserved_tile_word_count; ++index) {
        if (identity == (route->preserved_tile_words[index] &
                         UINT16_C(0x3FFF))) {
          preserved = true;
          break;
        }
      }
      if (!preserved) continue;
      if (count >= preserve_capacity) return SIZE_MAX;
      preserves[count++] = (ArTextCellRegion){
          (uint8_t)column, (uint8_t)row, 1, 1};
    }
  }
  return count;
}
