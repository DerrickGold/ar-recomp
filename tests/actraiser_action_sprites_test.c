/* Exercise the production action scan/emitter with synthetic object records,
 * composition data and a runner metadata sink; no ROM or GPU is required. */
#include "actraiser/actraiser_rtl.h"
#include "actraiser/actraiser_sprite_ownership.h"
#include "actraiser/regional/actraiser_actor_art.h"
#include "actraiser_game.h"
#include "app/settings.h"
#include "byte_order.h"
#include "present/display_geometry.h"
#include "sim/sim_render_metadata.h"
#include "snesrecomp/game/cpu.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <string.h>

uint8 g_ram[kActRaiserWramSize];
Settings g_settings;
static SrPpuStateSnapshot s_ppu;
static SrPpuObjPositionUpdate s_positions[128];
static unsigned s_position_count, s_composition_reads;
static int s_runner;
static ActRaiserDisplayGeometry s_geometry;
const ActRaiserDisplayGeometry *const g_actraiser_display_geometry = &s_geometry;
extern RecompReturn ActRaiser_ObjectVisibilityScanWide(CpuState *cpu);
extern RecompReturn ActRaiser_BuildObjectSprites(CpuState *cpu);

static uint16_t Read(unsigned at) { return ByteOrder_ReadLe16(g_ram + at); }
static void Write(unsigned at, uint16_t v) { ByteOrder_WriteLe16(g_ram + at, v); }
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 at) {
  (void)cpu;
  assert(bank == 0 || bank == 0x7e);
  if (at >= 0x4000) ++s_composition_reads;
  return g_ram[at];
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 at) {
  return cpu_read8(cpu, bank, at) | (uint16)cpu_read8(cpu, bank, at + 1) << 8;
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 at, uint8 v) {
  (void)cpu;
  assert(bank == 0 || bank == 0x7e);
  g_ram[at] = v;
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 at, uint16 v) {
  cpu_write8(cpu, bank, at, v);
  cpu_write8(cpu, bank, at + 1, v >> 8);
}
static SrResult Query(SrRunnerHandle *runner, SrPpuStateSnapshot *out) {
  (void)runner;
  *out = s_ppu;
  return SR_RESULT_OK;
}
static SrResult Metadata(SrRunnerHandle *runner, const SrPpuObjMetadataRequest *req) {
  (void)runner;
  if (req->flags & SR_PPU_OBJ_METADATA_CLEAR_POSITIONS) s_position_count = 0;
  assert(s_position_count + req->update_count <= 128);
  for (unsigned i = 0; i < req->update_count; ++i)
    s_positions[s_position_count++] = req->updates[i];
  return SR_RESULT_OK;
}
const SnesRunnerApi *sr_runner_get_api(uint32_t version) {
  assert(version == SR_RUNNER_ABI_VERSION);
  static const SnesRunnerApi api = {
      .struct_size = sizeof(SnesRunnerApi),
      .capabilities = SR_RUNNER_CAP_PPU_STATE | SR_RUNNER_CAP_PPU_OBJ_METADATA,
      .query_ppu_state = Query,
      .update_ppu_obj_metadata = Metadata,
  };
  return &api;
}
ActionApronGeometry ActRaiser_ObjApronGeometry(void) { return (ActionApronGeometry){0}; }
RecompReturn bank_00_923A_M0X0(CpuState *cpu) {
  /* No HUD parts in these pressure fixtures. */
  cpu->Y = 0;
  cpu->S += 2;
  return RECOMP_RETURN_NORMAL;
}
bool ActRaiserActorArt_Active(void) { return false; }
bool ActRaiserActorArt_Draw(uint16_t base, uint16_t composition, unsigned visual,
                            ActRaiserActorArtDraw *out) {
  (void)base;
  (void)composition;
  (void)visual;
  (void)out;
  assert(false);
  return false;
}
void ActRaiserActorArt_ResolvePart(const ActRaiserActorArtDraw *draw, unsigned index, bool flip_x,
                                   bool flip_y, int16_t left, int16_t top,
                                   ActRaiserActorArtPart *part) {
  (void)draw;
  (void)index;
  (void)flip_x;
  (void)flip_y;
  (void)left;
  (void)top;
  (void)part;
  assert(false);
}
/* The shared translation unit also contains SIM emitters, which these action
 * tests must never invoke. */
