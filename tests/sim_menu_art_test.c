#include "sim/menu/sim_menu_art.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <string.h>

static uint8_t capture_rom[0x100000];
static bool memory_available = true, generation_available = true;
static unsigned raster_calls, fail_raster;
static size_t rom_size = sizeof(capture_rom);
static SrRunnerHandle *const runner = (SrRunnerHandle *)(uintptr_t)1;

static SrResult BorrowRom(SrRunnerHandle *actual, SrMemoryRegion region, SrBorrowedSpan *out) {
  assert(actual == runner && region == SR_MEMORY_ROM);
  if (!memory_available) return SR_RESULT_UNSUPPORTED;
  out->data = capture_rom;
  out->byte_size = rom_size;
  return SR_RESULT_OK;
}

static SrResult Generations(SrRunnerHandle *actual, SrGenerationSnapshot *out) {
  assert(actual == runner);
  if (!generation_available) return SR_RESULT_UNSUPPORTED;
  out->lifetime_generation = 42;
  return SR_RESULT_OK;
}

static SrResult Raster(SrRunnerHandle *actual, const SrPpuObjPartsRasterRequest *in,
                       SrPpuObjRasterResult *out) {
  (void)out;
  assert(actual == runner && in->lifetime_generation == 42);
  assert(in->part_count == 1 && in->parts[0].tile_attr == 1);
  if (++raster_calls == fail_raster) return SR_RESULT_UNSUPPORTED;
  for (unsigned i = 0; i < in->pixel_byte_size / sizeof(uint32_t); ++i)
    in->pixels[i] = 0xff123456;
  return SR_RESULT_OK;
}

static void TestArtworkFailures(void) {
  SnesRunnerApi api = {.struct_size = sizeof(api), .borrow_memory = BorrowRom,
                      .query_generations = Generations, .rasterize_ppu_obj_parts = Raster};
  /* All synthetic icon families resolve through one valid composition. This
   * exercises the real preflight decoder without needing the player's ROM. */
  for (unsigned family = 0; family < 80; ++family) {
    capture_rom[0xa227 + family * 2] = 0;
    capture_rom[0xa228 + family * 2] = 0x20;
  }
  capture_rom[0x2001] = capture_rom[0x2003] = 0x21;
  capture_rom[0x2102] = 0x22;
  capture_rom[0x2200] = 1;
  capture_rom[0x2204] = 1;
  SimMenuFrame frame = {0};
  assert(SimMenuArt_Capture(&frame, &api, runner) && frame.valid);
  assert(raster_calls == kSimMenuArtCount * 2);
  const uint32_t revision = frame.art_revision;
  assert(frame.argb[0] == 0xff123456 && frame.argb[31] == 0xff123456);

  /* Every failure must invalidate a previously usable capture. In
   * particular, partial raster output must never count as modern artwork. */
  assert(!SimMenuArt_Capture(&frame, NULL, runner) && !frame.valid);
  frame.valid = true;
  assert(!SimMenuArt_Capture(&frame, &api, NULL) && !frame.valid);
  api.struct_size = SNES_RUNNER_API_PPU_OBJ_PARTS_SIZE - 1;
  frame.valid = true;
  assert(!SimMenuArt_Capture(&frame, &api, runner) && !frame.valid);
  api.struct_size = sizeof(api);
  api.rasterize_ppu_obj_parts = NULL;
  frame.valid = true;
  assert(!SimMenuArt_Capture(&frame, &api, runner) && !frame.valid);
  api.rasterize_ppu_obj_parts = Raster;
  memory_available = false;
  frame.valid = true;
  assert(!SimMenuArt_Capture(&frame, &api, runner) && !frame.valid);
  memory_available = true;
  rom_size = 0x80000;
  frame.valid = true;
  assert(!SimMenuArt_Capture(&frame, &api, runner) && !frame.valid);
  rom_size = sizeof(capture_rom);
  generation_available = false;
  frame.valid = true;
  assert(!SimMenuArt_Capture(&frame, &api, runner) && !frame.valid);
  generation_available = true;
  capture_rom[0x2100] = 0xfd;
  frame.valid = true;
  assert(!SimMenuArt_Capture(&frame, &api, runner) && !frame.valid);
  capture_rom[0x2100] = 0;
  capture_rom[0x2200] = 0;
  frame.valid = true;
  assert(!SimMenuArt_Capture(&frame, &api, runner) && !frame.valid);
  capture_rom[0x2200] = 5;
  frame.valid = true;
  assert(!SimMenuArt_Capture(&frame, &api, runner) && !frame.valid);
  capture_rom[0x2200] = 1;
  raster_calls = 0;
  fail_raster = 5;
  frame.valid = true;
  assert(!SimMenuArt_Capture(&frame, &api, runner) && !frame.valid && raster_calls == 5);
  fail_raster = 0;
  assert(SimMenuArt_Capture(&frame, &api, runner) && frame.valid);
  assert(frame.art_revision == revision);
}

int main(void) {
  TestArtworkFailures();
  static uint8_t rom[0x26000];
  unsigned width=0,height=0;
  /* Synthetic ROM data: a dictionary word, native break and trailing paper.
   * Extents describe the entire prompt before any glyph has been painted. */
  memcpy(rom+0x258f3,"Example     ",12);
  const uint8_t text[]={5,0x80,'?',0x0d,'O','K',' ',' ',1};
  memcpy(rom+0xff57,text,sizeof(text));
  assert(SimMenuArt_MeasurePrompt(rom,sizeof(rom),0xff57,&width,&height));
  assert(width==73 && height==17);
  const uint8_t named[]={5,6,0x0d,'T','e','x','t',1};
  memcpy(rom+0xf99b,named,sizeof(named));
  assert(SimMenuArt_MeasurePrompt(rom,sizeof(rom),0xf99b,&width,&height));
  assert(width==97 && height==17);
  /* Unknown sources/controls, short ROMs and unbounded lines fail closed. */
  assert(!SimMenuArt_MeasurePrompt(rom,sizeof(rom),0xf000,&width,&height));
  assert(!SimMenuArt_MeasurePrompt(rom,0x10000,0xff57,&width,&height));
  rom[0xff58]=2;
  assert(!SimMenuArt_MeasurePrompt(rom,sizeof(rom),0xff57,&width,&height));
  memset(rom+0xff57,'A',256);
  assert(!SimMenuArt_MeasurePrompt(rom,sizeof(rom),0xff57,&width,&height));
  puts("sim_menu_art: OK");
}
