#include "platform/sdl/text_rasterizer_sdl.h"
#include <stdio.h>

int main(void) {
  ArTextBackend backend = {0};
  ArSdlTextBackend_Init(&backend);
  ArSdlTextRasterizer adapter = {0};
  char error[128];
  if (ArTextBackend_IsReady(&backend) ||
      ArSdlTextRasterizer_Init(&adapter, NULL, error, sizeof(error)) ||
      !error[0] || ArSdlTextRasterizer_Get(&adapter)) {
    fprintf(stderr, "No-TTF build advertised a font backend\n");
    return 1;
  }
  ArSdlTextRasterizer_Destroy(&adapter);
  ArSdlTextRasterizer_Destroy(&adapter);
  return 0;
}
