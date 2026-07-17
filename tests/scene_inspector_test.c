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
