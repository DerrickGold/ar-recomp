#ifndef ACTRAISER_LOCALIZATION_GRID_H
#define ACTRAISER_LOCALIZATION_GRID_H

#include <stdbool.h>

#include "localization/localization_frame.h"
#include "localization/language_row_shape.h"

/* ActRaiser's fixed cell menus. These identities stay on the game side: the
 * renderer receives the derived ArLocalizationTextGrid and never learns which
 * screen it is drawing. */
typedef enum ActRaiserLocalizationMenu {
  kActRaiserLocalizationMenu_None = kArLanguageRowShape_None,
  kActRaiserLocalizationMenu_StatusCities = kArLanguageRowShape_Cities,
  kActRaiserLocalizationMenu_StatusScore = kArLanguageRowShape_Score,
  kActRaiserLocalizationMenu_StatusMaster = kArLanguageRowShape_Master,
  kActRaiserLocalizationMenu_FixedRows = kArLanguageRowShape_FixedRows,
  kActRaiserLocalizationMenu_MessageSpeed = kArLanguageRowShape_MessageSpeed,
  kActRaiserLocalizationMenu_SoundTest = kArLanguageRowShape_SoundTest,
} ActRaiserLocalizationMenu;

/* Derives the renderer-neutral description of one menu's native geometry for
 * the region it is drawn in. The result is a function of the menu and the
 * region only, so it can be built once per surface per frame.
 *
 * Rules are produced by walking every line and row shape the region can hold
 * and coalescing identical neighbours, so the published grid is the menu's own
 * geometry by construction rather than a hand transcription of it. */
bool ActRaiserLocalizationGrid_Build(ActRaiserLocalizationMenu menu,
                                     ArTextCellRegion region,
                                     ArLocalizationTextGrid *grid);

#endif /* ACTRAISER_LOCALIZATION_GRID_H */
