#include <stdio.h>
#include <string.h>
#if defined(_WIN32) || defined(__ANDROID__)
#include <SDL3/SDL.h>
/* Windows: SDL supplies UTF-8 argv from the wide command line. Android: this
 * renames main to SDL_main, which SDLActivity calls from libmain.so. */
#include <SDL3/SDL_main.h>
#endif

#include "app/application.h"
#include "platform/sdl/font_coverage_cli.h"
#include "platform/sdl/text_preview_cli.h"

#ifdef __ANDROID__
#include "platform/android/android_platform.h"
#endif

static int RunProcess(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "--text-preview-v1"))
    return ArSdlTextPreview_Run(argc - 1, argv + 1);
  if (argc > 1 && !strcmp(argv[1], "--font-coverage-v1"))
    return ArSdlFontCoverage_Run(argc - 1, argv + 1);
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  return Application_Run(argc, argv);
}

int main(int argc, char **argv) {
#ifdef __ANDROID__
  return AndroidPlatform_RunWithDesktopStack(RunProcess, argc, argv);
#else
  return RunProcess(argc, argv);
#endif
}
