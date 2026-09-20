#include "platform/sdl/text_preview_cli.h"
#include <string.h>
int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "--text-preview-v1"))
    return ArSdlTextPreview_Run(argc - 1, argv + 1);
  return ArSdlTextPreview_Run(argc, argv);
}
