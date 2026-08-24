#ifndef DETERMINISTIC_HASH_H
#define DETERMINISTIC_HASH_H

#include <stddef.h>
#include <stdint.h>

#define DETERMINISTIC_HASH_FNV1A32_OFFSET UINT32_C(2166136261)
#define DETERMINISTIC_HASH_FNV1A64_OFFSET UINT64_C(14695981039346656037)

static inline uint32_t DeterministicHash_Mix32(uint32_t value) {
  value ^= value >> 16;
  value *= UINT32_C(0x7FEB352D);
  value ^= value >> 15;
  value *= UINT32_C(0x846CA68B);
  return value ^ (value >> 16);
}

static inline uint32_t DeterministicHash_Fnv1a32Byte(
    uint32_t hash, uint8_t value) {
  return (hash ^ value) * UINT32_C(16777619);
}

/* Some persisted tool data historically mixes whole 32-bit pixels per step. */
static inline uint32_t DeterministicHash_Fnv1a32Word(
    uint32_t hash, uint32_t value) {
  return (hash ^ value) * UINT32_C(16777619);
}

static inline uint64_t DeterministicHash_Fnv1a64Byte(
    uint64_t hash, uint8_t value) {
  return (hash ^ value) * UINT64_C(1099511628211);
}

static inline uint64_t DeterministicHash_Fnv1a64(
    uint64_t hash, const void *data, size_t size) {
  const uint8_t *bytes = (const uint8_t *)data;
  for (size_t i = 0; i < size; i++)
    hash = DeterministicHash_Fnv1a64Byte(hash, bytes[i]);
  return hash;
}

#endif
