/* Shared by the shipped headless game command and the ROM-free test tool. */
#include "platform/sdl/font_coverage_cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/sdl/text_rasterizer_sdl.h"

int ArSdlFontCoverage_Run(int argc, char **argv) {
  if (argc < 2 || argc > 10) {
    fprintf(stderr, "usage: %s FONT [FALLBACK ...] < scalars.txt\n", argv[0]);
    return 2;
  }
  const ArSdlTextRasterizerConfig config = {
      .struct_size = sizeof(config),
      .abi_version = AR_SDL_TEXT_RASTERIZER_CONFIG_ABI_VERSION,
      .font_stack_id = "author-coverage",
      .primary_font_path = argv[1],
      .fallback_font_paths = (const char *const *)(argv + 2),
      .fallback_font_count = (size_t)argc - 2u,
      .font_revision = 1,
      .cached_size_capacity = 1,
  };
  ArSdlTextRasterizer adapter = {0};
  char error[kArTextRasterErrorCapacity] = {0};
  int result = 2;
  if (!ArSdlTextRasterizer_Init(&adapter, &config, error, sizeof(error)))
    goto done;
  /* Force font opening even for an empty or entirely non-rendering input. */
  bool provided;
  if (!ArTextRasterizer_HasGlyph(ArSdlTextRasterizer_Get(&adapter), 'A',
                                 &provided, error, sizeof(error)))
    goto done;
  char line[32];
  while (fgets(line, sizeof(line), stdin)) {
    const size_t digits = strspn(line, "0123456789abcdefABCDEF");
    char *end = NULL;
    const unsigned long scalar = strtoul(line, &end, 16);
    if (!digits || digits > 6 || end != line + digits ||
        (*end != '\n' && *end) || (*end == '\n' && end[1]) ||
        scalar > 0x10fffful || (scalar >= 0xd800ul && scalar <= 0xdffful)) {
      snprintf(error, sizeof(error), "invalid scalar input");
      goto done;
    }
    if (!ArTextRasterizer_HasGlyph(ArSdlTextRasterizer_Get(&adapter),
                                   (uint32_t)scalar, &provided, error,
                                   sizeof(error)))
      goto done;
    if (printf("%04lX\t%d\n", scalar, provided ? 1 : 0) < 0)
      goto done;
  }
  if (ferror(stdin) || fflush(stdout))
    goto done;
  result = 0;
done:
  if (result)
    fprintf(stderr, "font coverage: %s\n", error[0] ? error : "I/O failure");
  ArSdlTextRasterizer_Destroy(&adapter);
  return result;
}
