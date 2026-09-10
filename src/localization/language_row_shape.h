#ifndef AR_LOCALIZATION_LANGUAGE_ROW_SHAPE_H
#define AR_LOCALIZATION_LANGUAGE_ROW_SHAPE_H

#include <stdbool.h>
#include <stdint.h>
#include "localization/language_pack.h"

/* Portable content shapes. Geometry and native observation stay in the game
 * adapter; authoring and the adapter consult the same generated row rules. */
typedef enum ArLanguageRowShape {
  kArLanguageRowShape_None = 0,
  kArLanguageRowShape_Cities,
  kArLanguageRowShape_Score,
  kArLanguageRowShape_Master,
  kArLanguageRowShape_FixedRows,
  kArLanguageRowShape_MessageSpeed,
  /* Extracted JP reference only; playable packs use the US contract. */
  kArLanguageRowShape_MessageSpeedJP,
} ArLanguageRowShape;

ArLanguageRowShape ArLanguageRowShape_ForRoute(const char *semantic_id);
ArLanguageRowShape ArLanguageRowShape_ForProfile(
    const char *semantic_id, ArLanguageSourceProfile profile);
bool ArLanguageRowShape_Allows(ArLanguageRowShape shape, uint32_t line,
                               uint32_t fields);
bool ArLanguageRowShape_IsReserved(ArLanguageRowShape shape, uint32_t line);

#endif
