/* AR_TILE_CENSUS=1: HD tile-pack sizing survey (Stage 1 of the `tiles`
 * replacement plane — see docs/rendering-engine.md §13.1.1).
 *
 * Each emulated frame, walk everything the current PPU state can display —
 * the visible tile window of every enabled BG layer (mode-aware bpp), every
 * on-screen OAM sprite's 8x8 subtiles, and the visible Mode-7 canvas tiles —
 * and record the unique tiles by content hash. Palette *content* variants are
 * tracked per tile (CGRAM-fade behavior is the open question that decides the
 * tile-pack identity scheme). Read-only: no emulated or renderer state is
 * touched, so a census run is bit-identical to a normal run.
 *
 * At exit, writes to the run dir:
 *   tile_census.txt        summary (unique counts per class, palette variance)
 *   tile_census.jsonl      one record per unique tile, in sheet order
 *   tile_sheet_<class>.ppm contact sheets, first-seen palette, 2x scale
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes/ppu.h"
#include "run_dir.h"

extern uint8 g_ram[0x20000];
extern Ppu *g_ppu;

enum {
  kCensusHashBits = 16, /* 65536 slots, open addressing */
  kCensusMaxRecords = 32768,
  kCensusMaxPaletteVariants = 8,
  kCensusSheetColumns = 32,
};

typedef enum CensusClass {
  kCensusClass_Bg2bpp = 0,
  kCensusClass_Bg4bpp = 1,
  kCensusClass_Obj4bpp = 2,
  kCensusClass_Mode7 = 3,
  kCensusClass_Count = 4,
} CensusClass;

static const char *const kClassNames[kCensusClass_Count] = {
  "bg-2bpp", "bg-4bpp", "obj-4bpp", "mode7",
};

/* Per-(tile, palette-content) temporal signature. Palette-swap animations
 * are identified by shape: a hit-flash variant recurs in 1-3 consecutive
 * frame runs (small max_run, many bursts); a fade is a long chain of
 * one-frame variants; a stable recolor accumulates one long run. */
typedef struct PaletteVariant {
  uint64 hash;             /* FNV-1a of the sighting's CGRAM range */
  uint32 sightings;        /* distinct frames seen with this palette */
  uint32 bursts;           /* separate consecutive-frame runs */
  uint16 first_gf, last_gf;
  uint16 run_length, max_run;
} PaletteVariant;

typedef struct TileRecord {
  uint64 hash;             /* FNV-1a over raw tile data, seeded by class */
  uint8 class_;
  uint8 layer_mask;        /* bit0-3 BG1-4, bit4 OBJ, bit5 Mode-7 canvas */
  uint8 palette_group_mask;
  uint8 palette_variant_count;
  uint8 palette_overflow;  /* variants seen beyond the stored maximum */
  uint32 sightings;        /* frames this tile was visible */
  uint16 first_gf, last_gf;
  PaletteVariant palette_variants[kCensusMaxPaletteVariants];
  uint8 pixels[64];        /* decoded 8x8 palette indices */
  uint8 first_palette_rgb[16][3];
} TileRecord;

/* Global CGRAM dynamics: classify every per-frame change of each 16-color
 * group by shape — the game-agnostic detector for how this game implements
 * fades, hit-flashes, and color cycling (docs/nx-pipeline.md). */
typedef struct GroupDynamics {
  uint32 change_frames;
  uint32 fade_like;   /* new ~= prev * k across all colors */
  uint32 flash_like;  /* non-black colors ~= one uniform color */
  uint32 cycle_like;  /* same color multiset, reordered */
  uint32 other;
  uint32 bursts;      /* separate consecutive-change runs */
  uint16 last_change_gf, run_length, max_run;
} GroupDynamics;

static GroupDynamics g_group_dynamics[16];
static uint16 g_prev_cgram[256];
static uint16 g_prev_cgram_gf;
static int g_prev_cgram_valid;

static TileRecord *g_records;
static int g_record_count;
static int32_t *g_hash_slots; /* -1 empty, else record index */
static int g_enabled = -1;
static uint32 g_frames_surveyed;
static uint32 g_skipped_mode_mask;

