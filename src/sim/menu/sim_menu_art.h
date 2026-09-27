#ifndef AR_SIM_MENU_ART_H
#define AR_SIM_MENU_ART_H
/* SimMenuArt: captures the SimMenuFrame the menu presenter draws: the menu
 * model and help page, localized labels, and icon art the runner decodes from
 * the scene's OBJ VRAM and CGRAM using the ROM's own composition tables.
 * Phase: capture (reads the runner).
 * Tests: tests/sim_menu_art_test.c */

#include "sim/menu/sim_menu_model.h"
#include "sim/menu/sim_menu_help.h"
#include "localization/localization_frame.h"
#include "snesrecomp/runner.h"

enum { kSimMenuArtDescribeAngel = 43, kSimMenuArtCount = 44, kSimMenuArtWidth = 32,
       kSimMenuArtHeight = kSimMenuArtCount * 16 };
typedef struct SimMenuFrame {
  SimMenuModel model;
  SimMenuHelpPage help;
  /* Native BG3 content retained alongside the modern menu, even when
   * enhanced text is disabled. Bounds come from the game adapter. */
  ArTextCellRegion preserved_bg3_region;
  /* All modern labels share their own bounded frame. Six dock titles plus
   * a category and eight inventory rows fit without consuming HUD/dialogue
   * snapshots or falling back when the native inventory fills that frame. */
  ArLocalizationFrame label_frame;
  bool valid;
  uint8_t scale_percent;
  /* Captured with input state; presentation never polls a live controller. */
  char describe_binding[64];
  uint32_t art_revision;
  /* Complete ROM prompt footprint, available before its first glyph reveals.
   * Enhanced prose is measured by the dialogue renderer instead. */
  uint16_t prompt_width, prompt_height;
  uint32_t argb[kSimMenuArtWidth * kSimMenuArtHeight];
  char labels[kSimMenuArtCount][64];
} SimMenuFrame;

/* Runtime ROM tables select both genuine color and grey compositions. The
 * runner decodes their pixels from the scene's current OBJ VRAM and CGRAM. */
bool SimMenuArt_Capture(SimMenuFrame *frame, const SnesRunnerApi *api,
                        SrRunnerHandle *runner);

/* Measures only the audited selector prompts; never executes text controls. */
bool SimMenuArt_MeasurePrompt(const uint8_t *rom, size_t bytes, unsigned source,
                             unsigned *width, unsigned *height);

#endif
