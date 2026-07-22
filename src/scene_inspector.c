#include "scene_inspector.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "snes/ppu.h"

extern uint8 g_ram[0x20000];
extern Ppu *g_ppu;

enum {
  kPanelCapacity = 4096,
  kReportCapacity = 16384,
  kMaxObjectLines = 4,
};

typedef struct TextBuilder {
  char *data;
  size_t capacity;
  size_t length;
} TextBuilder;

typedef struct InspectionState {
  bool selected;
  int x, y;
  bool have_highlight;
  int highlight_x0, highlight_y0, highlight_x1, highlight_y1;
  int best_priority;
  char panel[kPanelCapacity];
  char report[kReportCapacity];
} InspectionState;

static InspectionState s;
static SimFrameData s_sim;
static bool s_sim_valid;

void SceneInspector_SetSimFrameData(const SimFrameData *frame) {
  if (!frame) {
    memset(&s_sim, 0, sizeof(s_sim));
    s_sim_valid = false;
    return;
  }
  s_sim = *frame;
  s_sim_valid = true;
}

static void Append(TextBuilder *builder, const char *format, ...) {
  if (!builder || !builder->data || builder->length >= builder->capacity)
    return;
  va_list args;
  va_start(args, format);
  int written = vsnprintf(builder->data + builder->length,
                          builder->capacity - builder->length,
                          format, args);
  va_end(args);
  if (written <= 0) return;
  size_t room = builder->capacity - builder->length;
  builder->length += (size_t)written < room ? (size_t)written : room - 1;
}

static uint16 ReadWram16(unsigned address) {
  address &= 0x1ffff;
  return (uint16)(g_ram[address] | (g_ram[(address + 1) & 0x1ffff] << 8));
}

static uint64_t Fnv1a(uint64_t hash, const void *data, size_t size) {
  const uint8_t *bytes = (const uint8_t *)data;
  for (size_t i = 0; i < size; i++) {
    hash ^= bytes[i];
    hash *= 0x100000001b3ull;
  }
  return hash;
}

/* Class seeds deliberately match hd_tile_census.c: 0=BG2bpp, 1=BG4bpp,
 * 2=OBJ4bpp, 3=Mode7. This makes a click hash directly searchable in
 * tile_census.jsonl. */
static uint64_t PlanarHash(int word_address, int bpp, int class_) {
  uint16 words[16];
  int count = bpp == 4 ? 16 : 8;
  for (int i = 0; i < count; i++)
    words[i] = g_ppu->vram[(word_address + i) & 0x7fff];
  return Fnv1a(0xcbf29ce484222325ull ^ (uint64_t)class_,
               words, (size_t)count * sizeof(words[0]));
}

static int PlanarPixel(int word_address, int bpp, int x, int y) {
  uint16 plane01 = g_ppu->vram[(word_address + (y & 7)) & 0x7fff];
  uint16 plane23 = bpp == 4
      ? g_ppu->vram[(word_address + 8 + (y & 7)) & 0x7fff] : 0;
  int shift = 7 - (x & 7);
  uint32 bits01 = plane01 >> shift;
  uint32 bits23 = plane23 >> shift;
  return (bits01 & 1) | ((bits01 >> 7) & 2) |
         ((bits23 << 2) & 4) | ((bits23 >> 5) & 8);
}

static int LayerBpp(int mode, int layer) {
  if (mode == 0 && layer >= 0 && layer < 4) return 2;
  if (mode == 1) {
    if (layer == 0 || layer == 1) return 4;
    if (layer == 2) return 2;
  }
  return 0;
}

/* Convert a displayed widescreen x back to the x that the layer renderer
 * fetched. Returns false when that layer is clamped/transparent at the point.
 * The strings mirror the runner's policy vocabulary for useful diagnostics. */
