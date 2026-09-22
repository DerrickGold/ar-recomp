#ifndef SNESRECOMP_SUPPORT_DIGEST_H
#define SNESRECOMP_SUPPORT_DIGEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Portable one-shot hashing for host artifact identities. No runner handle,
 * private context layout, allocator or game-specific schema is exposed.
 * NULL data is allowed only for length zero. Invalid input leaves out intact. */
bool sr_support_sha256(const void *data, size_t length, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
#endif
