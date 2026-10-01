#include "diorama/diorama_layer_manifest.h"
#include "app/user_data_dir.h"
#include "host/atomic_replace.h"
#include "snesrecomp/support/utf8_fs.h"

#include "support/test_assert.h"
#ifndef _WIN32
#include <unistd.h>
#endif

static const char kRoot[] = "diorama-manifest-test";
static const char kPath[] = "diorama-manifest-test/diorama-layers.ini";
static const char kTemporary[] = "diorama-manifest-test/diorama-layers.ini.tmp";
static bool s_fail_replace;

/* Isolate the production file boundary from the checkout and user data. */
char *UserDataFile(char *buffer, size_t size, const char *leaf) {
  const int written = snprintf(buffer, size, "%s/%s", kRoot, leaf);
  assert(written >= 0 && (size_t)written < size);
  return buffer;
}

bool AtomicReplaceFile(const char *temporary, const char *path) {
  return !s_fail_replace && sr_replace_file(temporary, path) != 0;
}

static void RemoveDirectory(const char *path) {
#ifdef _WIN32
  wchar_t *wide = sr_win_path(path);
  assert(wide && !_wrmdir(wide));
  free(wide);
#else
  assert(!rmdir(path));
#endif
}

static void Write(const void *data, size_t size) {
  FILE *file = sr_fopen(kPath, "wb");
  assert(file && fwrite(data, 1, size, file) == size);
  assert(!fclose(file));
}

static char *Read(size_t *size) {
  FILE *file = sr_fopen(kPath, "rb");
  assert(file && !fseek(file, 0, SEEK_END));
  const long length = ftell(file);
  assert(length >= 0 && !fseek(file, 0, SEEK_SET));
  char *text = malloc((size_t)length + 1);
  assert(text && fread(text, 1, (size_t)length, file) == (size_t)length);
  assert(!fclose(file));
  text[length] = 0;
  *size = (size_t)length;
  return text;
}

static void ExpectUnchanged(const void *expected, size_t size) {
  size_t actual_size;
  char *actual = Read(&actual_size);
  assert(size == actual_size && !memcmp(expected, actual, size));
  free(actual);
}

