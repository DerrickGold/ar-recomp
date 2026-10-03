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
static bool s_sim_test;
static SrPpuObjPart s_sim_parts[4];
static unsigned s_sim_part_count, s_sim_native_count, s_sim_clipped_count;
static ActRaiserDisplayGeometry s_geometry;
static ActionApronGeometry s_apron;
const ActRaiserDisplayGeometry *const g_actraiser_display_geometry = &s_geometry;
extern RecompReturn ActRaiser_ObjectVisibilityScanWide(CpuState *cpu);
extern RecompReturn ActRaiser_BuildObjectSprites(CpuState *cpu);
extern RecompReturn ActRaiser_BuildSimSprites(CpuState *cpu);
extern RecompReturn ActRaiser_BuildSimSpritesAlt(CpuState *cpu);

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
ActionApronGeometry ActRaiser_ObjApronGeometry(void) { return s_apron; }
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
/* Metadata sink for SIM edge fixtures. Action fixtures must never invoke it. */
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
  assert(s_sim_test);
  return true;
}
SimObjectClassification Sim3D_ClassifyObject(uint8_t tier, uint16_t type, uint16_t state,
                                            uint16_t record, uint16_t composition) {
  (void)type;
  (void)state;
  assert(s_sim_test && tier == kSimRecordTier_World);
  assert(record == kActRaiserWram_SimWorldRecords);
  assert(composition == 0xD32B || composition == 0xD4FA);
  return (SimObjectClassification){.traits = composition == 0xD32B
      ? kSimObjectTrait_StructureOverlay : 0};
}
void SimRenderMetadata_RecordWord06(uint16_t v) {
  (void)v;
  assert(s_sim_test);
}
void SimRenderMetadata_RecordAnchor(int16_t x, int16_t y) {
  (void)x;
  (void)y;
  assert(s_sim_test);
}
void SimRenderMetadata_RecordPart(uint16_t oam, uint16_t attr) {
  (void)attr;
  assert(s_sim_test && oam == s_sim_native_count * 4);
  ++s_sim_native_count;
}
void SimRenderMetadata_RecordExactOamPart(const SrPpuObjPart *p) {
  assert(s_sim_test && s_sim_part_count < 4);
  s_sim_parts[s_sim_part_count++] = *p;
}
void SimRenderMetadata_RecordSyntheticPart(uint16_t oam, const SrPpuObjPart *p) {
  assert(s_sim_test && oam == s_sim_native_count * 4 && s_sim_part_count < 4);
  s_sim_parts[s_sim_part_count++] = *p;
}
void SimRenderMetadata_RecordClippedPart(uint8_t reason) {
  (void)reason;
  assert(s_sim_test);
  ++s_sim_clipped_count;
}
void SimRenderMetadata_EndRecord(uint16_t oam) {
  assert(s_sim_test && oam == s_sim_native_count * 4);
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
  s_apron = (ActionApronGeometry){0};
  s_sim_test = false;
  s_sim_part_count = s_sim_native_count = s_sim_clipped_count = 0;
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
static void TestCaptureGuardParts(void) {
  /* Includes Aitos' X=370 large bamboo tile straddling the 376px scanout
   * edge, wholly outside parts, left-edge straddlers and finite guard limits. */
  const int positions[] = {
      -200,-199,-192,-184,-144,-136,-124,-104,100,360,362,370,376,384,432,440};
  for (unsigned large=0;large<2;++large) for (unsigned i=0;i<sizeof(positions)/sizeof(*positions);++i) {
    Reset(64);
    s_ppu.margin_left = s_ppu.margin_right = 120;
    s_apron = (ActionApronGeometry){120,64};
    const int x = positions[i], size = large ? 16 : 8;
    const unsigned obj = Object(0,40,1,0x230c);
    Write(obj + kActRaiserActionObject_WorldX,(uint16_t)x);
    Write(obj + kActRaiserActionObject_RightExtent,(uint16_t)size);
    g_ram[0x4005] = large;
    Scan();
    const bool emitted = x >= -136 && x < 376;
    const bool guard = (x < -120 && x+size > -184) || (x < 440 && x+size > 376);
    assert(s_position_count == (unsigned)emitted);
    assert(ActionApron_Count() == (int)guard);
    if (guard) {
      const SrPpuObjPart *part = ActionApron_Parts();
      assert(part[0].x == x && part[0].y == 39 && part[0].size == size);
      assert(part[0].tile_attr == 0x230c);
    }
    if (emitted) assert(s_positions[0].x == x && Read(kActRaiserOamShadow+2) == 0x230c);
    else assert(Read(kActRaiserOamShadow) == 0xe080);
  }
  /* Completing the last real OAM slot must still record its outer pixels. */
  Reset(0);
  s_ppu.margin_left = s_ppu.margin_right = 120;
  s_apron = (ActionApronGeometry){120,64};
  Object(0,40,127,1);
  const unsigned last = Object(1,40,1,2);
  Write(last+kActRaiserActionObject_WorldX,370);
  g_ram[0x4805] = 1;
  Scan();
  assert(s_position_count == 128 && ActionApron_Count() == 1);
  assert(ActionApron_Parts()[0].x == 370 && ActionApron_Parts()[0].tile_attr == 2);
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
static void TestCompleteBubblesAtWindowEdges(void) {
  /* The bottom part crosses the flat emitter's edge while the projected
   * bubble can still be visible above its roof. Check both emitters, every
   * edge, and the two-pixel bounce through each boundary. */
  const int edges[][2] = {{128,15}, {128,239}, {-8,128}, {264,128}};
  for (unsigned alternate = 0; alternate < 2; ++alternate)
    for (unsigned bubble = 0; bubble < 2; ++bubble)
      for (unsigned edge = 0; edge < 4; ++edge)
        for (int bounce = -3; bounce <= 3; ++bounce) {
          Reset(0);
          s_sim_test = true;
          const unsigned record = kActRaiserWram_SimWorldRecords;
          const unsigned composition = bubble ? 0xD32B : 0xD4FA;
          const int x = edges[edge][0] + (edge >= 2 ? bounce : 0);
          const int y = edges[edge][1] + (edge < 2 ? bounce : 0);
          Write(record + 8, composition);
          Write(record + 0x0a, (uint16_t)x);
          Write(record + 0x0c, (uint16_t)y);
          Write(0x9a, kActRaiserOamHighTable);
          Write(0x9c, 1);
          g_ram[composition] = 2;
          unsigned expected_native = 0;
          for (unsigned part = 0; part < 2; ++part) {
            const unsigned at = composition + 1 + part * 5;
            g_ram[at] = 1;  /* 16px part */
            g_ram[at + 1] = 8;
            g_ram[at + 2] = part ? 1 : 0xf1;
            Write(at + 3, 0x2001 + part);
            const int biased_y = y + (part ? 1 : -15);
            expected_native += (uint16_t)(x + 8) < kSimSpriteWindowBiasedWidth &&
                (uint16_t)biased_y < kSimSpriteWindowBiasedHeight;
          }
          CpuState cpu = {.X = record, .S = 0x1ff};
          assert((alternate ? ActRaiser_BuildSimSpritesAlt(&cpu)
                            : ActRaiser_BuildSimSprites(&cpu)) == RECOMP_RETURN_NORMAL);
          assert(s_sim_native_count == expected_native);
          assert(Read(0x98) == expected_native * 4);
          assert(s_position_count == expected_native);
          assert(s_sim_part_count == (bubble ? 2 : expected_native));
          assert(s_sim_clipped_count == (bubble ? 0 : 2 - expected_native));
          if (bubble)
            for (unsigned part = 0; part < 2; ++part) {
              assert(s_sim_parts[part].x == x - 8);
              assert(s_sim_parts[part].y == y - 32 + (int)part * 16);
              assert(s_sim_parts[part].size == 16);
              assert(s_sim_parts[part].tile_attr ==
                     (alternate ? 0x2601 + part : 0x2001 + part));
            }
        }
}

static void TestAutoDrawIgnoresManualProfile(void) {
  /* Square and wide drawables, flat and Diorama, retain the saved 4:3 flags.
   * Exercise both the object scan and component emitter, not a gate proxy. */
  for (unsigned diorama = 0; diorama < 2; ++diorama) {
    for (unsigned vertical = 0; vertical < 2; ++vertical) {
      Reset(vertical ? 37 : 0);
      g_settings = (Settings){.extended_aspect = kScreenAspect_Auto,
          .display_mode = kDisplayMode_43, .diorama_mode = diorama};
      s_ppu.margin_left = s_ppu.margin_right = vertical ? 0 : 43;
      const unsigned obj = Object(0, vertical ? -40 : 40, 1, 7);
      if (!vertical) Write(obj + kActRaiserActionObject_WorldX, 280);
      const uint16_t camera_x = Read(kActRaiserWram_Bg1CameraX);
      const uint16_t camera_y = Read(kActRaiserWram_Bg1CameraY);
      Scan();
      assert(s_position_count == 1);
      assert(s_positions[0].x == (vertical ? 100 : 280));
      assert(s_positions[0].y == (vertical ? -41 : 39));
      assert(Read(kActRaiserOamShadow + 2) == 7);
      assert(Read(obj + kActRaiserActionObject_Flags) &
          kActRaiserObjectFlag_OutsideActivation);
      assert(Read(kActRaiserWram_Bg1CameraX) == camera_x);
      assert(Read(kActRaiserWram_Bg1CameraY) == camera_y);
      assert(!g_settings.ws_sprites && !g_settings.ws_margin_objects &&
          !g_settings.ws_margin_activation);

      /* The same retained manual profile still culls margin-only actors. */
      g_settings.extended_aspect = kScreenAspect_169;
      Scan();
      assert(s_position_count == 0);
    }
  }
}

int main(int argc, char **argv) {
  TestAutoDrawIgnoresManualProfile();
  TestCaptureGuardParts();
  if (argc == 2 && !strcmp(argv[1], "sim-bubbles")) {
    TestCompleteBubblesAtWindowEdges();
    puts("SIM bubble edge regression passed");
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "priority"))
    TestVerticalPartsCannotDisplaceNativeParts();
  else if (argc == 2 && !strcmp(argv[1], "empty"))
    TestEmptyCompositionIsEmpty();
  else
    TestFullPoolStillUpdatesActivation();
  puts("action sprite pool regression passed");
  return 0;
}
