#ifndef ACTRAISER_LOCALIZATION_ROUTES_H
#define ACTRAISER_LOCALIZATION_ROUTES_H

#include <stdbool.h>
#include <stdint.h>

#include "actraiser/actraiser_localization_text.h"

#include "localization/text_cell_record.h"

/* Interactive dialogue scope remains separate from fixed UI/capture scope. */
bool ActRaiserLocalizationRoute_InScope(uint8_t map_group, uint8_t map_number);
bool ActRaiserLocalizationRoute_FixedTextInScope(uint8_t map_group,
                                               uint8_t map_number);

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
  uint8_t match_flags;
  uint8_t map_number;
} ActRaiserLocalizationRoute;

typedef enum ActRaiserLocalizationComposeRouteMatch {
  kActRaiserLocalizationComposeRouteMatch_SourceTable = 1u << 0,
  kActRaiserLocalizationComposeRouteMatch_Selector = 1u << 1,
} ActRaiserLocalizationComposeRouteMatch;

/* Persistent fixed-composer surfaces use one stable surface ID per ownership
 * slot. A later route for that slot replaces it without exposing USA-ROM
 * identity to the renderer or language pack. */
typedef struct ActRaiserLocalizationComposeRoute {
  uint32_t source_pc24;
  uint32_t source_table_pc24;
  const char *semantic_id;
  uint32_t surface_id;
  ArTextCellRegion region;
  uint16_t destination;
  uint16_t source_selector;
  uint8_t native_font_pixels;
  uint8_t match_flags;
} ActRaiserLocalizationComposeRoute;

const ActRaiserLocalizationRoute *ActRaiserLocalizationRoute_ResolveDialogue(
    const ActRaiserLocalizationTextObservation *observation);
const ActRaiserLocalizationComposeRoute *
ActRaiserLocalizationRoute_ResolveCompose(
    const ActRaiserLocalizationComposeObservation *observation);
uint16_t ActRaiserLocalizationRoute_PageUnitCount(
    const ActRaiserLocalizationRoute *route, uint32_t native_page_index);

#endif /* ACTRAISER_LOCALIZATION_ROUTES_H */
