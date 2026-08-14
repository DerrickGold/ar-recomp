/* Isolated widescreen Sky Palace background repair.
 *
 * Action-stage world margins are owned by the bounded HLE provider. This file
 * retains the unrelated Sky Palace source path: decode its traced ROM metatile
 * page into only the offscreen BG2 columns used for presentation, render, then
 * restore the game's UI/staging tilemap verbatim. */

#include "actraiser_game.h"
#include "actraiser_rtl.h"
#include "action/action_bg_metatile.h"
#include "common_rtl.h"
#include "settings.h"
#include "snes/ppu.h"
#include "widescreen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern Ppu *g_ppu;

enum {
  kSkyPalaceSourceMap = 0x07D0A0,
  kSkyPalaceSourceMetatilesPerRow = 16,
  kSkyPalaceSourceTileRows = 32,
  kSkyPalaceSourceShaftRow = 8,
  kSkyPalaceDialogFirstMetatileRow = 9,
  kSkyPalaceDialogLastMetatileRow = 12,
  kSkyPalaceFloorMetatileRow = 13,
  kSkyPalaceMetatileDefinitionBase = 0x56,
  kSkyPalaceMetatileWordMask = 0x58,
  kSkyPalaceMetatileAttributeHigh = 0x6F,
  kTilemapWidthTiles = 64,
  kTilemapPageWidthTiles = 32,
  kMetatileSizeTiles = 2,
  kTileSizePixels = 8,
};

static uint16 s_sky_palace_bg2_backup[kActRaiserTilemapWords];
static int s_sky_palace_restore_pending;

static int ws_bg_debug_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("AR_WS_BGDBG");
    enabled = (e && e[0] && e[0] != '0');
  }
  return enabled;
}

static int ws_sky_palace_bg_enabled(void) {
  return g_settings.ws_skypalace_bg;
}

static uint16 ws_tilemap_addr_64(uint16 base, int tile_x, int tile_y) {
  tile_x &= 63;
  tile_y &= 63;
  return (uint16)(base + (tile_x & 31) + ((tile_y & 31) << 5) +
                  ((tile_x & 32) ? 0x400 : 0) +
                  ((tile_y & 32) ? 0x800 : 0));
}

void ActRaiser_WidescreenSkyPalaceRestore(void) {
  if (!s_sky_palace_restore_pending)
    return;
  if (g_ppu) {
    memcpy(&g_ppu->vram[kActRaiserBg2TilemapVram],
           s_sky_palace_bg2_backup,
           sizeof(s_sky_palace_bg2_backup));
  }
  s_sky_palace_restore_pending = 0;
}

/* Build a box-free Sky Palace BG2 only in tile columns sampled by the side
 * margins. The game's source-map setup at $02:B6F8-$B726 copies the 16x16
 * metatile page at ROM $07:D0A0 to WRAM $7E:C200. That page supplies the
 * authentic beam/capitals/shafts/floor. Its metatile rows 9-12 are the dialog
 * box itself; the underlying colonnade is the same shaft row already present
 * in rows 3-8, so use row 8 for those four obscured rows.
 *
 * We expand the source IDs through the game's live BG2 metatile table and
 * attribute/mask descriptors, exactly as $02:B90D does, but write only
 * offscreen VRAM columns. The center keeps the game's real BG2 bounding box.
 * The complete 64x64 BG2 ring is backed up and restored after scanout, making
 * the operation invisible to game logic and future UI composition. */
