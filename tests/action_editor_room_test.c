/* Native oracle for full-room scanout. The browser test generates its assets
 * from the embedded catalogue; the renderer itself never needs a capture. */
#include "room_scene.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static char *Read(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb"); if (!f) return NULL;
  if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
  long n = ftell(f); rewind(f);
  if (n < 0 || n > 1024 * 1024) { fclose(f); return NULL; }
  char *p = malloc((size_t)n + 1);
  if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
  fclose(f); p[n] = 0; *size = n; return p;
}
int main(int argc, char **argv) {
  if (argc != 3 && argc != 4) return 2;
  size_t n;
  char *bytes = Read(argv[1], &n);
  static ActionSceneSnapshot assets;
  assert(bytes && ActionSceneSnapshot_Decode((const uint8_t *)bytes, n, &assets)); free(bytes);
  EditorRoomScene *room = EditorRoomScene_Create(&assets); assert(room);
  char *ini = Read(argv[2], &n); assert(ini && EditorRoomScene_Configure(room, ini)); free(ini);
  if (argc == 4) {
    char *effects = Read(argv[3],&n); unsigned line;
    assert(effects && EditorRoomScene_ConfigureEffects(room,effects,n,&line));free(effects);
  }
  int x, y, extra, vertical; unsigned frame;
  while (scanf("%d %d %u %d %d", &x, &y, &frame, &extra, &vertical) == 5) {
    assert(EditorRoomScene_Render(room, x, y, frame, extra, vertical));
    const uint32_t hash = EditorRoomScene_Hash(room);
    assert(!EditorRoomScene_Render(room, -1, y, frame, extra, vertical));
    assert(!EditorRoomScene_Configure(room, "[unterminated"));
    assert(EditorRoomScene_Hash(room) == hash);
    printf("%08x %08x\n", hash, EditorRoomScene_EffectHash(room));
  }
  if (assets.scene.group == 1 && assets.scene.map == 1 && !assets.terrain_profile) {
    assert(EditorRoomScene_Configure(room, ""));
    assert(EditorRoomScene_Render(room, 0, 400, 0, 0, 0));
    uint32_t original = EditorRoomScene_Hash(room), pixel = 0;
    const SrSceneSurfaces *surface = EditorRoomScene_Surfaces(room);
    int sx = 0, sy = 0;
    for (int row = 0; row < 224 && !pixel; ++row)
      for (int col = 64; col < 320 && !pixel; ++col)
        for (int band = 0; band < 3 && !pixel; ++band)
          if ((pixel = surface->bands[0][band][row * surface->pitch_pixels + col])) {
            sx = col; sy = row;
          }
    assert(pixel);
    const int cx = (sx - 64) / 16, cy = (400 + sy + 1) / 16;
    char edits[256];
    snprintf(edits, sizeof(edits), "[layers:01:01]\nbg1-virtual = cells:%d,%d-%d,%d band:0\n", cx, cy, cx, cy);
    assert(EditorRoomScene_Configure(room, edits));
    assert(EditorRoomScene_Render(room, 0, 400, 0, 0, 0));
    size_t at = (size_t)sy * surface->pitch_pixels + sx;
    assert(surface->bands[0][2][at] == pixel);
    assert(!surface->bands[0][0][at] && !surface->bands[0][1][at]);
    snprintf(edits, sizeof(edits), "[layers:01:01]\nbg1-pixels = cell:%d,%d transparent:"
        "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\n", cx, cy);
    assert(EditorRoomScene_Configure(room, edits));
    assert(EditorRoomScene_Render(room, 0, 400, 0, 0, 0));
    for (int band = 0; band < 3; ++band) assert(!surface->bands[0][band][at]);
    assert(EditorRoomScene_Configure(room, ""));
    assert(EditorRoomScene_Render(room, 0, 400, 0, 0, 0));
    assert(EditorRoomScene_Hash(room) == original);
  }
  if (assets.scene.group == 2 && assets.scene.map == 1) {
    assert(EditorRoomScene_Configure(room, ""));
    assert(EditorRoomScene_Render(room, 1152, 240, 37, 120, 64));
    const SrSceneSurfaces *surface = EditorRoomScene_Surfaces(room);
    assert(EditorRoomScene_Capture(room)->authentic_y0 > 0);
    /* BG2 is at its own top even though the foreground can expose extra rows.
     * No water from the bottom of the native page may wrap above its sky. */
    for (int band = 0; band < 3; ++band)
      for (int col = 0; col < surface->pitch_pixels; ++col)
        assert(surface->bands[1][band][col] == 0);
  }
  EditorRoomScene_Destroy(room);
  return 0;
}
