/* Isolated widescreen background presentation repairs.
 *
 * This is deliberately separate from the experimental OAM port on
 * widescreen-sprites-wip/widescreen-bg. It reuses the original recompiled
 * $02:B825 column decoder to populate only BG1/BG2 tilemap VRAM margin cells;
 * $00:8C98/$00:8D68 and the original $02:B158/$02:B1AF streamers remain
 * untouched.  Fast vertical motion is covered separately with the original
 * $02:B8A0 row decoder, avoiding a full set of column rebuilds every 16px.
 *
 * Static audit of $02:B825->$02:B90D (2026-07-12): it reads level-map and
 * metatile data, writes the caller-supplied WRAM upload record, uses DP $0E,
 * and invokes the SNES multiply registers through $02:BED3. It does not touch
 * PPU/OAM/CGRAM registers. To make those temporary effects unobservable to
 * game logic, the refresh snapshots and restores the complete CpuState, all
 * 128 KiB of WRAM, and the SNES multiply/divide state. The only persistent
 * writes are the explicitly range-checked BG tilemap VRAM words below.
 *
 * Sky Palace uses a separate, state-free source path: decode its traced ROM
 * metatile page into only the offscreen BG2 columns used for presentation,
 * render, then restore the game's UI/staging tilemap verbatim. */

#include "actraiser_game.h"
#include "actraiser_rtl.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "funcs.h"
#include "settings.h"
#include "snes/ppu.h"
#include "snes/snes.h"
#include "widescreen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern Ppu *g_ppu;

/* Internal bank-02 routine used by the original $02:B158 streamer. It is
 * already emitted on main; declaring it here does not require a cfg change or
 * regeneration. */
RecompReturn bank_02_B825_M1X0(CpuState *cpu);
RecompReturn bank_02_B8A0_M1X0(CpuState *cpu);

static uint8 s_wram_snapshot[kActRaiserWramSize];

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

  kColumnBuilderReturnAddress = 0xB1AB,
  kRowBuilderReturnAddress = 0xB1F3,
  kUploadRecordWords = 0x81,
  kUploadRecordBytes = 0x102,
  kUploadChunkWords = 0x20,
  kTilemapPageWords = 0x400,

  kBgState_CameraX = 0x22,
  kBgState_CameraY = 0x24,
  kBgState_Width = 0x2E,
  kBgState_Height = 0x30,
  kBgState_Decoder46 = 0x46,
  kBgState_Decoder48 = 0x48,
  kBgState_Decoder52 = 0x52,
  kBgState_Decoder54 = 0x54,
  kBgState_ColumnRecordCursor = 0x5E,
  kBgState_RowRecordCursor = 0x60,
  kBgState_Decoder6B = 0x6B,
  kBgStateStride = kActRaiserBgLayerStateStride,

  /* Caller-populated direct-page contract for $02:B825/$02:B8A0. These
   * names are deliberately local: the same DP bytes have unrelated meanings
   * in other game subsystems. */
  kDecoderDp_State48 = 0x00,
  kDecoderDp_State46 = 0x02,
  kDecoderDp_Width = 0x04,
  kDecoderDp_Zero = 0x06,
  kDecoderDp_State6B = 0x07,
  kDecoderDp_DimensionProduct = 0x08,
  kDecoderDp_DecoderBias = 0x09,
  kDecoderDp_State54 = 0x0A,
  kDecoderDp_WorldX = 0x14,
  kDecoderDp_WorldY = 0x16,
  kDecoderDp_State52 = 0xA5,
};

static uint16 s_sky_palace_bg2_backup[kActRaiserTilemapWords];
static int s_sky_palace_restore_pending;

/* The original streamers update a layer when the camera crosses a 16px map
 * boundary, not on every scanout.  Keep the host-only margin decoder on that
 * same cadence.  This is particularly important in rooms with two wide
 * layers: rebuilding every visible margin strip for BG1 and BG2 on every
 * rendered frame is enough to miss the host's 60Hz presentation deadline. */
