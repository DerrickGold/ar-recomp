#include "snes_snapshot.h"
#include "snesrecomp/support/utf8_fs.h"
#include <stdio.h>

static bool WritePpuMetadata(
    const char *prefix, const SrPpuStateSnapshot *ppu) {
  if (!prefix || !ppu) return false;
  char path[4096];
  if (snprintf(path, sizeof(path), "%s.ppu.json", prefix) >= (int)sizeof(path)) return false;
  FILE *file = sr_fopen(path, "w");
  if (!file) return false;
  fprintf(file,
          "{\n"
          "  \"format\": 1,\n"
          "  \"inidisp\": %u, \"bgmode\": %u, \"mosaic\": %u,\n"
          "  \"bg_sc\": [%u, %u, %u, %u],\n"
          "  \"bg_tile_adr\": %u,\n"
          "  \"hscroll\": [%u, %u, %u, %u],\n"
          "  \"vscroll\": [%u, %u, %u, %u],\n"
          "  \"screen_main\": %u, \"screen_sub\": %u,\n"
          "  \"window_main\": %u, \"window_sub\": %u,\n"
          "  \"windowsel\": %u, \"wbgobjlog\": %u,\n"
          "  \"window_edges\": [%u, %u, %u, %u],\n"
          "  \"cgwsel\": %u, \"cgadsub\": %u, \"setini\": %u,\n"
          "  \"widescreen\": {\"left\": %u, \"right\": %u, "
          "\"top\": %u, \"bottom\": %u, \"clamp\": %u, "
          "\"mirror\": %u, \"repeat\": %u}\n"
          "}\n",
          (unsigned)ppu->display_control,
          (unsigned)ppu->bg_mode_control,
          (unsigned)ppu->mosaic_control,
          (unsigned)ppu->background_tilemap_control[0],
          (unsigned)ppu->background_tilemap_control[1],
          (unsigned)ppu->background_tilemap_control[2],
          (unsigned)ppu->background_tilemap_control[3],
          (unsigned)ppu->background_tile_base_control,
          (unsigned)ppu->backgrounds[0].h_scroll,
          (unsigned)ppu->backgrounds[1].h_scroll,
          (unsigned)ppu->backgrounds[2].h_scroll,
          (unsigned)ppu->backgrounds[3].h_scroll,
          (unsigned)ppu->backgrounds[0].v_scroll,
          (unsigned)ppu->backgrounds[1].v_scroll,
          (unsigned)ppu->backgrounds[2].v_scroll,
          (unsigned)ppu->backgrounds[3].v_scroll,
          (unsigned)ppu->main_screen,
          (unsigned)ppu->sub_screen,
          (unsigned)ppu->main_windowed,
          (unsigned)ppu->sub_windowed,
          (unsigned)ppu->window_select, (unsigned)ppu->window_logic,
          (unsigned)ppu->window1_left, (unsigned)ppu->window1_right,
          (unsigned)ppu->window2_left, (unsigned)ppu->window2_right,
          (unsigned)ppu->color_math_control,
          (unsigned)ppu->color_math_designation,
          (unsigned)ppu->setini_control,
          (unsigned)ppu->margin_left, (unsigned)ppu->margin_right,
          (unsigned)ppu->margin_top, (unsigned)ppu->margin_bottom,
          (unsigned)ppu->layer_clamp_mask,
          (unsigned)ppu->layer_mirror_mask,
          (unsigned)ppu->layer_repeat_mask);
  bool ok = !ferror(file);
  return fclose(file) == 0 && ok;
}

static bool WriteMemory(const char *prefix, const char *suffix, const void *data,
                         size_t count, bool words) {
  char path[4096];
  if (snprintf(path, sizeof(path), "%s.%s.bin", prefix, suffix) >= (int)sizeof(path)) return false;
  FILE *file = sr_fopen(path, "wb");
  if (!file) return false;
  bool ok = true;
  if (words) {
    const uint16_t *values = data;
    for (size_t i = 0; i < count && ok; i++) {
      const uint8_t bytes[] = {values[i], values[i] >> 8};
      ok = fwrite(bytes, 1, 2, file) == 2;
    }
  } else {
    ok = fwrite(data, 1, count, file) == count;
  }
  return fclose(file) == 0 && ok;
}

bool SnesSnapshot_Write(const char *prefix, SrRunnerHandle *runner, const SnesRunnerApi *api) {
  if (!prefix || !runner || !api) return false;
  SrPpuStateSnapshot ppu = {.struct_size = sizeof(ppu)};
  SrBorrowedSpan wram = {.struct_size = sizeof(wram)}, high = {.struct_size = sizeof(high)};
  SrBorrowedU16Span vram = {.struct_size = sizeof(vram)}, cgram = {.struct_size = sizeof(cgram)},
      oam = {.struct_size = sizeof(oam)};
  if (api->query_ppu_state(runner, &ppu) != SR_RESULT_OK ||
      api->borrow_memory(runner, SR_MEMORY_WRAM, &wram) != SR_RESULT_OK ||
      api->borrow_memory(runner, SR_MEMORY_HIGH_OAM, &high) != SR_RESULT_OK ||
      api->borrow_u16_memory(runner, SR_MEMORY_VRAM, &vram) != SR_RESULT_OK ||
      api->borrow_u16_memory(runner, SR_MEMORY_CGRAM, &cgram) != SR_RESULT_OK ||
      api->borrow_u16_memory(runner, SR_MEMORY_OAM, &oam) != SR_RESULT_OK) return false;
  bool ok = WriteMemory(prefix, "wram", wram.data, wram.byte_size, false);
  ok &= WriteMemory(prefix, "vram", vram.data, vram.element_count, true);
  ok &= WriteMemory(prefix, "cgram", cgram.data, cgram.element_count, true);
  ok &= WriteMemory(prefix, "oam", oam.data, oam.element_count, true);
  ok &= WriteMemory(prefix, "highoam", high.data, high.byte_size, false);
  ok &= WritePpuMetadata(prefix, &ppu);
  return ok;
}
