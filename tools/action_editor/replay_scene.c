/* Native replay of the editor's portable original-background comparison scene.
 * No ROM, SDL, GPU, gameplay or settings globals are needed. */
#include "action/action_scene_snapshot.h"
#include "deterministic_hash.h"
#include <stdio.h>
#include <string.h>

static uint8_t s_bytes[kActionSceneSnapshotMaxBytes + 1];
static ActionSceneSnapshot s_scene;
static uint32_t s_pixels[kActionRoomSceneFramePixels];

static bool Replay(const char *path, const char *ppm) {
  FILE *file = fopen(path, "rb");
  if (!file) return false;
  const size_t size = fread(s_bytes, 1, sizeof(s_bytes), file);
  const bool read_ok = !ferror(file);
  fclose(file);
  ActionRoomSceneFrameState frame;
  if (!read_ok || !ActionSceneSnapshot_Decode(s_bytes, size, &s_scene) ||
      !ActionRoomScene_BuildFrameState(&s_scene.scene, &s_scene.frame, &frame) ||
      !ActionRoomScene_RenderNativeFrame(&s_scene.scene, &frame,
          s_pixels, kActionRoomSceneFramePixels)) return false;
  uint32_t hash = DETERMINISTIC_HASH_FNV1A32_OFFSET;
  for (unsigned i = 0; i < kActionRoomSceneFramePixels; ++i)
    hash = DeterministicHash_Fnv1a32Word(hash, s_pixels[i]);
  if (ppm) {
    file = fopen(ppm, "wb");
    if (!file) return false;
    fprintf(file, "P6\n256 224\n255\n");
    for (unsigned i = 0; i < kActionRoomSceneFramePixels; ++i) {
      const uint8_t rgb[] = {(uint8_t)(s_pixels[i] >> 16),
          (uint8_t)(s_pixels[i] >> 8), (uint8_t)s_pixels[i]};
      if (fwrite(rgb, 1, sizeof(rgb), file) != sizeof(rgb)) { fclose(file); return false; }
    }
    if (fclose(file)) return false;
  }
  printf("%08x\n", (unsigned)hash);
  return true;
}

int main(int argc, char **argv) {
  if (argc == 4 && !strcmp(argv[1], "--ppm")) {
    if (Replay(argv[3], argv[2])) return 0;
  } else if (argc > 1 && strcmp(argv[1], "--ppm")) {
    for (int i = 1; i < argc; ++i) {
      if (!Replay(argv[i], NULL)) {
        fprintf(stderr, "Cannot replay scene: %s\n", argv[i]);
        return 1;
      }
    }
    return 0;
  }
  fprintf(stderr, "usage: %s scene.arscene [...] | --ppm output.ppm scene.arscene\n", argv[0]);
  return 1;
}