typedef struct WsLayerRefreshKey {
  uint16 camera_x_tile;
  uint16 camera_y_page;
  uint16 width;
  uint16 height;
  uint16 decoder_state_46;
  uint16 decoder_state_48;
  uint16 decoder_state_52;
  uint16 decoder_state_54;
  uint16 column_record_cursor;
  uint16 decoder_bytes_47_6b;
  uint16 dimension_bytes_2f_31;
} WsLayerRefreshKey;

typedef struct WsRefreshKey {
  uint8 map_group;
  uint8 current_map;
  uint8 destination_map;
  uint8 destination_map_group;
  uint16 margin_left, margin_right;
  WsLayerRefreshKey layer[2];
} WsRefreshKey;

static WsRefreshKey s_refresh_key;
static int s_refresh_key_valid;
static uint16 s_row_y[2];

int ActRaiser_WidescreenBgRefreshEnabled(void) {
  return g_settings.ws_bgrefresh;
}

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
      uint16 tilemap_word = (uint16)(
          (definition_word & metatile_word_mask) | metatile_attributes);
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

/* Invoke the already-emitted recompiled builder with the same paired-call
 * stack protocol as a generated JSR site. Complete WRAM/CPU restoration at
 * the outer boundary removes the synthetic return frame and all scratch. */
static int ws_call_builder(CpuState *cpu,
                           RecompReturn (*builder)(CpuState *),
                           uint16 jsr_return) {
  uint16 s = cpu->S;
  cpu_write8(cpu, 0x00, cpu->S, (uint8)(jsr_return >> 8));
  cpu->S = (uint16)(cpu->S - 1);
  cpu_write8(cpu, 0x00, cpu->S, (uint8)jsr_return);
  cpu->S = (uint16)(cpu->S - 1);
  cpu->host_return_valid = 1;
  RecompReturn r = builder(cpu);
  cpu->S = s;
  return r == RECOMP_RETURN_NORMAL;
}

static int ws_call_column_builder(CpuState *cpu) {
  /* $02:B1A9 JSR $B825 leaves $B1AB on the emulated stack. */
  return ws_call_builder(cpu, bank_02_B825_M1X0,
                         kColumnBuilderReturnAddress);
}

static int ws_call_row_builder(CpuState *cpu) {
  /* $02:B1F1 JSR $B8A0 leaves $B1F3 on the emulated stack. */
  return ws_call_builder(cpu, bank_02_B8A0_M1X0,
                         kRowBuilderReturnAddress);
}

/* Drain a freshly built two-column record exactly like $02:ACE5, but only
 * after proving every target stays within this layer's 4 KiB tilemap. */
static int ws_drain_column_record(uint16 record_buffer,
                                  uint16 tilemap_base) {
  uint8 *record = &g_ram[record_buffer];
  uint16 vram_base = (uint16)(record[0] | (record[1] << 8));
  static const uint16 chunk_vram[4] = { 0x0000, 0x0001, 0x0800, 0x0801 };
  static const uint16 chunk_word[4] = { 1, 0x21, 0x41, 0x61 };

  if (vram_base == 0)
    return 0;

  /* Highest target = base + $801 + 31*$20. This per-layer check is stricter
   * than the experimental branch's global $6000-$7FFF check: a bad BG1
   * record cannot reach BG2, and neither layer can reach OBJ chars. */
  uint32 highest_vram_address =
      (uint32)vram_base + 0x0801u + 31u * kUploadChunkWords;
  if (vram_base < tilemap_base ||
      highest_vram_address >=
          (uint32)tilemap_base + kActRaiserTilemapWords) {
    if (ws_bg_debug_enabled()) {
      unsigned game_frame = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
      fprintf(stderr,
              "[ws-bg] gf=%u reject buf=$%04X map=$%04X base=$%04X hi=$%04X\n",
              game_frame, record_buffer, tilemap_base, vram_base,
              (unsigned)highest_vram_address);
    }
    return 0;
  }

  for (int c = 0; c < 4; c++) {
    uint16 vram_address = (uint16)(vram_base + chunk_vram[c]);
    const uint8 *source_words = record + 2 * chunk_word[c];
    for (int k = 0; k < kUploadChunkWords; k++) {
      g_ppu->vram[vram_address] = (uint16)(
          source_words[0] | (source_words[1] << 8));
      source_words += 2;
      vram_address = (uint16)(vram_address + kUploadChunkWords);
    }
  }
  return 1;
}