/* ---- hashing ----------------------------------------------------------- */

static uint64 Fnv1a(uint64 hash, const void *data, size_t size) {
  const uint8 *bytes = (const uint8 *)data;
  for (size_t i = 0; i < size; i++) {
    hash ^= bytes[i];
    hash *= 0x100000001b3ull;
  }
  return hash;
}

static TileRecord *FindOrAddRecord(uint64 hash, CensusClass class_) {
  uint32 slot = (uint32)hash & ((1u << kCensusHashBits) - 1);
  for (;;) {
    int32_t index = g_hash_slots[slot];
    if (index < 0) {
      if (g_record_count >= kCensusMaxRecords) return NULL;
      TileRecord *record = &g_records[g_record_count];
      memset(record, 0, sizeof(*record));
      record->hash = hash;
      record->class_ = (uint8)class_;
      record->first_gf = (uint16)(g_ram[0x88] | (g_ram[0x89] << 8));
      g_hash_slots[slot] = g_record_count++;
      return record;
    }
    if (g_records[index].hash == hash && g_records[index].class_ == class_)
      return &g_records[index];
    slot = (slot + 1) & ((1u << kCensusHashBits) - 1);
  }
}

/* Per-sighting bookkeeping shared by all walkers. `palette_base`/`colors`
 * describe the CGRAM range this sighting uses. */
static void RecordSighting(TileRecord *record, uint8 layer_bit,
                           uint8 palette_group, int palette_base, int colors) {
  if (!record) return;
  uint16 gf = (uint16)(g_ram[0x88] | (g_ram[0x89] << 8));
  if (record->last_gf != gf || !record->sightings) {
    record->sightings++;
    record->last_gf = gf;
  }
  record->layer_mask |= layer_bit;
  record->palette_group_mask |= (uint8)(1u << (palette_group & 7));

  uint64 palette_hash =
      Fnv1a(0xcbf29ce484222325ull, &g_ppu->cgram[palette_base & 0xff],
            (size_t)colors * 2);
  PaletteVariant *variant = NULL;
  for (int i = 0; i < record->palette_variant_count; i++)
    if (record->palette_variants[i].hash == palette_hash) {
      variant = &record->palette_variants[i];
      break;
    }
  if (!variant) {
    if (record->palette_variant_count == 0) {
      for (int c = 0; c < 16; c++) {
        uint16 xbgr = c < colors ? g_ppu->cgram[(palette_base + c) & 0xff]
                                 : 0;
        record->first_palette_rgb[c][0] = (uint8)(((xbgr >> 0) & 0x1f) << 3);
        record->first_palette_rgb[c][1] = (uint8)(((xbgr >> 5) & 0x1f) << 3);
        record->first_palette_rgb[c][2] = (uint8)(((xbgr >> 10) & 0x1f) << 3);
      }
    }
    if (record->palette_variant_count >= kCensusMaxPaletteVariants) {
      if (record->palette_overflow < 255) record->palette_overflow++;
      return;
    }
    variant = &record->palette_variants[record->palette_variant_count++];
    memset(variant, 0, sizeof(*variant));
    variant->hash = palette_hash;
    variant->first_gf = gf;
    variant->last_gf = (uint16)(gf - 2); /* force new-burst accounting */
  }
  if (variant->last_gf == gf && variant->sightings) return;
  if ((uint16)(gf - variant->last_gf) == 1) {
    variant->run_length++;
  } else {
    variant->bursts++;
    variant->run_length = 1;
  }
  if (variant->run_length > variant->max_run)
    variant->max_run = variant->run_length;
  variant->last_gf = gf;
  variant->sightings++;
}

/* ---- tile decoding ------------------------------------------------------ */

static void DecodePlanar(const uint16 *words, int bpp, uint8 *out64) {
  for (int row = 0; row < 8; row++) {
    uint32 plane01 = words[row];
    uint32 plane23 = bpp == 4 ? words[row + 8] : 0;
    for (int px = 0; px < 8; px++) {
      int shift = 7 - px;
      uint32 bits01 = plane01 >> shift;
      uint32 bits23 = plane23 >> shift;
      out64[row * 8 + px] = (uint8)((bits01 & 1) | (bits01 >> 7) & 2 |
                                    (bits23 << 2) & 4 | (bits23 >> 5) & 8);
    }
  }
}

