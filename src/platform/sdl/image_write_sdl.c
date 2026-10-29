#include "render/image_write.h"
#include <SDL3/SDL.h>
#include <limits.h>

bool ArImage_WritePng(FILE *file, const uint8_t *pixels, int width, int height,
    int pitch, ArImageFormat format) {
  if (!file || !pixels || width <= 0 || height <= 0 ||
      format < kArImageRgb24 || format > kArImageXrgb8888) return false;
  const int channels = format == kArImageRgb24 ? 3 : 4;
  if (width > INT_MAX / channels || pitch < width * channels) return false;
  const SDL_PixelFormat formats[] = {
    SDL_PIXELFORMAT_RGB24, SDL_PIXELFORMAT_RGBA32, SDL_PIXELFORMAT_XRGB8888
  };
  SDL_Surface *surface = SDL_CreateSurfaceFrom(width, height, formats[format], (void *)pixels, pitch);
  SDL_IOStream *stream = surface ? SDL_IOFromDynamicMem() : NULL;
  bool ok = stream && SDL_SavePNG_IO(surface, stream, false);
  if (ok) {
    const Sint64 length = SDL_GetIOSize(stream);
    const void *data = SDL_GetPointerProperty(SDL_GetIOProperties(stream),
        SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, NULL);
    ok = length > 0 && (uint64_t)length <= SIZE_MAX && data &&
        fwrite(data, 1, (size_t)length, file) == (size_t)length;
  }
  if (stream) ok = SDL_CloseIO(stream) && ok;
  SDL_DestroySurface(surface);
  return ok && !ferror(file);
}
