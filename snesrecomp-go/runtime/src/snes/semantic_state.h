#ifndef SNESRECOMP_SEMANTIC_STATE_H
#define SNESRECOMP_SEMANTIC_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Snes Snes;

typedef bool SnesSemanticWriteFunc(
    void *context, const uint8_t *bytes, size_t byte_count);

/** Internal, append-only canonical writer used by explicit semantic schemas.
 * It is deliberately independent from SaveLoadInfo: save-state revisions must
 * not silently redefine a published digest schema. */
typedef struct SnesSemanticWriter {
    SnesSemanticWriteFunc *write;
    void *context;
    bool failed;
} SnesSemanticWriter;

static inline void snes_semantic_write_bytes(
        SnesSemanticWriter *writer, const void *bytes, size_t byte_count) {
    if (writer == NULL || writer->failed || writer->write == NULL ||
        (bytes == NULL && byte_count != 0u)) {
        if (writer != NULL) writer->failed = true;
        return;
    }
    if (byte_count != 0u &&
        !writer->write(writer->context, (const uint8_t *)bytes, byte_count))
        writer->failed = true;
}

static inline void snes_semantic_write_u8(
        SnesSemanticWriter *writer, uint8_t value) {
    snes_semantic_write_bytes(writer, &value, sizeof(value));
}

static inline void snes_semantic_write_bool(
        SnesSemanticWriter *writer, bool value) {
    snes_semantic_write_u8(writer, value ? 1u : 0u);
}

static inline void snes_semantic_write_u16(
        SnesSemanticWriter *writer, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
    snes_semantic_write_bytes(writer, bytes, sizeof(bytes));
}

static inline void snes_semantic_write_i16(
        SnesSemanticWriter *writer, int16_t value) {
    snes_semantic_write_u16(writer, (uint16_t)value);
}

static inline void snes_semantic_write_u32(
        SnesSemanticWriter *writer, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t)value, (uint8_t)(value >> 8),
        (uint8_t)(value >> 16), (uint8_t)(value >> 24),
    };
    snes_semantic_write_bytes(writer, bytes, sizeof(bytes));
}

static inline void snes_semantic_write_i32(
        SnesSemanticWriter *writer, int32_t value) {
    snes_semantic_write_u32(writer, (uint32_t)value);
}

static inline void snes_semantic_write_u64(
        SnesSemanticWriter *writer, uint64_t value) {
    uint8_t bytes[8];
    unsigned index;
    for (index = 0u; index < sizeof(bytes); ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
    snes_semantic_write_bytes(writer, bytes, sizeof(bytes));
}

void snes_semantic_write_f64(SnesSemanticWriter *writer, double value);

/** Write exactly schema 2 of main-thread-owned emulated hardware state. */
bool snes_write_semantic_main_state_v2(
    const Snes *snes, SnesSemanticWriter *writer);
/** Append exactly schema 2 of APU-thread-owned hardware state. The caller
 * owns the APU lock; game extension hooks are never visited. */
bool snes_write_semantic_apu_state_v2(
    const Snes *snes, SnesSemanticWriter *writer);

#ifdef __cplusplus
}
#endif

#endif