/* Drain a freshly built two-row record exactly like the row half of
 * $02:ACE5. VMAIN=$80 makes each 64-byte chunk advance horizontally. */
static int ws_drain_row_record(uint16 record_buffer,
                               uint16 tilemap_base) {
  uint8 *record = &g_ram[record_buffer];
  uint16 vram_base = (uint16)(record[0] | (record[1] << 8));
  static const uint16 chunk_vram[4] = { 0x0000, 0x0020, 0x0400, 0x0420 };
  static const uint16 chunk_word[4] = { 1, 0x21, 0x41, 0x61 };

  if (vram_base == 0)
    return 0;

  uint32 highest_vram_address = (uint32)vram_base + 0x0420u +
                                kUploadChunkWords - 1;
  if (vram_base < tilemap_base ||
      highest_vram_address >=
          (uint32)tilemap_base + kActRaiserTilemapWords) {
    if (ws_bg_debug_enabled()) {
      unsigned game_frame = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
      fprintf(stderr,
              "[ws-bg] gf=%u reject row buf=$%04X map=$%04X base=$%04X hi=$%04X\n",
              game_frame, record_buffer, tilemap_base, vram_base,
              (unsigned)highest_vram_address);
    }
    return 0;
  }

  for (int c = 0; c < 4; c++) {
    uint16 vram_address = (uint16)(vram_base + chunk_vram[c]);
    const uint8 *source_words = record + 2 * chunk_word[c];
    for (int k = 0; k < kUploadChunkWords; k++) {
      g_ppu->vram[vram_address++] = (uint16)(
          source_words[0] | (source_words[1] << 8));
      source_words += 2;
    }
  }
  return 1;
}

/* A neighboring 512px row band aliases half of the same 64-column ring. Do
 * not drain that whole record: doing so would fix one margin by replacing the
 * opposite visible half with world data 512px away. Copy only tile columns
 * intersecting the out-of-band margin. */
static int ws_drain_row_record_range(uint16 record_buffer,
                                     uint16 tilemap_base,
                                     int world_left, int world_right) {
  uint8 *record = &g_ram[record_buffer];
  uint16 vram_base = (uint16)(record[0] | (record[1] << 8));
  uint32 highest_vram_address = (uint32)vram_base + 0x0420u +
                                kUploadChunkWords - 1;

  if (vram_base == 0 || world_right <= world_left)
    return 0;
  if (vram_base < tilemap_base ||
      highest_vram_address >=
          (uint32)tilemap_base + kActRaiserTilemapWords) {
    if (ws_bg_debug_enabled()) {
      unsigned game_frame = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
      fprintf(stderr,
              "[ws-bg] gf=%u reject partial row buf=$%04X map=$%04X base=$%04X hi=$%04X\n",
              game_frame, record_buffer, tilemap_base, vram_base,
              (unsigned)highest_vram_address);
    }
    return 0;
  }

  int first_tile = world_left >> 3;
  int last_tile = (world_right + 7) >> 3;
  for (int tile = first_tile; tile < last_tile; tile++) {
    int col = tile & 0x3F;
    int half_col = col & 0x1F;
    uint16 top_addr = (uint16)(
        vram_base + half_col + (col >= 32 ? kTilemapPageWords : 0));
    uint16 bottom_addr = (uint16)(top_addr + 0x20);
    int top_off = 2 + half_col * 2 + (col >= 32 ? 0x80 : 0);
    int bottom_off = top_off + 0x40;
    g_ppu->vram[top_addr] = (uint16)(
        record[top_off] | (record[top_off + 1] << 8));
    g_ppu->vram[bottom_addr] = (uint16)(
        record[bottom_off] | (record[bottom_off + 1] << 8));
  }
  return 1;
}

/* Marshal one 16px-wide world strip as $02:B158 does, ask the original
 * decoder to fill its fixed record buffer, then copy that record to VRAM. */