bool SimRenderMetadata_BeginRecord(uint16_t record, bool world, bool alternate,
                                   uint16_t composition, uint16_t x, uint16_t y, uint16_t type,
                                   uint16_t state, uint16_t status, uint16_t oam) {
  (void)record;
  (void)world;
  (void)alternate;
  (void)composition;
  (void)x;
  (void)y;
  (void)type;
  (void)state;
  (void)status;
  (void)oam;
  assert(false);
  return false;
}
void SimRenderMetadata_RecordWord06(uint16_t v) {
  (void)v;
  assert(false);
}
void SimRenderMetadata_RecordAnchor(int16_t x, int16_t y) {
  (void)x;
  (void)y;
  assert(false);
}
void SimRenderMetadata_RecordPart(uint16_t oam, uint16_t attr) {
  (void)oam;
  (void)attr;
  assert(false);
}
void SimRenderMetadata_RecordExactOamPart(const SrPpuObjPart *p) {
  (void)p;
  assert(false);
}
void SimRenderMetadata_RecordSyntheticPart(uint16_t oam, const SrPpuObjPart *p) {
  (void)oam;
  (void)p;
  assert(false);
}
void SimRenderMetadata_RecordClippedPart(uint8_t reason) {
  (void)reason;
  assert(false);
}
void SimRenderMetadata_EndRecord(uint16_t oam) {
  (void)oam;
  assert(false);
}
void SimRenderMetadata_RecordFlightPlan(SimEruptionFlightPlan plan) {
  (void)plan;
  assert(false);
}
SimEruptionFlightPlan SimEruptionScript_ResolveFlight(SimEruptionScriptFetch fetch, void *context,
                                                      uint16_t base, uint16_t cursor, int wait) {
  (void)fetch;
  (void)context;
  (void)base;
  (void)cursor;
  (void)wait;
  assert(false);
  return (SimEruptionFlightPlan){0};
}

