#ifndef ACTRAISER_DIALOGUE_WINDOW_H
#define ACTRAISER_DIALOGUE_WINDOW_H

#include "actraiser/actraiser_localization_text_style.h"

/* Normalized retained rows and their authored annotations. The game and the
 * Workshop share this composition; the host chooses page/clear transitions. */
typedef struct ActRaiserDialogueWindow {
  ArTextBidiSpans bidi;
  ActRaiserTextStylePlan styles;
  bool valid;
  uint16_t native_first_page;
  uint16_t native_clear_control_count;
  uint32_t current_page;
  uint64_t revision;
  size_t bytes;
  size_t current_page_offset;
  size_t current_page_source_offset;
  size_t current_page_source_bytes;
  uint16_t reveal_offsets[kArLocalizationFrameTextCapacity + 1u];
  uint8_t structural_boundaries[AR_TEXT_BOUNDARY_BYTES(
      kArLocalizationFrameTextCapacity)];
  uint32_t clusters;
  char text[kArLocalizationFrameTextCapacity];
} ActRaiserDialogueWindow;

bool ActRaiserDialogueWindow_Build(ActRaiserDialogueWindow *window,
                                   const ArDialogueSession *session,
                                   const ArDialoguePageSnapshot *current,
                                   uint16_t first_page,
                                   uint16_t clear_control_count);
size_t
ActRaiserDialogueWindow_RevealedBytes(const ActRaiserDialogueWindow *window,
                                      size_t source_revealed);
bool ActRaiserDialogueWindow_ControlClears(const char *control_id);

#endif
