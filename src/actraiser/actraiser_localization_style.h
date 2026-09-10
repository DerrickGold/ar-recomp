#ifndef ACTRAISER_LOCALIZATION_STYLE_H
#define ACTRAISER_LOCALIZATION_STYLE_H

#include "localization/localization_frame.h"
#include "snes_bgr555.h"

/* Ordinary retail lettering uses three opaque inks. The adapter supplies the
 * active four-entry CGRAM palette; black ink 1 is not transparency. Credits
 * have a different one-ink font and must not call this helper. Brightness is
 * applied once at presentation, not baked into these cacheable endpoints. */
static inline uint32_t ActRaiserLocalizationStyle_Rgb(uint16_t color) {
  return (uint32_t)ExpandColor5(color, 15) << 16 |
      (uint32_t)ExpandColor5(color >> 5, 15) << 8 |
      ExpandColor5(color >> 10, 15);
}

static inline void ActRaiserLocalizationStyle_Ordinary(
    ArLocalizationTextSnapshot *snapshot, const uint16_t palette[4]) {
  snapshot->style_id = kArTextStyle_RetailPaletteBands;
  snapshot->shadow_enabled = true;
  /* Keep the established shape until the directional-keyline A/B is accepted. */
  snapshot->shadow_shape = kArTextShadow_Diagonal;
  snapshot->slant_ascii_numerals = true;
  snapshot->shadow_rgb = ActRaiserLocalizationStyle_Rgb(palette[1]);
  snapshot->band_rgb = ActRaiserLocalizationStyle_Rgb(palette[2]);
  snapshot->body_rgb = ActRaiserLocalizationStyle_Rgb(palette[3]);
}

#endif
