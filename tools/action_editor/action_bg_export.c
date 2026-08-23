/* action_bg_export — dump every action room's background assets as JSON.
 *
 * Feeds tools/action_editor/build.sh, which bakes the result into the
 * standalone tile-classification editor. Read-only: it opens the ROM, walks
 * each room's asset script through the shared ActionRoomScene decoder, and
 * writes what that decoder already reconstructed. This is the same immutable
 * room authority linked into the game; the exporter owns no ROM interpretation.
 *
 *   cc -I src -I recomp -I snesrecomp-go/runtime/src \
 *      tools/action_editor/action_bg_export.c -o /tmp/action_bg_export
 *   /tmp/action_bg_export ar.sfc rooms.json
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "action/action_room_scene.h"
#include "actraiser_game.h"

/* Assets repeat heavily: rooms in one act inherit the act's character and
 * palette uploads, so the same 16 KiB CHR blob backs many rooms. Emitting one
 * copy per room would multiply the editor's payload several times over for no
 * added information, so identical blobs are pooled and referenced by index. */
enum { kMaxBlobs = 512 };
static struct {
  unsigned char *bytes;
  size_t size;
} g_blobs[kMaxBlobs];
static int g_blob_count;

static int InternBlob(const unsigned char *bytes, size_t size) {
  for (int i = 0; i < g_blob_count; i++)
    if (g_blobs[i].size == size && !memcmp(g_blobs[i].bytes, bytes, size))
      return i;
  if (g_blob_count >= kMaxBlobs) return -1;
  unsigned char *copy = malloc(size);
  if (!copy) return -1;
  memcpy(copy, bytes, size);
  g_blobs[g_blob_count].bytes = copy;
  g_blobs[g_blob_count].size = size;
  return g_blob_count++;
}

