#ifdef NDEBUG
#undef NDEBUG
#endif
#include "platform/sdl/gpu_texture_upload_layout.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  uint32_t cursor = 0;
  ArSdlTextureUploadLayout layout;
  assert(ArSdlTextureUploadLayout_Append(1, 1, true, &cursor, &layout));
  assert(layout.offset == 0 && layout.row_pitch == 256 && cursor == 256);
  assert(ArSdlTextureUploadLayout_Append(65, 3, true, &cursor, &layout));
  assert(layout.offset == 512 && layout.row_pitch == 512 && cursor == 2048);
  assert(ArSdlTextureUploadLayout_Append(64, 2, true, &cursor, &layout));
  assert(layout.offset == 2048 && layout.row_pitch == 256 && cursor == 2560);
  const uint32_t saved = cursor;
  assert(!ArSdlTextureUploadLayout_Append(0, 1, true, &cursor, &layout));
  assert(!ArSdlTextureUploadLayout_Append(1, -1, true, &cursor, &layout));
  assert(!ArSdlTextureUploadLayout_Append(INT_MAX, INT_MAX, true, &cursor, &layout));
  assert(!ArSdlTextureUploadLayout_Append(1, INT_MAX, true, &cursor, &layout));
  assert(cursor == saved);
  cursor = UINT32_MAX - 255;
  assert(!ArSdlTextureUploadLayout_Append(1, 1, true, &cursor, &layout));
  assert(cursor == UINT32_MAX - 255);
  cursor = 0;
  assert(ArSdlTextureUploadLayout_Append(1, 1, false, &cursor, &layout));
  assert(layout.offset == 0 && layout.row_pitch == 4 && cursor == 4);
  assert(ArSdlTextureUploadLayout_Append(65, 3, false, &cursor, &layout));
  assert(layout.offset == 4 && layout.row_pitch == 260 && cursor == 784);
  puts("gpu_texture_upload_layout_test: PASS");
  return 0;
}
