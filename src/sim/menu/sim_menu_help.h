#ifndef AR_SIM_MENU_HELP_H
#define AR_SIM_MENU_HELP_H
/* SimMenuHelp: retains authored help for the dialogue system. Only the native
 * glyph renderer needs a 22 x 6 grid; enhanced text uses measured dialogue
 * pages. Names the help entries for menu actions, categories and items.
 * Phase: pure.
 * Tests: tests/sim_menu_help_test.c */
#include "localization/dialogue_pager.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { kSimMenuHelpColumns=22, kSimMenuHelpRows=6,
       kSimMenuHelpGlyphs=kSimMenuHelpColumns*kSimMenuHelpRows,
       kSimMenuHelpTextCapacity=16384 };
typedef struct SimMenuHelpPage {
  bool active, more, enhanced;
  ArDialoguePager pager;
  uint32_t revealed_bytes;
  uint32_t authored_page, source_start, source_end;
  uint32_t glyph_count, revealed_glyphs, bytes;
  uint32_t source_ends[kSimMenuHelpGlyphs];
  uint32_t text_ends[kSimMenuHelpGlyphs];
  uint32_t scalars[kSimMenuHelpGlyphs];
  uint8_t row[kSimMenuHelpGlyphs], column[kSimMenuHelpGlyphs];
  /* Exact source slice; row/column above describe native-only soft wrapping. */
  char text[kSimMenuHelpTextCapacity];
  char locale[33];
  uint8_t direction;
} SimMenuHelpPage;

/* Keep an authored page for enhanced layout, or a native-sized source slice.
 * Reveals and acknowledgements still belong to its dialogue session. */
bool SimMenuHelp_Build(SimMenuHelpPage *page, const char *text, size_t bytes,
                       size_t start, bool more_authored_pages, bool enhanced);
const char *SimMenuHelp_Action(unsigned action);
const char *SimMenuHelp_Category(unsigned category);
const char *SimMenuHelp_Item(unsigned item);
#endif