static const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void WriteBase64(FILE *out, const unsigned char *bytes, size_t size) {
  for (size_t i = 0; i < size; i += 3) {
    const unsigned a = bytes[i];
    const unsigned b = i + 1 < size ? bytes[i + 1] : 0;
    const unsigned c = i + 2 < size ? bytes[i + 2] : 0;
    const unsigned triple = (a << 16) | (b << 8) | c;
    fputc(kB64[(triple >> 18) & 63], out);
    fputc(kB64[(triple >> 12) & 63], out);
    fputc(i + 1 < size ? kB64[(triple >> 6) & 63] : '=', out);
    fputc(i + 2 < size ? kB64[triple & 63] : '=', out);
  }
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <rom> <out.json>\n", argv[0]);
    return 2;
  }
  FILE *rom_file = fopen(argv[1], "rb");
  if (!rom_file) { perror(argv[1]); return 1; }
  fseek(rom_file, 0, SEEK_END);
  const long rom_size = ftell(rom_file);
  fseek(rom_file, 0, SEEK_SET);
  unsigned char *rom = malloc((size_t)rom_size);
  if (!rom || fread(rom, 1, (size_t)rom_size, rom_file) != (size_t)rom_size) {
    fprintf(stderr, "could not read %s\n", argv[1]);
    return 1;
  }
  fclose(rom_file);

  FILE *out = fopen(argv[2], "wb");
  if (!out) { perror(argv[2]); return 1; }
  uint32_t *native_pixels = malloc(
      kActionRoomSceneFramePixels * sizeof(*native_pixels));
  if (!native_pixels) {
    fprintf(stderr, "could not allocate native-frame oracle\n");
    return 1;
  }
  fprintf(out, "{\n\"schema\":\"actraiser-action-bg-v4\",\n\"rooms\":[\n");

  int rooms = 0, failures = 0, raster_waveform_blob = -1;
  int raster_r4_window_blob = -1;
  for (unsigned group = 1; group <= 7; group++) {
    for (unsigned map = 1; map <= 8; map++) {
      if (!ActRaiser_IsActionMap((uint8_t)group, (uint8_t)map)) continue;
      static ActionRoomScene scene;
      if (!ActionRoomScene_Load(&scene, rom, (size_t)rom_size,
                                (uint8_t)group, (uint8_t)map)) {
        fprintf(stderr, "[export] %u:%u asset script failed\n", group, map);
        failures++;
        continue;
      }
      const ActionRoomSceneFrameRequest golden_request = {
        .camera_x = 0,
        .camera_y = 0,
        .game_frame = 37,
        .animation_phase = -1,
        .page_phase = -1,
      };
      ActionRoomSceneFrameState golden_state;
      if (!ActionRoomScene_BuildFrameState(
              &scene, &golden_request, &golden_state) ||
          !ActionRoomScene_RenderNativeFrame(
              &scene, &golden_state, native_pixels,
              kActionRoomSceneFramePixels)) {
        fprintf(stderr, "[export] %u:%u native oracle failed\n", group, map);
        failures++;
        continue;
      }
      uint32_t native_hash = UINT32_C(2166136261);
      for (size_t i = 0; i < kActionRoomSceneFramePixels; i++) {
        native_hash ^= native_pixels[i];
        native_hash *= UINT32_C(16777619);
      }
      if (rooms) fprintf(out, ",\n");
      if (scene.have_raster_waveform)
        raster_waveform_blob = InternBlob(
            scene.raster_waveform, kActionRoomSceneRasterWaveformBytes);
      if (scene.have_raster_r4_window)
        raster_r4_window_blob = InternBlob(
            scene.raster_r4_window, kActionRoomSceneRasterR4WindowBytes);
      fprintf(out,
              "{\"group\":%u,\"map\":%u,\"chars\":%d,\"extraChars\":%d,"
              "\"palette\":%d,\"rasterWorkspace\":%d,\"bg\":[",
              group, map,
              scene.have_character_bank[0]
                  ? InternBlob(scene.characters,
                               kActionRoomSceneCharacterBytes) : -1,
              scene.have_extra_characters
                  ? InternBlob(scene.extra_characters,
                               kActionRoomSceneExtraCharacterBytes) : -1,
              scene.have_palette
                  ? InternBlob(scene.palette, kActionRoomScenePaletteBytes) : -1,
              scene.have_raster_workspace
                  ? InternBlob(scene.raster_workspace,
                               kActionRoomSceneRasterWorkspaceBytes) : -1);
      for (unsigned bg = 0; bg < 2; bg++) {
        if (bg) fputc(',', out);
        const ActionRoomSceneBg *layer = &scene.bg[bg];
        if (!layer->have_map || !layer->have_metatiles) {
          fprintf(out, "null");
          continue;
        }
        fprintf(out,
                "{\"metatiles\":%d,\"map\":%d,\"pagesWide\":%u,"
                "\"pagesHigh\":%u}",
                InternBlob(layer->metatiles,
                           kActionRoomSceneMetatileBytes),
                InternBlob(layer->map, layer->map_size),
                layer->pages_wide, layer->pages_high);
      }
      fprintf(out, "],\"videoProfile\":%d,\"video\":[",
              scene.have_video_profile ? scene.video_profile_index : -1);
      if (scene.have_video_profile) {
        for (unsigned i = 0; i < kActionRoomSceneVideoProfileBytes; i++) {
          if (i) fputc(',', out);
          fprintf(out, "%u", scene.video_profile[i]);
        }
      }
      fprintf(out, "],\"animation\":");
      if (ActionRoomScene_HasCharacterAnimation(&scene)) {
        fprintf(out,
                "{\"target\":%u,\"stride\":%u,\"phases\":%u,"
                "\"cadence\":%u,\"continuation\":%s}",
                ActionRoomScene_CharacterAnimationTarget(&scene),
                ActionRoomScene_CharacterAnimationStride(&scene),
                ActionRoomScene_CharacterAnimationPhaseCount(&scene),
                ActionRoomScene_CharacterAnimationCadence(&scene),
                ActionRoomScene_CharacterAnimationContinues(&scene)
                    ? "true" : "false");
      } else {
        fprintf(out, "null");
      }
      fprintf(out, ",\"bg2PageCycle\":");
      if (ActionRoomScene_HasBg2PageCycle(&scene))
        fprintf(out,
                "{\"phases\":4,\"cadence\":5,\"order\":[1,2,3,0]}");
      else
        fprintf(out, "null");
      fprintf(out,
              ",\"raster\":%u,\"nativeGolden\":{"
              "\"frame\":37,\"cameraX\":0,\"cameraY\":0,"
              "\"hash\":%u}}",
              (unsigned)scene.raster_preset, native_hash);
      rooms++;
    }
  }

  fprintf(out, "\n],\n\"tileWordMask\":%u,\"bg1Attributes\":%u,"
               "\"bg2Attributes\":%u,\"rasterWaveform\":%d,"
               "\"rasterR4Window\":%d,"
               "\"frameWidth\":%u,\"frameHeight\":%u,\n\"blobs\":[\n",
          kActionRoomSceneTileWordMask,
          kActionRoomSceneBg1AttributeByte,
          kActionRoomSceneBg2AttributeByte, raster_waveform_blob,
          raster_r4_window_blob,
          kActionRoomSceneFrameWidth, kActionRoomSceneFrameHeight);
  size_t blob_bytes = 0;
  for (int i = 0; i < g_blob_count; i++) {
    if (i) fprintf(out, ",\n");
    fputc('"', out);
    WriteBase64(out, g_blobs[i].bytes, g_blobs[i].size);
    fputc('"', out);
    blob_bytes += g_blobs[i].size;
  }
  fprintf(out, "\n]\n}\n");
  fclose(out);
  free(native_pixels);
  fprintf(stderr,
          "[export] %d rooms, %d failures, %d pooled blobs, %zu KiB raw\n",
          rooms, failures, g_blob_count, blob_bytes / 1024);
  return failures ? 1 : 0;
}
