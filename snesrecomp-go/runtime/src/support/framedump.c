#include "snesrecomp/host/framedump.h"
#include "snesrecomp/support/crc32.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
static int create_directory(const char *path) {
    return _mkdir(path);
}
#else
static int create_directory(const char *path) {
    return mkdir(path, 0755);
}
#endif

FrameDumpCallback g_framedump_callback = NULL;

static char s_output_directory[512];

static void write_metadata(const char *path, uint32_t frame,
                           const uint8_t *wram) {
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return;
    }
    const uint32_t checksum = crc32_compute(wram, SR_SNES_WRAM_SIZE);
    (void)fprintf(file,
                  "{\n"
                  "  \"frame\": %u,\n"
                  "  \"wram_size\": %u,\n"
                  "  \"crc32_wram\": \"0x%08X\"\n"
                  "}\n",
                  frame, (unsigned)SR_SNES_WRAM_SIZE, checksum);
    (void)fclose(file);
}

static void write_wram(const char *path, const uint8_t *wram) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return;
    }
    (void)fwrite(wram, 1u, SR_SNES_WRAM_SIZE, file);
    (void)fclose(file);
}

static void dump_frame(uint32_t frame, const uint8_t *wram) {
    if (wram == NULL) {
        return;
    }
    char path[768];
    const int metadata_length = snprintf(
        path, sizeof(path), "%s/frame_%06u.json", s_output_directory, frame);
    if (metadata_length > 0 && (size_t)metadata_length < sizeof(path)) {
        write_metadata(path, frame, wram);
    }
    const int binary_length = snprintf(
        path, sizeof(path), "%s/frame_%06u_wram.bin", s_output_directory, frame);
    if (binary_length > 0 && (size_t)binary_length < sizeof(path)) {
        write_wram(path, wram);
    }
}

void FrameDump_Init(const char *directory) {
    g_framedump_callback = NULL;
    if (directory == NULL || directory[0] == '\0') {
        return;
    }
    const int length = snprintf(
        s_output_directory, sizeof(s_output_directory), "%s", directory);
    if (length <= 0 || (size_t)length >= sizeof(s_output_directory)) {
        s_output_directory[0] = '\0';
        return;
    }
    if (create_directory(s_output_directory) != 0 && errno != EEXIST) {
        s_output_directory[0] = '\0';
        return;
    }
    g_framedump_callback = dump_frame;
}
