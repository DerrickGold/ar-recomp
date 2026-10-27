/* Exercise the production camera adapter, not just its arithmetic helpers.
 * ROM parallax/player tails are sinks; planning and projection are real C. */
#include "actraiser/actraiser_action_bg.h"
#include "actraiser/actraiser_action_camera.h"
#include "actraiser_game.h"
#include "app/settings.h"
#include "byte_order.h"
#include "diorama/diorama_camera_policy.h"
#include "present/display_geometry.h"
#include "snesrecomp/game/cpu.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <string.h>

uint8 g_ram[kActRaiserWramSize];
Settings g_settings;
static DioramaLayerOrderTable s_layers;
static bool s_hle = true;
static int s_dx, s_dy, s_calls;
extern RecompReturn ActRaiser_UpdateActionCamera(CpuState *cpu);
static uint16_t Read(unsigned at) { return ByteOrder_ReadLe16(g_ram + at); }
static void Write(unsigned at, uint16_t value) { ByteOrder_WriteLe16(g_ram + at, value); }
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 at) { (void)cpu; assert(!bank); return g_ram[at]; }
void cpu_write8(CpuState *cpu, uint8 bank, uint16 at, uint8 value) {
  (void)cpu; assert(!bank); g_ram[at] = value;
}
RecompReturn bank_02_B9D5_M0X0(CpuState *cpu) { (void)cpu; s_dx = (int16_t)Read(0x7c); ++s_calls; return RECOMP_RETURN_NORMAL; }
RecompReturn bank_02_BA0B_M0X0(CpuState *cpu) { (void)cpu; s_dy = (int16_t)Read(0x7e); ++s_calls; return RECOMP_RETURN_NORMAL; }
RecompReturn bank_00_A1B0_M0X0(CpuState *cpu) { (void)cpu; ++s_calls; return RECOMP_RETURN_NORMAL; }
bool ActRaiserActionBg_HleEnabled(void) { return s_hle; }
const DioramaLayerOrderTable *DioramaLayerManifest_Table(void) { return &s_layers; }
const DioramaRoomOverride *ActRaiser_CurrentVirtualLayerRoom(void) {
  if (!g_settings.diorama_mode) return NULL;
  return DioramaLayerOrder_Find(&s_layers, g_ram[kActRaiserWram_MapGroup], g_ram[kActRaiserWram_CurrentMap]);
}
int Settings_VisibleWidth(void) { return 256 + 2 * g_ws_display_extra; }
bool Settings_IgnoreAspectRatio(void) { return false; }
bool ActRaiser_DioramaNeedsBgApron(const DioramaRoomOverride *room,
    unsigned bg, bool edits, bool world) {
  return DioramaCameraPolicy_NeedsBgApron(room, bg, edits, world && g_ws_active,
      g_actraiser_display_geometry->output_width, g_actraiser_display_geometry->output_height);
}
bool ActRaiserActionBg_BuildCurrentPlan(const uint8_t *wram, size_t size, bool padding,
    ActionBgPlan *out, ActionBgPresentationPolicy *presentation) {
  assert(wram == g_ram && size == sizeof(g_ram));
  ActionBgFrameState state = {.map_group = g_ram[kActRaiserWram_MapGroup],
    .map_number = g_ram[kActRaiserWram_CurrentMap], .decorative_padding_enabled = padding};
  for (unsigned bg = 0; bg < 2; ++bg) state.layer[bg] = (ActionBgLayerState){
    .camera_x = Read(kActRaiserWram_Bg1CameraX + bg * kActRaiserBgLayerStateStride),
    .camera_y = Read(kActRaiserWram_Bg1CameraY + bg * kActRaiserBgLayerStateStride),
    .world_width = Read(kActRaiserWram_Bg1Width), .world_height = Read(kActRaiserWram_Bg1Height),
    .tilemap_base = (uint16_t)(0x6000 + bg * 0x1000), .bgsc = (uint8_t)(0x63 + bg * 0x10)};
  *presentation = (ActionBgPresentationPolicy){0};
  return ActionBgPlan_Build(&state, out);
}
static void Reset(void) {
  memset(g_ram, 0, sizeof(g_ram)); DioramaLayerOrder_ClearTable(&s_layers);
  s_hle = true;
  g_ram[kActRaiserWram_MapGroup] = 1; g_ram[kActRaiserWram_CurrentMap] = 1;
  Write(kActRaiserWram_Bg1Width, 4096); Write(kActRaiserWram_Bg1Height, 768);
  Write(kActRaiserWram_Bg1CameraY, 300);
  g_settings = (Settings){.ws_action = true, .ws_bg2_padding = true,
    .display_mode = kDisplayMode_WideFull, .extended_aspect = kScreenAspect_1610,
    .diorama_camera_mode = kDioramaCam_Dynamic, .diorama_vertical_extend = 64,
    .diorama_dyncam_baseline_distance_x100 = 325, .diorama_layer_bg1 = true,
    .diorama_skybox = kDioramaSky_Only};
  DisplayGeometry_SetAutoVerticalBudget(0);
  DisplayGeometry_SetHorizontal(120, 120);
  DisplayGeometry_SetVertical(0, 0);
  DisplayGeometry_SetOutputSize(960, 600);
}
static void Step(int x, int dx, int dy) {
  Write(kActRaiserWram_Bg1CameraX, x); Write(0x7c, dx); Write(0x7e, dy);
  s_calls = 0;
  CpuState cpu = {.S = 0x1fc, .P = 0x30, .PB = 2};
  assert(ActRaiser_UpdateActionCamera(&cpu) == RECOMP_RETURN_NORMAL);
  assert(s_calls == 3 && cpu.S == 0x1ff && cpu.P == 0x30 && cpu.PB == 2);
}
static void AutoProfiles(void) {
  for (int diorama = 0; diorama < 2; ++diorama) {
    Reset(); g_settings.diorama_mode = diorama; g_settings.extended_aspect = kScreenAspect_Auto;
    Step(3840, 1, 0);
    uint16_t lo, hi; assert(ActRaiser_ActionCameraTravel(&lo, &hi));
    const int stop = Read(kActRaiserWram_Bg1CameraX);
    assert(stop == hi && hi < 3840);
    for (int profile = kDisplayMode_43; profile <= kDisplayMode_WideFull; ++profile) {
      g_settings.display_mode = profile; g_settings.ws_action = false;
      Step(3840, 1, 0);
      assert(Read(kActRaiserWram_Bg1CameraX) == stop);
      assert(g_settings.display_mode == profile && !g_settings.ws_action);
      for (int frame = 0; frame < 32; ++frame) {
        Step(stop, 1, 0); assert(Read(kActRaiserWram_Bg1CameraX) == stop && s_dx == 0);
      }
    }
    g_settings.extended_aspect = kScreenAspect_43; g_settings.display_mode = kDisplayMode_43;
    Step(3840, 1, 0); assert(Read(kActRaiserWram_Bg1CameraX) == 3840 && s_dx == 1);
    g_settings.display_mode = kDisplayMode_WideRaw; g_settings.ws_action = true;
    Step(3840, 1, 0); assert(Read(kActRaiserWram_Bg1CameraX) == 3840 && s_dx == 1);
  }
}
static void SavedPolicy(void) {
  Reset();
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&s_layers, 1, 1); assert(room);
  room->bg_policy[0] = (DioramaBgPolicyOverride){.set_horizontal = true,
    .horizontal = {.mode = kActionBgExtent_Fixed, .left = 24, .right = 48}};
  for (int diorama = 0; diorama < 2; ++diorama) {
    g_settings.diorama_mode = diorama;
    Step(3840, 1, 0); assert(Read(kActRaiserWram_Bg1CameraX) == (diorama ? 3792 : 3720));
    Step(0, -1, 0); assert(Read(kActRaiserWram_Bg1CameraX) == (diorama ? 24 : 120));
  }
  room->bg_policy[0].horizontal.mode = kActionBgExtent_Inherit; /* invalid at layer scope */
  Step(3840, 1, 0); assert(Read(kActRaiserWram_Bg1CameraX) == 3720);
  room->bg_policy[0].horizontal.mode = kActionBgExtent_Fixed;
  room->bg_policy[0].horizontal.left = room->bg_policy[0].horizontal.right = 0;
  Step(0, 0, 0); assert(Read(kActRaiserWram_Bg1CameraX) == 0);
  /* A wider capture must never alter the native vertical camera/ROM delta. */
  Write(kActRaiserWram_Bg1CameraY, 543);
  Step(500, 0, 1); assert(Read(kActRaiserWram_Bg1CameraY) == 543 && s_dy == 1);
  Write(kActRaiserWram_Bg1CameraY, 600);
  Step(500, 0, 0); assert(Read(kActRaiserWram_Bg1CameraY) == 600);
  s_hle = false;
  Step(3840, 1, 0); assert(Read(kActRaiserWram_Bg1CameraX) == 3840);
}
static void CapturePolicy(void) {
  Reset(); ActionBgPlan plan; ActionBgPresentationPolicy presentation;
  assert(ActRaiserActionBg_BuildCurrentPlan(g_ram, sizeof(g_ram), true, &plan, &presentation));
  assert(DioramaCameraPolicy_WorldApron(&plan.layer[0]));
  assert(!DioramaCameraPolicy_NeedsBgApron(NULL, 0, false, true, 16, 10));
  assert(DioramaCameraPolicy_NeedsBgApron(NULL, 0, false, true, 16, 9));
  assert(!DioramaCameraPolicy_NeedsBgApron(NULL, 0, false, false, 21, 9));
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&s_layers, 1, 1); assert(room);
  room->framing[0].x = 12;
  assert(DioramaCameraPolicy_HasEdits(room, 0));
  assert(DioramaCameraPolicy_NeedsBgApron(room, 0, true, false, 16, 10));
  room->framing[0].x = 0;
  for (int width = 960; width <= 1440; width += 240) {
    const bool apron = DioramaCameraPolicy_NeedsBgApron(room, 0, false, true, width, 600);
    const DioramaCapture capture = DioramaCameraPolicy_Capture(&plan, room, 3800, 543, 120, 64, apron);
    assert(capture.authentic_y0 == 128 && capture.height == 352);
    assert(capture.bg_apron_mask == apron && capture.horizontal_bounds.valid);
    assert(capture.vertical_bounds.valid);
  }
  plan.layer[0].default_edge = kActionBgEdge_Mirror;
  assert(!DioramaCameraPolicy_FiniteLayer(&plan.layer[0]));
  assert(!DioramaCameraPolicy_WorldApron(&plan.layer[0]));
}

