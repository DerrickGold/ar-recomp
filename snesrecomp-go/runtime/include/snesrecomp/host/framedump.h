/**
 * @file framedump.h
 * @brief Optional per-frame WRAM capture callback.
 * @ingroup sr_host
 */
#ifndef SNESRECOMP_HOST_FRAMEDUMP_H
#define SNESRECOMP_HOST_FRAMEDUMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @addtogroup sr_host
 *  @{
 */

enum { SR_SNES_WRAM_SIZE = 128 * 1024 };

/**
 * @brief Receives a callback-lifetime view of the current 128 KiB WRAM.
 * @param[in] frame Monotonic emulated frame number.
 * @param[in] wram Immutable WRAM valid only until the callback returns.
 */
typedef void (*FrameDumpCallback)(uint32_t frame, const uint8_t *wram);

extern FrameDumpCallback g_framedump_callback;

void FrameDump_Init(const char *directory);

/** @} */

#ifdef __cplusplus
}
#endif

#endif
