#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "diorama/diorama_bg_gpu.h"
#include "platform/sdl/render_sdl_interop.h"
#include "support/test_assert.h"

static void Fill(SrPpuBgPacket *p, unsigned width, unsigned height, unsigned phase) {
  memset(p, 0, sizeof(*p)); SrPpuBgPacket_Begin(p, width, height); p->words[2] = 3;
  for (unsigned bg = 0; bg < 2; ++bg) for (unsigned y = 0; y < height; ++y) {
    unsigned row = SrPpuBgPacket_Row(bg, y);
    uint32_t *meta = p->words + SR_PPU_BG_PACKET_HEADER_WORDS + row * SR_PPU_BG_PACKET_ROW_WORDS;
    meta[0] = y % 4 + 1; meta[1] = 0xff345678; meta[2] = width / 4; meta[3] = width * 3 / 4;
    uint32_t colors[256];
    for (unsigned i = 0; i < 256; ++i) colors[i] =
        ((i & 1 ? 128u : 255u) << 24) | ((i * 543217u + y * 231u + bg * 999u + phase) & 0xffffff);
    assert(SrPpuBgPacket_SetPalette(p, bg, y, colors));
    uint32_t *row_pixels = SrPpuBgPacket_AllocatePixels(p, bg, y); assert(row_pixels);
    if (meta[0] >= 3) {
      meta[4] = (y + phase) & 7u;
      for (unsigned cell = 0; cell < (width + meta[4] + 7) / 8; ++cell) {
        row_pixels[cell * 2] = 0x2a6547fbu ^ (cell * 827129u + y * 32997u);
        row_pixels[cell * 2 + 1] = (cell & 7u) | ((cell % 3) << 3) |
            (cell & 1 ? 32u : 0u) | (((cell * 717u + 93u) & 255u) << 8);
      }
      if (meta[0] == 4) {
        uint32_t *edits = SrPpuBgPacket_AllocateEdits(p, meta); assert(edits);
        for (unsigned cell = 0; cell < (width + 7) / 8; ++cell)
          for (unsigned band = 0; band < 3; ++band) {
            uint32_t *edit = edits + cell * 9 + band * 3;
            edit[0] = 0x196ec037u ^ (cell * 736129u + phase * 99827u);
            edit[1] = (cell * 348177u + y * 128731u) & 0xffffffu;
            edit[2] = cell * 9252373u ^ (band * 272941u) ^ 0x195ac380u;
          }
      }
      continue;
    }
    if (meta[0] == 2) {
      for (unsigned cell = 0; cell < (width + 7) / 8; ++cell) {
        uint32_t *out = row_pixels + cell * 9;
        for (unsigned band = 0; band < 3; ++band) {
          out[band * 3] = 0x173bdc96u ^ (cell * 736129u + phase * 99827u);
          out[band * 3 + 1] = (cell * 348177u + y * 128731u) & 0xffffffu;
          out[band * 3 + 2] = cell * 9252373u ^ (band * 272941u);
        }
      }
      continue;
    }
    for (unsigned x = 0; x < width; ++x) {
      uint32_t value = (x % 13 == 0 ? 0x80000000u : 0);
      for (unsigned band = 0; band < 3; ++band) {
        unsigned code = (x + y * 17 + band * 31) % 259;
        if (x % 5 == 0) code = 0;
        if (x % 101 == 0) code = 511; /* Malformed codes must remain transparent. */
        value |= code << (band * 9);
      }
      row_pixels[x] = value;
    }
  }
}

