#include "actraiser_stage_terrain.h"
#include "actraiser_regional_media.h"
#include "actraiser_stage_placements.h"
#include "actraiser_regional_runtime.h"
#include "actraiser_action_bg.h"
#include "actraiser_game.h"
#include "actraiser_hle_fatal.h"
#include "regional/regional_terrain.h"

static bool Context(CpuState *cpu, bool byte_a, uint8_t bank) {
  return cpu && !cpu->emulation && cpu->m_flag == byte_a && !cpu->x_flag &&
      !cpu->D && !cpu->PB && cpu->DB == bank && !cpu->_flag_D && !(cpu->P & CPU_P_D);
}
static uint8_t Read(void *context, ArRegionalTerrainPlane plane, size_t offset) {
  return cpu_read8(context,0x7e,(uint16_t)(plane == kArRegionalTerrain_Map ?
      0x8000+offset : 0x2100+(offset^1u)));
}
static void Write(void *context, ArRegionalTerrainPlane plane, size_t offset, uint8_t byte) {
  cpu_write8(context,0x7e,(uint16_t)(plane == kArRegionalTerrain_Map ?
      0x8000+offset : 0x2100+(offset^1u)),byte);
}
bool ActRaiser_StageTerrainEntry(CpuState *cpu) {
  return Context(cpu,true,0) && ActRaiserRegional_TerrainSnapshot() < 3 &&
      ArRegionalTerrain_HasScene(cpu_read16(cpu,0,kActRaiserWram_MapGroup));
}
RecompReturn ActRaiser_StageTerrain(CpuState *cpu) {
  if (!ActRaiser_StageTerrainEntry(cpu)) ActRaiserHleFatal("Invalid regional terrain entry");
  const uint8_t profile = ActRaiserRegional_TerrainSnapshot();
  const uint16_t scene = cpu_read16(cpu,0,kActRaiserWram_MapGroup);
  const ArRegionalTerrainStorage storage = {cpu,Read,Write,
      cpu_read16(cpu,0,0x2e)>>8,cpu_read16(cpu,0,0x30)>>8};
  if (!ArRegionalTerrain_Project(profile,scene,&storage))
    ActRaiserHleFatal("Unknown action terrain at native asset-load boundary");
  ArRegionalPlacementPolicy placements;
  ArRegionalDifficulty difficulty;
  if (!ActRaiserRegional_PlacementSnapshot(&placements,&difficulty) ||
      !ActRaiserStagePlacements_Prepare(scene,&placements,
          cpu_read8(cpu,0,0x0349)!=0,difficulty,profile))
    ActRaiserHleFatal("Cannot capture regional room placements");
  /* Only the known Fillmore retry anchor is convertible. Arbitrary developer
   * coordinates and all other checkpoints remain untouched. No live teleport. */
  if (scene == 0x0101 && cpu_read16(cpu,0,0x032c) && cpu_read16(cpu,0,0x032e) == 156*16) {
    const uint16_t y = cpu_read16(cpu,0,0x0330);
    if (y == 23*16 || y == 25*16)
      cpu_write16(cpu,0,0x0330,(uint16_t)(16*ArRegionalTerrain_FillmoreCheckpointY(profile)));
  }
  ActRaiserActionBg_BeginRoomVariants(profile,ActRaiserRegional_MosaicSnapshot(),
      ActRaiserRegionalMedia_DeathHeimCharacters(
          (ActRaiserRegional_ArtworkSnapshot()&(1u<<kArRegionalArtwork_DeathHeim))!=0,scene));
  /* LDX #0 only; native continuation rebuilds attributes, collision, camera
   * and staged tilemaps after the complete asset VM, before any actors run. */
  cpu->X = 0;
  cpu->P = (uint8_t)((cpu->P & ~(CPU_P_N|CPU_P_Z)) | CPU_P_Z);
  cpu_p_to_mirrors(cpu);
  cpu_hle_tailcall_request(0x00832c,0x008329);
  return RECOMP_RETURN_TAILCALL;
}
static bool Fillmore(CpuState *cpu) {
  return Context(cpu,false,0x0a) && ActRaiserRegional_TerrainSnapshot() == 1 &&
      cpu_read16(cpu,0,kActRaiserWram_MapGroup) == 0x0101 &&
      cpu->X >= 0x06a0 && cpu->X < 0x1aa0 && (cpu->X-0x06a0)%0x40 == 0;
}
bool ActRaiser_TerrainStartEntry(CpuState *cpu) {
  return Fillmore(cpu) && cpu->Y >= 0xb101 && cpu->Y < 0xffff &&
      cpu_read8(cpu,0x0a,cpu->Y-1) == 5 && cpu_read8(cpu,0x0a,cpu->Y) == 34 &&
      cpu_read8(cpu,0x0a,cpu->Y+1) == 0 && cpu_read16(cpu,0,cpu->X+0x34) == 5*16;
}
RecompReturn ActRaiser_TerrainStart(CpuState *cpu) {
  if (!ActRaiser_TerrainStartEntry(cpu)) ActRaiserHleFatal("Invalid regional terrain start");
  cpu->A = ArRegionalTerrain_FillmoreStartY(1);++cpu->Y;
  cpu->P &= ~(CPU_P_N|CPU_P_Z);cpu_p_to_mirrors(cpu);
  cpu_hle_tailcall_request(0x009343,0x00933c);
  return RECOMP_RETURN_TAILCALL;
}
bool ActRaiser_TerrainCheckpointEntry(CpuState *cpu) {
  static const uint8_t record[] = {0xfe,155,0,156,23};
  if (!Fillmore(cpu) || cpu->Y < 0xb100 || cpu->Y > 0xfffb) return false;
  for (unsigned i = 0; i < sizeof(record); ++i)
    if (cpu_read8(cpu,0x0a,(uint16_t)(cpu->Y+i)) != record[i]) return false;
  return cpu_read16(cpu,0,cpu->X+2) == 156*16;
}
RecompReturn ActRaiser_TerrainCheckpoint(CpuState *cpu) {
  if (!ActRaiser_TerrainCheckpointEntry(cpu)) ActRaiserHleFatal("Invalid regional terrain checkpoint");
  cpu->A = ArRegionalTerrain_FillmoreCheckpointY(1);
  cpu->P &= ~(CPU_P_N|CPU_P_Z);cpu_p_to_mirrors(cpu);
  cpu_hle_tailcall_request(0x0094b7,0x0094b1);
  return RECOMP_RETURN_TAILCALL;
}
