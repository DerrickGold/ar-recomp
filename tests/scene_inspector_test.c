#include "scene_inspector.h"

#include <stdio.h>
#include <string.h>

#include "snes/ppu.h"

uint8 g_ram[0x20000];
Ppu *g_ppu;
static Ppu s_ppu;
static int s_failures;

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

static void ResetPpu(void) {
  memset(&s_ppu, 0, sizeof(s_ppu));
  memset(g_ram, 0, sizeof(g_ram));
  g_ppu = &s_ppu;
  s_ppu.bgmode = 1;
  s_ppu.inidisp = 15;
  s_ppu.extraLeftRight = 43;
  s_ppu.extraLeftCur = 43;
  s_ppu.extraRightCur = 43;
  g_ram[0x18] = 1;
  g_ram[0x19] = 2;
  SceneInspector_SetSimFrameData(NULL);
  SceneInspector_Clear();
}

static void TestBgAndMirrorMapping(void) {
  ResetPpu();
  s_ppu.screenEnabled[0] = 1;
  s_ppu.bgXsc[0] = 0;
  s_ppu.bgTileAdr = 1; /* BG1 character base = VRAM word $1000. */
  s_ppu.vram[0] = 5 | (2 << 10) | (1 << 13);
  int char_address = 0x1000 + 5 * 16;
  s_ppu.vram[char_address + 1] = 1 << 3; /* output x=4, scanline 1 */

  CHECK(SceneInspector_Select(4, 0));
  CHECK(strstr(SceneInspector_PanelText(), "BG1 T$005") != NULL);
  CHECK(strstr(SceneInspector_PanelText(), "PIX1 CENTER") != NULL);

  s_ppu.wsLayerMirror = 1;
  CHECK(SceneInspector_Select(-4, 0));
  CHECK(strstr(SceneInspector_PanelText(), "PIX1 MIRROR") != NULL);
}

static void TestObjectIdentity(void) {
  ResetPpu();
  s_ppu.screenEnabled[0] = 0x10;
  int slot = 3;
  int index = slot * 2;
  s_ppu.oam[index] = 20 | (30 << 8);
  s_ppu.oam[index + 1] = 7 | (5 << 9) | (2 << 12);
  int char_address = 7 * 16;
  s_ppu.vram[char_address + 2] = 1 << 5; /* local pixel 2,2 */

  CHECK(SceneInspector_Select(22, 32));
  CHECK(strstr(SceneInspector_PanelText(), "OBJ#3 8X8") != NULL);
  CHECK(strstr(SceneInspector_PanelText(), "BASE$07 SUB$07") != NULL);
  int x0, y0, x1, y1;
  CHECK(SceneInspector_GetHighlight(&x0, &y0, &x1, &y1));
  CHECK(x0 == 20 && y0 == 30 && x1 == 28 && y1 == 38);

  SimFrameData sim = {0};
  sim.town = 1;
  sim.view = kSimView_Enhanced;
  sim.metadata_valid = true;
  sim.build_serial = 42;
  sim.requested_features = kSimFeature_All;
  sim.separated_valid = true;
  sim.separated_status = kSim3DCapture_Ready;
  sim.separated_hash = UINT64_C(0x123456789abcdef0);
  sim.object_count = 1;
  sim.objects[0] = (SimRenderObject){
    .record_address = 0x0A00,
    .composition = 0xE71B,
    .oam_first = slot,
    .oam_count = 1,
    .priority = 2,
    .tier = kSimRecordTier_World,
    .foot_x = 120,
    .foot_y = 104,
  };
  /* The panel must report the classifier's own verdict for this Napper
   * ground-pluck composition, not a second inspector-local guess. */
  SimObjectClassification classification = Sim3D_ClassifyObject(
      sim.objects[0].tier, 0x13, 11, sim.objects[0].record_address,
      sim.objects[0].composition);
  sim.objects[0].height_class = classification.height_class;
  sim.objects[0].virtual_height = classification.virtual_height;
  sim.objects[0].traits = classification.traits;
  SceneInspector_SetSimFrameData(&sim);
  CHECK(SceneInspector_Select(22, 32));
  CHECK(strstr(SceneInspector_PanelText(),
               "SIM3D enhanced META OK SERIAL 42") != NULL);
  CHECK(strstr(SceneInspector_PanelText(),
               "FLAT READY STATUS ready MISMATCH 0 "
               "HASH 123456789ABCDEF0") != NULL);
  CHECK(strstr(SceneInspector_PanelText(),
               "SIMW REC$0A00 CMP$E71B OAM3+1 P2 FOOT120,104 "
               "semi_grounded H8 ATLAS PENDING") != NULL);
}

static void TestPlaneFiltering(void) {
  ResetPpu();
  s_ppu.screenEnabled[0] = 1;
  s_ppu.bgTileAdr = 1;
  s_ppu.vram[0] = 5 | (1 << 13);
  int char_address = 0x1000 + 5 * 16;
  s_ppu.vram[char_address + 1] = 1 << 3;

  CHECK(SceneInspector_SelectFiltered(
      4, 0, kSceneInspectorBg3, false));
  CHECK(strstr(SceneInspector_PanelText(), "BG1") == NULL);
  CHECK(strstr(SceneInspector_PanelText(), "NO VISIBLE") != NULL);
}

int main(void) {
  TestBgAndMirrorMapping();
  TestObjectIdentity();
  TestPlaneFiltering();
  SceneInspector_Clear();
  CHECK(!SceneInspector_HasSelection());
  if (s_failures) {
    fprintf(stderr, "scene inspector tests: %d failure(s)\n", s_failures);
    return 1;
  }
  printf("scene inspector tests passed\n");
  return 0;
}
