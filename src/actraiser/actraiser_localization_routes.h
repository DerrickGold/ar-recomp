#ifndef ACTRAISER_LOCALIZATION_ROUTES_H
#define ACTRAISER_LOCALIZATION_ROUTES_H

#include <stdbool.h>
#include <stdint.h>

#include "actraiser/actraiser_localization_text.h"
#include "render/text_cell_record.h"

enum {
  kActRaiserLocalizationMaximumNativePreserves = 8,
  kActRaiserLocalizationMaximumPreservedTileWords = 4,
};

typedef enum ActRaiserLocalizationRouteMatch {
  kActRaiserLocalizationRouteMatch_Caller = 1u << 0,
  kActRaiserLocalizationRouteMatch_Context = 1u << 1,
  kActRaiserLocalizationRouteMatch_SelectorX = 1u << 2,
  kActRaiserLocalizationRouteMatch_MapNumber = 1u << 3,
} ActRaiserLocalizationRouteMatch;

/* USA-ROM addresses are deliberately confined to this game adapter. Language
 * packs and renderer-facing snapshots receive only stable semantic IDs. */
typedef struct ActRaiserLocalizationRoute {
  uint32_t source_pc24;
  uint32_t caller_pc24;
  uint32_t context_pc24;
  const char *semantic_id;
  uint32_t surface_id;
  ArTextCellRegion region;
  uint16_t selector_x;
  uint8_t native_page_count;
  uint16_t native_page_units[8];
  uint8_t native_font_pixels;
  uint8_t preserved_tile_word_count;
  uint16_t preserved_tile_words[
      kActRaiserLocalizationMaximumPreservedTileWords];
  uint8_t match_flags;
  uint8_t map_number;
} ActRaiserLocalizationRoute;

const ActRaiserLocalizationRoute *ActRaiserLocalizationRoute_ResolveDialogue(
    const ActRaiserLocalizationTextObservation *observation);
uint16_t ActRaiserLocalizationRoute_PageUnitCount(
    const ActRaiserLocalizationRoute *route, uint32_t native_page_index);

/* Locate native non-text cells embedded in a route's ownership rectangle.
 * Tile identity includes palette/priority and ignores only H/V flip flags.
 * SIZE_MAX means malformed/capacity failure and must retain native rendering. */
size_t ActRaiserLocalizationRoute_FindNativePreserves(
    const ActRaiserLocalizationRoute *route,
    uint16_t tilemap_base_words,
    unsigned tilemap_width_tiles,
    unsigned tilemap_height_tiles,
    const uint16_t *vram_words,
    size_t vram_word_count,
    ArTextCellRegion *preserves,
    size_t preserve_capacity);

#endif /* ACTRAISER_LOCALIZATION_ROUTES_H */
