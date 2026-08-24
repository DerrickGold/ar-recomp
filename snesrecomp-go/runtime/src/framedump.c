#include "framedump.h"
#include "crc32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#endif

FrameDumpCallback g_framedump_callback;

static char g_framedump_dir[512];

// Game-agnostic per-frame metadata. Per-game offline tools decode the
// accompanying .bin dump for game-specific fields.
static void write_json(const char *path, uint32_t frame, const uint8_t *wram) {
  uint32_t crc = crc32_compute(wram, kSnesWramSize);
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f,
    "{\n"
    "  \"frame\": %u,\n"
    "  \"wram_size\": %u,\n"
    "  \"crc32_wram\": \"0x%08X\"\n"
    "}\n",
    frame, kSnesWramSize, crc);
  fclose(f);
}

static void write_bin(const char *path, const uint8_t *wram) {
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fwrite(wram, 1, kSnesWramSize, f);
  fclose(f);
}

static void framedump_callback(uint32_t frame, const uint8_t *wram) {
  if (!wram) return;
  char path[768];
  snprintf(path, sizeof(path), "%s/frame_%06u.json", g_framedump_dir, frame);
  write_json(path, frame, wram);
  snprintf(path, sizeof(path), "%s/frame_%06u_wram.bin", g_framedump_dir, frame);
  write_bin(path, wram);
}

void FrameDump_Init(const char *dir) {
  strncpy(g_framedump_dir, dir, sizeof(g_framedump_dir) - 1);
  MKDIR(dir);
  g_framedump_callback = framedump_callback;
  fprintf(stderr, "framedump: writing to '%s'\n", dir);
}
