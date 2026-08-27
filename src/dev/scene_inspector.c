#include "scene_inspector.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "actraiser_game.h"
#include "snesrecomp/game_runtime.h"
#include "deterministic_hash.h"
#include "snesrecomp/runner.h"

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

typedef struct ScenePpuView {
  SrRunnerHandle *runner;
  const SnesRunnerApi *api;
  SrPpuStateSnapshot state;
  SrPpuFrameSnapshot frame;
  SrBorrowedU16Span vram;
  SrBorrowedU16Span oam;
  SrBorrowedSpan high_oam;
} ScenePpuView;

static InspectionState s;
static SimFrameData s_sim;
static bool s_sim_valid;

static bool CapturePpuView(ScenePpuView *view) {
  static const uint64_t required_caps =
      SR_RUNNER_CAP_PPU_STATE | SR_RUNNER_CAP_PPU_FRAME_STATE |
      SR_RUNNER_CAP_BORROWED_BYTE_SPANS |
      SR_RUNNER_CAP_BORROWED_U16_SPANS |
      SR_RUNNER_CAP_PPU_BACKGROUND_COORDINATE;
  if (!view || !RtlGameRunner()) return false;
  memset(view, 0, sizeof(*view));
  view->api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  view->runner = RtlGameRunner();
  if (!view->api || !view->runner ||
      view->api->struct_size <
          SNES_RUNNER_API_PPU_BACKGROUND_COORDINATE_SIZE ||
      (view->api->capabilities & required_caps) != required_caps)
    return false;
  view->state.struct_size = sizeof(view->state);
  view->frame.struct_size = sizeof(view->frame);
  view->vram.struct_size = sizeof(view->vram);
  view->oam.struct_size = sizeof(view->oam);
  view->high_oam.struct_size = sizeof(view->high_oam);
  if (view->api->query_ppu_state(view->runner, &view->state) !=
          SR_RESULT_OK ||
      view->api->query_ppu_frame_state(view->runner, &view->frame) !=
          SR_RESULT_OK ||
      view->api->borrow_u16_memory(
          view->runner, SR_MEMORY_VRAM, &view->vram) != SR_RESULT_OK ||
      view->api->borrow_u16_memory(
          view->runner, SR_MEMORY_OAM, &view->oam) != SR_RESULT_OK ||
      view->api->borrow_memory(
          view->runner, SR_MEMORY_HIGH_OAM, &view->high_oam) != SR_RESULT_OK)
    return false;
  const uint64_t generation = view->state.lifetime_generation;
  return view->frame.lifetime_generation == generation &&
      view->vram.lifetime_generation == generation &&
      view->oam.lifetime_generation == generation &&
      view->high_oam.lifetime_generation == generation &&
      view->vram.element_count >= SR_PPU_VRAM_WORD_COUNT &&
      view->oam.element_count >= SR_PPU_OAM_WORD_COUNT &&
      view->high_oam.byte_size >= SR_PPU_HIGH_OAM_BYTE_COUNT &&
      view->api->borrow_u16_is_valid(view->runner, &view->vram) &&
      view->api->borrow_u16_is_valid(view->runner, &view->oam) &&
      view->api->borrow_is_valid(view->runner, &view->high_oam);
}

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

/* Class seeds deliberately match hd_tile_census.c: 0=BG2bpp, 1=BG4bpp,
 * 2=OBJ4bpp, 3=Mode7. This makes a click hash directly searchable in
 * tile_census.jsonl. */
static uint64_t PlanarHash(const ScenePpuView *view, int word_address,
                           int bpp, int class_) {
  uint16 words[16];
  int count = bpp == 4 ? 16 : 8;
  for (int i = 0; i < count; i++)
    words[i] = view->vram.data[(word_address + i) & 0x7fff];
  return DeterministicHash_Fnv1a64(
      DETERMINISTIC_HASH_FNV1A64_OFFSET ^ (uint64_t)class_, words,
      (size_t)count * sizeof(words[0]));
}