static bool MapLayerX(int layer, int scan_y, int screen_x,
                      int *source_x, const char **policy) {
  const int bit = 1 << layer;

  /* Promoted BG3 HUD chunks use the full allocated margin, even when a finite
   * world's live margin is smaller. Reproduce the exact source biases. */
  int hud_extra = g_ppu->extraLeftRight;
  if (layer == 2 && g_ppu->wsHudSplitHeight &&
      scan_y < g_ppu->wsHudSplitHeight && hud_extra) {
    if (screen_x < -hud_extra || screen_x >= 256 + hud_extra)
      return false;
    if (g_ppu->wsHudLeftOnlyY < g_ppu->wsHudSplitHeight &&
        scan_y >= g_ppu->wsHudLeftOnlyY) {
      if (screen_x >= 256 - hud_extra) return false;
      *source_x = screen_x + hud_extra;
      *policy = "HUD-LEFT";
      return true;
    }
    if (g_ppu->wsHudLeftEnd == g_ppu->wsHudRightStart) {
      if (screen_x < g_ppu->wsHudLeftEnd - hud_extra) {
        *source_x = screen_x + hud_extra;
        *policy = "HUD-LEFT";
        return true;
      }
      if (screen_x >= g_ppu->wsHudRightStart + hud_extra) {
        *source_x = screen_x - hud_extra;
        *policy = "HUD-RIGHT";
        return true;
      }
      return false;
    }
    if (screen_x < g_ppu->wsHudLeftEnd - hud_extra) {
      *source_x = screen_x + hud_extra;
      *policy = "HUD-LEFT";
      return true;
    }
    if (screen_x >= g_ppu->wsHudLeftEnd &&
        screen_x < g_ppu->wsHudRightStart) {
      *source_x = screen_x;
      *policy = "HUD-CENTER";
      return true;
    }
    if (screen_x >= g_ppu->wsHudRightStart + hud_extra) {
      *source_x = screen_x - hud_extra;
      *policy = "HUD-RIGHT";
      return true;
    }
    return false;
  }

  if (screen_x < -g_ppu->extraLeftCur ||
      screen_x >= 256 + g_ppu->extraRightCur)
    return false;
  if (screen_x >= 0 && screen_x < 256) {
    *source_x = screen_x;
    *policy = "CENTER";
    return true;
  }

  bool repeat_band = g_ppu->wsRepeatY1[layer] >
                         g_ppu->wsRepeatY0[layer] &&
                     scan_y >= g_ppu->wsRepeatY0[layer] &&
                     scan_y < g_ppu->wsRepeatY1[layer];
  bool repeat = repeat_band || (g_ppu->wsLayerRepeat & bit);
  bool mirror = (g_ppu->wsLayerMirror & bit) != 0;
  if (repeat || mirror) {
    if (screen_x < 0)
      *source_x = repeat ? 256 + screen_x : -screen_x;
    else
      *source_x = repeat ? screen_x - 256 : 510 - screen_x;
    *policy = repeat ? (repeat_band ? "REPEAT-BAND" : "REPEAT")
                     : "MIRROR";
    return *source_x >= 0 && *source_x < 256;
  }

  bool clamp_band = g_ppu->wsClampY1[layer] >
                        g_ppu->wsClampY0[layer] &&
                    scan_y >= g_ppu->wsClampY0[layer] &&
                    scan_y < g_ppu->wsClampY1[layer];
  if ((g_ppu->wsLayerClamp & bit) || clamp_band)
    return false;
  if (layer == 2 &&
      !(g_ppu->wsBg3WidenY && scan_y >= g_ppu->wsBg3WidenY))
    return false;

  *source_x = screen_x;
  if (screen_x < 0 && g_ppu->wsMarginGapL[layer]) {
    *source_x -= g_ppu->wsMarginGapL[layer];
    *policy = "WIDE-GAP-L";
  } else if (screen_x >= 256 && g_ppu->wsMarginGapR[layer]) {
    *source_x += g_ppu->wsMarginGapR[layer];
    *policy = "WIDE-GAP-R";
  } else {
    *policy = "WIDE";
  }
  return true;
}

static void ConsiderHighlight(int priority, int pixel,
                              int x0, int y0, int x1, int y1) {
  if (!pixel || priority < s.best_priority) return;
  s.best_priority = priority;
  s.have_highlight = true;
  s.highlight_x0 = x0;
  s.highlight_y0 = y0;
  s.highlight_x1 = x1;
  s.highlight_y1 = y1;
}

static int BgPriority(int mode, int layer, int high) {
  if (mode == 1) {
    static const int low[3] = { 8, 7, 1 };
    static const int high_normal[3] = { 12, 11, 3 };
    if (layer == 2 && (g_ppu->bgmode & 8)) return high ? 15 : 1;
    return high ? high_normal[layer] : low[layer];
  }
  return high ? 2 : 1;
}

