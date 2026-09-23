#ifndef ACTRAISER_LOCALIZATION_COMPOSE_STATE_H
#define ACTRAISER_LOCALIZATION_COMPOSE_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "actraiser/actraiser_localization_grid.h"
#include "actraiser/actraiser_localization_resolved_text.h"
#include "actraiser/actraiser_localization_routes.h"

#define ACTRAISER_LOCALIZATION_COMPOSE_STATE_ABI_VERSION UINT32_C(14)

enum {
  kActRaiserLocalizationComposeSurfaceFirst = 2,
  kActRaiserLocalizationComposeSurfaceLast = 17,
  kActRaiserLocalizationComposeSurfaceCapacity = 16,
  kActRaiserLocalizationComposeSemanticIdCapacity = 256,
};

/* Surfaces with a lifecycle managed explicitly by the runtime. */
enum {
  kActRaiserLocalizationNameEntrySurface = 5,
  kActRaiserLocalizationTitleTextSurface = 14,
  kActRaiserLocalizationTitleSelectorSurface = 15,
  kActRaiserLocalizationSoundTestSurface = 16,
  kActRaiserLocalizationTitleCopyrightSurface = 17,
};

typedef struct ActRaiserLocalizationComposeSnapshot {
  /* A live native source can remain observed without claiming any pixels.
   * This permits enabling/recovering enhanced text without a native redraw. */
  bool observed;
  bool active;
  uint32_t surface_id;
  uint64_t generation_serial;
  ActRaiserResolvedText text;
  ArTextCellRegion region;
  uint16_t native_destination;
  uint8_t native_font_pixels;
  ArLocalizationTextLayoutKind layout;
  /* Non-None when this surface is one of the game's fixed cell menus. The
   * geometry is derived once here, when the route and region are decided,
   * rather than rebuilt for every frame that publishes the surface. */
  ActRaiserLocalizationMenu menu;
  ArLocalizationTextGrid grid;
  char semantic_id[kActRaiserLocalizationComposeSemanticIdCapacity];
} ActRaiserLocalizationComposeSnapshot;

typedef struct ActRaiserLocalizationComposeState {
  size_t struct_size;
  uint32_t abi_version;
  uint64_t dialogue_replacement_serial;
  uint8_t map_group;
  uint8_t map_number;
  bool scene_valid;
  uint8_t message_speed_maximum;
  ActRaiserLocalizationComposeSnapshot
      surfaces[kActRaiserLocalizationComposeSurfaceCapacity];
} ActRaiserLocalizationComposeState;

void ActRaiserLocalizationComposeState_Init(
    ActRaiserLocalizationComposeState *state);
/* Applies to the next scale generation only, not an already open selector. */
bool ActRaiserLocalizationComposeState_SetMessageSpeedMaximum(
    ActRaiserLocalizationComposeState *state, unsigned maximum);
void ActRaiserLocalizationComposeState_Clear(
    ActRaiserLocalizationComposeState *state);
void ActRaiserLocalizationComposeState_SetScene(
    ActRaiserLocalizationComposeState *state,
    uint8_t map_group, uint8_t map_number);
/* Releases one semantic surface at a proven native generation boundary. */
bool ActRaiserLocalizationComposeState_ReleaseSurface(
    ActRaiserLocalizationComposeState *state, uint32_t surface_id);
bool ActRaiserLocalizationComposeState_Process(
    ActRaiserLocalizationComposeState *state,
    const ActRaiserLocalizationComposeObservation *observation,
    ActRaiserLocalizationComposeTextResolver resolve_text,
    void *resolve_context, char *error, size_t error_capacity);
/* Rebuilds one active persistent surface only when its expected revision has
 * changed. A failed rebuild releases ownership so current native cells remain
 * visible instead of leaving stale enhanced values on screen. */
bool ActRaiserLocalizationComposeState_Refresh(
    ActRaiserLocalizationComposeState *state, uint32_t surface_id,
    uint64_t expected_source_revision,
    ActRaiserLocalizationComposeTextResolver resolve_text,
    void *resolve_context, char *error, size_t error_capacity);
/* Resolve the latest revision transactionally when the revision is itself
 * derived from live UI state. A failed refresh releases native-cell ownership
 * rather than retaining stale enhanced text. */
bool ActRaiserLocalizationComposeState_RefreshLatest(
    ActRaiserLocalizationComposeState *state, uint32_t surface_id,
    ActRaiserLocalizationComposeTextResolver resolve_text,
    void *resolve_context, char *error, size_t error_capacity);
bool ActRaiserLocalizationComposeState_AppendFrame(
    const ActRaiserLocalizationComposeState *state, ArLocalizationFrame *frame,
    ArTextCellDestination destination, const ActRaiserTextPalette *palette);
bool ActRaiserLocalizationComposeState_DialogueWasReplaced(
    const ActRaiserLocalizationComposeState *state,
    uint64_t terminal_compose_serial);
size_t ActRaiserLocalizationComposeState_ActiveCount(
    const ActRaiserLocalizationComposeState *state);
const ActRaiserLocalizationComposeSnapshot *
ActRaiserLocalizationComposeState_Find(
    const ActRaiserLocalizationComposeState *state, uint32_t surface_id);
const ActRaiserLocalizationComposeSnapshot *
ActRaiserLocalizationComposeState_FindObserved(
    const ActRaiserLocalizationComposeState *state, uint32_t surface_id);

#endif /* ACTRAISER_LOCALIZATION_COMPOSE_STATE_H */