static void Reset(unsigned extend) {
  memset(g_ram, 0, sizeof(g_ram));
  s_composition_reads = 0;
  s_ppu = (SrPpuStateSnapshot){
      .struct_size = sizeof(s_ppu),
      .lifetime_generation = 1,
      .margin_top = extend,
      .margin_bottom = extend,
      .object_small_size_pixels = 8,
      .object_large_size_pixels = 16,
  };
  g_settings =
      (Settings){.ws_sprites = true, .ws_margin_objects = true, .ws_margin_activation = true};
  g_ram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Fillmore;
  g_ram[kActRaiserWram_CurrentMap] = 1;
  Write(kActRaiserWram_Bg1CameraY, 128);
  Write(kActRaiserWram_Bg1Width, 1024);
  ActRaiser_WidescreenSpritesBindRunner((SrRunnerHandle *)&s_runner);
  ActRaiserSpriteOwnership_Reset();
}
static unsigned Object(unsigned index, int screen_y, unsigned count, uint16_t tile) {
  const unsigned obj = kActRaiserWram_ActionObjectTable + index * kActRaiserActionObjectStride;
  const unsigned def = 0x4000 + index * 0x800;
  Write(obj, 0);
  Write(obj + kActRaiserActionObject_WorldX, 100);
  Write(obj + kActRaiserActionObject_WorldY, 128 + screen_y);
  Write(obj + kActRaiserActionObject_RightExtent, 8);
  Write(obj + kActRaiserActionObject_BottomExtent, 8);
  Write(obj + kActRaiserActionObject_Composition, def);
  g_ram[obj + kActRaiserActionObject_AnimationBank] = 0x7e;
  Write(obj + kActRaiserActionObject_FlipAttributes, 0x100);
  g_ram[def + 4] = count;
  for (unsigned i = 0; i < count; ++i)
    Write(def + 5 + i * 7 + 5, tile);
  Write(obj + kActRaiserActionObjectStride, kActRaiserObjectStatus_End);
  return obj;
}
static void Scan(void) {
  CpuState cpu = {.S = 0x1ff, .ram = g_ram};
  assert(ActRaiser_ObjectVisibilityScanWide(&cpu) == RECOMP_RETURN_NORMAL);
  assert(cpu.S == 0x201);
}
static void TestFullPoolStillUpdatesActivation(void) {
  for (unsigned extend = 0; extend <= 64; extend += 64) {
    Reset(extend);
    unsigned filling = Object(0, 40, 128, 1);
    unsigned outside = Object(1, -40, 1, 2);
    unsigned inside = Object(2, 80, 1, 3);
    Write(filling + kActRaiserActionObject_Flags, kActRaiserObjectFlag_OutsideActivation);
    Write(inside + kActRaiserActionObject_Flags, kActRaiserObjectFlag_OutsideActivation);
    Scan();
    assert(
        !(Read(filling + kActRaiserActionObject_Flags) & kActRaiserObjectFlag_OutsideActivation));
    assert(Read(outside + kActRaiserActionObject_Flags) & kActRaiserObjectFlag_OutsideActivation);
    assert(!(Read(inside + kActRaiserActionObject_Flags) & kActRaiserObjectFlag_OutsideActivation));
  }
}
static void TestVerticalPartsCannotDisplaceNativeParts(void) {
  const int margin_y[] = {-40, 260};
  for (unsigned side = 0; side < 2; ++side) {
    Reset(64);
    Object(0, margin_y[side], 128, 1);
    Object(1, 40, 1, 2);
    Scan();
    assert(s_position_count == 128);
    assert(s_positions[0].y == 39);
    assert(s_positions[1].y == margin_y[side] - 1);
    assert(Read(kActRaiserOamShadow + 2) == 2);
  }
  /* A tall actor spans both windows. Its extra components must also wait
   * until the next actor's native components have been allocated. */
  Reset(64);
  unsigned tall = Object(0, -40, 128, 1);
  Write(tall + kActRaiserActionObject_BottomExtent, 88);
  g_ram[0x4000 + 5 + 3] = 80;
  Object(1, 90, 1, 2);
  Scan();
  assert(s_position_count == 128);
  assert(s_positions[0].y == 39 && s_positions[1].y == 89);
  assert(Read(kActRaiserOamShadow + 6) == 2);

  /* Exact signed coordinates, size bits and partial high-table flush survive
   * the pass boundary, including a bottom Y that cannot fit in OAM's byte. */
  Reset(64);
  Object(0, 260, 2, 1);
  Object(1, 40, 3, 2);
  for (unsigned i = 0; i < 2; ++i)
    g_ram[0x4005 + i * 7] = 1;
  for (unsigned i = 0; i < 3; ++i)
    g_ram[0x4805 + i * 7] = 1;
  Scan();
  assert(s_position_count == 5);
  assert(s_positions[2].y == 39 && s_positions[3].y == 259);
  assert(g_ram[kActRaiserOamHighTable] == 0xaa);
  assert(g_ram[kActRaiserOamHighTable + 1] == 2);
}
static void TestEmptyCompositionIsEmpty(void) {
  Reset(64);
  Object(0, 40, 0, 1);
  Scan();
  assert(s_position_count == 0);
  assert(s_composition_reads <= 2);

  Reset(64);
  unsigned empty = Object(0, -40, 1, 1);
  Write(empty + kActRaiserActionObject_Composition, 0);
  g_ram[4] = 128; /* Scratch bytes are not a pending object's picture. */
  Object(1, 40, 1, 2);
  Scan();
  assert(s_position_count == 1 && s_positions[0].y == 39);
  assert(Read(kActRaiserOamShadow + 2) == 2);

  Reset(64);
  unsigned obj = Object(0, 40, 1, 1);
  memset(g_ram + kActRaiserOamHighTable, 0xa5, 32);
  CpuState cpu = {.X = obj, .Y = kActRaiserOamLowTableBytes, .S = 0x1ff};
  assert(ActRaiser_BuildObjectSprites(&cpu) == RECOMP_RETURN_NORMAL);
  assert(cpu._flag_C && cpu.Y == kActRaiserOamLowTableBytes);
  for (unsigned i = 0; i < 32; ++i)
    assert(g_ram[kActRaiserOamHighTable + i] == 0xa5);
}
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "priority"))
    TestVerticalPartsCannotDisplaceNativeParts();
  else if (argc == 2 && !strcmp(argv[1], "empty"))
    TestEmptyCompositionIsEmpty();
  else
    TestFullPoolStillUpdatesActivation();
  puts("action sprite pool regression passed");
  return 0;
}
