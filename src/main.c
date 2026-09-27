#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h> /* SDL supplies UTF-8 argv from the wide command line. */
#endif

#include "app/application.h"
#include "platform/sdl/font_coverage_cli.h"
#include "platform/sdl/text_preview_cli.h"

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "--text-preview-v1"))
    return ArSdlTextPreview_Run(argc - 1, argv + 1);
  if (argc > 1 && !strcmp(argv[1], "--font-coverage-v1"))
    return ArSdlFontCoverage_Run(argc - 1, argv + 1);
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  return Application_Run(argc, argv);
}