static int ws_build_strip(CpuState *cpu, uint16 layer_x, uint16 world_x,
                          uint16 world_y) {
  const uint16 expected_record_buffer = layer_x == 0
      ? kActRaiserBg1ColumnRecord : kActRaiserBg2ColumnRecord;
  const uint16 tilemap_base = layer_x == 0
      ? kActRaiserBg1TilemapVram : kActRaiserBg2TilemapVram;

  ActRaiser_WriteWram16(kDecoderDp_WorldX, world_x);
  ActRaiser_WriteWram16(kDecoderDp_WorldY, world_y);

  uint16 state_word = ActRaiser_ReadWram16((uint16)(
      kBgState_Decoder48 + layer_x));
  ActRaiser_WriteWram16(kDecoderDp_State48, state_word);
  state_word = ActRaiser_ReadWram16((uint16)(
      kBgState_Decoder46 + layer_x));
  ActRaiser_WriteWram16(kDecoderDp_State46, state_word);
  state_word = ActRaiser_ReadWram16((uint16)(kBgState_Width + layer_x));
  ActRaiser_WriteWram16(kDecoderDp_Width, state_word);
  g_ram[kDecoderDp_Zero] = 0;
  g_ram[kDecoderDp_State6B] =
      g_ram[(uint16)(kBgState_Decoder6B + layer_x)];

  /* $B158's first $BED3 call multiplies the two bytes assembled from
   * $2F/$31. Compute it directly so only $B825's internal multiply touches
   * the emulated math unit (which the outer snapshot restores). */
  uint16 dimension_product = (uint16)(
      g_ram[(uint16)(kBgState_Width + 1 + layer_x)] *
      g_ram[(uint16)(kBgState_Height + 1 + layer_x)]);
  g_ram[kDecoderDp_DimensionProduct] = 0;
  g_ram[kDecoderDp_DecoderBias] = (uint8)(
      dimension_product + g_ram[(uint16)(kBgState_Decoder46 + 1 + layer_x)]);
  state_word = ActRaiser_ReadWram16((uint16)(
      kBgState_Decoder54 + layer_x));
  ActRaiser_WriteWram16(kDecoderDp_State54, state_word);
  state_word = ActRaiser_ReadWram16((uint16)(
      kBgState_Decoder52 + layer_x));
  ActRaiser_WriteWram16(kDecoderDp_State52, state_word);

  uint16 record_buffer = ActRaiser_ReadWram16((uint16)(
      kBgState_ColumnRecordCursor + layer_x));
  if (record_buffer != expected_record_buffer) {
    if (ws_bg_debug_enabled()) {
      unsigned game_frame = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
      fprintf(stderr, "[ws-bg] gf=%u reject layerX=%u cursor=$%04X expected=$%04X\n",
              game_frame, layer_x, record_buffer, expected_record_buffer);
    }
    return 0;
  }

  cpu->A = dimension_product;
  cpu->X = record_buffer;
  cpu->Y = record_buffer;
  if (!ws_call_column_builder(cpu))
    return 0;
  return ws_drain_column_record(record_buffer, tilemap_base);
}

/* Marshal one 16px-tall world strip as $02:B1AF does. world_x must remain
 * 256px-aligned: $B8A0's level-map fetch is page-keyed. */