static int InspectBackground(int layer, int bpp, int mode, int scan_y,
                             TextBuilder *panel, TextBuilder *report) {
  int enabled = (g_ppu->screenEnabled[0] | g_ppu->screenEnabled[1]) &
                (1 << layer);
  if (!enabled) return 0;
  int source_x = 0;
  const char *policy = NULL;
  if (!MapLayerX(layer, scan_y, s.x, &source_x, &policy)) return 0;

  int world_x = source_x + g_ppu->hScroll[layer];
  int world_y = scan_y + g_ppu->vScroll[layer];
  int tx = world_x >> 3;
  int ty = world_y >> 3;
  int wider = g_ppu->bgXsc[layer] & 1;
  int higher = g_ppu->bgXsc[layer] & 2;
  int index = (ty & 0x1f) * 32 + (tx & 0x1f);
  if ((tx & 0x20) && wider) index += 0x400;
  if ((ty & 0x20) && higher) index += wider ? 0x800 : 0x400;
  int map_base = (g_ppu->bgXsc[layer] & 0xfc) << 8;
  int map_address = (map_base + index) & 0x7fff;
  uint16 entry = g_ppu->vram[map_address];
  int tile = entry & 0x3ff;
  int palette = (entry >> 10) & 7;
  int high = (entry >> 13) & 1;
  int hflip = (entry >> 14) & 1;
  int vflip = (entry >> 15) & 1;
  int tile_base = ((g_ppu->bgTileAdr >> (layer * 4)) & 0xf) << 12;
  int char_address =
      (tile_base + tile * (bpp == 4 ? 16 : 8)) & 0x7fff;
  int local_x = world_x & 7;
  int local_y = world_y & 7;
  int sample_x = hflip ? 7 - local_x : local_x;
  int sample_y = vflip ? 7 - local_y : local_y;
  int pixel = PlanarPixel(char_address, bpp, sample_x, sample_y);
  int priority = BgPriority(mode, layer, high);
  int palette_index = (bpp == 4 ? palette * 16 : palette * 4) + pixel;
  uint64_t hash = PlanarHash(char_address, bpp, bpp == 4 ? 1 : 0);

  Append(panel,
         "BG%d T$%03X P%d PAL%d PIX%d %s MAP$%04X\n",
         layer + 1, tile, priority, palette, pixel, policy, map_address);
  Append(report,
         "BG%d: %dbpp %s  tile=$%03X hash=%016llX  pixel=%d "
         "CGRAM=$%02X\n"
         "     entry=$%04X map-word=$%04X char-word=$%04X "
         "palette=%d priority=%d hflip=%d vflip=%d scroll=$%04X,$%04X\n",
         layer + 1, bpp, policy, tile, (unsigned long long)hash,
         pixel, palette_index & 0xff, entry, map_address, char_address,
         palette, priority, hflip, vflip,
         g_ppu->hScroll[layer], g_ppu->vScroll[layer]);

  int tile_x0 = !strcmp(policy, "MIRROR")
      ? s.x - (7 - local_x) : s.x - local_x;
  int tile_y0 = s.y - local_y;
  ConsiderHighlight(priority, pixel, tile_x0, tile_y0,
                    tile_x0 + 8, tile_y0 + 8);
  return 1;
}