static TileRecord *RecordPlanarTile(int tile_word_adr, int bpp,
                                    CensusClass class_) {
  uint16 words[16];
  int word_count = bpp == 4 ? 16 : 8;
  for (int i = 0; i < word_count; i++)
    words[i] = g_ppu->vram[(tile_word_adr + i) & 0x7fff];
  uint64 hash = Fnv1a(0xcbf29ce484222325ull ^ (uint64)class_,
                      words, (size_t)word_count * 2);
  TileRecord *record = FindOrAddRecord(hash, class_);
  if (record && !record->sightings)
    DecodePlanar(words, bpp, record->pixels);
  return record;
}

/* ---- CGRAM dynamics classifier ------------------------------------------ */

static int Channel(uint16 xbgr, int i) { return (xbgr >> (5 * i)) & 0x1f; }

/* All non-black colors within a small distance of one another: the
 * hit-flash signature (palette jumps to near-uniform white/red). */
static int GroupIsUniform(const uint16 *cur) {
  int lo[3] = { 31, 31, 31 }, hi[3] = { 0, 0, 0 }, colored = 0;
  for (int i = 1; i < 16; i++) {
    uint16 c = cur[i] & 0x7fff;
    if (!c) continue;
    colored++;
    for (int ch = 0; ch < 3; ch++) {
      int v = Channel(c, ch);
      if (v < lo[ch]) lo[ch] = v;
      if (v > hi[ch]) hi[ch] = v;
    }
  }
  return colored >= 4 && hi[0] - lo[0] <= 3 && hi[1] - lo[1] <= 3 &&
         hi[2] - lo[2] <= 3;
}

/* Every channel of every color moved by one consistent scale factor: the
 * fade signature (CGRAM fade-in/out toward black or full). */
static int GroupIsScaled(const uint16 *prev, const uint16 *cur) {
  int knum = -1, kden = 0;
  for (int i = 1; i < 16 && kden == 0; i++)
    for (int ch = 0; ch < 3; ch++) {
      int p = Channel(prev[i], ch);
      if (p >= 12) { knum = Channel(cur[i], ch); kden = p; break; }
    }
  if (!kden) return 0;
  for (int i = 1; i < 16; i++)
    for (int ch = 0; ch < 3; ch++) {
      int p = Channel(prev[i], ch);
      int c = Channel(cur[i], ch);
      int expect = p * knum / kden;
      int diff = c - expect;
      if (diff < -2 || diff > 2) return 0;
    }
  return 1;
}

/* Same color multiset, different order: the color-cycling signature. */
static int GroupIsPermutation(const uint16 *prev, const uint16 *cur) {
  uint16 a[16], b[16];
  memcpy(a, prev, sizeof(a));
  memcpy(b, cur, sizeof(b));
  for (int i = 1; i < 16; i++) {
    uint16 va = a[i]; int j = i;
    while (j > 0 && a[j - 1] > va) { a[j] = a[j - 1]; j--; }
    a[j] = va;
    uint16 vb = b[i]; j = i;
    while (j > 0 && b[j - 1] > vb) { b[j] = b[j - 1]; j--; }
    b[j] = vb;
  }
  return !memcmp(a, b, sizeof(a));
}

/* Once per game frame, classify each changed 16-color CGRAM group. A gap in
 * surveyed frames (forced blank, census disabled scene) resnapshots without
 * classifying so scene transitions don't pollute the statistics. */
