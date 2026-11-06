#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/generated_support.h"
#include "snesrecomp/game/runtime.h"
#include "snes/cpu.h"
#include "snes/snes.h"
#include "runner_internal.h"
#include "runner_state_internal.h"
#include "snesrecomp/game/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const DispatchEntry g_dispatch_table[] = {{0}};
const unsigned g_dispatch_table_count = 0;
static unsigned failures;
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "snapshot load %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static const char *const kSaved = "snapshot_load_test_saved.sav";
static const char *const kLive = "snapshot_load_test_live.sav";
static const char *const kAfter = "snapshot_load_test_after.sav";
static const char *const kBad = "snapshot_load_test_bad.sav";

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    uint8_t *bytes = NULL;
    long length;
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) > 0 &&
        fseek(file, 0, SEEK_SET) == 0) {
        bytes = malloc((size_t)length);
        if (bytes != NULL && fread(bytes, 1u, (size_t)length, file) !=
                (size_t)length) {
            free(bytes);
            bytes = NULL;
        }
        *size = (size_t)length;
    }
    fclose(file);
    return bytes;
}

static void write_file(const char *path, const uint8_t *bytes, size_t size) {
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    if (file == NULL) return;
    CHECK(fwrite(bytes, 1u, size, file) == size);
    fclose(file);
}

/* Two different guest states: WRAM plus PPU registers through the B bus. */
static void set_state(uint8_t *wram, uint8_t seed) {
    for (size_t index = 0; index < 0x20000u; index += 97u)
        wram[index] = (uint8_t)(index * 7u + seed);
    snes_write(g_snes, 0x002100u, (uint8_t)(seed & 0x0fu));
    snes_write(g_snes, 0x002105u, (uint8_t)(seed & 0x07u));
    snes_write(g_snes, 0x00210du, seed);
    snes_write(g_snes, 0x00210du, 0x01u);
}

/* A rejected file leaves every serialized byte of the live state unchanged. */
static void check_rejected(const uint8_t *bytes, size_t size,
                           const uint8_t *live, size_t live_size) {
    uint8_t *after;
    bool emulation = g_snes->cpu->e;
    size_t after_size = 0u;
    write_file(kBad, bytes, size);
    CHECK(!RtlLoadSnapshot(kBad));
    CHECK(g_snes->cpu->e == emulation);
    RtlSaveSnapshot(kAfter);
    after = read_file(kAfter, &after_size);
    CHECK(after != NULL && after_size == live_size &&
          memcmp(after, live, live_size) == 0);
    free(after);
}

int main(void) {
    uint8_t *wram = calloc(1, 0x20000);
    uint8_t *saved = NULL, *live = NULL, *corrupt = NULL;
    size_t saved_size = 0u, live_size = 0u;
    g_snes = snes_init(wram);
    if (g_snes == NULL) { free(wram); return 1; }
    set_state(wram, 0x31u);
    RtlSaveSnapshot(kSaved);
    set_state(wram, 0xc4u);
    saved = read_file(kSaved, &saved_size);
    CHECK(saved != NULL && saved_size > 64u);
    if (saved == NULL || saved_size <= 64u) goto done;

    /* A bad PPU section header fails after earlier sections were applied. */
    corrupt = malloc(saved_size);
    CHECK(corrupt != NULL);
    if (corrupt != NULL) {
        size_t index;
        memcpy(corrupt, saved, saved_size);
        for (index = 8u; index + 4u <= saved_size; ++index) {
            if (memcmp(corrupt + index, "PPU1", 4u) == 0) break;
        }
        CHECK(index + 4u <= saved_size);
        if (index + 4u <= saved_size) {
            corrupt[index] ^= 0xffu;
        } else {
            free(corrupt);
            corrupt = NULL;
        }
    }

    /* Rollback preserves either live CPU mode; an ordinary load intentionally
     * selects native mode, which must not leak into rejected loads. */
    for (unsigned emulation = 0u; emulation < 2u; ++emulation) {
        g_snes->cpu->e = emulation != 0u;
        RtlSaveSnapshot(kLive);
        free(live);
        live = read_file(kLive, &live_size);
        CHECK(live != NULL);
        if (live == NULL) goto done;
        CHECK(saved_size != live_size || memcmp(saved, live, live_size) != 0);

        /* Truncation inside the first section, mid-stream and at the end. */
        check_rejected(saved, 12u, live, live_size);
        check_rejected(saved, saved_size / 2u, live, live_size);
        check_rejected(saved, saved_size - 1u, live, live_size);
        if (corrupt != NULL) check_rejected(corrupt, saved_size, live, live_size);
    }

    /* A complete snapshot still loads and reproduces its saved state. */
    CHECK(RtlLoadSnapshot(kSaved));
    CHECK(!g_snes->cpu->e);
    RtlSaveSnapshot(kAfter);
    {
        size_t after_size = 0u;
        uint8_t *after = read_file(kAfter, &after_size);
        CHECK(after != NULL && after_size == saved_size &&
              memcmp(after, saved, saved_size) == 0);
        free(after);
    }

done:
    remove(kSaved); remove(kLive); remove(kAfter); remove(kBad);
    free(saved); free(live); free(corrupt);
    snes_free(g_snes); g_snes = NULL; free(wram);
    if (failures == 0u) printf("snapshot load tests: pass\n");
    return failures ? 1 : 0;
}
