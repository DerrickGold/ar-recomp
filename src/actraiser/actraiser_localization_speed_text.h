#ifndef ACTRAISER_LOCALIZATION_SPEED_TEXT_H
#define ACTRAISER_LOCALIZATION_SPEED_TEXT_H

#include "actraiser_localization_resolved_text.h"

/* Project the authored numeric row onto the captured gameplay range. All
 * other wording, inline artwork, styles and directional spans are retained.
 * Returns false on an incompatible row; callers retain native fallback. */
bool ActRaiserLocalizationSpeedText_Project(ActRaiserResolvedText *text, unsigned maximum);

#endif