static int ws_build_row(CpuState *cpu, uint16 layer_x, uint16 world_x,
                        uint16 world_y, int range_left, int range_right) {
  const uint16 expected_record_buffer = layer_x == 0
      ? kActRaiserBg1RowRecord : kActRaiserBg2RowRecord;
  const uint16 tilemap_base = layer_x == 0
      ? kActRaiserBg1TilemapVram : kActRaiserBg2TilemapVram;

  ActRaiser_WriteWram16(kDecoderDp_WorldX, world_x);
  ActRaiser_WriteWram16(kDecoderDp_WorldY, world_y);

  uint16 state_word = ActRaiser_ReadWram16((uint16)(
      kBgState_Decoder48 + layer_x));
  ActRaiser_WriteWram16(kDecoderDp_State48, state_word);
  state_word = ActRaiser_ReadWram16((uint16)(
      kBgState_Decoder46 + layer_x));
  ActRaiser_WriteWram16(kDecoderDp_State46, state_word);
  state_word = ActRaiser_ReadWram16((uint16)(kBgState_Width + layer_x));
  ActRaiser_WriteWram16(kDecoderDp_Width, state_word);
  g_ram[kDecoderDp_Zero] = 0;
  g_ram[kDecoderDp_State6B] =
      g_ram[(uint16)(kBgState_Decoder6B + layer_x)];
  state_word = ActRaiser_ReadWram16((uint16)(
      kBgState_Decoder54 + layer_x));
  ActRaiser_WriteWram16(kDecoderDp_State54, state_word);
  state_word = ActRaiser_ReadWram16((uint16)(
      kBgState_Decoder52 + layer_x));
  ActRaiser_WriteWram16(kDecoderDp_State52, state_word);

  uint16 record_buffer = ActRaiser_ReadWram16((uint16)(
      kBgState_RowRecordCursor + layer_x));
  if (record_buffer != expected_record_buffer) {
    if (ws_bg_debug_enabled()) {
      unsigned game_frame = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
      fprintf(stderr,
              "[ws-bg] gf=%u reject row layerX=%u cursor=$%04X expected=$%04X\n",
              game_frame, layer_x, record_buffer, expected_record_buffer);
    }
    return 0;
  }

  cpu->X = record_buffer;
  cpu->Y = record_buffer;
  if (!ws_call_row_builder(cpu))
    return 0;
  if (range_left >= 0)
    return ws_drain_row_record_range(record_buffer, tilemap_base,
                                     range_left, range_right);
  return ws_drain_row_record(record_buffer, tilemap_base);
}

/* Build the authentic page-aligned row band plus a neighboring band when a
 * wide margin falls outside its 512px span. This directly addresses the
 * phase hole documented in rendering-engine.md without changing $B8A0. */
static void ws_build_visible_row(CpuState *cpu, uint16 layer_x,
                                 uint16 world_y, int *built, int *requested) {
  uint16 camera_x = ActRaiser_ReadWram16((uint16)(
      kBgState_CameraX + layer_x));
  uint16 world_width = ActRaiser_ReadWram16((uint16)(
      kBgState_Width + layer_x));
  uint16 page_aligned_x = (uint16)(camera_x & 0xFF00);
  int view_left = (int)camera_x - (int)g_ppu->extraLeftCur;
  int view_right = (int)camera_x + kActRaiserAuthenticWidth +
                   (int)g_ppu->extraRightCur;

  (*requested)++;
  *built += ws_build_row(cpu, layer_x, page_aligned_x, world_y, -1, -1);

  if (view_left < (int)page_aligned_x &&
      page_aligned_x >= kActRaiserAuthenticWidth) {
    (*requested)++;
    *built += ws_build_row(
        cpu, layer_x,
        (uint16)(page_aligned_x - kActRaiserAuthenticWidth), world_y,
        view_left, page_aligned_x);
  }
  if (view_right > (int)page_aligned_x + kActRaiserTownWorldWidth &&
      (uint32)page_aligned_x + kActRaiserAuthenticWidth < world_width) {
    (*requested)++;
    *built += ws_build_row(
        cpu, layer_x,
        (uint16)(page_aligned_x + kActRaiserAuthenticWidth), world_y,
        (int)page_aligned_x + kActRaiserTownWorldWidth, view_right);
  }
}

