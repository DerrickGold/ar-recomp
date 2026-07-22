#define _POSIX_C_SOURCE 200809L

#include "sim_phase0_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "actraiser_game.h"
#include "snes/ppu.h"

uint8 g_ram[kActRaiserWramSize];

static int failures;

#define CHECK(expr) do {                                                    \
  if (!(expr)) {                                                            \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    failures++;                                                             \
  }                                                                         \
} while (0)

static void Write16(uint8 *wram, uint32 address, uint16 value) {
  wram[address] = (uint8)value;
  wram[address + 1] = (uint8)(value >> 8);
}

int main(void) {
  char path[256];
  snprintf(path, sizeof(path), "/tmp/actraiser-sim-phase0-%ld.jsonl",
           (long)getpid());

  uint8 *wram = calloc(1, kActRaiserWramSize);
  Ppu *ppu = calloc(1, sizeof(*ppu));
  CHECK(wram != NULL && ppu != NULL);
  if (!wram || !ppu) return 1;

  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  wram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Fillmore;
  Write16(wram, kActRaiserWram_GameFrame, 100);
  Write16(wram, kActRaiserWram_SimPendingWorldType, 0x000B);
  Write16(wram, kActRaiserWram_SimMiracleKind, 5);
  Write16(wram, kActRaiserWram_SimAimedMapCellX, 7);
  Write16(wram, kActRaiserWram_SimAimedMapCellY, 11);
  Write16(wram, kActRaiserWram_SimCameraTargetX, 0x70);
  Write16(wram, kActRaiserWram_SimCameraTargetY, 0xB0);
  ppu->bgmode = 9;
  ppu->screenEnabled[0] = 0x15;
  ppu->cgwsel = 2;
  ppu->cgadsub = 0x51;
  ppu->overlayCaptures[kPpuOverlaySource_Obj] = (PpuOverlayCapture){
    .x0 = -4, .y0 = 0, .x1 = 260, .y1 = 224,
    .flags = kPpuOverlayFlag_RemoveFromGame,
    .oamFirst = 12, .oamCount = 7,
  };

  CHECK(SimPhase0Trace_Open(path));
  SimPhase0Trace_Frame(80, wram, ppu);
  SimPhase0Trace_Frame(80, wram, ppu);  /* identical redraw is suppressed */
  Write16(wram, kActRaiserWram_SimMapPickerFlag, 0x0100);
  SimPhase0Trace_Frame(81, wram, ppu);  /* word semantics, same game frame */
  Write16(wram, kActRaiserWram_GameFrame, 101);
  wram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_WorldMap;
  SimPhase0Trace_Frame(82, wram, ppu);  /* one out-of-scope exit record */
  SimPhase0Trace_Frame(83, wram, ppu);
  SimPhase0Trace_Close();

  FILE *file = fopen(path, "rb");
  CHECK(file != NULL);
  char *text = NULL;
  if (file) {
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    text = malloc((size_t)size + 1);
    CHECK(text != NULL);
    if (text) {
      CHECK(fread(text, 1, (size_t)size, file) == (size_t)size);
      text[size] = 0;
    }
    fclose(file);
  }

  if (text) {
    int lines = 0;
    for (const char *p = text; *p; p++) lines += *p == '\n';
    CHECK(lines == 3);
    CHECK(strstr(text, "\"expected_view\":\"enhanced_candidate\"") != NULL);
    CHECK(strstr(text, "\"picker_flag\":256") != NULL);
    CHECK(strstr(text, "\"pending_world_type\":11") != NULL);
    CHECK(strstr(text, "\"miracle_kind\":5") != NULL);
    CHECK(strstr(text, "\"expected_view\":\"authentic_picker\"") != NULL);
    CHECK(strstr(text, "\"expected_view\":\"out_of_scope\"") != NULL);
    CHECK(strstr(text, "\"aimed_cell_x\":7,\"aimed_cell_y\":11") != NULL);
    CHECK(strstr(text, "\"sim_target_x\":112,\"sim_target_y\":176") != NULL);
    CHECK(strstr(text, "\"oam_first\":12,\"oam_count\":7") != NULL);
  }

  free(text);
  free(ppu);
  free(wram);
  unlink(path);
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("sim_phase0_trace_test: PASS");
  return 0;
}
