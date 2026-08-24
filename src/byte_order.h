#ifndef BYTE_ORDER_H
#define BYTE_ORDER_H

#include <stdint.h>

static inline uint16_t ByteOrder_ReadLe16(const uint8_t *bytes) {
  return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static inline uint32_t ByteOrder_ReadLe32(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
      ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static inline void ByteOrder_WriteLe16(uint8_t *bytes, uint16_t value) {
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8);
}

static inline void ByteOrder_WriteLe32(uint8_t *bytes, uint32_t value) {
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8);
  bytes[2] = (uint8_t)(value >> 16);
  bytes[3] = (uint8_t)(value >> 24);
}

static inline uint16_t ByteOrder_ReadBe16(const uint8_t *bytes) {
  return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static inline void ByteOrder_WriteBe32(uint8_t *bytes, uint32_t value) {
  bytes[0] = (uint8_t)(value >> 24);
  bytes[1] = (uint8_t)(value >> 16);
  bytes[2] = (uint8_t)(value >> 8);
  bytes[3] = (uint8_t)value;
}

#endif
