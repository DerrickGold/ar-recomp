#ifndef AR_LOCALIZATION_TEXT_PRESENTATION_H
#define AR_LOCALIZATION_TEXT_PRESENTATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "localization/font_resource.h"

#define AR_TEXT_PRESENTATION_ABI_VERSION UINT32_C(3)

enum { kArTextPresentationMaximumFallbackFonts = 8 };

/* Borrowed for the duration of prepare_font. The host owns font instances,
 * texture uploads and caching; the dialogue runtime owns only this identity. */
typedef struct ArTextPresentationFont {
  size_t struct_size;
  uint32_t abi_version;
  const char *stack_id;
  ArFontResourceId primary;
  uint64_t revision;
  const ArFontResourceId *fallbacks;
  size_t fallback_count;
  const ArTextFontRole *roles;
  size_t role_count;
} ArTextPresentationFont;

/* Injected once on the game/presenter thread, before applying settings. A true
 * result means the font was opened and a probe rasterized and uploaded, not
 * merely that a filename exists. Failure must retain the active resources.
 * This is activation readiness, not a promise of coverage for every glyph or
 * of future allocation success. No renderer or platform handle crosses here. */
typedef struct ArTextPresentationHost {
  size_t struct_size;
  uint32_t abi_version;
  void *context;
  bool (*prepare_font)(void *context, const ArTextPresentationFont *font,
                       char *error, size_t error_capacity);
  /* Host input only: a pack locator and logical member (including builtin:).
   * The game never resolves a physical font path. Each nonzero result owns a
   * registration, retired on rejection/replacement/shutdown. */
  ArFontResourceId (*register_font)(void *context, const char *manifest,
                                    const char *member, char *error, size_t capacity);
  void (*retire_font)(void *context, ArFontResourceId font);
  /* Aborts only the staged preflight, never the active font/cache. Called
   * on rejected selection so an unused candidate cannot hold the budget. */
  void (*discard_prepared_font)(void *context);
} ArTextPresentationHost;

/* Same-thread, pointer-free feedback mailbox. A nonzero ticket identifies one
 * logical dialogue window, not a surface, source file or frame-slot address.
 * Begin/End bracket a whole presented frame, including fallback/early-return
 * paths. Only a fully drawn enhanced window marks its ticket ready. Failures
 * remain latched through re-presents; old tickets cannot overwrite newer ones. */
void ArTextPresentation_BeginFrame(uint64_t ticket);
void ArTextPresentation_MarkReady(uint64_t ticket);
void ArTextPresentation_EndFrame(void);
bool ArTextPresentation_Failed(uint64_t ticket);

#endif
