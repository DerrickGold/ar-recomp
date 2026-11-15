#ifndef AR_DIORAMA_TERRAIN_TRANSFER_H
#define AR_DIORAMA_TERRAIN_TRANSFER_H

#include "present/presentation_upload_mirror.h"

typedef struct DioramaTerrainSpare {
  ArRenderTexture textures[3];
  int width, height;
} DioramaTerrainSpare;

static inline void DioramaTerrainSpare_Reset(ArRenderDevice *device, DioramaTerrainSpare *spare) {
  for (unsigned i = 0; i < 3; ++i) ArRenderDevice_DestroyTexture(device, spare->textures[i]);
  *spare = (DioramaTerrainSpare){0};
}

static inline bool DioramaTerrainTextures(ArRenderDevice *device, ArRenderTexture textures[3], int w, int h) {
  const ArRenderTextureDesc desc = {.width = w, .height = h, .format = kArRenderPixelFormat_Argb8888,
      .usage = kArRenderTextureUsage_Streaming, .filter = kArRenderFilter_Nearest,
      .blend = kArRenderBlendMode_Alpha};
  for (unsigned i = 0; i < 3; ++i)
    if (!ArRenderTexture_IsValid(textures[i]) && !ArRenderDevice_CreateTexture(device, &desc, &textures[i]))
      return false;
  return true;
}

static inline void TerrainUploadRect(ArRenderTextureRegionUpload *uploads, unsigned *count, uint64_t *bytes,
    ArRenderTexture texture, const uint32_t *pixels, ArRenderRectI rect, int pitch) {
  if (rect.w <= 0 || rect.h <= 0) return;
  uploads[(*count)++] = (ArRenderTextureRegionUpload){texture, rect,
      pixels + (size_t)rect.y * pitch + rect.x, pitch * (int)sizeof(*pixels)};
  *bytes += (uint64_t)rect.w * rect.h * sizeof(*pixels);
}

/* Publish a capture-local view to the compositor. When the window moves, the
 * old textures supply the GPU-local overlap, and one packed upload supplies
 * exposed strips and changed pixels. Filter and priority UVs stay local. */
static inline bool DioramaTransferTerrainWindow(ArRenderDevice *device, DioramaTerrainSpare *spare,
    ArRenderTexture active[3], ArRenderRectI b, ArRenderRectI old, bool certified,
    const uint32_t *const pixels[3], const uint32_t *const old_pixels[3], uint64_t *bytes,
    unsigned *upload_count) {
  const bool moved = old.w > 0 && (b.x != old.x || b.y != old.y || b.w != old.w || b.h != old.h);
  ArRenderTexture *textures = moved ? spare->textures : active;
  if (moved && (spare->width != b.w || spare->height != b.h))
    DioramaTerrainSpare_Reset(device, spare);
  if (!DioramaTerrainTextures(device, textures, b.w, b.h)) return false;
  const int x = b.x > old.x ? b.x : old.x, y = b.y > old.y ? b.y : old.y;
  const int right = b.x + b.w < old.x + old.w ? b.x + b.w : old.x + old.w;
  const int bottom = b.y + b.h < old.y + old.h ? b.y + b.h : old.y + old.h;
  const bool overlap = old.w > 0 && right > x && bottom > y;
  const ArRenderRectI dst = {x - b.x, y - b.y, right - x, bottom - y};
  const ArRenderRectI src = {x - old.x, y - old.y, dst.w, dst.h};
  const ArRenderRectI exposed[] = {
    {0, 0, b.w, dst.y},
    {0, dst.y + dst.h, b.w, b.h - dst.y - dst.h},
    {0, dst.y, dst.x, dst.h},
    {dst.x + dst.w, dst.y, b.w - dst.x - dst.w, dst.h},
  };
  ArRenderTextureRegionCopy copies[3];
  ArRenderTextureRegionUpload uploads[15];
  unsigned copied = 0, updated = 0;
  for (unsigned band = 0; band < 3; ++band) {
    if (!overlap) {
      TerrainUploadRect(uploads, &updated, bytes, textures[band], pixels[band],
          (ArRenderRectI){0, 0, b.w, b.h}, b.w);
      continue;
    }
    if (moved) {
      copies[copied++] = (ArRenderTextureRegionCopy){
        active[band], textures[band], src, dst.x, dst.y};
      for (unsigned i = 0; i < sizeof(exposed) / sizeof(*exposed); ++i)
        TerrainUploadRect(uploads, &updated, bytes, textures[band], pixels[band], exposed[i], b.w);
    }
    if (certified) continue;
    const uint32_t *next = pixels[band] + (size_t)dst.y * b.w + dst.x;
    const uint32_t *old_pixel = old_pixels[band] + (size_t)src.y * old.w + src.x;
    ArRenderRectI dirty;
    if (PresentationUploadMirror_FindDirtyRect((const uint8_t *)next, b.w * 4, (const uint8_t *)old_pixel,
        old.w * 4, dst.w, dst.h, &dirty)) {
      dirty.x += dst.x;
      dirty.y += dst.y;
      TerrainUploadRect(uploads, &updated, bytes, textures[band], pixels[band], dirty, b.w);
    }
  }
  if (!ArRenderDevice_TransferTextureRegions(device, copies, copied, uploads, updated)) return false;
  if (upload_count) *upload_count = updated;
  if (moved) {
    for (unsigned i = 0; i < 3; ++i) {
      const ArRenderTexture old_texture = active[i];
      active[i] = spare->textures[i];
      spare->textures[i] = old_texture;
    }
    spare->width = old.w;
    spare->height = old.h;
  }
  return true;
}

#endif