static void ClassifyPaletteWrites(void) {
  uint16 gf = (uint16)(g_ram[0x88] | (g_ram[0x89] << 8));
  if (g_prev_cgram_valid && gf == g_prev_cgram_gf) return;
  int classify = g_prev_cgram_valid && (uint16)(gf - g_prev_cgram_gf) == 1;
  if (classify) {
    for (int group = 0; group < 16; group++) {
      const uint16 *prev = &g_prev_cgram[group * 16];
      const uint16 *cur = &g_ppu->cgram[group * 16];
      if (!memcmp(prev, cur, 16 * sizeof(uint16))) continue;
      GroupDynamics *dyn = &g_group_dynamics[group];
      dyn->change_frames++;
      if ((uint16)(gf - dyn->last_change_gf) == 1) {
        dyn->run_length++;
      } else {
        dyn->bursts++;
        dyn->run_length = 1;
      }
      if (dyn->run_length > dyn->max_run) dyn->max_run = dyn->run_length;
      dyn->last_change_gf = gf;
      if (GroupIsPermutation(prev, cur)) dyn->cycle_like++;
      else if (GroupIsUniform(cur)) dyn->flash_like++;
      else if (GroupIsScaled(prev, cur)) dyn->fade_like++;
      else dyn->other++;
    }
  }
  memcpy(g_prev_cgram, g_ppu->cgram, sizeof(g_prev_cgram));
  g_prev_cgram_gf = gf;
  g_prev_cgram_valid = 1;
}

/* ---- per-frame walkers -------------------------------------------------- */

static void SurveyBgLayer(int layer, int bpp) {
  int enabled = (g_ppu->screenEnabled[0] | g_ppu->screenEnabled[1]) &
                (1 << layer);
  if (!enabled) return;
  CensusClass class_ = bpp == 4 ? kCensusClass_Bg4bpp : kCensusClass_Bg2bpp;
  int tilemap_adr = (g_ppu->bgXsc[layer] & 0xfc) << 8;
  int wider = g_ppu->bgXsc[layer] & 1;
  int higher = g_ppu->bgXsc[layer] & 2;
  int tile_adr = ((g_ppu->bgTileAdr >> (layer * 4)) & 0xf) << 12;
  int first_col = g_ppu->hScroll[layer] >> 3;
  int first_row = g_ppu->vScroll[layer] >> 3;

  for (int row = 0; row < 29; row++) {
    for (int col = 0; col < 33; col++) {
      int tx = (first_col + col);
      int ty = (first_row + row);
      int index = (ty & 0x1f) * 32 + (tx & 0x1f);
      if ((tx & 0x20) && wider) index += 0x400;
      if ((ty & 0x20) && higher) index += wider ? 0x800 : 0x400;
      uint16 entry = g_ppu->vram[(tilemap_adr + index) & 0x7fff];
      int tile = entry & 0x3ff;
      int group = (entry >> 10) & 7;
      TileRecord *record = RecordPlanarTile(
          tile_adr + tile * (bpp == 4 ? 16 : 8), bpp, class_);
      int palette_base = bpp == 4 ? group * 16 : group * 4;
      RecordSighting(record, (uint8)(1 << layer), (uint8)group,
                     palette_base, bpp == 4 ? 16 : 4);
    }
  }
}

