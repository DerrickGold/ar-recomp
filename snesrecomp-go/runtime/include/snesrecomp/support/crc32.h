/**
 * @file crc32.h
 * @brief Stateless IEEE 802.3 CRC32 helpers.
 * @ingroup sr_support
 */
#ifndef SNESRECOMP_SUPPORT_CRC32_H
#define SNESRECOMP_SUPPORT_CRC32_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @addtogroup sr_support
 *  @{
 */

/** Update an IEEE 802.3 CRC32 using zlib's running-state convention. */
uint32_t crc32_update(uint32_t state, const uint8_t *data, size_t len);
/** Compute an IEEE 802.3 CRC32 for one complete byte span. */
uint32_t crc32_compute(const uint8_t *data, size_t len);

/** @} */

#ifdef __cplusplus
}
#endif

#endif
