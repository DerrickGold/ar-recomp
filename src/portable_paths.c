#include "portable_paths.h"

#include <stddef.h>
#include <sys/stat.h>

#include "constants.h"
#include "snesrecomp/host/launcher.h"

static bool PathExists(const char *path) {
  struct stat path_info;
  return stat(path, &path_info) == 0;
}

bool PortablePaths_IsBundle(void) {
  static const char *const kBundleMarkers[] = {
      "game-assets",
      "portable.txt",
  };
  char candidate_path[kHostPathCapacity];

  const size_t marker_count =
      sizeof(kBundleMarkers) / sizeof(kBundleMarkers[0]);
  for (size_t marker_index = 0; marker_index < marker_count; marker_index++) {
    if (snesrecomp_exe_dir_path(
            kBundleMarkers[marker_index], candidate_path,
            sizeof(candidate_path)) &&
        PathExists(candidate_path)) {
      return true;
    }
  }
  return false;
}
