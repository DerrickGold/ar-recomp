#include "snes/msu1.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef MSU1_TEST_BASE
#define MSU1_TEST_BASE "runtime-next-msu1-fixture"
#endif

static int failures;
static unsigned lock_depth;

void RtlApuLock(void) { ++lock_depth; }
void RtlApuUnlock(void) {
    if (lock_depth == 0u) {
        ++failures;
        return;
    }
    --lock_depth;
}

static void check(bool condition, const char *message) {
    if (condition) return;
    fprintf(stderr, "runtime-next MSU-1 failed: %s\n", message);
    ++failures;
}

static void put_u32(FILE *file, uint32_t value) {
    const uint8_t bytes[4] = {
        (uint8_t)value, (uint8_t)(value >> 8),
        (uint8_t)(value >> 16), (uint8_t)(value >> 24)
    };
    (void)fwrite(bytes, 1u, sizeof(bytes), file);
}

static void put_frame(FILE *file, int16_t left, int16_t right) {
    const uint8_t bytes[4] = {
        (uint8_t)left, (uint8_t)((uint16_t)left >> 8),
        (uint8_t)right, (uint8_t)((uint16_t)right >> 8)
    };
    (void)fwrite(bytes, 1u, sizeof(bytes), file);
}

static bool create_fixture(char *data_path, size_t data_capacity,
                           char *track_path, size_t track_capacity) {
    if (snprintf(data_path, data_capacity, "%s.msu", MSU1_TEST_BASE) >=
            (int)data_capacity ||
        snprintf(track_path, track_capacity, "%s-1.pcm", MSU1_TEST_BASE) >=
            (int)track_capacity) {
        return false;
    }
    FILE *data = fopen(data_path, "wb");
    if (data == NULL) return false;
    const uint8_t payload[] = {0x10, 0x20, 0x30, 0x40, 0x50};
    const bool data_ok = fwrite(payload, 1u, sizeof(payload), data) ==
                         sizeof(payload) && fclose(data) == 0;
    if (!data_ok) return false;

    FILE *track = fopen(track_path, "wb");
    if (track == NULL) return false;
    const bool magic_ok = fwrite("MSU1", 1u, 4u, track) == 4u;
    put_u32(track, 1u);
    put_frame(track, 1000, -1000);
    put_frame(track, 2000, -2000);
    put_frame(track, 3000, -3000);
    return magic_ok && fclose(track) == 0;
}

static void select_track(unsigned track) {
    msu1_write(0x2004u, (uint8_t)track);
    msu1_write(0x2005u, (uint8_t)(track >> 8));
}

static void test_registers_and_data(void) {
    check(msu1_configure_base(MSU1_TEST_BASE), "explicit base configuration");
    check(msu1_enabled(), "configured device enabled");
    const char identity[] = "S-MSU1";
    for (unsigned index = 0; index < 6u; ++index) {
        check(msu1_read((uint16_t)(0x2002u + index)) ==
                  (uint8_t)identity[index], "identity register");
    }
    msu1_write(0x2000u, 2u);
    msu1_write(0x2001u, 0u);
    msu1_write(0x2002u, 0u);
    msu1_write(0x2003u, 0u);
    check(msu1_read(0x2001u) == 0x30u && msu1_read(0x2001u) == 0x40u,
          "data seek and auto-increment");
    check(lock_depth == 0u, "register lock calls remain balanced");
}

static void test_playback_looping_and_resampling(void) {
    select_track(1u);
    check((msu1_read(0x2000u) & 0x08u) == 0u, "track opened");
    msu1_write(0x2007u, 1u);
    int16_t first[4] = {0};
    msu1_mix(first, 2, 44100);
    check(first[0] == 1000 && first[1] == -1000 &&
          first[2] == 2000 && first[3] == -2000,
          "native-rate PCM samples are exact");
    check((msu1_read(0x2000u) & 0x10u) != 0u,
          "last buffered frame keeps playing state");
    int16_t last[2] = {32000, -32000};
    msu1_mix(last, 1, 44100);
    check(last[0] == 32767 && last[1] == -32768,
          "mix saturates instead of wrapping");
    check((msu1_read(0x2000u) & 0x10u) == 0u, "non-looping EOF stops track");

    select_track(1u);
    msu1_write(0x2007u, 3u);
    int16_t looped[8] = {0};
    msu1_mix(looped, 4, 44100);
    check(looped[0] == 1000 && looped[2] == 2000 && looped[4] == 3000 &&
          looped[6] == 2000, "repeat resumes at header loop frame");
    check((msu1_read(0x2000u) & 0x30u) == 0x30u,
          "status exposes play and repeat bits");

    select_track(1u);
    msu1_write(0x2007u, 1u);
    int16_t downsampled[4] = {0};
    msu1_mix(downsampled, 2, 22050);
    check(downsampled[0] == 1000 && downsampled[2] == 3000,
          "source phase is independent of output rate");

    select_track(99u);
    check((msu1_read(0x2000u) & 0x08u) != 0u,
          "missing selection raises audio error");
}

int main(void) {
    char data_path[1200];
    char track_path[1200];
    check(create_fixture(data_path, sizeof(data_path),
                         track_path, sizeof(track_path)), "create fixture");
    msu1_shutdown();
    check(!msu1_enabled() && msu1_read(0x2000u) == 0u,
          "disabled device is inert");
    test_registers_and_data();
    test_playback_looping_and_resampling();
    msu1_shutdown();
    check(remove(data_path) == 0 && remove(track_path) == 0,
          "remove fixture");
    if (failures != 0) {
        fprintf(stderr, "runtime-next MSU-1: %d failure(s)\n", failures);
        return 1;
    }
    puts("runtime-next MSU-1: PASS");
    return 0;
}
