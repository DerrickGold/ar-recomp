#include "action/action_scene_snapshot.h"
#include "deterministic_hash.h"
#include <stdio.h>
#include <string.h>

static ActionSceneSnapshot s_source, s_decoded, s_unchanged;
static uint8_t s_bytes[kActionSceneSnapshotMaxBytes], s_encoded[kActionSceneSnapshotMaxBytes];
static int s_failures;
#define CHECK(c) do { \
  if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); ++s_failures; } \
} while (0)

static void Word(unsigned word, uint32_t value) {
  for (unsigned byte = 0; byte < 4; ++byte)
    s_bytes[word * 4 + byte] = (uint8_t)(value >> (byte * 8));
}
static void Seal(size_t size) {
  Word(26, 0);
  uint32_t hash = DETERMINISTIC_HASH_FNV1A32_OFFSET;
  for (size_t i = 0; i < size; ++i) hash = DeterministicHash_Fnv1a32Byte(hash, s_bytes[i]);
  Word(26, hash);
}
static void Reject(size_t size) {
  s_unchanged = s_decoded;
  CHECK(!ActionSceneSnapshot_Decode(s_bytes, size, &s_decoded));
  CHECK(!memcmp(&s_decoded, &s_unchanged, sizeof(s_decoded)));
}

int main(int argc, char **argv) {
  s_source.scene.group = 1;
  s_source.scene.map = 1;
  s_source.frame = (ActionRoomSceneFrameRequest){.camera_x = -37, .camera_y = 65535,
      .raster_camera_x = -32768, .game_frame = UINT32_MAX, .animation_phase = -1,
      .page_phase = -1, .have_raster_camera_x = true, .bgsc_override_mask = 3,
      .bgsc_override = {0x40, 0x48}, .raster_entry_frame = true};
  s_source.terrain_profile = 2;
  ActionRoomScene *scene = &s_source.scene;
  scene->have_character_bank[0] = scene->have_character_bank[1] = true;
  scene->have_palette = scene->have_video_profile = true;
  scene->have_extra_characters = scene->have_raster_waveform = true;
  scene->have_raster_workspace = scene->have_raster_mosaic_wave_window = true;
  scene->have_raster_entry_camera_x = true;
  scene->raster_entry_camera_x = 65535;
  scene->raster_effect = kActionRoomRaster_DualBgOpposedWaves;
  for (unsigned bg = 0; bg < 2; ++bg) {
    ActionRoomSceneBg *b = &scene->bg[bg];
    b->have_map = b->have_metatiles = true;
    b->pages_wide = 16;
    b->pages_high = 4;
    b->map_size = sizeof(b->map);
    memset(b->map, 17 + bg, sizeof(b->map));
    memset(b->metatiles, 37 + bg, sizeof(b->metatiles));
  }
  for (unsigned i = 0; i < sizeof(scene->characters); ++i)
    scene->characters[i] = (uint8_t)(i * 13 + (i >> 4));
  memset(scene->extra_characters, 43, sizeof(scene->extra_characters));
  memset(scene->palette, 67, sizeof(scene->palette));
  memset(scene->video_profile, 71, sizeof(scene->video_profile));
  memset(scene->raster_waveform, 89, sizeof(scene->raster_waveform));
  memset(scene->raster_mosaic_wave_window, 97, sizeof(scene->raster_mosaic_wave_window));
  memset(scene->raster_workspace, 101, sizeof(scene->raster_workspace));
  size_t size = 0, encoded_size = 0;
  CHECK(ActionSceneSnapshot_Encode(&s_source, s_bytes, sizeof(s_bytes), &size));
  CHECK(ActionSceneSnapshot_Decode(s_bytes, size, &s_decoded));
  CHECK(s_decoded.frame.camera_x == -37 && s_decoded.frame.camera_y == 65535);
  CHECK(s_decoded.frame.raster_camera_x == -32768 && s_decoded.frame.game_frame == UINT32_MAX);
  CHECK(s_decoded.frame.animation_phase == -1 && s_decoded.frame.page_phase == -1);
  CHECK(ActionSceneSnapshot_Encode(&s_decoded, s_encoded, sizeof(s_encoded), &encoded_size));
  CHECK(encoded_size == size && !memcmp(s_bytes, s_encoded, size));
  /* Recompute checksums after invalid header edits: exercise semantic validation,
   * not only accidental corruption detection. */
  const uint32_t invalid[][2] = {{0, 0}, {1, 2}, {2, 112}, {3, 3}, {4, 255}, {5, 255},
      {6, 512}, {7, 256}, {8, 11}, {9, 65536}, {10, 65536}, {11, (uint32_t)-32769},
      {12, 65536}, {14, 256}, {15, 4}, {16, 16}, {17, 65536}, {18, 17}, {19, 5},
      {20, UINT32_MAX}, {21, 4}, {22, 0}, {23, 0}, {24, 0}, {25, 0}, {27, 1}};
  for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    memcpy(s_bytes, s_encoded, size);
    Word(invalid[i][0], invalid[i][1]);
    Seal(size);
    Reject(size);
  }
  memcpy(s_bytes, s_encoded, size);
  Reject(0);
  Reject(111);
  Reject(size - 1);
  Reject(size + 1);
  Reject(sizeof(s_bytes) + 1);
  s_bytes[size - 1] ^= 1;
  Reject(size);
  memset(s_bytes, 0xCC, sizeof(s_bytes));
  CHECK(!ActionSceneSnapshot_Encode(&s_source, s_bytes, size - 1, &encoded_size));
  CHECK(encoded_size == 0);
  for (unsigned i = 0; i < sizeof(s_bytes); ++i) CHECK(s_bytes[i] == 0xCC);
  s_source.frame.bgsc_override_mask = 4;
  CHECK(!ActionSceneSnapshot_Encode(&s_source, s_bytes, sizeof(s_bytes), &encoded_size));
  if (argc == 2 && !s_failures) {
    /* Also emit a native-authored renderable fixture for the WASM cross-check. */
    s_source.frame = (ActionRoomSceneFrameRequest){.game_frame = 37,
        .animation_phase = -1, .page_phase = -1};
    scene->raster_effect = kActionRoomRaster_None;
    memset(scene->video_profile, 0, sizeof(scene->video_profile));
    scene->video_profile[0] = 3;
    scene->video_profile[6] = 1;
    CHECK(ActionSceneSnapshot_Encode(&s_source, s_encoded, sizeof(s_encoded), &size));
    FILE *file = fopen(argv[1], "wb");
    CHECK(file != NULL);
    if (file) { CHECK(fwrite(s_encoded, 1, size, file) == size); CHECK(fclose(file) == 0); }
  }
  if (!s_failures)
    puts("Snapshot round trip, bounds, version, checksum and atomic rejection passed");
  return s_failures ? 1 : 0;
}