void ActRaiser_WidescreenSkyPalacePrepare(void) {
  ActRaiser_WidescreenSkyPalaceRestore();

  if (!ws_sky_palace_bg_enabled() || !g_ws_active || !g_ppu ||
      g_ram[kActRaiserWram_MapGroup] != kActRaiserMapGroup_NonAction ||
      g_ram[kActRaiserWram_CurrentMap] != kActRaiserNonActionMap_SkyPalace ||
      (g_ppu->extraLeftCur == 0 && g_ppu->extraRightCur == 0)) {
    return;
  }

  uint16 tilemap_base = (uint16)(
      (g_ppu->bgXsc[kActRaiserPpuLayer_Bg2] & 0xfc) << 8);
  uint16 metatile_definition_base = ActRaiser_ReadWram16(
      kSkyPalaceMetatileDefinitionBase);
  uint16 metatile_word_mask = ActRaiser_ReadWram16(
      kSkyPalaceMetatileWordMask);
  uint16 metatile_attributes = (uint16)(
      g_ram[kSkyPalaceMetatileAttributeHigh] << 8);
  const uint8 *source = RomFixedPtr(kSkyPalaceSourceMap);

  /* This source decoder is deliberately specific to the statically traced
   * Sky Palace layout. Reject unexpected descriptors/ROM revisions rather
   * than writing a plausible-looking but incorrect tilemap. */
  if (tilemap_base != kActRaiserBg2TilemapVram ||
      (g_ppu->bgXsc[kActRaiserPpuLayer_Bg2] & 3) != 3 ||
      metatile_definition_base > 0xF800 ||
      source[0] != 0x43 || source[16] != 0x2A ||
      source[32] != 0x32 || source[48] != 0x00) {
    if (ws_bg_debug_enabled()) {
      fprintf(stderr,
              "[ws-sky] source reject BG2SC=$%02X base=$%04X meta=$%04X "
              "header=%02X/%02X/%02X/%02X\n",
              g_ppu->bgXsc[kActRaiserPpuLayer_Bg2], tilemap_base,
              metatile_definition_base, source[0], source[16], source[32],
              source[48]);
    }
    return;
  }

  uint8 margin_col[64] = {0};
  uint8 center_col[64] = {0};
  int hscroll = g_ppu->hScroll[kActRaiserPpuLayer_Bg2] & 0x1ff;
  for (int x = 0; x < kActRaiserAuthenticWidth; x++)
    center_col[((hscroll + x) & 0x1ff) >> 3] = 1;
  for (int x = -(int)g_ppu->extraLeftCur; x < 0; x++)
    margin_col[((hscroll + x) & 0x1ff) >> 3] = 1;
  for (int x = kActRaiserAuthenticWidth;
       x < kActRaiserAuthenticWidth + (int)g_ppu->extraRightCur; x++)
    margin_col[((hscroll + x) & 0x1ff) >> 3] = 1;

  memcpy(s_sky_palace_bg2_backup,
         &g_ppu->vram[kActRaiserBg2TilemapVram],
         sizeof(s_sky_palace_bg2_backup));
  s_sky_palace_restore_pending = 1;

  int patched_columns = 0;
  for (int tile_x = 0; tile_x < 64; tile_x++) {
    /* A non-tile-aligned scroll can make one boundary tile serve center and
     * margin pixels. Preserve the center in that case; Sky Palace is normally
     * aligned at scroll zero, so no required column is skipped. */
    if (!margin_col[tile_x] || center_col[tile_x])
      continue;
    patched_columns++;
    int source_tile_x = tile_x & 31;  /* repeat the authentic 256px page */
    int meta_x = source_tile_x >> 1;
    for (int tile_y = 0; tile_y < 64; tile_y++) {
      int source_tile_y = tile_y & 31;
      int meta_y = source_tile_y >> 1;
      /* The 64x64 map is four quadrant canvases (2 x-pages x 2 y-bands); the
       * game picks one per UI state via h/v scroll (menu: vscroll~504 shows
       * the top band, dialogs: vscroll~248 the bottom band). Every canvas
       * shares one scene layout: page meta rows 0-8 (sky/beam/capitals/
       * shaft) and 13-15 (lower floor) apply directly, but the page's box
       * rows 9-12 overlap scene rows that the box merely covers:
       *  - meta cols 0/15 there hold the page-seam pillar-base halves
       *    (42/40 tops at sy 22-23, 4A/48 bottoms at sy 24-25) that
       *    complete the half-base visible at each center edge;
       *  - shaft columns get their base flare + floor reflection from the
       *    dedicated metatiles $41 (flare top; upper half = plain shaft)
       *    and $49 (flare bottom), flanked one metatile to each side by
       *    the curved skirt pieces $40/$48 (left) and $42/$4A (right) --
       *    the same metatiles the page seam uses, wrapped around a shaft.
       *    This art exists only in the metatile table, never in a page row
       *    (the page's box always covers it); recovered from the words the
       *    game's own boot colonnade left in the scratch columns;
       *  - the floor plane's top two rows sit under the box bottom, so
       *    sy 24-25 continue as floor (meta 13) at plain columns
       *    (validated against the live floor edge in both bands -- the
       *    page-native floor rows 13-15 only cover sy 26-31);
       *  - everything else continues the shaft row 8.
       * Layout validated cell-by-cell against the live map of both UI
       * states (2026-07-13). */
      int seam = (meta_x == 0 || meta_x == 15);
      int shaft_col = source[8 * 16 + meta_x] == 0x39;
      int left_flank = !seam && !shaft_col && meta_x < 15 &&
                       source[8 * 16 + meta_x + 1] == 0x39;
      int right_flank = !seam && !shaft_col && meta_x > 0 &&
                        source[8 * 16 + meta_x - 1] == 0x39;
      uint8 metatile;
      if (meta_y >= 11 && meta_y <= 12 && seam) {
        metatile = source[meta_y * 16 + meta_x];       /* seam base halves */
      } else if (meta_y >= 11 && meta_y <= 12 && shaft_col) {
        metatile = meta_y == 11 ? 0x41 : 0x49;         /* pillar base flare */
      } else if (meta_y >= 11 && meta_y <= 12 && left_flank) {
        metatile = meta_y == 11 ? 0x40 : 0x48;         /* skirt left of shaft */
      } else if (meta_y >= 11 && meta_y <= 12 && right_flank) {
        metatile = meta_y == 11 ? 0x42 : 0x4A;         /* skirt right of shaft */
      } else if (meta_y == 12) {
        metatile = source[13 * 16 + meta_x];           /* floor top rows */
      } else if (meta_y >= 9 && meta_y <= 12) {
        metatile = source[8 * 16 + meta_x];            /* shaft/empty */
      } else {
        metatile = source[meta_y * 16 + meta_x];
      }
      /* Definition words are stored row-major within the metatile:
       * TL, TR, BL, BR (validated against the authentic VRAM decode of the
       * same page — the x-major order transposes each 2x2 block, rendering
       * the split shaft metatiles as 8px checkerboards). */
      int quadrant = ((source_tile_y & 1) << 1) | (source_tile_x & 1);
      uint16 definition_word = ActRaiser_ReadWram16((uint16)(
          metatile_definition_base + metatile * 8 + quadrant * 2));
      uint16 tilemap_word = ActionBg_ComposeTilemapWord(
          definition_word, metatile_word_mask, metatile_attributes);
      g_ppu->vram[ws_tilemap_addr_64(
          tilemap_base, tile_x, tile_y)] = tilemap_word;
    }
  }

  if (!patched_columns) {
    ActRaiser_WidescreenSkyPalaceRestore();
    return;
  }

  if (ws_bg_debug_enabled()) {
    static unsigned last_gf = ~0u;
    unsigned game_frame = (unsigned)ActRaiser_ReadWram16(
        kActRaiserWram_GameFrame);
    if (game_frame != last_gf) {
      last_gf = game_frame;
      fprintf(stderr,
              "[ws-sky] gf=%u source=$07:D0A0 meta=$7E:%04X cols=%d "
              "scroll=$%04X margins=%u/%u (render-only)\n",
              game_frame, metatile_definition_base, patched_columns, hscroll,
              g_ppu->extraLeftCur, g_ppu->extraRightCur);
    }
  }
}