static int PlanarPixel(const ScenePpuView *view, int word_address,
                       int bpp, int x, int y) {
  uint16 plane01 =
      view->vram.data[(word_address + (y & 7)) & 0x7fff];
  uint16 plane23 = bpp == 4
      ? view->vram.data[(word_address + 8 + (y & 7)) & 0x7fff] : 0;
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
static bool MapLayerX(const ScenePpuView *view, int layer, int screen_y,
                      int screen_x, int *source_x, int *sample_y,
                      const char **policy, bool *mirrored) {
  const int scan_y = screen_y + 1;
  *sample_y = scan_y;
  *mirrored = false;
  /* Promoted BG3 HUD chunks use the full allocated margin, even when a finite
   * world's live margin is smaller. Reproduce the exact source biases. */
  int hud_extra = view->frame.margin_budget;
  if (layer == 2 && view->frame.hud_split_height &&
      scan_y < view->frame.hud_split_height && hud_extra) {
    if (screen_x < -hud_extra ||
      screen_x >= kActRaiserAuthenticWidth + hud_extra)
      return false;
    if (view->frame.hud_left_only_y < view->frame.hud_split_height &&
        scan_y >= view->frame.hud_left_only_y) {
      if (screen_x >= kActRaiserAuthenticWidth - hud_extra) return false;
      *source_x = screen_x + hud_extra;
      *policy = "HUD-LEFT";
      return true;
    }
    if (view->frame.hud_left_end == view->frame.hud_right_start) {
      if (screen_x < view->frame.hud_left_end - hud_extra) {
        *source_x = screen_x + hud_extra;
        *policy = "HUD-LEFT";
        return true;
      }
      if (screen_x >= view->frame.hud_right_start + hud_extra) {
        *source_x = screen_x - hud_extra;
        *policy = "HUD-RIGHT";
        return true;
      }
      return false;
    }
    if (screen_x < view->frame.hud_left_end - hud_extra) {
      *source_x = screen_x + hud_extra;
      *policy = "HUD-LEFT";
      return true;
    }
    if (screen_x >= view->frame.hud_left_end &&
        screen_x < view->frame.hud_right_start) {
      *source_x = screen_x;
      *policy = "HUD-CENTER";
      return true;
    }
    if (screen_x >= view->frame.hud_right_start + hud_extra) {
      *source_x = screen_x - hud_extra;
      *policy = "HUD-RIGHT";
      return true;
    }
    return false;
  }

  SrPpuBackgroundCoordinateRequest request = {
    .struct_size = sizeof(request),
    .lifetime_generation = view->state.lifetime_generation,
    .layer = (uint32_t)layer,
    .screen_x = screen_x,
    .screen_y = screen_y,
  };
  SrPpuBackgroundCoordinateResult result = { sizeof(result), 0u };
  if (view->api->resolve_ppu_background_coordinate(
          view->runner, &request, &result) != SR_RESULT_OK ||
      !(result.flags & SR_PPU_BACKGROUND_COORDINATE_MAPPED))
    return false;
  *source_x = result.source_x;
  *sample_y = result.sample_y;
  *mirrored = result.fill == SR_PPU_BACKGROUND_FILL_MIRROR;
  if (screen_x >= 0 && screen_x < kActRaiserAuthenticWidth)
    *policy = "CENTER";
  else if (result.fill == SR_PPU_BACKGROUND_FILL_REPEAT)
    *policy = (result.flags & SR_PPU_BACKGROUND_COORDINATE_BAND_OVERRIDE)
        ? "REPEAT-BAND" : "REPEAT";
  else if (result.fill == SR_PPU_BACKGROUND_FILL_MIRROR)
    *policy = (result.flags & SR_PPU_BACKGROUND_COORDINATE_BAND_OVERRIDE)
        ? "MIRROR-BAND" : "MIRROR";
  else
    *policy = "WIDE";
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

static int BgPriority(const ScenePpuView *view, int mode, int layer,
                      int high) {
  if (mode == 1) {
    static const int low[3] = { 8, 7, 1 };
    static const int high_normal[3] = { 12, 11, 3 };
    if (layer == 2 && (view->state.bg_mode_control & 8))
      return high ? 15 : 1;
    return high ? high_normal[layer] : low[layer];
  }
  return high ? 2 : 1;
}

static int InspectBackground(const ScenePpuView *view, int layer, int bpp,
                             int mode, int screen_y, TextBuilder *panel,
                             TextBuilder *report) {
  const SrPpuBackgroundState *background = &view->state.backgrounds[layer];
  int enabled = (view->state.main_screen | view->state.sub_screen) &
                (1 << layer);
  if (!enabled) return 0;
  int source_x = 0;
  int fetch_y = screen_y + 1;
  const char *policy = NULL;
  bool mirrored = false;
  if (!MapLayerX(view, layer, screen_y, s.x, &source_x, &fetch_y,
                 &policy, &mirrored))
    return 0;

  int world_x = source_x + background->h_scroll;
  int world_y = fetch_y + background->v_scroll;
  int tx = world_x >> 3;
  int ty = world_y >> 3;
  int wider = background->tilemap_width_tiles == 64u;
  int higher = background->tilemap_height_tiles == 64u;
  int index = (ty & 0x1f) * 32 + (tx & 0x1f);
  if ((tx & 0x20) && wider) index += 0x400;
  if ((ty & 0x20) && higher) index += wider ? 0x800 : 0x400;
  int map_base = background->tilemap_base_word;
  int map_address = (map_base + index) & 0x7fff;
  uint16 entry = view->vram.data[map_address];
  int tile = entry & 0x3ff;
  int palette = (entry >> 10) & 7;
  int high = (entry >> 13) & 1;
  int hflip = (entry >> 14) & 1;
  int vflip = (entry >> 15) & 1;
  int tile_base = background->tile_base_word;
  int char_address =
      (tile_base + tile * (bpp == 4 ? 16 : 8)) & 0x7fff;
  int local_x = world_x & 7;
  int local_y = world_y & 7;
  int sample_x = hflip ? 7 - local_x : local_x;
  int sample_y = vflip ? 7 - local_y : local_y;
  int pixel = PlanarPixel(view, char_address, bpp, sample_x, sample_y);
  int priority = BgPriority(view, mode, layer, high);
  int palette_index = (bpp == 4 ? palette * 16 : palette * 4) + pixel;
  uint64_t hash =
      PlanarHash(view, char_address, bpp, bpp == 4 ? 1 : 0);

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
         background->h_scroll, background->v_scroll);

  int tile_x0 = mirrored ? s.x - (7 - local_x) : s.x - local_x;
  int tile_y0 = s.y - local_y;
  ConsiderHighlight(priority, pixel, tile_x0, tile_y0,
                    tile_x0 + 8, tile_y0 + 8);
  return 1;
}

static int InspectObjects(const ScenePpuView *view, TextBuilder *panel,
                          TextBuilder *report) {
  static const uint8_t sprite_sizes[8][2] = {
    {8, 16}, {8, 32}, {8, 64}, {16, 32},
    {16, 64}, {32, 64}, {16, 32}, {16, 32}
  };
  if (!((view->state.main_screen | view->state.sub_screen) & 0x10))
    return 0;
  int matches = 0;
  for (int slot = 0; slot < 128; slot++) {
    int index = slot * 2;
    int y = view->oam.data[index] >> 8;
    int display_y =
        y >= (int)SR_PPU_OBJ_Y_NEGATIVE_FROM ? y - SR_PPU_OBJ_Y_WRAP : y;
    int size_bit =
        (view->high_oam.data[index >> 3] >> ((index & 7) + 1)) & 1;
    int size = sprite_sizes[view->state.object_size_select][size_bit];
    int row = (uint8_t)(s.y - y);
    if (row >= size) continue;
    int x = view->oam.data[index] & 0xff;
    x |= ((view->high_oam.data[index >> 3] >> (index & 7)) & 1) << 8;
    if (x >= kActRaiserAuthenticWidth + view->state.margin_right)
      x -= SR_PPU_OBJ_X_WRAP;
    int local_x = s.x - x;
    if (local_x < 0 || local_x >= size) continue;

    int oam1 = view->oam.data[index + 1];
    int hflip = (oam1 >> 14) & 1;
    int vflip = (oam1 >> 15) & 1;
    int used_x = hflip ? size - 1 - local_x : local_x;
    int used_y = vflip ? size - 1 - row : row;
    int base_tile = oam1 & 0xff;
    int tile = ((((base_tile >> 4) + (used_y >> 3)) << 4) |
                (((base_tile & 0xf) + (used_x >> 3)) & 0xf)) & 0xff;
    int obj_base = (int)((oam1 & 0x100)
        ? view->state.object_tile_base_2_word
        : view->state.object_tile_base_1_word);
    int char_address = (obj_base + tile * 16) & 0x7fff;
    int pixel =
        PlanarPixel(view, char_address, 4, used_x & 7, used_y & 7);
    int palette = (oam1 >> 9) & 7;
    int priority_group = (oam1 >> 12) & 3;
    int priority = priority_group * 4 + 2 + (palette < 4 ? 2 : 0);
    uint64_t hash = PlanarHash(view, char_address, 4, 2);
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

static bool InspectMode7(const ScenePpuView *view, int screen_y,
                         TextBuilder *panel, TextBuilder *report) {
  const int16_t *matrix = view->state.mode7_matrix;
  if (!((view->state.main_screen | view->state.sub_screen) & 1))
    return false;
  int source_x = 0;
  int draw_y = screen_y + 1;
  const char *policy = NULL;
  bool mirrored = false;
  if (!MapLayerX(view, 0, screen_y, s.x, &source_x, &draw_y,
                 &policy, &mirrored))
    return false;
  int hscroll = SignExtend13(matrix[6]);
  int vscroll = SignExtend13(matrix[7]);
  int xcenter = SignExtend13(matrix[4]);
  int ycenter = SignExtend13(matrix[5]);
  int clipped_h = hscroll - xcenter;
  int clipped_v = vscroll - ycenter;
  clipped_h = (clipped_h & 0x2000) ? (clipped_h | ~1023)
                                   : (clipped_h & 1023);
  clipped_v = (clipped_v & 0x2000) ? (clipped_v | ~1023)
                                   : (clipped_v & 1023);
  uint32_t ry = (view->state.mode7_select & SR_PPU_MODE7_Y_FLIP)
      ? 255 - draw_y : draw_y;
  uint32_t start_x =
      (matrix[0] * clipped_h & ~63) +
      (matrix[1] * ry & ~63) +
      (matrix[1] * clipped_v & ~63) + (xcenter << 8);
  uint32_t start_y =
      (matrix[2] * clipped_h & ~63) +
      (matrix[3] * ry & ~63) +
      (matrix[3] * clipped_v & ~63) + (ycenter << 8);
  uint32_t rx = (view->state.mode7_select & SR_PPU_MODE7_X_FLIP)
      ? 255 - source_x : source_x;
  uint32_t xpos = start_x + matrix[0] * rx;
  uint32_t ypos = start_y + matrix[2] * rx;
  bool outside = (view->state.mode7_select & SR_PPU_MODE7_LARGE_FIELD) &&
                 (uint32_t)(xpos | ypos) > 0x3ffff;
  int canvas_x = (xpos >> 8) & 0x3ff;
  int canvas_y = (ypos >> 8) & 0x3ff;
  int map_address = ((canvas_y >> 3) * 128 + (canvas_x >> 3)) & 0x3fff;
  int tile = outside &&
      (view->state.mode7_select & SR_PPU_MODE7_CHARACTER_FILL)
      ? 0 : (view->vram.data[map_address] & 0xff);
  int char_address =
      (tile * 64 + (canvas_y & 7) * 8 + (canvas_x & 7)) & 0x7fff;
  int pixel = view->vram.data[char_address] >> 8;
  uint8_t bytes[64];
  for (int i = 0; i < 64; i++)
    bytes[i] =
        (uint8_t)(view->vram.data[(tile * 64 + i) & 0x7fff] >> 8);
  uint64_t hash = DeterministicHash_Fnv1a64(
      DETERMINISTIC_HASH_FNV1A64_OFFSET ^ UINT64_C(3), bytes,
      sizeof(bytes));

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
         (uint16)matrix[0], (uint16)matrix[1],
         (uint16)matrix[2], (uint16)matrix[3],
         (uint16)matrix[4], (uint16)matrix[5],
         (uint16)matrix[6], (uint16)matrix[7]);
  Append(report,
         "Manifest starting point (expand canvas_rect to the full graphic):\n"
         "[replace:inspected-mode7]\nplane = mode7\n"
         "canvas_rect = %d,%d,%d,%d\nimage = hd/replacement.png\n"
         "when = wram[0018]==0x%02X, wram[0019]==0x%02X, mode==7\n",
         canvas_x & ~7, canvas_y & ~7,
         (canvas_x & ~7) + 8, (canvas_y & ~7) + 8,
         g_ram[kActRaiserWram_MapGroup],
         g_ram[kActRaiserWram_CurrentMap]);
  return true;
}

bool SceneInspector_Select(int screen_x, int screen_y) {
  return SceneInspector_SelectFiltered(
      screen_x, screen_y, kSceneInspectorBgAll, true);
}

bool SceneInspector_SelectFiltered(int screen_x, int screen_y,
                                   unsigned bg_mask,
                                   bool inspect_objects) {
  ScenePpuView view;
  if (screen_y < 0 ||
      screen_y >= kActRaiserAuthenticHeight ||
      screen_x < -(int)SR_PPU_HORIZONTAL_MARGIN_MAX ||
      screen_x >= kActRaiserAuthenticWidth +
          (int)SR_PPU_HORIZONTAL_MARGIN_MAX ||
      !CapturePpuView(&view))
    return false;

  memset(&s, 0, sizeof(s));
  s.selected = true;
  s.x = screen_x;
  s.y = screen_y;
  s.best_priority = -1;
  TextBuilder panel = { s.panel, sizeof(s.panel), 0 };
  TextBuilder report = { s.report, sizeof(s.report), 0 };
  int mode = view.state.bg_mode;
  uint16 gf = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
  uint16 camera_x = ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraX);
  uint16 camera_y = ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraY);

  Append(&panel, "CLICK %d,%d  WORLD $%04X,$%04X\n",
         screen_x, screen_y,
         (uint16)(camera_x + screen_x),
         (uint16)(camera_y + screen_y));
  Append(&panel,
         "GF $%04X STATE $%02X/$%02X CAM $%04X,$%04X MAP $%04X,$%04X\n",
         gf, g_ram[kActRaiserWram_MapGroup],
         g_ram[kActRaiserWram_CurrentMap], camera_x, camera_y,
         ActRaiser_ReadWram16(kActRaiserWram_Bg1Width),
         ActRaiser_ReadWram16(kActRaiserWram_Bg1Height));
  Append(&panel,
         "PPU MODE %d BRIGHT %d MAIN $%02X SUB $%02X MARGIN %d/%d\n",
         mode, view.state.brightness, view.state.main_screen,
         view.state.sub_screen, view.state.margin_left,
         view.state.margin_right);
  if (s_sim_valid && s_sim.view != kSimView_None) {
    Append(&panel,
           "SIM3D %s META %s SERIAL %u REQ$%03X EFF$%03X\n",
           Sim3D_ViewName(s_sim.view),
           s_sim.metadata_valid ? "OK" : "FALLBACK",
           (unsigned)s_sim.build_serial,
           (unsigned)s_sim.requested_features,
           (unsigned)s_sim.effective_features);
    if (s_sim.world_navigation_state_valid) {
      const SimWorldNavigationFrame *navigation = &s_sim.world_navigation;
      Append(&panel,
             "WORLD FOCUS $%04X,$%04X M [%d,%d,%d,%d]\n"
             "ROT $%04X ZOOM $%04X->$%04X LOC %u BRIGHT %u "
             "MAPSER %u SCENE %s\n",
             navigation->focus_x, navigation->focus_y,
             navigation->matrix[0], navigation->matrix[1],
             navigation->matrix[2], navigation->matrix[3],
             navigation->rotation, navigation->zoom_current,
             navigation->zoom_target, navigation->active_location,
             (unsigned)s_sim.world_navigation_brightness,
             (unsigned)s_sim.underlay_serial,
             s_sim.world_navigation_scene.valid ? "READY" : "FALLBACK");
      const SimWorldNavigationComposition *composition =
          &s_sim.world_navigation_scene.composition;
      Append(&panel,
             "COMP %s%s UI %u+%u PALACE %u+%u FX L/C/B/H %u/%u/%u/%u\n",
             composition->valid ? "READY" : "FALLBACK",
             composition->empty_animation ? "/EMPTY" : "",
             (unsigned)composition->ui.oam_first,
             (unsigned)composition->ui.oam_count,
             (unsigned)composition->palace.oam_first,
             (unsigned)composition->palace.oam_count,
             (unsigned)s_sim.world_navigation_lighting,
             (unsigned)s_sim.world_navigation_clouds,
             (unsigned)s_sim.world_navigation_backdrop,
             (unsigned)s_sim.world_navigation_haze);
    } else {
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
         (uint16)(camera_y + screen_y), gf,
         g_ram[kActRaiserWram_MapGroup],
         g_ram[kActRaiserWram_CurrentMap], camera_x, camera_y,
         ActRaiser_ReadWram16(kActRaiserWram_Bg1Width),
         ActRaiser_ReadWram16(kActRaiserWram_Bg1Height),
         mode, view.state.brightness,
         !!(view.state.flags & SR_PPU_STATE_FORCED_BLANK),
         view.state.main_screen, view.state.sub_screen,
         view.state.main_windowed, view.state.sub_windowed,
         view.state.margin_left, view.state.margin_right,
         view.frame.margin_budget, g_ram[kActRaiserWram_MapGroup],
         g_ram[kActRaiserWram_CurrentMap], mode);
  if (s_sim_valid && s_sim.view != kSimView_None) {
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
    if (s_sim.world_navigation_state_valid) {
      const SimWorldNavigationFrame *navigation = &s_sim.world_navigation;
      Append(&report,
             "world-navigation: focus=$%04X,$%04X scroll=$%04X,$%04X "
             "matrix=[%d,%d,%d,%d] next=[%d,%d,%d,%d] "
             "rotation=$%04X zoom=$%04X->$%04X location=%u brightness=%u "
             "map_serial=%u "
             "scene=%s texture=%ux%u "
             "source_to_screen=[%.6g,%.6g,%.6g,%.6g,%.6g,%.6g]\n",
             navigation->focus_x, navigation->focus_y,
             s_sim.camera_x, s_sim.camera_y,
             navigation->matrix[0], navigation->matrix[1],
             navigation->matrix[2], navigation->matrix[3],
             navigation->next_matrix[0], navigation->next_matrix[1],
             navigation->next_matrix[2], navigation->next_matrix[3],
             navigation->rotation, navigation->zoom_current,
             navigation->zoom_target, navigation->active_location,
             (unsigned)s_sim.world_navigation_brightness,
             (unsigned)s_sim.underlay_serial,
             s_sim.world_navigation_scene.valid ? "ready" : "fallback",
             (unsigned)s_sim.world_navigation_scene.texture_width,
             (unsigned)s_sim.world_navigation_scene.texture_height,
             (double)s_sim.world_navigation_scene.source_to_screen[0],
             (double)s_sim.world_navigation_scene.source_to_screen[1],
             (double)s_sim.world_navigation_scene.source_to_screen[2],
             (double)s_sim.world_navigation_scene.source_to_screen[3],
             (double)s_sim.world_navigation_scene.source_to_screen[4],
             (double)s_sim.world_navigation_scene.source_to_screen[5]);
      Append(&report,
             "world-navigation-active-region: valid=%d bounds=%u,%u %ux%u\n",
             s_sim.world_navigation_scene.active_region_valid,
             (unsigned)s_sim.world_navigation_scene.active_region_x,
             (unsigned)s_sim.world_navigation_scene.active_region_y,
             (unsigned)s_sim.world_navigation_scene.active_region_width,
             (unsigned)s_sim.world_navigation_scene.active_region_height);
      const SimWorldNavigationComposition *composition =
          &s_sim.world_navigation_scene.composition;
      Append(&report,
             "world-navigation-composition: valid=%d empty=%d "
             "ui=%u+%u bounds=%d,%d %ux%u "
             "palace=%u+%u bounds=%d,%d %ux%u effects=%u/%u/%u/%u\n",
             composition->valid, composition->empty_animation,
             (unsigned)composition->ui.oam_first,
             (unsigned)composition->ui.oam_count,
             (int)composition->ui.screen_x, (int)composition->ui.screen_y,
             (unsigned)composition->ui.width,
             (unsigned)composition->ui.height,
             (unsigned)composition->palace.oam_first,
             (unsigned)composition->palace.oam_count,
             (int)composition->palace.screen_x,
             (int)composition->palace.screen_y,
             (unsigned)composition->palace.width,
             (unsigned)composition->palace.height,
             (unsigned)s_sim.world_navigation_lighting,
             (unsigned)s_sim.world_navigation_clouds,
             (unsigned)s_sim.world_navigation_backdrop,
             (unsigned)s_sim.world_navigation_haze);
    }
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
      bg_count += InspectMode7(&view, screen_y, &panel, &report);
  } else {
    for (int layer = 0; layer < 4; layer++) {
      if (!(bg_mask & (1u << layer))) continue;
      int bpp = LayerBpp(mode, layer);
      if (bpp)
        bg_count += InspectBackground(&view, layer, bpp, mode, screen_y,
                                      &panel, &report);
    }
  }
  int object_count = inspect_objects
      ? InspectObjects(&view, &panel, &report) : 0;

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
           g_ram[kActRaiserWram_MapGroup],
           g_ram[kActRaiserWram_CurrentMap], mode);
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