static void SurveyObjects(void) {
  static const uint8 kSpriteSizes[8][2] = {
    {8, 16}, {8, 32}, {8, 64}, {16, 32},
    {16, 64}, {32, 64}, {16, 32}, {16, 32}
  };
  if (!((g_ppu->screenEnabled[0] | g_ppu->screenEnabled[1]) & 0x10)) return;
  for (int i = 0; i < 128; i++) {
    int index = i * 2;
    int y = g_ppu->oam[index] >> 8;
    int size = kSpriteSizes[g_ppu->obsel >> 5]
                           [(g_ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1];
    if (y >= 224 && y + size <= 256) continue; /* fully below the screen */
    int x = g_ppu->oam[index] & 0xff;
    x |= ((g_ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8;
    if (x >= 256 && x + size <= 512) continue; /* fully off right/left */
    int oam1 = g_ppu->oam[index + 1];
    int obj_adr = (oam1 & 0x100) ? PPU_objTileAdr2(g_ppu)
                                 : PPU_objTileAdr1(g_ppu);
    int group = (oam1 >> 9) & 7;
    for (int ty = 0; ty < size; ty += 8) {
      for (int tx = 0; tx < size; tx += 8) {
        int tile = ((((oam1 & 0xff) >> 4) + (ty >> 3)) << 4) |
                   (((oam1 & 0xf) + (tx >> 3)) & 0xf);
        TileRecord *record = RecordPlanarTile(obj_adr + tile * 16, 4,
                                              kCensusClass_Obj4bpp);
        RecordSighting(record, 0x10, (uint8)group, 0x80 + group * 16, 16);
      }
    }
  }
}

static void SurveyMode7(void) {
  if (!((g_ppu->screenEnabled[0] | g_ppu->screenEnabled[1]) & 1)) return;
  /* The matrix can show any part of the 128x128-tile canvas; a screen-window
   * estimate under rotation is unreliable, so record every canvas tile that
   * is referenced by the tilemap. Coarse but complete, and the canvas is at
   * most 256 tiles. */
  uint8 seen[256] = { 0 };
  for (int i = 0; i < 128 * 128; i++)
    seen[g_ppu->vram[i] & 0xff] = 1;
  for (int tile = 0; tile < 256; tile++) {
    if (!seen[tile]) continue;
    uint8 bytes[64];
    for (int p = 0; p < 64; p++)
      bytes[p] = (uint8)(g_ppu->vram[tile * 64 + p] >> 8);
    uint64 hash = Fnv1a(0xcbf29ce484222325ull ^ (uint64)kCensusClass_Mode7,
                        bytes, sizeof(bytes));
    TileRecord *record = FindOrAddRecord(hash, kCensusClass_Mode7);
    if (record && !record->sightings)
      memcpy(record->pixels, bytes, 64);
    /* 8bpp: record the first 16 CGRAM colors for the sheet; variants hash
     * the full 256-color palette. */
    RecordSighting(record, 0x20, 0, 0, 16);
  }
}

/* ---- output ------------------------------------------------------------- */

static void WriteContactSheet(CensusClass class_, const int *indices,
                              int count) {
  if (!count) return;
  char path[320];
  RunDirFile(path, sizeof(path), "tile_sheet_%s.ppm", kClassNames[class_]);
  FILE *f = fopen(path, "wb");
  if (!f) return;
  int rows = (count + kCensusSheetColumns - 1) / kCensusSheetColumns;
  int width = kCensusSheetColumns * 16, height = rows * 16;
  fprintf(f, "P6\n%d %d\n255\n", width, height);
  for (int py = 0; py < height; py++) {
    for (int px = 0; px < width; px++) {
      int cell = (py / 16) * kCensusSheetColumns + (px / 16);
      uint8 rgb[3] = { 24, 24, 24 };
      if (cell < count) {
        const TileRecord *record = &g_records[indices[cell]];
        uint8 pixel = record->pixels[(py % 16) / 2 * 8 + (px % 16) / 2];
        if (record->class_ == kCensusClass_Mode7) {
          uint16 xbgr = g_ppu->cgram[pixel];
          rgb[0] = (uint8)(((xbgr >> 0) & 0x1f) << 3);
          rgb[1] = (uint8)(((xbgr >> 5) & 0x1f) << 3);
          rgb[2] = (uint8)(((xbgr >> 10) & 0x1f) << 3);
        } else {
          memcpy(rgb, record->first_palette_rgb[pixel & 15], 3);
        }
      }
      fwrite(rgb, 1, 3, f);
    }
  }
  fclose(f);
  fprintf(stderr, "[tile-census] %s: %d tiles -> %s\n",
          kClassNames[class_], count, path);
}

static void CensusDump(void) {
  char path[320];
  RunDirFile(path, sizeof(path), "tile_census.jsonl");
  FILE *jsonl = fopen(path, "w");

  int class_counts[kCensusClass_Count] = { 0 };
  int multi_palette[kCensusClass_Count] = { 0 };
  int overflow_palette[kCensusClass_Count] = { 0 };
  static int indices[kCensusClass_Count][kCensusMaxRecords];

  int flash_suspects = 0;
  for (int i = 0; i < g_record_count; i++) {
    const TileRecord *record = &g_records[i];
    int class_ = record->class_;
    int sheet_index = class_counts[class_];
    indices[class_][class_counts[class_]++] = i;
    if (record->palette_variant_count > 1 || record->palette_overflow)
      multi_palette[class_]++;
    if (record->palette_overflow)
      overflow_palette[class_]++;
    /* A recurring short-run variant beyond the first is the hit-flash /
     * i-frame palette-swap fingerprint (docs/nx-pipeline.md). */
    int flash_suspect = 0;
    for (int v = 1; v < record->palette_variant_count; v++)
      if (record->palette_variants[v].max_run <= 3 &&
          record->palette_variants[v].bursts >= 2)
        flash_suspect = 1;
    flash_suspects += flash_suspect;
    if (jsonl) {
      fprintf(jsonl, "{\"hash\":\"%016llx\",\"class\":\"%s\",\"sheet\":%d,"
              "\"layers\":%u,\"groups\":%u,\"palettes\":%u,\"overflow\":%u,"
              "\"frames\":%u,\"first_gf\":%u,\"last_gf\":%u,"
              "\"flash_suspect\":%d,\"variants\":[",
              (unsigned long long)record->hash, kClassNames[class_],
              sheet_index, record->layer_mask, record->palette_group_mask,
              record->palette_variant_count, record->palette_overflow,
              record->sightings, record->first_gf, record->last_gf,
              flash_suspect);
      for (int v = 0; v < record->palette_variant_count; v++) {
        const PaletteVariant *variant = &record->palette_variants[v];
        fprintf(jsonl, "%s[\"%016llx\",%u,%u,%u]", v ? "," : "",
                (unsigned long long)variant->hash, variant->sightings,
                variant->bursts, variant->max_run);
      }
      fprintf(jsonl, "]}\n");
    }
  }
  /* CGRAM group dynamics: one record per group that ever changed. */
  if (jsonl)
    for (int group = 0; group < 16; group++) {
      const GroupDynamics *dyn = &g_group_dynamics[group];
      if (!dyn->change_frames) continue;
      fprintf(jsonl, "{\"cgram_group\":%d,\"changes\":%u,\"fade\":%u,"
              "\"flash\":%u,\"cycle\":%u,\"other\":%u,\"bursts\":%u,"
              "\"max_run\":%u}\n",
              group, dyn->change_frames, dyn->fade_like, dyn->flash_like,
              dyn->cycle_like, dyn->other, dyn->bursts, dyn->max_run);
    }
  if (jsonl) fclose(jsonl);

  for (int c = 0; c < kCensusClass_Count; c++)
    WriteContactSheet((CensusClass)c, indices[c], class_counts[c]);

  RunDirFile(path, sizeof(path), "tile_census.txt");
  FILE *summary = fopen(path, "w");
  if (summary) {
    fprintf(summary, "frames surveyed: %u\nunique tiles: %d%s\n",
            g_frames_surveyed, g_record_count,
            g_record_count >= kCensusMaxRecords ? " (CAP HIT)" : "");
    for (int c = 0; c < kCensusClass_Count; c++)
      fprintf(summary, "%-8s: %5d unique, %d with >1 palette variant, "
              "%d with >%d variants\n", kClassNames[c], class_counts[c],
              multi_palette[c], overflow_palette[c],
              kCensusMaxPaletteVariants);
    fprintf(summary, "flash-suspect tiles (short recurring variant): %d\n",
            flash_suspects);
    fprintf(summary, "\nCGRAM group dynamics (per-frame change shapes):\n");
    for (int group = 0; group < 16; group++) {
      const GroupDynamics *dyn = &g_group_dynamics[group];
      if (!dyn->change_frames) continue;
      fprintf(summary, "  %s group %2d: %5u changes  fade=%u flash=%u "
              "cycle=%u other=%u  bursts=%u max_run=%u\n",
              group < 8 ? "BG " : "OBJ", group, dyn->change_frames,
              dyn->fade_like, dyn->flash_like, dyn->cycle_like, dyn->other,
              dyn->bursts, dyn->max_run);
    }
    if (g_skipped_mode_mask)
      fprintf(summary, "skipped BG modes (mask): %02x\n",
              g_skipped_mode_mask);
    fclose(summary);
  }
  fprintf(stderr, "[tile-census] %u frames, %d unique tiles -> "
          "tile_census.{txt,jsonl}\n", g_frames_surveyed, g_record_count);
}

/* ---- Mode-7 canvas dump -------------------------------------------------
 * AR_M7_DUMP=1: whenever the Mode-7 canvas content changes while BG mode 7
 * is active, render the full 1024x1024 canvas through the current CGRAM
 * palette to the run dir (m7_canvas_<gf>_<hash>.ppm). This is the artist
 * source for `plane = mode7` manifest entries: paint over the dump at any
 * scale; the canvas rect in the manifest maps the art back. Read-only. */

static void HdMode7Dump_Frame(void) {
  static int enabled = -1;
  static uint64 last_hash;
  if (enabled < 0) {
    const char *env = getenv("AR_M7_DUMP");
    enabled = env && env[0] && env[0] != '0';
  }
  if (!enabled || !g_ppu || (g_ppu->bgmode & 7) != 7 ||
      (g_ppu->inidisp & 0x80))
    return;
  /* Canvas content = the high byte (pixels) of all 128*128 tile words plus
   * the low-byte tilemap. Hash both so tilemap rearrangements re-dump. */
  uint64 hash = Fnv1a(0xcbf29ce484222325ull, g_ppu->vram, 0x8000 * 2);
  if (hash == last_hash) return;
  last_hash = hash;

  char path[320];
  unsigned gf = (unsigned)(g_ram[0x88] | (g_ram[0x89] << 8));
  RunDirFile(path, sizeof(path), "m7_canvas_gf%u_%08x.ppm", gf,
             (unsigned)(hash & 0xffffffffu));
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P6\n1024 1024\n255\n");
  for (int py = 0; py < 1024; py++) {
    for (int px = 0; px < 1024; px++) {
      int tile = g_ppu->vram[(py >> 3) * 128 + (px >> 3)] & 0xff;
      uint8 pixel = (uint8)(g_ppu->vram[tile * 64 + (py & 7) * 8 + (px & 7)]
                            >> 8);
      uint16 xbgr = g_ppu->cgram[pixel];
      uint8 rgb[3] = {
        (uint8)(((xbgr >> 0) & 0x1f) << 3),
        (uint8)(((xbgr >> 5) & 0x1f) << 3),
        (uint8)(((xbgr >> 10) & 0x1f) << 3),
      };
      fwrite(rgb, 1, 3, f);
    }
  }
  fclose(f);
  fprintf(stderr, "[m7-dump] gf=%u canvas %016llx -> %s\n", gf,
          (unsigned long long)hash, path);
}

/* ---- entry point -------------------------------------------------------- */

void HdTileCensus_Frame(void) {
  HdMode7Dump_Frame();
  if (g_enabled < 0) {
    const char *env = getenv("AR_TILE_CENSUS");
    g_enabled = env && env[0] && env[0] != '0';
    if (g_enabled) {
      g_records = calloc(kCensusMaxRecords, sizeof(TileRecord));
      g_hash_slots = malloc(sizeof(int32_t) << kCensusHashBits);
      if (!g_records || !g_hash_slots) { g_enabled = 0; return; }
      memset(g_hash_slots, 0xff, sizeof(int32_t) << kCensusHashBits);
      atexit(CensusDump);
      fprintf(stderr, "[tile-census] enabled\n");
    }
  }
  if (!g_enabled || !g_ppu || (g_ppu->inidisp & 0x80)) return;

  ClassifyPaletteWrites();
  g_frames_surveyed++;
  int mode = g_ppu->bgmode & 7;
  switch (mode) {
    case 0:
      for (int layer = 0; layer < 4; layer++) SurveyBgLayer(layer, 2);
      break;
    case 1:
      SurveyBgLayer(0, 4);
      SurveyBgLayer(1, 4);
      SurveyBgLayer(2, 2);
      break;
    case 7:
      SurveyMode7();
      break;
    default:
      g_skipped_mode_mask |= (uint32)(1u << mode);
      break;
  }
  SurveyObjects();
}