static int InspectObjects(TextBuilder *panel, TextBuilder *report) {
  static const uint8 sprite_sizes[8][2] = {
    {8, 16}, {8, 32}, {8, 64}, {16, 32},
    {16, 64}, {32, 64}, {16, 32}, {16, 32}
  };
  if (!((g_ppu->screenEnabled[0] | g_ppu->screenEnabled[1]) & 0x10))
    return 0;
  int matches = 0;
  for (int slot = 0; slot < 128; slot++) {
    int index = slot * 2;
    int y = g_ppu->oam[index] >> 8;
    int display_y = y >= 224 ? y - 256 : y;
    int size_bit =
        (g_ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1;
    int size = sprite_sizes[PPU_objSize(g_ppu)][size_bit];
    int row = (uint8)(s.y - y);
    if (row >= size) continue;
    int x = g_ppu->oam[index] & 0xff;
    x |= ((g_ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8;
    if (x >= 256 + g_ppu->extraRightCur) x -= 512;
    int local_x = s.x - x;
    if (local_x < 0 || local_x >= size) continue;

    int oam1 = g_ppu->oam[index + 1];
    int hflip = (oam1 >> 14) & 1;
    int vflip = (oam1 >> 15) & 1;
    int used_x = hflip ? size - 1 - local_x : local_x;
    int used_y = vflip ? size - 1 - row : row;
    int base_tile = oam1 & 0xff;
    int tile = ((((base_tile >> 4) + (used_y >> 3)) << 4) |
                (((base_tile & 0xf) + (used_x >> 3)) & 0xf)) & 0xff;
    int obj_base = (oam1 & 0x100) ? PPU_objTileAdr2(g_ppu)
                                  : PPU_objTileAdr1(g_ppu);
    int char_address = (obj_base + tile * 16) & 0x7fff;
    int pixel = PlanarPixel(char_address, 4, used_x & 7, used_y & 7);
    int palette = (oam1 >> 9) & 7;
    int priority_group = (oam1 >> 12) & 3;
    int priority = priority_group * 4 + 2 + (palette < 4 ? 2 : 0);
    uint64_t hash = PlanarHash(char_address, 4, 2);
    matches++;

    if (matches <= kMaxObjectLines)
      Append(panel,
             "OBJ#%d %dX%d BASE$%02X SUB$%02X PAL%d PRI%d PIX%d\n",
             slot, size, size, base_tile, tile, palette,
             priority_group, pixel);
    Append(report,
           "OBJ #%d: rect=%d,%d,%d,%d size=%dx%d base/frame=$%02X "
           "subtile=$%02X hash=%016llX pixel=%d CGRAM=$%02X\n"
           "         char-word=$%04X name-select=%d palette=%d priority=%d "
           "hflip=%d vflip=%d\n",
           slot, x, display_y, x + size, display_y + size,
           size, size, base_tile, tile,
           (unsigned long long)hash, pixel,
           (0x80 + palette * 16 + pixel) & 0xff, char_address,
           (oam1 >> 8) & 1, palette, priority_group, hflip, vflip);
    if (s_sim_valid) {
      for (unsigned sim_index = 0; sim_index < s_sim.object_count;
           sim_index++) {
        const SimRenderObject *object = &s_sim.objects[sim_index];
        if (slot < object->oam_first ||
            slot >= object->oam_first + object->oam_count)
          continue;
        const char tier = object->tier == kSimRecordTier_World ? 'W' : 'F';
        if (matches <= kMaxObjectLines) {
          Append(panel,
                 " SIM%c REC$%04X CMP$%04X OAM%d+%d P%d FOOT%d,%d "
                 "%s H%d %s\n",
                 tier, object->record_address, object->composition,
                 object->oam_first, object->oam_count, object->priority,
                 object->foot_x, object->foot_y,
                 Sim3D_HeightClassName((SimHeightClass)object->height_class),
                 (int)object->virtual_height,
                 object->atlas_valid ? "ATLAS READY" : "ATLAS PENDING");
        }
        Append(report,
               "         SIM3D source=%u tier=%c record=$%04X "
               "composition=$%04X type=$%04X state=$%04X "
               "world=$%04X,$%04X foot=%d,%d OAM=%u+%u priority=%u "
               "height=%s/%d traits=$%02X shadow=%s "
               "local=[%d,%d,%d,%d] atlas=%s [%u,%u,%u,%u]\n",
               (unsigned)object->source_index, tier,
               (unsigned)object->record_address,
               (unsigned)object->composition, (unsigned)object->type,
               (unsigned)object->semantic_state,
               (unsigned)object->world_x, (unsigned)object->world_y,
               object->foot_x, object->foot_y,
               (unsigned)object->oam_first, (unsigned)object->oam_count,
               (unsigned)object->priority,
               Sim3D_HeightClassName((SimHeightClass)object->height_class),
               (int)object->virtual_height, (unsigned)object->traits,
               Sim3D_ObjectCastsShadow(object) ? "caster" : "none",
               object->local_x0, object->local_y0,
               object->local_x1, object->local_y1,
               object->atlas_valid ? "ready" : "pending",
               (unsigned)object->atlas_x, (unsigned)object->atlas_y,
               (unsigned)object->atlas_w, (unsigned)object->atlas_h);
        break;
      }
    }
    ConsiderHighlight(priority, pixel, x, display_y,
                      x + size, display_y + size);
  }
  if (matches > kMaxObjectLines)
    Append(panel, "... %d MORE OBJ MATCHES (SEE CONSOLE)\n",
           matches - kMaxObjectLines);
  return matches;
}

static int SignExtend13(int value) {
  value &= 0x1fff;
  return (value & 0x1000) ? value | ~0x1fff : value;
}

static bool InspectMode7(int scan_y, TextBuilder *panel,
                         TextBuilder *report) {
  if (!((g_ppu->screenEnabled[0] | g_ppu->screenEnabled[1]) & 1))
    return false;
  int hscroll = SignExtend13(g_ppu->m7matrix[6]);
  int vscroll = SignExtend13(g_ppu->m7matrix[7]);
  int xcenter = SignExtend13(g_ppu->m7matrix[4]);
  int ycenter = SignExtend13(g_ppu->m7matrix[5]);
  int clipped_h = hscroll - xcenter;
  int clipped_v = vscroll - ycenter;
  clipped_h = (clipped_h & 0x2000) ? (clipped_h | ~1023)
                                   : (clipped_h & 1023);
  clipped_v = (clipped_v & 0x2000) ? (clipped_v | ~1023)
                                   : (clipped_v & 1023);
  int draw_y = scan_y;
  if (PPU_mosaicEnabled(g_ppu, 0)) draw_y = g_ppu->mosaicModulo[scan_y];
  uint32_t ry = PPU_m7yFlip(g_ppu) ? 255 - draw_y : draw_y;
  uint32_t start_x =
      (g_ppu->m7matrix[0] * clipped_h & ~63) +
      (g_ppu->m7matrix[1] * ry & ~63) +
      (g_ppu->m7matrix[1] * clipped_v & ~63) + (xcenter << 8);
  uint32_t start_y =
      (g_ppu->m7matrix[2] * clipped_h & ~63) +
      (g_ppu->m7matrix[3] * ry & ~63) +
      (g_ppu->m7matrix[3] * clipped_v & ~63) + (ycenter << 8);
  uint32_t rx = PPU_m7xFlip(g_ppu) ? 255 - s.x : s.x;
  uint32_t xpos = start_x + g_ppu->m7matrix[0] * rx;
  uint32_t ypos = start_y + g_ppu->m7matrix[2] * rx;
  bool outside = PPU_m7largeField(g_ppu) &&
                 (uint32_t)(xpos | ypos) > 0x3ffff;
  int canvas_x = (xpos >> 8) & 0x3ff;
  int canvas_y = (ypos >> 8) & 0x3ff;
  int map_address = ((canvas_y >> 3) * 128 + (canvas_x >> 3)) & 0x3fff;
  int tile = outside && PPU_m7charFill(g_ppu)
      ? 0 : (g_ppu->vram[map_address] & 0xff);
  int char_address =
      (tile * 64 + (canvas_y & 7) * 8 + (canvas_x & 7)) & 0x7fff;
  int pixel = g_ppu->vram[char_address] >> 8;
  uint8 bytes[64];
  for (int i = 0; i < 64; i++)
    bytes[i] = (uint8)(g_ppu->vram[(tile * 64 + i) & 0x7fff] >> 8);
  uint64_t hash = Fnv1a(0xcbf29ce484222325ull ^ 3ull,
                        bytes, sizeof(bytes));

  Append(panel,
         "M7 CANVAS %d,%d TILE$%02X PIX$%02X HASH %08llX\n",
         canvas_x, canvas_y, tile, pixel,
         (unsigned long long)(hash & 0xffffffffull));
  Append(report,
         "MODE7: canvas=%d,%d tile=$%02X hash=%016llX pixel=$%02X "
         "CGRAM=$%02X map-word=$%04X char-word=$%04X outside=%d\n"
         "       matrix=[%04X %04X %04X %04X] center=$%04X,$%04X "
         "scroll=$%04X,$%04X\n",
         canvas_x, canvas_y, tile, (unsigned long long)hash, pixel, pixel,
         map_address, char_address, outside,
         (uint16)g_ppu->m7matrix[0], (uint16)g_ppu->m7matrix[1],
         (uint16)g_ppu->m7matrix[2], (uint16)g_ppu->m7matrix[3],
         (uint16)g_ppu->m7matrix[4], (uint16)g_ppu->m7matrix[5],
         (uint16)g_ppu->m7matrix[6], (uint16)g_ppu->m7matrix[7]);
  Append(report,
         "Manifest starting point (expand canvas_rect to the full graphic):\n"
         "[replace:inspected-mode7]\nplane = mode7\n"
         "canvas_rect = %d,%d,%d,%d\nimage = hd/replacement.png\n"
         "when = wram[0018]==0x%02X, wram[0019]==0x%02X, mode==7\n",
         canvas_x & ~7, canvas_y & ~7,
         (canvas_x & ~7) + 8, (canvas_y & ~7) + 8,
         g_ram[0x18], g_ram[0x19]);
  return true;
}

bool SceneInspector_Select(int screen_x, int screen_y) {
  return SceneInspector_SelectFiltered(
      screen_x, screen_y, kSceneInspectorBgAll, true);
}

bool SceneInspector_SelectFiltered(int screen_x, int screen_y,
                                   unsigned bg_mask,
                                   bool inspect_objects) {
  if (!g_ppu || screen_y < 0 || screen_y >= 224 ||
      screen_x < -kPpuExtraLeftRight ||
      screen_x >= 256 + kPpuExtraLeftRight)
    return false;

  memset(&s, 0, sizeof(s));
  s.selected = true;
  s.x = screen_x;
  s.y = screen_y;
  s.best_priority = -1;
  TextBuilder panel = { s.panel, sizeof(s.panel), 0 };
  TextBuilder report = { s.report, sizeof(s.report), 0 };
  int mode = g_ppu->bgmode & 7;
  int scan_y = screen_y + 1; /* PPU scanline 1 is visible output row 0. */
  uint16 gf = ReadWram16(0x88);
  uint16 camera_x = ReadWram16(0x22);
  uint16 camera_y = ReadWram16(0x24);

  Append(&panel, "CLICK %d,%d  WORLD $%04X,$%04X\n",
         screen_x, screen_y,
         (uint16)(camera_x + screen_x),
         (uint16)(camera_y + screen_y));
  Append(&panel,
         "GF $%04X STATE $%02X/$%02X CAM $%04X,$%04X MAP $%04X,$%04X\n",
         gf, g_ram[0x18], g_ram[0x19], camera_x, camera_y,
         ReadWram16(0x2e), ReadWram16(0x30));
  Append(&panel,
         "PPU MODE %d BRIGHT %d MAIN $%02X SUB $%02X MARGIN %d/%d\n",
         mode, g_ppu->inidisp & 0xf, g_ppu->screenEnabled[0],
         g_ppu->screenEnabled[1], g_ppu->extraLeftCur,
         g_ppu->extraRightCur);
  if (s_sim_valid && s_sim.town) {
    Append(&panel,
           "SIM3D %s META %s SERIAL %u REQ$%03X EFF$%03X\n",
           Sim3D_ViewName(s_sim.view),
           s_sim.metadata_valid ? "OK" : "FALLBACK",
           (unsigned)s_sim.build_serial,
           (unsigned)s_sim.requested_features,
           (unsigned)s_sim.effective_features);
    Append(&panel, "ATLAS %s %ux%u USED %ux%u\n",
           s_sim.atlas_valid ? "READY" : "UNAVAILABLE",
           (unsigned)s_sim.atlas_width, (unsigned)s_sim.atlas_height,
           (unsigned)s_sim.atlas_used_width,
           (unsigned)s_sim.atlas_used_height);
    Append(&panel, "FLAT %s STATUS %s MISMATCH %u HASH %016llX\n",
           s_sim.separated_valid ? "READY" : "FALLBACK",
           Sim3D_CaptureStatusName(
               (Sim3DCaptureStatus)s_sim.separated_status),
           (unsigned)s_sim.separated_mismatch_pixels,
           (unsigned long long)s_sim.separated_hash);
  }

  Append(&report,
         "[scene-inspector] click screen=%d,%d world=$%04X,$%04X\n"
         "game: gf=$%04X state=$%02X/$%02X camera=$%04X,$%04X "
         "map=$%04X,$%04X\n"
         "ppu: mode=%d brightness=%d forced_blank=%d main=$%02X sub=$%02X "
         "window-main=$%02X window-sub=$%02X margins=%d/%d budget=%d\n"
         "manifest gate: when = wram[0018]==0x%02X, "
         "wram[0019]==0x%02X, mode==%d\n",
         screen_x, screen_y, (uint16)(camera_x + screen_x),
         (uint16)(camera_y + screen_y), gf, g_ram[0x18], g_ram[0x19],
         camera_x, camera_y, ReadWram16(0x2e), ReadWram16(0x30),
         mode, g_ppu->inidisp & 0xf, (g_ppu->inidisp >> 7) & 1,
         g_ppu->screenEnabled[0], g_ppu->screenEnabled[1],
         g_ppu->screenWindowed[0], g_ppu->screenWindowed[1],
         g_ppu->extraLeftCur, g_ppu->extraRightCur,
         g_ppu->extraLeftRight, g_ram[0x18], g_ram[0x19], mode);
  if (s_sim_valid && s_sim.town) {
    Append(&report,
           "sim3d: view=%s metadata_valid=%d integrity=$%X serial=%u "
           "requested=$%03X effective=$%03X sources=%u fragments=%u "
           "world-oam=%u+%u atlas=%s %ux%u used=%ux%u\n",
           Sim3D_ViewName(s_sim.view), s_sim.metadata_valid,
           (unsigned)s_sim.integrity_flags, (unsigned)s_sim.build_serial,
           (unsigned)s_sim.requested_features,
           (unsigned)s_sim.effective_features,
           (unsigned)s_sim.source_count, (unsigned)s_sim.object_count,
           (unsigned)s_sim.world_oam_first,
           (unsigned)s_sim.world_oam_count,
           s_sim.atlas_valid ? "ready" : "unavailable",
           (unsigned)s_sim.atlas_width, (unsigned)s_sim.atlas_height,
           (unsigned)s_sim.atlas_used_width,
           (unsigned)s_sim.atlas_used_height);
    Append(&report,
           "sim3d-flat: valid=%d status=%s mismatch_pixels=%u hash=%016llX\n",
           s_sim.separated_valid,
           Sim3D_CaptureStatusName(
               (Sim3DCaptureStatus)s_sim.separated_status),
           (unsigned)s_sim.separated_mismatch_pixels,
           (unsigned long long)s_sim.separated_hash);
  }

  int bg_count = 0;
  if (mode == 7) {
    if (bg_mask & kSceneInspectorBg1)
      bg_count += InspectMode7(scan_y, &panel, &report);
  } else {
    for (int layer = 0; layer < 4; layer++) {
      if (!(bg_mask & (1u << layer))) continue;
      int bpp = LayerBpp(mode, layer);
      if (bpp)
        bg_count += InspectBackground(layer, bpp, mode, scan_y,
                                      &panel, &report);
    }
  }
  int object_count = inspect_objects
      ? InspectObjects(&panel, &report) : 0;

  if (!bg_count && !object_count)
    Append(&panel, "NO VISIBLE BG/OBJ CANDIDATE AT THIS POINT\n");
  Append(&panel,
         "CANDIDATES; WINDOWS/COLOR MATH MAY MASK A LAYER\n"
         "HASHES MATCH AR_TILE_CENSUS; TILE PLANE RESERVED\n"
         "LEFT CLICK INSPECT  RIGHT CLICK CLEAR  F3 DISABLE\n");

  if (mode != 7 && bg_count) {
    Append(&report,
           "Screen-plane draft note: use the BG line above to choose layer; "
           "rect must cover the complete screen-locked graphic, not merely "
           "this 8x8 tile. Scrolling scenery belongs to the future tiles "
           "plane.\n"
           "when = wram[0018]==0x%02X, wram[0019]==0x%02X, mode==%d\n",
           g_ram[0x18], g_ram[0x19], mode);
  }
  Append(&report,
         "Candidates are geometry/pixel matches. Live PPU windows, color "
         "math, and OBJ scanline limits can still suppress a candidate.\n"
         "Tile hashes use the same class seeds as AR_TILE_CENSUS. "
         "The manifest's hash-keyed tiles plane is identified but still "
         "reserved/inert; screen and mode7 are the live replacement planes.\n");
  fprintf(stderr, "\n%s\n", s.report);
  return true;
}

void SceneInspector_Clear(void) {
  memset(&s, 0, sizeof(s));
}

bool SceneInspector_HasSelection(void) {
  return s.selected;
}

const char *SceneInspector_PanelText(void) {
  return s.panel;
}

bool SceneInspector_GetPoint(int *screen_x, int *screen_y) {
  if (!s.selected) return false;
  if (screen_x) *screen_x = s.x;
  if (screen_y) *screen_y = s.y;
  return true;
}

bool SceneInspector_GetHighlight(int *x0, int *y0, int *x1, int *y1) {
  if (!s.selected || !s.have_highlight) return false;
  if (x0) *x0 = s.highlight_x0;
  if (y0) *y0 = s.highlight_y0;
  if (x1) *x1 = s.highlight_x1;
  if (y1) *y1 = s.highlight_y1;
  return true;
}
