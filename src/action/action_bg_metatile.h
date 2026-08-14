#ifndef ACTION_BG_METATILE_H
#define ACTION_BG_METATILE_H

#include <stdint.h>

/* Apply the live action-background descriptor to one metatile definition
 * word. The same rule is used by the native strip builders, the complete
 * finite-world decoder, render-only Sky Palace repair, and ROM backdrops. */
static inline uint16_t ActionBg_ComposeTilemapWord(
    uint16_t definition_word, uint16_t preserved_bit_mask,
    uint16_t common_attribute_bits) {
  return (uint16_t)((definition_word & preserved_bit_mask) |
                    common_attribute_bits);
}

#endif /* ACTION_BG_METATILE_H */
