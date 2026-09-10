#ifndef ACTRAISER_LOCALIZATION_WORLD_NAVIGATION_H
#define ACTRAISER_LOCALIZATION_WORLD_NAVIGATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "actraiser/actraiser_localization_compose_state.h"

enum {
  /* Allocate immediately after the native fixed-composer range so extending
   * that range cannot silently collide with the navigation label. */
  kActRaiserLocalizationWorldNavigationSurface =
      kActRaiserLocalizationComposeSurfaceLast + 1,
  kActRaiserLocalizationWorldNavigationTextCapacity = 512,
};

typedef struct ActRaiserLocalizationWorldNavigation {
  bool resolved;
  uint16_t attempted_location;
  uint32_t cluster_count;
  uint64_t source_revision;
  size_t utf8_bytes;
  ArLocalizationTextLanguage language;
  ArTextBidiSpans bidi;
  char utf8[kActRaiserLocalizationWorldNavigationTextCapacity];
} ActRaiserLocalizationWorldNavigation;

void ActRaiserLocalizationWorldNavigation_Init(
    ActRaiserLocalizationWorldNavigation *state);
void ActRaiserLocalizationWorldNavigation_Invalidate(
    ActRaiserLocalizationWorldNavigation *state);

/* Publishes the current destination name into the authentic screen-space
 * plaque interior. A hidden native label is not claimed. Resolution is cached
 * per location and invalidated by the runtime whenever the selected pack
 * changes. */
bool ActRaiserLocalizationWorldNavigation_Append(
    ActRaiserLocalizationWorldNavigation *state,
    ArLocalizationFrame *frame, uint16_t active_location,
    bool native_label_visible,
    const uint16_t *cgram_words, size_t cgram_word_count,
    ActRaiserLocalizationComposeTextResolver resolve_text,
    void *resolve_context, char *error, size_t error_capacity);

#endif /* ACTRAISER_LOCALIZATION_WORLD_NAVIGATION_H */
