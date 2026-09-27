#include "actraiser/enhancements/actraiser_world_resume.h"

#include "actraiser_game.h"
#include "actraiser_world_locations.h"
#include "byte_order.h"
#include "save/save_system.h"
#include <stdio.h>

void ActRaiserWorldResume_Begin(uint8_t *wram, size_t size, bool continuing,
                               bool remember_last_town, bool permitted) {
  if (!permitted || !wram || size < kSnesWramSize) return;
  const uint8_t town = SaveSystem_BeginTownVisits(continuing);
  ActRaiserWorldRegion region;
  if (!continuing || !remember_last_town || town > kActRaiserSaveRegionCount ||
      !ActRaiserWorldLocation_Region(town, &region)) return;
  /* The final-island emergence/ending owns its camera. Never replace it with
   * the last ordinary town, or modify native zoom/rotation/transition flags. */
  if (ByteOrder_ReadLe16(wram + kActRaiserWram_WorldEmergenceState) ||
      wram[kActRaiserWram_DeathHeimProgress] == 8) return;
  const uint16_t x = region.x + 128, y = region.y + 128;
  ByteOrder_WriteLe16(wram + kActRaiserWram_WorldFocusX, x);
  ByteOrder_WriteLe16(wram + kActRaiserWram_WorldFocusY, y);
  ByteOrder_WriteLe16(wram + kActRaiserWram_Bg1CameraX, x - 128);
  ByteOrder_WriteLe16(wram + kActRaiserWram_Bg1CameraY, y - 112);
  wram[kActRaiserWram_WorldLocation] = town;
  fprintf(stderr, "[world-resume] restored town=%u focus=%u,%u\n", town, x, y);
}

void ActRaiserWorldResume_Observe(const uint8_t *wram, size_t size, bool permitted) {
  if (!permitted || !wram || size < kSnesWramSize ||
      !ActRaiser_IsSimulationTown(wram[kActRaiserWram_MapGroup],
                                  wram[kActRaiserWram_CurrentMap])) return;
  SaveSystem_RecordTownVisit(wram[kActRaiserWram_CurrentMap]);
}
