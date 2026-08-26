#include "framedump.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "runtime-next framedump contract failed: %s\n", message);
        ++failures;
    }
}

int main(void) {
    static const char directory[] = "runtime_next_framedump_output";
    static const char metadata_path[] =
        "runtime_next_framedump_output/frame_000042.json";
    static const char binary_path[] =
        "runtime_next_framedump_output/frame_000042_wram.bin";

    FrameDump_Init(NULL);
    check(g_framedump_callback == NULL, "null directory disables dumping");

    uint8_t *wram = (uint8_t *)malloc(SR_SNES_WRAM_SIZE);
    check(wram != NULL, "WRAM fixture allocation");
    if (wram == NULL) {
        return 1;
    }
    for (size_t index = 0; index < SR_SNES_WRAM_SIZE; ++index) {
        wram[index] = (uint8_t)index;
    }

    FrameDump_Init(directory);
    check(g_framedump_callback != NULL, "valid directory enables dumping");
    if (g_framedump_callback != NULL) {
        g_framedump_callback(42u, wram);
    }

    FILE *metadata = fopen(metadata_path, "rb");
    check(metadata != NULL, "metadata file created");
    if (metadata != NULL) {
        char content[256] = {0};
        const size_t length = fread(content, 1u, sizeof(content) - 1u, metadata);
        (void)fclose(metadata);
        content[length] = '\0';
        check(strstr(content, "\"frame\": 42") != NULL, "metadata frame");
        check(strstr(content, "\"wram_size\": 131072") != NULL,
              "metadata WRAM size");
        check(strstr(content, "\"crc32_wram\": \"0x") != NULL,
              "metadata checksum");
    }

    FILE *binary = fopen(binary_path, "rb");
    check(binary != NULL, "WRAM file created");
    if (binary != NULL) {
        check(fseek(binary, 0, SEEK_END) == 0, "seek WRAM file");
        check(ftell(binary) == SR_SNES_WRAM_SIZE, "WRAM file size");
        (void)fclose(binary);
    }

    free(wram);
    (void)remove(metadata_path);
    (void)remove(binary_path);
    return failures == 0 ? 0 : 1;
}