static void NearlyScreenSizedRoom(void) {
  /* Marahna 05:03: a 512-pixel room with one painted column on the left.
   * At the user's 3.25 baseline it just fills 16:9; a slight zoom-in leaves
   * two pixels of travel. Neither should scroll, while 16:10 must still do so. */
  const int outputs[][2] = {{960, 540}, {1920, 1080}, {3420, 2140}};
  const int distances[] = {320, 324, 325};
  for (unsigned output = 0; output < sizeof(outputs) / sizeof(*outputs); ++output) {
    for (int aspect = 9; aspect <= 10; ++aspect) {
      Reset(); g_settings.diorama_mode = true;
      g_settings.extended_aspect = aspect == 9 ? kScreenAspect_169 : kScreenAspect_1610;
      Write(0x18, 0x0305);
      Write(kActRaiserWram_Bg1Width, 512); Write(kActRaiserWram_Bg1Height, 256);
      Write(kActRaiserWram_Bg1CameraY, 31);
      DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&s_layers, 5, 3); assert(room);
      assert(DioramaLayerOrder_ParseLine(room, "bg1-map:us+jp+eu = bounds:-1,-6,32,19", NULL));
      assert(DioramaLayerOrder_ParseLine(room,
          "bg1-stamp:us+jp+eu = cell:-1,-6 metatile:4F words:1CBE,1CBE,1CBE,1CBE bands:1,1,1,1", NULL));
      assert(DioramaLayerOrder_ParseLine(room,
          "bg1-stamp:us+jp+eu = cell:31,18 metatile:02 words:3466,3466,3466,3466 bands:2,2,2,2", NULL));
      assert(DioramaLayerOrder_ParseLine(room, "bg1hi = z:0.5", NULL));
      DisplayGeometry_SetOutputSize(outputs[output][0], outputs[output][1]);
      const ActRaiserDisplayGeometry geometry =
          DisplayGeometry_CalculateHorizontal(224, 16, aspect, false, true);
      DisplayGeometry_SetHorizontal(geometry.render_extra, geometry.display_extra);
      for (unsigned zoom = 0; zoom < sizeof(distances) / sizeof(*distances); ++zoom) {
        const int distance = distances[zoom];
        g_settings.diorama_dyncam_baseline_distance_x100 = distance;
        uint16_t lo, hi;
        Step(120, 0, 0); assert(ActRaiser_ActionCameraTravel(&lo, &hi));
        if (aspect == 9 && distance >= 324) {
          assert(lo == 120 && hi == 120);
          for (int x = 0; x <= 256; x += 64) {
            Step(x, 1, 0); assert(Read(kActRaiserWram_Bg1CameraX) == 120 && s_dx == 0);
            Step(x, -1, 0); assert(Read(kActRaiserWram_Bg1CameraX) == 120 && s_dx == 0);
          }
        } else {
          assert(hi - lo >= (aspect == 10 ? 50 : 3));
          Step(120, 1, 0); assert(Read(kActRaiserWram_Bg1CameraX) == 121 && s_dx == 1);
          Step(120, -1, 0); assert(Read(kActRaiserWram_Bg1CameraX) == 119 && s_dx == -1);
        }
        assert(Read(kActRaiserWram_Bg1CameraY) == 31);
      }
    }
  }
}
int main(void) {
  AutoProfiles(); SavedPolicy(); CapturePolicy(); NearlyScreenSizedRoom();
  DioramaLayerOrder_ClearTable(&s_layers);
  puts("Production action camera: Auto/manual policies, saved extents, edge stops, native Y and capture guards passed");
  return 0;
}
