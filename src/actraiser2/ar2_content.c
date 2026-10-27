#include "actraiser2/ar2_content.h"
#include "actraiser2/ar2_scenes.h"
#include "actraiser/actraiser_ar2_master_assets.h"
#include "snesrecomp/support/utf8_fs.h"
#include "snesrecomp/support/digest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *s_pack;
static Ar2WorldAtlas *s_atlas;
enum { kHeaderBytes = 48, kRomBytes = 0x180000 };

void Ar2Content_Close(void) {
  free(s_atlas);
  free(s_pack);
  s_atlas = NULL;
  s_pack = NULL;
}

void Ar2Content_Load(void) {
  Ar2Content_Close();
  FILE *file = sr_fopen("game-assets/imports/ar2-world.argame", "rb");
  if (!file) return;
  s_pack = malloc(kHeaderBytes + kRomBytes);
  bool valid = s_pack &&
               fread(s_pack, 1, kHeaderBytes + kRomBytes, file) == kHeaderBytes + kRomBytes &&
               fgetc(file) == EOF && !ferror(file);
  fclose(file);
  static const uint8_t version_size[8] = {1, 0, 0, 0, 0, 0, 0x18, 0};
  char error[160] = "invalid world pack header";
  const uint8_t *rom = NULL;
  size_t size = 0;
  valid = valid && !memcmp(s_pack, "AR2WORLD", 8) && !memcmp(s_pack + 8, version_size, 8) &&
          Ar2MasterAssets_IdentifyRom(s_pack + kHeaderBytes, kRomBytes, &rom, &size, error,
                                      sizeof(error));
  uint8_t digest[32];
  valid =
      valid && sr_support_sha256(rom, size, digest) && !memcmp(digest, s_pack + 16, sizeof(digest));
  if (valid) {
    s_atlas = malloc(sizeof(*s_atlas));
    valid = s_atlas && Ar2WorldAtlas_Decode(s_atlas, rom, size);
  }
  if (!valid) {
    fprintf(stderr, "[ar2-world] ignored: %s\n", error);
    Ar2Content_Close();
  } else {
    fprintf(stderr, "[ar2-world] continent loaded; Industen and Benefic available\n");
  }
}

const Ar2WorldAtlas *Ar2Content_Atlas(void) { return s_atlas; }
const uint8_t *Ar2Content_Rom(size_t *size) {
  if (size) *size = s_atlas ? kRomBytes : 0;
  return s_atlas ? s_pack + kHeaderBytes : NULL;
}
bool Ar2Content_Playable(const Ar2WorldDestination *destination) {
  return destination && Ar2Scenes_IsEntrance(destination->flags_and_scene & 0xff, false);
}
