#ifndef SNESRECOMP_SAVELOAD_H
#define SNESRECOMP_SAVELOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SNESRECOMP_PORTABLE_SAVELOAD 1

typedef struct SaveLoadInfo SaveLoadInfo;
typedef void SaveLoadInfoFunc(SaveLoadInfo *info, void *data, size_t data_size);

struct SaveLoadInfo {
    SaveLoadInfoFunc *func;
    bool saving;
    bool portable;
    /* Digest-only mode: encode emulated semantic state while omitting
     * host-consumed presentation transport such as the DSP PCM ring. */
    bool semantic;
    bool failed;
};

/* Legacy callers leave portable and semantic clear and retain the historical
 * raw-memory callback contract. Snapshot files set portable and use these
 * canonical little-endian primitives instead. */
void saveload_bytes(SaveLoadInfo *info, void *data, size_t size);
void saveload_u8(SaveLoadInfo *info, uint8_t *value);
void saveload_i8(SaveLoadInfo *info, int8_t *value);
void saveload_bool(SaveLoadInfo *info, bool *value);
void saveload_u16(SaveLoadInfo *info, uint16_t *value);
void saveload_i16(SaveLoadInfo *info, int16_t *value);
void saveload_u32(SaveLoadInfo *info, uint32_t *value);
void saveload_i32(SaveLoadInfo *info, int32_t *value);
void saveload_u64(SaveLoadInfo *info, uint64_t *value);
void saveload_f64(SaveLoadInfo *info, double *value);
void saveload_u16_array(SaveLoadInfo *info, uint16_t *values, size_t count);
void saveload_i16_array(SaveLoadInfo *info, int16_t *values, size_t count);
void saveload_u32_array(SaveLoadInfo *info, uint32_t *values, size_t count);
bool saveload_decode_snapshot_header(const uint8_t bytes[8], uint32_t magic,
                                     uint32_t portable_version,
                                     uint32_t legacy_version,
                                     bool *portable);

#endif
