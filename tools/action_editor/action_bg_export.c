/* action_bg_export — dump every action room's background assets as JSON.
 *
 * Feeds tools/action_editor/build.sh, which bakes the result into the
 * standalone tile-classification editor. Read-only: it opens the ROM, walks
 * each room's asset script through the SHIPPED decoder, and writes what that
 * decoder already reconstructed.
 *
 * It includes diorama_rom_backdrop.c rather than linking it, because the
 * asset-script walker and its ActionBgAssets result are file-static there.
 * Re-implementing the walk was the alternative and is exactly the kind of
 * re-derivation that goes subtly wrong: the $ECFF tile-word mask, the per-BG
 * attribute merge that maps BG1 definitions to chars $000-$0FF and BG2 to
 * $100-$1FF, and asset inheritance within an act are all easy to get almost
 * right. Using the shipped decoder means the editor sees exactly what the
 * game sees.
 *
 *   cc -I src -I recomp -I snesrecomp-go/runtime/src \
 *      tools/action_editor/action_bg_export.c -o /tmp/action_bg_export
 *   /tmp/action_bg_export ar.sfc rooms.json
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diorama/diorama_rom_backdrop.c"

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
  fprintf(out, "{\n\"schema\":\"actraiser-action-bg-v1\",\n\"rooms\":[\n");

  int rooms = 0, failures = 0;
  for (unsigned group = 1; group <= 7; group++) {
    for (unsigned map = 1; map <= 8; map++) {
      if (!ActRaiser_IsActionMap((uint8)group, (uint8)map)) continue;
      static ActionBgAssets assets;
      if (!LoadAssetsForRoom(rom, (size_t)rom_size, (uint8_t)group,
                             (uint8_t)map, &assets)) {
        fprintf(stderr, "[export] %u:%u asset script failed\n", group, map);
        failures++;
        continue;
      }
      if (rooms) fprintf(out, ",\n");
      fprintf(out,
              "{\"group\":%u,\"map\":%u,\"chars\":%d,\"extraChars\":%d,"
              "\"palette\":%d,\"bg\":[",
              group, map,
              assets.have_chars[0]
                  ? InternBlob(assets.chars, kChrVramBytes) : -1,
              assets.have_extra_chars
                  ? InternBlob(assets.extra_chars, kChrBytes) : -1,
              assets.have_palette
                  ? InternBlob(assets.palette, kPaletteBytes) : -1);
      for (unsigned bg = 0; bg < 2; bg++) {
        if (bg) fputc(',', out);
        if (!assets.have_map[bg] || !assets.have_metatiles[bg]) {
          fprintf(out, "null");
          continue;
        }
        fprintf(out,
                "{\"metatiles\":%d,\"map\":%d,\"pagesWide\":%u,"
                "\"pagesHigh\":%u}",
                InternBlob(assets.metatiles[bg], kMetatileBytes),
                InternBlob(assets.map[bg], assets.map_size[bg]),
                assets.map_pages_wide[bg], assets.map_pages_high[bg]);
      }
      fprintf(out, "]}");
      rooms++;
    }
  }

  fprintf(out, "\n],\n\"tileWordMask\":%u,\"bg1Attributes\":%u,"
               "\"bg2Attributes\":%u,\n\"blobs\":[\n",
          kActionBgTileWordMask, kActionBg1Attributes, kActionBg2Attributes);
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
  fprintf(stderr,
          "[export] %d rooms, %d failures, %d pooled blobs, %zu KiB raw\n",
          rooms, failures, g_blob_count, blob_bytes / 1024);
  return failures ? 1 : 0;
}
