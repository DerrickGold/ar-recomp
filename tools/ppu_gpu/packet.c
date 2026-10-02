#include "packet.h"
#include <string.h>

static void Word(PpuGpuProbePacket *p, unsigned at, uint32_t value) {
  for (unsigned b = 0; b < 4; ++b) p->rgba[at * 4 + b] = (uint8_t)(value >> (b * 8));
}
static int Floor8(int x) { return x >= 0 ? x / 8 : -((7 - x) / 8); }
static int Delta(uint16_t value, uint16_t anchor) {
  int d = (value - anchor) & 1023;
  return d >= 512 ? d - 1024 : d;
}
static uint32_t Color(uint16_t color, uint16_t subtract, unsigned alpha) {
  uint32_t out = alpha << 24;
  for (unsigned c = 0; c < 3; ++c) {
    int v = (int)((color >> (c * 5)) & 31) - (int)((subtract >> (c * 5)) & 31);
    if (v < 0) v = 0;
    out |= (uint32_t)((v << 3) | (v >> 2)) << (16 - c * 8);
  }
  return out;
}

bool PpuGpuProbe_Build(const SrSceneFrame *f, PpuGpuProbePacket *p) {
  if (!p) return false;
  p->width = p->height = 0;
  if (!f || f->struct_size != sizeof(*f) || !f->vram || !f->cgram || !f->mosaic ||
      f->extra_x > 128 || f->top + f->bottom > 128 ||
      (f->main_screen | f->sub_screen) & ~3u || f->cgwsel & 1u ||
      f->background_view.width || f->native_page_mask) return false;
  const unsigned flags = SR_PPU_OVERLAY_REMOVE_FROM_GAME | SR_PPU_OVERLAY_MARK_BG_HALF_ADD |
      SR_PPU_OVERLAY_APPLY_BG_FIXED_COLOR_SUBTRACT;
  for (unsigned bg = 0; bg < 2; ++bg) {
    const SrSceneBackground *b = &f->backgrounds[bg];
    if (!b->rows || !b->hscroll || !b->vscroll || !b->tiles.lookup ||
        b->tiles.flags != SR_PPU_VIRTUAL_TILEMAP_INCLUDE_AUTHENTIC || b->tiles.reserved ||
        b->tiles.camera_x < -65535 || b->tiles.camera_x > 65535 ||
        b->tiles.camera_y < -65535 || b->tiles.camera_y > 65535 ||
        b->tiles.hscroll_anchor > 1023 || b->tiles.vscroll_anchor > 1023 ||
        b->capture_flags & ~flags || b->edits.lookup || b->edits.apron || b->edits.reserved ||
        b->fill_mode > 2 || b->clip_top > 128 || b->clip_bottom > 128) return false;
    for (unsigned row = 0; row < 224; ++row)
      if ((f->mosaic[row] & (1u << bg)) && (f->mosaic[row] >> 4)) return false;
    for (unsigned row = 0; row < 224; ++row)
      if (b->rows[row].fill != SR_PPU_BACKGROUND_FILL_LIVE_WORLD ||
          b->rows[row].motion != SR_PPU_BACKGROUND_MOTION_NORMAL_SCROLL) return false;
  }
  memset(p->rgba, 0, sizeof(p->rgba));
  const unsigned width = 256 + f->extra_x * 2, height = 224 + f->top + f->bottom;
  Word(p, 0, width + 128); Word(p, 1, height); Word(p, 2, width);
  /* Two VRAM words per texel. No decoded atlas or palette expansion per pixel. */
  for (unsigned i = 0; i < 16384; ++i)
    Word(p, kProbeVramBase + i, f->vram[i * 2] | ((uint32_t)f->vram[i * 2 + 1] << 16));
  for (unsigned bg = 0; bg < 2; ++bg) {
    const SrSceneBackground *b = &f->backgrounds[bg];
    const unsigned alpha = b->capture_flags & SR_PPU_OVERLAY_MARK_BG_HALF_ADD ? 128 : 255;
    const uint16_t sub = b->capture_flags & SR_PPU_OVERLAY_APPLY_BG_FIXED_COLOR_SUBTRACT
        ? f->fixed_color : 0;
    for (unsigned i = 0; i < 256; ++i)
      Word(p, kProbePaletteBase + bg * 256 + i, Color(f->cgram[i], sub, alpha));
    const uint32_t fill = !b->fill_configured || !b->fill_mode ? 0 :
        b->fill_mode == 1 ? 0xff000000u : Color(f->cgram[b->fill_cgram], 0, 255);
    for (unsigned y = 0; y < height; ++y) {
      const int screen_y = (int)y - f->top;
      const unsigned row = screen_y < 0 ? 0 : screen_y >= 224 ? 223 : (unsigned)screen_y;
      const int wx = b->tiles.camera_x - f->extra_x + Delta(b->hscroll[row], b->tiles.hscroll_anchor);
      const int wy = b->tiles.camera_y + screen_y + 1 + Delta(b->vscroll[row], b->tiles.vscroll_anchor);
      const int tx = Floor8(wx), ty = Floor8(wy);
      unsigned left = b->rows[row].left < f->extra_x ? f->extra_x - b->rows[row].left : 0;
      unsigned right = width - (b->rows[row].right < f->extra_x ? f->extra_x - b->rows[row].right : 0);
      if (!((f->main_screen | f->sub_screen) & (1u << bg)) ||
          (screen_y < 0 && (-screen_y > b->top || (b->clip_vertical && -screen_y > b->clip_top))) ||
          (screen_y >= 224 && (screen_y - 223 > b->bottom ||
              (b->clip_vertical && screen_y - 223 > b->clip_bottom)))) left = right = 0;
      const unsigned desc = kProbeRowBase + (bg * SR_SCENE_HEIGHT + y) * kProbeRowWords;
      const unsigned tiles = kProbeTileBase + (bg * SR_SCENE_HEIGHT + y) * kProbeTilesPerRow;
      Word(p, desc, (unsigned)(wx - tx * 8)); Word(p, desc + 1, (unsigned)(wy - ty * 8));
      Word(p, desc + 2, left); Word(p, desc + 3, right);
      Word(p, desc + 4, fill); Word(p, desc + 5, tiles);
      if (left >= right) continue;
      const unsigned count = (width + (unsigned)(wx - tx * 8) + 7) / 8;
      const uint16_t *entries = NULL;
      int64_t stride = 0;
      uint32_t remaining = 0;
      for (unsigned x = 0; x < count; ++x) {
        uint16_t entry = 0;
        if (!remaining && b->tiles.lookup_span) {
          remaining = b->tiles.lookup_span(b->tiles.user_data, tx + (int)x, ty, 1,
              count - x, &entries, &stride);
          if (remaining > count - x || stride < PTRDIFF_MIN || stride > PTRDIFF_MAX) return false;
        }
        uint32_t result;
        if (remaining) {
          result = entries != NULL;
          if (entries) { entry = *entries; if (remaining > 1) entries += (ptrdiff_t)stride; }
          --remaining;
        } else result = b->tiles.lookup(b->tiles.user_data, tx + (int)x, ty, &entry);
        if (result > 1) return false; /* Native fallback requires a different command. */
        if (!result) continue;
        unsigned band = (entry >> 13) & 1u;
        uint8_t semantic = 0xff;
        if (b->tiles.band_lookup) {
          (void)b->tiles.band_lookup(b->tiles.user_data, tx + (int)x, ty, entry, &semantic);
          if (semantic != 0xff) band = semantic == 1 ? 0 : semantic == 2 ? 1 : 2;
        }
        Word(p, tiles + x, entry | (1u << 16) | (band << 17));
      }
    }
  }
  p->width = width + 128; p->height = height;
  return true;
}
