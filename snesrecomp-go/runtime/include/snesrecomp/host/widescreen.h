/**
 * @file widescreen.h
 * @brief Legacy host presentation adapter for a margin-extended surface.
 * @ingroup sr_host
 */
#ifndef SNESRECOMP_HOST_WIDESCREEN_H
#define SNESRECOMP_HOST_WIDESCREEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @addtogroup sr_host
 *  @{
 */

extern bool g_ws_active;
extern int g_ws_extra;

enum { kWsExtraMax = 120 };

void RtlWidescreenPresent(uint8_t *destination, size_t destination_pitch,
                          const uint8_t *source, int width, int height);

/** @} */

#ifdef __cplusplus
}
#endif

#endif
