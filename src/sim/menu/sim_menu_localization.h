#ifndef AR_SIM_MENU_LOCALIZATION_H
#define AR_SIM_MENU_LOCALIZATION_H
/* SimMenuLocalization: resolves and captures the modern menu's labels and
 * read-only help in the shared localization frame format.
 * Phase: game side.
 * Tests: tests/actraiser_localization_schedule_test.c */

#include "sim/menu/sim_menu_model.h"
#include "sim/menu/sim_menu_help.h"
#include "localization/localization_frame.h"
#include "localization/dialogue_session.h"

void SimMenuLocalization_CaptureLabels(
    ArLocalizationFrame *labels, const ArLocalizationFrame *source,
    const SimMenuModel *menu, const uint16_t *cgram, size_t cgram_count);
bool SimMenuLocalization_PrepareHelp(
    const ArDialoguePageSnapshot *source, SimMenuHelpPage *help);
void SimMenuLocalization_AppendHelp(
    ArLocalizationFrame *frame, const SimMenuHelpPage *help,
    const uint16_t *cgram, size_t cgram_count);

#endif
