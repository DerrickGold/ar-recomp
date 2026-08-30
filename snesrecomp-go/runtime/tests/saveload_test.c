#include "snes/cpu.h"
#include "snes/saveload.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct MemorySaveLoad {
    SaveLoadInfo base;
    uint8_t bytes[256];
    size_t offset;
} MemorySaveLoad;

static int failures;

static void check(bool condition, const char *message) {
    if (condition) return;
    fprintf(stderr, "runtime saveload failed: %s\n", message);
    ++failures;
}

static void transfer(SaveLoadInfo *base, void *data, size_t size) {
    MemorySaveLoad *state = (MemorySaveLoad *)base;
    if (state->offset + size > sizeof(state->bytes)) {
        base->failed = true;
        return;
    }
    if (base->saving)
        memcpy(state->bytes + state->offset, data, size);
    else
        memcpy(data, state->bytes + state->offset, size);
    state->offset += size;
}

static void test_canonical_primitives(void) {
    static const uint8_t expected[] = {
        0xa5, 0x01, 0x34, 0x12, 0xfe, 0xff,
        0x78, 0x56, 0x34, 0x12,
        0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01
    };
    MemorySaveLoad state = {
        .base = {
            .func = transfer,
            .saving = true,
            .portable = true,
        },
    };
    uint8_t byte = 0xa5u;
    bool flag = true;
    uint16_t word = 0x1234u;
    int16_t signed_word = -2;
    uint32_t dword = 0x12345678u;
    uint64_t qword = UINT64_C(0x0123456789abcdef);
    saveload_u8(&state.base, &byte);
    saveload_bool(&state.base, &flag);
    saveload_u16(&state.base, &word);
    saveload_i16(&state.base, &signed_word);
    saveload_u32(&state.base, &dword);
    saveload_u64(&state.base, &qword);
    check(state.offset == sizeof(expected) &&
              memcmp(state.bytes, expected, sizeof(expected)) == 0,
          "canonical encoding is packed little-endian");

    byte = 0u; flag = false; word = 0u; signed_word = 0;
    dword = 0u; qword = 0u;
    state.offset = 0u;
    state.base.saving = false;
    saveload_u8(&state.base, &byte);
    saveload_bool(&state.base, &flag);
    saveload_u16(&state.base, &word);
    saveload_i16(&state.base, &signed_word);
    saveload_u32(&state.base, &dword);
    saveload_u64(&state.base, &qword);
    check(byte == 0xa5u && flag && word == 0x1234u && signed_word == -2 &&
              dword == 0x12345678u &&
              qword == UINT64_C(0x0123456789abcdef),
          "canonical primitives round trip");
}

static void test_cpu_portable_roundtrip(void) {
    Cpu source = {
        0x1234u, 0x5678u, 0x9abcu, 0x01f0u, 0xdef0u, 0x2468u,
        0x80u, 0x7eu, true, false, true, false, true, false,
        true, false, true
    };
    Cpu restored;
    MemorySaveLoad state = {
        .base = {
            .func = transfer,
            .saving = true,
            .portable = true,
        },
    };
    memset(&restored, 0, sizeof(restored));
    cpu_saveload(&source, &state.base);
    state.offset = 0u;
    state.base.saving = false;
    cpu_saveload(&restored, &state.base);
    check(source.a == restored.a && source.x == restored.x &&
              source.y == restored.y && source.sp == restored.sp &&
              source.pc == restored.pc && source.dp == restored.dp &&
              source.k == restored.k && source.db == restored.db &&
              source.c == restored.c && source.z == restored.z &&
              source.v == restored.v && source.n == restored.n &&
              source.i == restored.i && source.d == restored.d &&
              source.xf == restored.xf && source.mf == restored.mf &&
              source.e == restored.e,
          "CPU state round trips without struct-layout serialization");
}

static void test_snapshot_header_migration(void) {
    const uint32_t magic = 0x52544c53u;
    const uint32_t legacy_version = 9u;
    uint32_t legacy[2] = {magic, legacy_version};
    uint8_t portable_header[8] = {
        0x53, 0x4c, 0x54, 0x52, 0x0a, 0x00, 0x00, 0x00
    };
    uint8_t legacy_header[8];
    bool portable = false;
    memcpy(legacy_header, legacy, sizeof(legacy_header));
    check(saveload_decode_snapshot_header(portable_header, magic, 10u,
                                          legacy_version, &portable) &&
              portable,
          "portable version-10 header is recognized");
    portable = true;
    check(saveload_decode_snapshot_header(legacy_header, magic, 10u,
                                          legacy_version, &portable) &&
              !portable,
          "native-layout version-9 header selects the legacy reader");
    portable_header[4] = 0x0bu;
    check(!saveload_decode_snapshot_header(portable_header, magic, 10u,
                                           legacy_version, &portable),
          "unknown snapshot versions are rejected");
}

int main(void) {
    test_canonical_primitives();
    test_cpu_portable_roundtrip();
    test_snapshot_header_migration();
    if (failures == 0) puts("runtime saveload: PASS");
    return failures == 0 ? 0 : 1;
}