int main(void) {
  if (!SDL_Init(SDL_INIT_VIDEO)) return 77;
  SDL_Window *window = SDL_CreateWindow("BG capture GPU parity", 64, 64, SDL_WINDOW_HIDDEN);
  ArRenderDevice device = {0};
  if (!ArSdlRenderBackend_CreateForWindow(&device, window, NULL)) {
    SDL_DestroyWindow(window); SDL_Quit(); return 77;
  }
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(&device);
  SDL_Texture *base_target = SDL_GetRenderTarget(renderer);
  SrPpuBgPacket *packet = malloc(sizeof(*packet)); assert(packet);
  const unsigned planes[2][3] = {{0, kDioramaPlane_Bg1Hi, kDioramaPlane_Bg1Far},
      {1, kDioramaPlane_Bg2Hi, kDioramaPlane_Bg2Far}};
  const unsigned widths[] = {640, 17, 384}, heights[] = {352, 9, 224};
  const uint32_t mask = (1u << 0) | (1u << 1) | (1u << kDioramaPlane_Bg1Hi) |
      (1u << kDioramaPlane_Bg2Hi) | (1u << kDioramaPlane_Bg1Far) | (1u << kDioramaPlane_Bg2Far);
  for (unsigned phase = 0; phase < 3; ++phase) {
    Fill(packet, widths[phase], heights[phase], phase);
    ArRenderTexture textures[kDioramaPlane_Count] = {0}; uint32_t changed = 0;
    assert(DioramaBgGpu_Resolve(&device, packet, mask, textures, NULL, &changed) == mask);
    assert(changed == mask && SDL_GetRenderTarget(renderer) == base_target);
    for (unsigned bg = 0; bg < 2; ++bg) for (unsigned band = 0; band < 3; ++band) {
      assert(SDL_SetRenderTarget(renderer, ArSdlRenderBackend_UnwrapTexture(textures[planes[bg][band]])));
      SDL_Surface *raw = SDL_RenderReadPixels(renderer, NULL); assert(raw);
      SDL_Surface *pixels = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_ARGB8888); assert(pixels);
      for (unsigned y = 0; y < 352; ++y) for (unsigned x = 0; x < 640; ++x) {
        uint32_t expected = x < widths[phase] && y < heights[phase]
            ? SrPpuBgPacket_Color(packet, bg, band, x, y) : 0;
        uint32_t got = ((uint32_t *)((uint8_t *)pixels->pixels + y * pixels->pitch))[x];
        if (got != expected) fprintf(stderr, "phase=%u bg=%u band=%u (%u,%u) %08x != %08x\n",
            phase, bg, band, x, y, got, expected);
        assert(got == expected);
      }
      SDL_DestroySurface(pixels); SDL_DestroySurface(raw);
    }
    assert(SDL_SetRenderTarget(renderer, base_target));
    assert(DioramaBgGpu_Resolve(&device, packet, mask, textures, NULL, &changed) == mask);
    assert(changed == 0);
  }
  /* An independent sky view aliases only the overlapping capture interval;
   * newly exposed columns and native periodic pages use their own tile rows. */
  for (unsigned periodic = 0; periodic < 2; ++periodic) {
    Fill(packet, 384, 256, periodic + 8);
    packet->words[2] |= 4; packet->words[4] = periodic ? 256 : 512;
    packet->words[5] = 256; packet->words[6] = 1;
    for (unsigned y = 0; y < 256; ++y) {
      uint32_t *meta = SrPpuBgPacket_Meta(packet, 2, y);
      meta[0] = 2; meta[1] = 0xff192b47; meta[3] = packet->words[4];
      if (!periodic) { meta[4] = 16u | (200u << 16); meta[5] = ((y + 1) % 256) * 640 + 16; }
      assert(SrPpuBgPacket_SetPalette(packet, 2, y, packet->words + SrPpuBgPacket_Meta(packet, 1, y)[6]));
      uint32_t *cells = SrPpuBgPacket_AllocatePixels(packet, 2, y); assert(cells);
      for (unsigned x = 0; x < packet->words[4] / 8; ++x) {
        cells[x * 3] = 0x925ea318u ^ (y * 73819u + x * 39371u);
        cells[x * 3 + 1] = (x * 537199u) & 0xffffffu;
      }
    }
    ArRenderTexture planes[kDioramaPlane_Count] = {0}, skybox = {0}; uint32_t changed;
    assert(DioramaBgGpu_Resolve(&device, packet, 0, planes, &skybox, &changed) == (1u << kDioramaPlane_Count));
    SDL_Texture *texture = ArSdlRenderBackend_UnwrapTexture(skybox);
    float w, h; assert(SDL_GetTextureSize(texture, &w, &h));
    assert(w == (periodic ? 256 : 640) && h == (periodic ? 256 : 352));
    assert(SDL_SetRenderTarget(renderer, texture));
    SDL_Surface *raw = SDL_RenderReadPixels(renderer, NULL); assert(raw);
    SDL_Surface *pixels = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_ARGB8888); assert(pixels);
    for (unsigned y = 0; y < (unsigned)h; ++y) for (unsigned x = 0; x < (unsigned)w; ++x) {
      const uint32_t expected = x < packet->words[4] && y < packet->words[5]
          ? SrPpuBgPacket_Color(packet, 2, 0, x, y) : 0;
      assert(((const uint32_t *)((const uint8_t *)pixels->pixels + y * pixels->pitch))[x] == expected);
    }
    SDL_DestroySurface(pixels); SDL_DestroySurface(raw);
    assert(SDL_SetRenderTarget(renderer, base_target));
  }
  Fill(packet, 384, 224, 2);
  ArRenderTexture restored_planes[kDioramaPlane_Count] = {0}; uint32_t restored_changed;
  assert(DioramaBgGpu_Resolve(&device, packet, mask, restored_planes, NULL, &restored_changed) == mask);
  ArRenderTexture resolved[kDioramaPlane_Count] = {0};
  uint32_t changed = 0;
  const uint32_t bg2_mask = (1u << 1) | (1u << kDioramaPlane_Bg2Hi) | (1u << kDioramaPlane_Bg2Far);
  const unsigned row = SrPpuBgPacket_Row(1, 0);
  /* One unsupported scanline rejects that entire source. The other source
   * stays cached, and restoring support refreshes only the rejected source. */
  packet->words[SR_PPU_BG_PACKET_HEADER_WORDS + row * SR_PPU_BG_PACKET_ROW_WORDS] = 0;
  assert(DioramaBgGpu_Resolve(&device, packet, mask, resolved, NULL, &changed) == (mask & ~bg2_mask));
  assert(changed == 0);
  packet->words[SR_PPU_BG_PACKET_HEADER_WORDS + row * SR_PPU_BG_PACKET_ROW_WORDS] = 1;
  assert(DioramaBgGpu_Resolve(&device, packet, mask, resolved, NULL, &changed) == mask);
  assert(changed == bg2_mask);
  packet->words[SrPpuBgPacket_Meta(packet, 1, 0)[6] + 10] ^= 255u;
  assert(DioramaBgGpu_Resolve(&device, packet, mask, resolved, NULL, &changed) == mask);
  assert(changed == bg2_mask);
  /* An edit-only change must invalidate the source even when its native tile
   * row, palette, and edit allocation offsets are all unchanged. */
  uint32_t *edit_row = SrPpuBgPacket_Meta(packet, 1, 3);
  assert(edit_row[0] == 4 && edit_row[5]);
  packet->words[edit_row[5] + 2] ^= 0x01000001u;
  assert(DioramaBgGpu_Resolve(&device, packet, mask, resolved, NULL, &changed) == mask);
  assert(changed == bg2_mask);
  const unsigned edit_offset = edit_row[5];
  edit_row[5] = packet->words[3] - SR_PPU_BG_PACKET_PIXEL_STRIDE + 1;
  assert(DioramaBgGpu_Resolve(&device, packet, mask, resolved, NULL, &changed) == 0);
  edit_row[5] = edit_offset;
  packet->words[0] = SR_PPU_BG_PACKET_WIDTH + 1;
  assert(DioramaBgGpu_Resolve(&device, packet, mask, resolved, NULL, &changed) == 0);
  assert(changed == 0 && SDL_GetRenderTarget(renderer) == base_target);
  Fill(packet, 17, 9, 4);
  uint32_t *bad_row = SrPpuBgPacket_Meta(packet, 0, 0);
  const unsigned palette = bad_row[6];
  bad_row[6] = packet->words[3] - 255;
  assert(DioramaBgGpu_Resolve(&device, packet, mask, resolved, NULL, &changed) == 0);
  bad_row[6] = palette; bad_row[7] = UINT32_MAX;
  assert(DioramaBgGpu_Resolve(&device, packet, mask, resolved, NULL, &changed) == 0);

  /* Resolving inside a compositor pass must restore its target, logical
   * presentation, viewport and clip. Private targets use native pixel units. */
  SDL_Texture *scene = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
      SDL_TEXTUREACCESS_TARGET, 64, 64);
  assert(scene && SDL_SetRenderTarget(renderer, scene));
  assert(SDL_SetRenderLogicalPresentation(renderer, 32, 32, SDL_LOGICAL_PRESENTATION_LETTERBOX));
  const SDL_Rect viewport = {2, 3, 24, 25}, clip = {4, 5, 12, 13};
  assert(SDL_SetRenderViewport(renderer, &viewport) && SDL_SetRenderClipRect(renderer, &clip));
  Fill(packet, 17, 9, 4);
  assert(DioramaBgGpu_Resolve(&device, packet, mask, resolved, NULL, &changed) == mask);
  assert(changed == mask && SDL_GetRenderTarget(renderer) == scene);
  int width, height;
  SDL_RendererLogicalPresentation mode;
  assert(SDL_GetRenderLogicalPresentation(renderer, &width, &height, &mode));
  assert(width == 32 && height == 32 && mode == SDL_LOGICAL_PRESENTATION_LETTERBOX);
  SDL_Rect restored;
  assert(SDL_GetRenderViewport(renderer, &restored));
  assert(memcmp(&restored, &viewport, sizeof(restored)) == 0);
  assert(SDL_RenderClipEnabled(renderer) && SDL_GetRenderClipRect(renderer, &restored));
  assert(memcmp(&restored, &clip, sizeof(restored)) == 0);
  assert(SDL_SetRenderTarget(renderer, base_target));
  SDL_DestroyTexture(scene);
  /* Queue consumers between changed packets without readbacks. Every saved
   * tile must retain its own revision while transfer/storage buffers cycle. */
  SDL_Texture *history = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
      SDL_TEXTUREACCESS_TARGET, 512, 32);
  assert(history && SDL_SetRenderTarget(renderer, history));
  assert(SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED));
  assert(SDL_SetRenderViewport(renderer, NULL) && SDL_SetRenderClipRect(renderer, NULL));
  for (unsigned step = 0; step < 16; ++step) {
    Fill(packet, 32, 32, 20 + step);
    const unsigned plane = planes[0][step % 3];
    assert(DioramaBgGpu_Resolve(&device, packet, 1u << plane, resolved, NULL, &changed) == (1u << plane));
    SDL_Texture *source = ArSdlRenderBackend_UnwrapTexture(resolved[plane]);
    const SDL_FRect from = {0, 0, 32, 32}, to = {(float)step * 32, 0, 32, 32};
    assert(SDL_SetTextureBlendMode(source, SDL_BLENDMODE_NONE));
    assert(SDL_RenderTexture(renderer, source, &from, &to));
  }
  SDL_Surface *raw = SDL_RenderReadPixels(renderer, NULL); assert(raw);
  SDL_Surface *pixels = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_ARGB8888); assert(pixels);
  for (unsigned step = 0; step < 16; ++step) {
    Fill(packet, 32, 32, 20 + step);
    for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 32; ++x) {
      uint32_t got = ((uint32_t *)((uint8_t *)pixels->pixels + y * pixels->pitch))[step * 32 + x];
      assert(got == SrPpuBgPacket_Color(packet, 0, step % 3, x, y));
    }
  }
  SDL_DestroySurface(pixels); SDL_DestroySurface(raw);
  assert(SDL_SetRenderTarget(renderer, base_target));
  SDL_DestroyTexture(history);
  DioramaBgGpu_Reset(&device);
  free(packet); ArSdlRenderBackend_Destroy(&device);
  SDL_DestroyWindow(window); SDL_Quit();
  puts("live BG capture GPU parity passed (all six bands, changing palettes, padding and extents)");
  return 0;
}
