#if defined(AR_HAS_SDL3_TTF) && AR_HAS_SDL3_TTF
#include "platform/sdl/text_fonts_sdl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool Fail(char *error, size_t capacity, const char *message) {
  if (error && capacity)
    snprintf(error, capacity, "%s", message);
  return false;
}

static void CloseSet(ArSdlFontSet *set) {
  if (set->primary) {
    TTF_ClearFallbackFonts(set->primary);
    TTF_CloseFont(set->primary);
  }
  for (size_t i = 0; i < kArTextBackendMaximumFallbackFonts; ++i)
    if (set->fallbacks[i])
      TTF_CloseFont(set->fallbacks[i]);
  for (size_t i = 0; i <= kArTextBackendMaximumFallbackFonts; ++i)
    if (set->streams[i])
      SDL_CloseIO(set->streams[i]);
  memset(set, 0, sizeof(*set));
}

void ArSdlTextFonts_Destroy(ArSdlTextFonts *fonts) {
  if (!fonts)
    return;
  for (size_t i = 0; fonts->sets && i < fonts->capacity; ++i)
    CloseSet(&fonts->sets[i]);
  free(fonts->sets);
  for (size_t i = 0; i < fonts->role_count; ++i) {
    ArFontResource_Release(&fonts->roles[i].primary);
    for (size_t j = 0; j < fonts->roles[i].fallback_count; ++j)
      ArFontResource_Release(&fonts->roles[i].fallbacks[j]);
  }
  memset(fonts, 0, sizeof(*fonts));
}

static bool AcquireRole(ArSdlFontRole *role, const char *name,
                        ArFontResourceId primary,
                        const ArFontResourceId *fallbacks, size_t count,
                        const ArFontResources *resources, char *error,
                        size_t capacity) {
  snprintf(role->name, sizeof(role->name), "%s", name);
  role->fallback_count = count;
  role->ids[0] = primary;
  for (size_t i = 0; i < count; ++i)
    role->ids[i + 1] = fallbacks[i];
  if (!ArFontResource_Acquire(&role->primary, resources, primary, error,
                              capacity))
    return false;
  for (size_t i = 0; i < count; ++i)
    if (!ArFontResource_Acquire(&role->fallbacks[i], resources, fallbacks[i],
                                error, capacity))
      return false;
  return true;
}

bool ArSdlTextFonts_Init(ArSdlTextFonts *fonts,
                         const ArTextBackendConfig *config, char *error,
                         size_t capacity) {
  if (!fonts || !ArTextBackendConfig_IsValid(config) ||
      config->cached_size_capacity > kArSdlMaximumFontVariants)
    return Fail(error, capacity, "invalid text font registry configuration");
  fonts->role_count = config->role_count + 1;
  fonts->capacity = config->cached_size_capacity;
  fonts->sets = calloc(fonts->capacity, sizeof(*fonts->sets));
  if (!fonts->sets)
    return Fail(error, capacity, "out of memory creating font cache");
  if (!AcquireRole(&fonts->roles[0], "body", config->primary_font,
                   config->fallback_fonts, config->fallback_font_count,
                   &config->resources, error, capacity))
    return false;
  for (size_t i = 0; i < config->role_count; ++i) {
    const ArTextFontRole *role = &config->roles[i];
    if (!AcquireRole(&fonts->roles[i + 1], role->name, role->primary,
                     role->fallbacks, role->fallback_count, &config->resources,
                     error, capacity))
      return false;
  }
  return true;
}

void ArSdlTextFonts_BeginLayout(ArSdlTextFonts *fonts) {
  for (size_t i = 0; i < fonts->capacity; ++i)
    fonts->sets[i].pinned = false;
}

static bool OpenSet(ArSdlFontSet *set, const ArSdlFontRole *role, int pixels,
                    bool italic) {
  set->role = role;
  set->pixels = pixels;
  set->italic = italic;
  for (size_t i = 0; i <= role->fallback_count; ++i) {
    const ArFontResourceData *data =
        i ? &role->fallbacks[i - 1].data : &role->primary.data;
    set->streams[i] = SDL_IOFromConstMem(data->bytes, data->size);
    TTF_Font *font = set->streams[i]
                         ? TTF_OpenFontIO(set->streams[i], false, (float)pixels)
                         : NULL;
    if (i)
      set->fallbacks[i - 1] = font;
    else
      set->primary = font;
    if (!font)
      return false;
    TTF_SetFontStyle(font, italic ? TTF_STYLE_ITALIC : TTF_STYLE_NORMAL);
    if (i && !TTF_AddFallbackFont(set->primary, font))
      return false;
  }
  return true;
}

ArSdlFontSet *ArSdlTextFonts_Acquire(ArSdlTextFonts *fonts, const char *name,
                                     int pixels, bool italic,
                                     ArTextRasterFailure *failure, char *error,
                                     size_t capacity) {
  if (failure)
    *failure = kArTextRasterFailure_Retryable;
  if (!name || !name[0])
    name = "body";
  const ArSdlFontRole *role = NULL;
  for (size_t i = 0; i < fonts->role_count; ++i)
    if (!strcmp(fonts->roles[i].name, name))
      role = &fonts->roles[i];
  if (!role || pixels < 1 || pixels > 4096) {
    if (failure)
      *failure = kArTextRasterFailure_Deterministic;
    Fail(error, capacity, "unknown font role or invalid font size");
    return NULL;
  }
  if (++fonts->clock == 0)
    fonts->clock = 1;
  size_t victim = fonts->capacity;
  for (size_t i = 0; i < fonts->capacity; ++i) {
    ArSdlFontSet *set = &fonts->sets[i];
    if (set->primary && set->role == role && set->pixels == pixels &&
        set->italic == italic) {
      set->last_use = fonts->clock;
      set->pinned = true;
      if (failure)
        *failure = kArTextRasterFailure_None;
      return set;
    }
    if (!set->pinned && (victim == fonts->capacity ||
                         set->last_use < fonts->sets[victim].last_use))
      victim = i;
  }
  if (victim == fonts->capacity) {
    if (failure)
      *failure = kArTextRasterFailure_Deterministic;
    Fail(error, capacity, "text page requires too many distinct font variants");
    return NULL;
  }
  ArSdlFontSet opened = {0};
  if (!OpenSet(&opened, role, pixels, italic)) {
    Fail(error, capacity, SDL_GetError());
    CloseSet(&opened);
    return NULL;
  }
  opened.last_use = fonts->clock;
  opened.pinned = true;
  CloseSet(&fonts->sets[victim]);
  fonts->sets[victim] = opened;
  if (failure)
    *failure = kArTextRasterFailure_None;
  return &fonts->sets[victim];
}
#endif
