#ifndef AR_PLATFORM_SDL_TEXT_FONTS_H
#define AR_PLATFORM_SDL_TEXT_FONTS_H

#include "localization/text_backend.h"
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

enum { kArSdlMaximumFontVariants = 32 };

typedef struct ArSdlFontRole {
  char name[kArTextBackendRoleCapacity];
  ArFontResourceLease primary;
  ArFontResourceLease fallbacks[kArTextBackendMaximumFallbackFonts];
  size_t fallback_count;
  ArFontResourceId ids[1 + kArTextBackendMaximumFallbackFonts];
} ArSdlFontRole;

typedef struct ArSdlFontSet {
  TTF_Font *primary;
  TTF_Font *fallbacks[kArTextBackendMaximumFallbackFonts];
  SDL_IOStream *streams[1 + kArTextBackendMaximumFallbackFonts];
  const ArSdlFontRole *role;
  int pixels;
  bool italic, pinned;
  uint64_t last_use;
} ArSdlFontSet;

/* One registry owns all role leases and a bounded shared cache. Each active
 * layout pins the variants it uses, so acquiring a later run's font cannot
 * evict a font retained by an earlier shaped run. Single-threaded per backend.
 */
typedef struct ArSdlTextFonts {
  ArSdlFontRole roles[1 + kArTextBackendMaximumFontRoles];
  size_t role_count;
  ArSdlFontSet *sets;
  size_t capacity;
  uint64_t clock;
} ArSdlTextFonts;

bool ArSdlTextFonts_Init(ArSdlTextFonts *fonts,
                         const ArTextBackendConfig *config, char *error,
                         size_t error_capacity);
void ArSdlTextFonts_Destroy(ArSdlTextFonts *fonts);
void ArSdlTextFonts_BeginLayout(ArSdlTextFonts *fonts);
ArSdlFontSet *ArSdlTextFonts_Acquire(ArSdlTextFonts *fonts, const char *role,
                                     int pixels, bool italic,
                                     ArTextRasterFailure *failure, char *error,
                                     size_t error_capacity);

#endif
