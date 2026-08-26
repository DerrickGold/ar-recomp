#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { SR_SNES_WRAM_SIZE = 128 * 1024 };

typedef void (*FrameDumpCallback)(uint32_t frame, const uint8_t *wram);

extern FrameDumpCallback g_framedump_callback;

void FrameDump_Init(const char *directory);

#ifdef __cplusplus
}
#endif
