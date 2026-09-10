#ifndef AR_PLATFORM_SDL_TEXT_RASTERIZER_H
#define AR_PLATFORM_SDL_TEXT_RASTERIZER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "localization/text_backend.h"

#define AR_SDL_TEXT_RASTERIZER_CONFIG_ABI_VERSION \
  AR_TEXT_BACKEND_CONFIG_ABI_VERSION

typedef ArTextBackendConfig ArSdlTextRasterizerConfig;

/* Platform-owned adapter with no SDL/SDL_ttf type in its public layout. */
typedef struct ArSdlTextRasterizer {
  void *implementation;
  ArTextRasterizer rasterizer;
} ArSdlTextRasterizer;

bool ArSdlTextRasterizer_Init(
    ArSdlTextRasterizer *adapter,
    const ArSdlTextRasterizerConfig *config,
    char *error, size_t error_capacity);
void ArSdlTextRasterizer_Destroy(ArSdlTextRasterizer *adapter);
const ArTextRasterizer *ArSdlTextRasterizer_Get(
    const ArSdlTextRasterizer *adapter);

/* Factory binding used by renderer-independent consumers. They receive only
 * ArTextBackend; this SDL-specific constructor stays at the host boundary. */
void ArSdlTextBackend_Init(ArTextBackend *backend);

#endif /* AR_PLATFORM_SDL_TEXT_RASTERIZER_H */
