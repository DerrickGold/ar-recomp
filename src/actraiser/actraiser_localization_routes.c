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

/* Address-only fixed-composer inventory for Sky Palace, simulation, action
 * cards and title options. Indicator-only sources are intentionally not
 * language routes; their typed ownership is handled by the surface adapter. */
#include "actraiser/actraiser_localization_compose_route_data.inc"

_Static_assert(sizeof(kDialogueRoutes) / sizeof(kDialogueRoutes[0]) == 365,
               "USA scoped dialogue route census changed");
_Static_assert(sizeof(kComposeRoutes) / sizeof(kComposeRoutes[0]) == 90,
               "USA scoped fixed-composer route census changed");

bool ActRaiserLocalizationRoute_InScope(uint8_t map_group, uint8_t map_number) {
  return map_group == kSkyPalaceMapGroup &&
      (map_number == kActRaiserNonActionMap_SkyPalace ||
       map_number == kActRaiserNonActionMap_Temple ||
       (map_number >= kFirstSimulationTown &&
        map_number <= kLastSimulationTown));
}

bool ActRaiserLocalizationRoute_FixedTextInScope(uint8_t group, uint8_t map) {
  return ActRaiserLocalizationRoute_InScope(group, map) ||
      (group == 0 && map == kActRaiserNonActionMap_Title) ||
      (group >= kActRaiserActionMapGroup_First &&
       group <= kActRaiserActionMapGroup_Last);
}

const ActRaiserLocalizationRoute *ActRaiserLocalizationRoute_ResolveDialogue(
    const ActRaiserLocalizationTextObservation *observation) {
  if (!observation ||
      observation->abi_version !=
          ACTRAISER_LOCALIZATION_TEXT_OBSERVATION_ABI_VERSION ||
      !ActRaiserLocalizationRoute_InScope(
          observation->map_group, observation->map_number))
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

const ActRaiserLocalizationComposeRoute *
ActRaiserLocalizationRoute_ResolveCompose(
    const ActRaiserLocalizationComposeObservation *observation) {
  if (!observation ||
      observation->abi_version !=
          ACTRAISER_LOCALIZATION_COMPOSE_OBSERVATION_ABI_VERSION ||
      !ActRaiserLocalizationRoute_FixedTextInScope(
          observation->map_group, observation->map_number))
    return NULL;
  size_t low = 0;
  size_t high = sizeof(kComposeRoutes) / sizeof(kComposeRoutes[0]);
  while (low < high) {
    const size_t middle = low + (high - low) / 2u;
    if (kComposeRoutes[middle].source_pc24 < observation->source_pc24)
      low = middle + 1u;
    else
      high = middle;
  }
  for (size_t index = low;
       index < sizeof(kComposeRoutes) / sizeof(kComposeRoutes[0]) &&
       kComposeRoutes[index].source_pc24 == observation->source_pc24;
       ++index) {
    const ActRaiserLocalizationComposeRoute *route = &kComposeRoutes[index];
    /* Identical native destinations have different owners in each scene. */
    switch (route->scope) {
      case kActRaiserLocalizationComposeScope_Title:
        if (observation->map_group || observation->map_number) continue;
        break;
      case kActRaiserLocalizationComposeScope_Action:
        if (observation->map_group < kActRaiserActionMapGroup_First ||
            observation->map_group > kActRaiserActionMapGroup_Last) continue;
        break;
      case kActRaiserLocalizationComposeScope_Simulation:
        if (!ActRaiserLocalizationRoute_InScope(
                observation->map_group, observation->map_number)) continue;
        break;
      case kActRaiserLocalizationComposeScope_SoundTest:
        /* The dormant native modal can overlay a scene. Source/destination
         * alone must not confuse it with an action card at the same cell. */
        if (observation->caller_pc24 != UINT32_C(0x0297F0)) continue;
        break;
      default:
        continue;
    }
    if (route->destination != observation->destination)
      continue;
    if ((route->match_flags &
         kActRaiserLocalizationComposeRouteMatch_SourceTable) &&
        route->source_table_pc24 != observation->source_table_pc24)
      continue;
    if ((route->match_flags &
         kActRaiserLocalizationComposeRouteMatch_Selector) &&
        route->source_selector != observation->source_selector)
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
