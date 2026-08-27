#include "snes/color_lut.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static int failures;

static void check(bool condition, const char *message) {
    if (condition) return;
    fprintf(stderr, "runtime color LUT failed: %s\n", message);
    ++failures;
}

int main(void) {
    const uint32_t source[] = {
        0x00000000u, 0xffffffffu, 0xaae03020u, 0x7f2080f0u
    };
    uint32_t output[4] = {0};
    check(snes_color_lut_configure(kSnesColorModelRaw) == 0 &&
          !snes_color_lut_active(), "raw model disabled");
    snes_color_lut_map(source, output, 4u);
    for (unsigned index = 0; index < 4u; ++index) {
        check(output[index] == source[index], "raw map is identity");
    }

    check(snes_color_lut_configure(kSnesColorModelCrt) == 1 &&
          snes_color_lut_active(), "CRT model enabled");
    snes_color_lut_map(source, output, 4u);
    check(output[0] == 0u, "black fixed point");
    check((output[1] & 0x00ffffffu) == 0x00ffffffu, "white fixed point");
    check((output[2] & 0xff000000u) == 0xaa000000u &&
          output[2] != source[2], "graded color preserves alpha");

    uint32_t in_place = 0x5a40a020u;
    snes_color_lut_map(&in_place, &in_place, 1u);
    check((in_place & 0xff000000u) == 0x5a000000u,
          "in-place mapping supported");
    const uint32_t crt_color = output[3];
    check(snes_color_lut_configure(kSnesColorModelTrinitron) == 1,
          "Trinitron model enabled");
    snes_color_lut_map(source + 3, output + 3, 1u);
    check(output[3] != crt_color, "screen models build distinct transforms");

    if (failures != 0) {
        fprintf(stderr, "runtime color LUT: %d failure(s)\n", failures);
        return 1;
    }
    puts("runtime color LUT: PASS");
    return 0;
}