int main(void) {
  assert(!sr_mkdir(kRoot));
  assert(!DioramaLayerManifest_Load());
  assert(DioramaLayerManifest_Table()->count == 0);
  const char original[] = "# Authored notes stay here.\n[foreign]\nkeep = exact text\n\n"
                          "[layers:01:00]\n# Cliff face\nbg2hi = rake:0.29\n"
                          "[layers:04:01\nbg2hi = alpha:10\n"
                          "[layers:02:00]\nbg1 = alpha:200\n";
  Write(original, sizeof(original) - 1);
  assert(DioramaLayerManifest_Load());
  const DioramaLayerOrderTable *view = DioramaLayerManifest_Table();
  assert(view->count == 2);
  const DioramaRoomOverride *room = DioramaLayerOrder_Find(view, 1, 0);
  assert(room && room->planes[kDioramaPlane_Bg2Hi].set_rake);
  /* A malformed section must not leak its edits into the previous room. */
  assert(!room->planes[kDioramaPlane_Bg2Hi].set_alpha);
  DioramaRoomOverride *edit = DioramaLayerOrder_FindOrAdd(DioramaLayerManifest_Edit(), 1, 0);
  assert(edit);
  edit->planes[kDioramaPlane_Bg2Hi].set_z = true;
  edit->planes[kDioramaPlane_Bg2Hi].z = 0.5f;
  assert(DioramaLayerManifest_Save());
  size_t saved_size;
  char *saved = Read(&saved_size);
  assert(strstr(saved, "# Authored notes stay here.\n[foreign]\nkeep = exact text\n\n"));
  assert(strstr(saved, "# Cliff face\n"));
  assert(DioramaLayerManifest_Load());
  room = DioramaLayerOrder_Find(view, 1, 0);
  assert(room && room->planes[kDioramaPlane_Bg2Hi].z == 0.5f);
  assert(sr_access(kTemporary, 0) != 0);

  /* Temp-open and final-replace failures leave all hand-authored bytes intact. */
  assert(!sr_mkdir(kTemporary));
  assert(!DioramaLayerManifest_Save());
  ExpectUnchanged(saved, saved_size);
  RemoveDirectory(kTemporary);
  s_fail_replace = true;
  assert(!DioramaLayerManifest_Save());
  ExpectUnchanged(saved, saved_size);
  assert(sr_access(kTemporary, 0) != 0);
  s_fail_replace = false;
  free(saved);

  /* Large valid tile patches must load and save beyond the old 1 MiB read cap. */
  const size_t large_capacity = 16000u * 128;
  char *large = malloc(large_capacity);
  assert(large);
  size_t large_size = (size_t)snprintf(large, large_capacity,
      "# Large authored patch\n[layers:02:08]\n");
  for (int i = 0; i < 15000; i++) {
    const int written = snprintf(large + large_size, large_capacity - large_size,
        "bg1-stamp:us+jp+eu = cell:%d,%d metatile:23 "
        "words:0010,4011,8012,E013 bands:0,1,2,1\n", i % 128, i / 128);
    assert(written > 0 && (size_t)written < large_capacity - large_size);
    large_size += (size_t)written;
  }
  assert(large_size > (1u << 20));
  Write(large, large_size);
  assert(DioramaLayerManifest_Load());
  room = DioramaLayerOrder_Find(view, 2, 8);
  assert(room && room->stamp_layers[0].count == 15000);
  const DioramaRoomOverride *resolved = DioramaLayerManifest_TerrainRoom(room, 0);
  assert(resolved && resolved->stamp_layers[0].count == 15000);
  assert(resolved->stamp_layers[0].occupied_bounds.set &&
      resolved->stamp_layers[0].occupied_bounds.x0 == 0 &&
      resolved->stamp_layers[0].occupied_bounds.x1 == 128 &&
      resolved->stamp_layers[0].occupied_bounds.y1 == 118);
  const DioramaTileStamp *last = DioramaLayerOrder_StampAt(
      &resolved->stamp_layers[0], 14999 % 128, 14999 / 128);
  assert(last && last->words[0] == 0x10);
  for (unsigned repeat = 0; repeat < 100; ++repeat) {
    assert(DioramaLayerManifest_TerrainRoom(room, 0) == resolved);
    assert(DioramaLayerOrder_StampAt(&resolved->stamp_layers[0],
        14999 % 128, 14999 / 128) == last);
  }
  edit = DioramaLayerOrder_FindOrAdd(DioramaLayerManifest_Edit(), 2, 8);
  edit->stamp_layers[0].cells[14999].terrain_mask = 1;
  edit->stamp_layers[0].cells[14999].words[0] = 0x25;
  resolved = DioramaLayerManifest_TerrainRoom(edit, 0);
  last = DioramaLayerOrder_StampAt(&resolved->stamp_layers[0],
      14999 % 128, 14999 / 128);
  assert(last && last->words[0] == 0x25);
  resolved = DioramaLayerManifest_TerrainRoom(edit, 1);
  assert(resolved && resolved->stamp_layers[0].count == 14999);
  assert(!DioramaLayerOrder_StampAt(&resolved->stamp_layers[0],
      14999 % 128, 14999 / 128));
  assert(!DioramaLayerManifest_TerrainRoom(NULL, 0));
  assert(!DioramaLayerManifest_TerrainRoom(edit, 99));
  assert(DioramaLayerManifest_TerrainRoom(edit, 0)->stamp_layers[0].count == 15000);
  assert(DioramaLayerManifest_Save());
  assert(DioramaLayerManifest_Load());
  room = DioramaLayerOrder_Find(view, 2, 8);
  assert(room && room->stamp_layers[0].count == 15000);
  saved = Read(&saved_size);
  assert(strstr(saved, "# Large authored patch\n"));
  free(saved);
  free(large);
  /* Non-text input must still never become a destructive successful save. */
  const char binary[] = "# prefix\0hand-authored suffix\n";
  Write(binary, sizeof(binary) - 1);
  assert(!DioramaLayerManifest_Save());
  ExpectUnchanged(binary, sizeof(binary) - 1);

  /* First save creates a documented manifest; repeated saves/reloads are stable. */
  assert(!sr_remove(kPath));
  assert(DioramaLayerManifest_Save());
  saved = Read(&saved_size);
  assert(strstr(saved, "# Diorama per-room layer overrides."));
  assert(DioramaLayerManifest_Load() && DioramaLayerManifest_Save());
  ExpectUnchanged(saved, saved_size);
  free(saved);
  assert(!sr_remove(kPath));
  RemoveDirectory(kRoot);
  puts("layer manifest: load/edit/merge round trips and failed-save preservation passed");
  return 0;
}
