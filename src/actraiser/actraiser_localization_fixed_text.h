#ifndef ACTRAISER_LOCALIZATION_FIXED_TEXT_H
#define ACTRAISER_LOCALIZATION_FIXED_TEXT_H

#include "actraiser/actraiser_localization_resolved_text.h"

/* Apply the game's fixed-surface text edits, preserving their annotations.
 * Name-entry values must already contain eight graphemes, padded with figure
 * spaces. The host adds the current key cursor after this shared composition.
 */
bool ActRaiserLocalizationFixedText_FromPage(const ArDialoguePageSnapshot *page,
                                             ActRaiserResolvedText *text,
                                             char *error,
                                             size_t error_capacity);

#endif
