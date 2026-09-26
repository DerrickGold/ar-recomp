#ifndef AR_SIM_MENU_HELP_H
#define AR_SIM_MENU_HELP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { kSimMenuHelpColumns=22, kSimMenuHelpRows=6,
       kSimMenuHelpGlyphs=kSimMenuHelpColumns*kSimMenuHelpRows,
       kSimMenuHelpTextCapacity=16384 };
typedef struct SimMenuHelpPage {
  bool active, more;
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

/* Native-sized read-only text pages, tracked by UTF-8 source boundaries.
 * Both renderers share these boundaries and reveal/acknowledgement state. */
bool SimMenuHelp_Build(SimMenuHelpPage *page, const char *text, size_t bytes,
                       size_t start, bool more_authored_pages);
const char *SimMenuHelp_Action(unsigned action);
const char *SimMenuHelp_Category(unsigned category);
const char *SimMenuHelp_Item(unsigned item);
#endif