void ActRaiser_WidescreenMarginRefresh(void) {
  if (!ActRaiser_WidescreenBgRefreshEnabled() ||
      !g_ws_active || !g_ppu || !g_snes) {
    s_refresh_key_valid = 0;
    return;
  }
  const uint8 map_group = g_ram[kActRaiserWram_MapGroup];
  const uint8 current_map = g_ram[kActRaiserWram_CurrentMap];
  if (!ActRaiser_IsActionMapGroup(map_group)) {
    s_refresh_key_valid = 0;
    return;
  }
  /* Death Heim $07:$01 presents both BG1 and BG2 from their authentic center;
   * only BG2's lower fog band is repeated by the renderer. No margin tilemap
   * cells are sampled, so running the world-strip transaction is unnecessary. */
  if (map_group == kActRaiserMapGroup_DeathHeim &&
      current_map == kActRaiserDeathHeimMap_Hub) {
    s_refresh_key_valid = 0;
    return;
  }
  if (g_ppu->extraLeftCur == 0 && g_ppu->extraRightCur == 0) {
    s_refresh_key_valid = 0;
    return;
  }

  WsRefreshKey key;
  memset(&key, 0, sizeof(key));
  key.map_group = map_group;
  key.current_map = current_map;
  key.destination_map = g_ram[kActRaiserWram_DestinationMap];
  key.destination_map_group = g_ram[kActRaiserWram_DestinationMapGroup];
  key.margin_left = g_ppu->extraLeftCur;
  key.margin_right = g_ppu->extraRightCur;
  for (int layer = 0; layer < 2; layer++) {
    uint16 layer_state_offset = (uint16)(layer * kBgStateStride);
    WsLayerRefreshKey *layer_key = &key.layer[layer];
    layer_key->camera_x_tile = (uint16)(ActRaiser_ReadWram16((uint16)(
        kBgState_CameraX + layer_state_offset)) & 0xFFF0);
    layer_key->camera_y_page = (uint16)(ActRaiser_ReadWram16((uint16)(
        kBgState_CameraY + layer_state_offset)) & 0xFF00);
    layer_key->width = ActRaiser_ReadWram16((uint16)(
        kBgState_Width + layer_state_offset));
    layer_key->height = ActRaiser_ReadWram16((uint16)(
        kBgState_Height + layer_state_offset));
    layer_key->decoder_state_46 = ActRaiser_ReadWram16((uint16)(
        kBgState_Decoder46 + layer_state_offset));
    layer_key->decoder_state_48 = ActRaiser_ReadWram16((uint16)(
        kBgState_Decoder48 + layer_state_offset));
    layer_key->decoder_state_52 = ActRaiser_ReadWram16((uint16)(
        kBgState_Decoder52 + layer_state_offset));
    layer_key->decoder_state_54 = ActRaiser_ReadWram16((uint16)(
        kBgState_Decoder54 + layer_state_offset));
    layer_key->column_record_cursor = ActRaiser_ReadWram16((uint16)(
        kBgState_ColumnRecordCursor + layer_state_offset));
    layer_key->decoder_bytes_47_6b = (uint16)(
        g_ram[(uint16)(kBgState_Decoder46 + 1 + layer_state_offset)] |
        (g_ram[(uint16)(kBgState_Decoder6B + layer_state_offset)] << 8));
    layer_key->dimension_bytes_2f_31 = (uint16)(
        g_ram[(uint16)(kBgState_Width + 1 + layer_state_offset)] |
        (g_ram[(uint16)(kBgState_Height + 1 + layer_state_offset)] << 8));
  }
  int rebuild_columns = !s_refresh_key_valid ||
      memcmp(&key, &s_refresh_key, sizeof(key)) != 0;
  uint16 row_y[2];
  int rebuild_rows = 0;
  for (int layer = 0; layer < 2; layer++) {
    uint16 layer_state_offset = (uint16)(layer * kBgStateStride);
    row_y[layer] = (uint16)(ActRaiser_ReadWram16((uint16)(
        kBgState_CameraY + layer_state_offset)) & 0xFFF0);
    if (!rebuild_columns && row_y[layer] != s_row_y[layer] &&
        ActRaiser_ReadWram16((uint16)(
            kBgState_Width + layer_state_offset)) >=
            kActRaiserTownWorldWidth &&
        g_ram[(uint16)(kBgState_Height + 1 + layer_state_offset)] >= 3)
      rebuild_rows = 1;
  }
  if (!rebuild_columns && !rebuild_rows)
    return;

  CpuState cpu_snapshot = g_cpu;
  uint8 multiply_a = g_snes->multiplyA;
  uint16 multiply_result = g_snes->multiplyResult;
  uint16 divide_a = g_snes->divideA;
  uint16 divide_result = g_snes->divideResult;
  memcpy(s_wram_snapshot, g_ram, sizeof(s_wram_snapshot));

  CpuState *cpu = &g_cpu;
  cpu->D = 0;
  cpu->m_flag = 1;
  cpu->x_flag = 0;
  cpu->P = (uint8)((cpu->P & ~0x30) | 0x20);
  cpu->host_return_valid = 0;

  int built_columns = 0, requested_columns = 0;
  int built_rows = 0, requested_rows = 0;
  if (rebuild_columns) {
    for (int layer = 0; layer < 2; layer++) {
      uint16 layer_state_offset = (uint16)(layer * kBgStateStride);
      uint16 camera_x = ActRaiser_ReadWram16((uint16)(
          kBgState_CameraX + layer_state_offset));
      uint16 world_width = ActRaiser_ReadWram16((uint16)(
          kBgState_Width + layer_state_offset));
      if (world_width < kActRaiserTownWorldWidth)
        continue;

      uint16 world_y = (uint16)(ActRaiser_ReadWram16((uint16)(
          kBgState_CameraY + layer_state_offset)) & 0xFF00);
      uint16 left_col = (uint16)(camera_x & 0xFFF0);
      uint16 right_col = (uint16)(
          (camera_x + kActRaiserAuthenticWidth) & 0xFFF0);
      int left_strips = g_ppu->extraLeftCur
          ? (g_ppu->extraLeftCur + 15) / 16 + 1 : 0;
      int right_strips = g_ppu->extraRightCur
          ? (g_ppu->extraRightCur + 15) / 16 + 1 : 0;

      for (int m = 1; m <= left_strips; m++) {
        int world_x = (int)left_col - m * 16;
        if (world_x >= 0 && world_x < (int)world_width) {
          requested_columns++;
          built_columns += ws_build_strip(
              cpu, layer_state_offset, (uint16)world_x, world_y);
        }
      }
      for (int m = 0; m <= right_strips; m++) {
        int world_x = (int)right_col + m * 16;
        if (world_x >= 0 && world_x < (int)world_width) {
          requested_columns++;
          built_columns += ws_build_strip(
              cpu, layer_state_offset, (uint16)world_x, world_y);
        }
      }
    }
  } else {
    for (int layer = 0; layer < 2; layer++) {
      uint16 layer_state_offset = (uint16)(layer * kBgStateStride);
      if (row_y[layer] == s_row_y[layer] ||
          ActRaiser_ReadWram16((uint16)(
              kBgState_Width + layer_state_offset)) <
              kActRaiserTownWorldWidth ||
          g_ram[(uint16)(kBgState_Height + 1 + layer_state_offset)] < 3)
        continue;

      if (row_y[layer] > s_row_y[layer]) {
        for (int y = (int)s_row_y[layer] + 0x10;
             y <= (int)row_y[layer]; y += 0x10)
          ws_build_visible_row(cpu, layer_state_offset,
                               (uint16)(y + kActRaiserAuthenticWidth),
                               &built_rows, &requested_rows);
      } else {
        for (int y = (int)s_row_y[layer] - 0x10;; y -= 0x10) {
          ws_build_visible_row(cpu, layer_state_offset, (uint16)y,
                               &built_rows, &requested_rows);
          if (y == (int)row_y[layer])
            break;
        }
      }
    }
  }

  /* Transaction boundary: discard every game-visible scratch write. */
  memcpy(g_ram, s_wram_snapshot, sizeof(s_wram_snapshot));
  g_cpu = cpu_snapshot;
  g_snes->multiplyA = multiply_a;
  g_snes->multiplyResult = multiply_result;
  g_snes->divideA = divide_a;
  g_snes->divideResult = divide_result;

  /* A rejected record may be transient during a room load.  Do not cache the
   * partial result; retry it on the next rendered frame. */
  if (built_columns == requested_columns && built_rows == requested_rows) {
    if (rebuild_columns) {
      s_refresh_key = key;
      s_refresh_key_valid = 1;
    }
    s_row_y[0] = row_y[0];
    s_row_y[1] = row_y[1];
  } else {
    s_refresh_key_valid = 0;
  }

  if (ws_bg_debug_enabled()) {
    static unsigned last_gf = ~0u;
    unsigned game_frame = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
    if (game_frame != last_gf) {
      last_gf = game_frame;
      fprintf(stderr,
              "[ws-bg] gf=%u strips=%d/%d rows=%d/%d margins=%u/%u (state restored)\n",
              game_frame, built_columns, requested_columns, built_rows,
              requested_rows, g_ppu->extraLeftCur, g_ppu->extraRightCur);
    }
  }
}
