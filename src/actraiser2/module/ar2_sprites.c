#include "ar2_sprites.h"
#include "ar2_world.h"
#include "snesrecomp/game/runtime.h"
#include <stdlib.h>

static int margin;
static unsigned position_count;
static SrPpuObjPositionUpdate positions[SR_PPU_OBJ_POSITION_UPDATE_MAX];
static bool screen_actor;
static unsigned screen_first, screen_count;

void Ar2Sprites_ScreenRange(unsigned *first, unsigned *count) {
  *first = screen_first;
  *count = screen_count;
}

void Ar2Sprites_Reset(void) {
  margin = 0;
  position_count = 0;
  screen_actor = false;
  screen_first = screen_count = 0;
}

void Ar2Sprites_SetMargin(int extra) { margin = extra; }

static void RecordPiece(void) {
  if (g_cpu.Y >= 512 || (g_cpu.Y & 3) || position_count >= SR_PPU_OBJ_POSITION_UPDATE_MAX) return;
  /* Both native and widened predicates accept exactly -17..222 vertically.
   * Record their accepted continuation, including at width 256, so resizing
   * a paused frame never loses its signed world positions. A is still X+16. */
  int y = g_ram[0x65e + g_cpu.Y];
  if (y >= 239) y -= 256;
  positions[position_count++] = (SrPpuObjPositionUpdate){
      .slot = g_cpu.Y / 4, .x = (int16_t)g_cpu.A - 16, .y = y,
      .flags = screen_actor ? 0 : SR_PPU_OBJ_POSITION_CAMERA_RELATIVE};
}

void Ar2Sprites_Checkpoint(uint32_t pc) {
  /* Keep positions with their OAM when the game pauses. Replace the list
   * only when the native producer starts rebuilding it. */
  if (pc == 0x809ecb) {
    position_count = 0;
    screen_first = screen_count = 0;
    screen_actor = false;
  }
  if (pc == 0x809f0b) {
    /* The native entrance task becomes START at $82:9461 and then PAUSE.
     * Its actor emits one contiguous OAM range. Identify that producer,
     * never its tile numbers, palette or overlapping screen rectangle. */
    const unsigned task = 0x10004 + g_cpu.X;
    const unsigned pc16 = g_ram[task] | (unsigned)g_ram[task + 1] << 8;
    screen_actor = g_ram[task + 2] == 0x82 && pc16 >= 0x9461 && pc16 < 0x9556;
    if (screen_actor) screen_first = g_cpu.Y / 4;
  }
  if (pc == 0x809fac && screen_actor) {
    screen_count = g_cpu.Y / 4 - screen_first;
    screen_actor = false;
  }
  if (pc == 0x809f77 || pc == 0x809fe0) RecordPiece();
}

bool Ar2Sprites_Wide(CpuState *cpu) {
  return margin > 0 && cpu && cpu->PB == 0x80 && !cpu->emulation && !cpu->m_flag && !cpu->x_flag &&
         !cpu->_flag_D && !cpu->D && Ar2World_SupportsScene(g_ram[0x51a]);
}

static RecompReturn branch(uint32_t target, uint32_t source) {
  if (!cpu_hle_tailcall_request(target, source)) abort();
  return RECOMP_RETURN_TAILCALL;
}

RecompReturn Ar2Sprites_CullActor(CpuState *cpu) {
  /* $97A1..$97C5 tests the actor's left/right bounds. The continuation owns
   * vertical culling, blinking, draw order and native offscreen destruction.
   * Positions stay in world coordinates; only the horizontal window grows. */
  const unsigned actor = cpu->X;
  const uint16_t x = cpu_read16(cpu, cpu->DB, actor);
  const uint16_t camera = cpu_read16(cpu, cpu->DB, 0x540);
  const int shift = Ar2World_CaptureShift(camera, cpu_read16(cpu, cpu->DB, 0x55c) * 16, margin);
  const int view_left = shift - margin, view_right = shift + 256 + margin;
  uint16_t edge = x - cpu_read16(cpu, cpu->DB, actor + 0x10);
  const int left = (int16_t)(edge - camera);
  const uint16_t right_edge = x + cpu_read16(cpu, cpu->DB, actor + 0x14);
  const int right = (int16_t)(right_edge - camera);
  const bool visible = (left >= view_left && left < view_right) ||
                       (left < view_left && right >= view_left && right < view_right);
  if (left < view_left) edge = right_edge;
  cpu->A = edge - camera;
  cpu->_flag_V = ((edge ^ camera) & (edge ^ cpu->A) & 0x8000) != 0;
  const uint16_t biased = cpu->A - view_left;
  const uint16_t width = 256 + 2 * margin;
  cpu->_flag_C = biased >= width;
  cpu->_flag_Z = biased == width;
  cpu->_flag_N = (uint16_t)(biased - width) >> 15;
  cpu_mirrors_to_p(cpu);
  return branch(visible ? 0x8097c6 : 0x809834, 0x8097a1);
}

static RecompReturn cull_piece(CpuState *cpu, uint32_t source, uint32_t keep, uint32_t skip) {
  /* A is biased by 16. Native CMP/BCS accepts [0,272); SBC #15 then
   * consumes carry=0. Keep that subtraction and all native packing intact. */
  const int biased_x = (int16_t)cpu->A;
  const int shift = Ar2World_CaptureShift(cpu_read16(cpu, cpu->DB, 0x540),
      cpu_read16(cpu, cpu->DB, 0x55c) * 16, margin);
  const bool visible = biased_x >= shift - margin && biased_x < shift + 272 + margin;
  cpu->_flag_C = !visible;
  cpu->_flag_Z = biased_x == shift + 272 + margin;
  cpu->_flag_N = (uint16_t)(cpu->A - (shift + 272 + margin)) >> 15;
  cpu_mirrors_to_p(cpu);
  return branch(visible ? keep : skip, source);
}

RecompReturn Ar2Sprites_CullPiece(CpuState *cpu) {
  return cull_piece(cpu, 0x809f72, 0x809f77, 0x809fae);
}

RecompReturn Ar2Sprites_CullEffect(CpuState *cpu) {
  return cull_piece(cpu, 0x809fdb, 0x809fe0, 0x80a00f);
}

bool Ar2Sprites_Publish(SrRunnerHandle *runner, const SnesRunnerApi *api, int camera_shift) {
  if (!Ar2World_SupportsScene(g_ram[0x51a])) position_count = 0;
  SrPpuStateSnapshot state = {.struct_size = sizeof(state)};
  if (api->query_ppu_state(runner, &state) != SR_RESULT_OK) return false;
  /* Translate a copy: paused frames retain positions in native coordinates. */
  SrPpuObjPositionUpdate captured[SR_PPU_OBJ_POSITION_UPDATE_MAX];
  for (unsigned i = 0; i < position_count; i++) {
    captured[i] = positions[i];
    if (captured[i].flags & SR_PPU_OBJ_POSITION_CAMERA_RELATIVE)
      captured[i].x -= camera_shift;
  }
  const SrPpuObjMetadataRequest request = {.struct_size = sizeof(request),
                                           .flags = SR_PPU_OBJ_METADATA_CLEAR_POSITIONS |
                                                    SR_PPU_OBJ_METADATA_CLEAR_CAMERA_RELATIVE,
                                           .lifetime_generation = state.lifetime_generation,
                                           .updates = position_count ? captured : NULL,
                                           .update_count = position_count};
  return api->update_ppu_obj_metadata(runner, &request) == SR_RESULT_OK;
}
