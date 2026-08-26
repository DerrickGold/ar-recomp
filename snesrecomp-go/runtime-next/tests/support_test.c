#include "recomp_hw.h"
#include "snes/cpu.h"
#include "snes/saveload.h"
#include "widescreen.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct Snes Snes;
struct Snes {
    uint8_t marker;
};

Snes *g_snes = NULL;

static int failures = 0;
static unsigned save_calls = 0u;
static void *save_pointer = NULL;
static size_t save_size = 0u;
static uint16_t last_register = 0u;
static uint8_t last_value = 0u;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "runtime-next support contract failed: %s\n", message);
        ++failures;
    }
}

static void capture_cpu(SaveLoadInfo *info, void *data, size_t data_size) {
    (void)info;
    ++save_calls;
    save_pointer = data;
    save_size = data_size;
}

void snes_writeReg(Snes *snes, uint16_t address, uint8_t value) {
    check(snes == g_snes, "register write used the selected SNES instance");
    last_register = address;
    last_value = value;
}

uint8_t snes_readReg(Snes *snes, uint16_t address) {
    check(snes == g_snes, "register read used the selected SNES instance");
    last_register = address;
    return 0xA7u;
}

static void test_cpu(void) {
    Cpu *cpu = cpu_init();
    check(cpu != NULL, "CPU allocation");
    if (cpu == NULL) {
        return;
    }
    cpu_reset(cpu);
    check(cpu->sp == 0x0100u && cpu->e && cpu->mf && cpu->xf && cpu->i,
          "CPU reset state");
    check(cpu_getFlags(cpu) == 0x34u, "CPU reset flags");

    cpu->e = false;
    cpu->x = 0xABCDu;
    cpu->y = 0x1234u;
    cpu_setFlags(cpu, 0xC9u);
    check(cpu_getFlags(cpu) == 0xC9u, "native-mode flag round trip");
    check(cpu->x == 0xABCDu && cpu->y == 0x1234u,
          "native 16-bit index preservation");

    cpu->e = true;
    cpu->sp = 0xABCDu;
    cpu->x = 0x1234u;
    cpu->y = 0xFEDCu;
    cpu_setFlags(cpu, 0u);
    check(cpu->mf && cpu->xf, "emulation mode forces M/X");
    check(cpu->sp == 0x01CDu, "emulation mode forces stack page");
    check(cpu->x == 0x0034u && cpu->y == 0x00DCu,
          "8-bit index truncation");

    SaveLoadInfo info = {capture_cpu};
    cpu_saveload(cpu, &info);
    check(save_calls == 1u && save_pointer == &cpu->a,
          "CPU save/load starts at first register");
    check(save_size == sizeof(*cpu) - offsetof(Cpu, a),
          "CPU save/load register span");
    cpu_free(cpu);
}

static void test_widescreen_copy(void) {
    const uint8_t source[24] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
        13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
    };
    uint8_t destination[32];
    memset(destination, 0xCC, sizeof(destination));
    RtlWidescreenPresent(destination, 16u, source, 3, 2);
    check(memcmp(destination, source, 12u) == 0, "first presented row");
    check(memcmp(destination + 16u, source + 12u, 12u) == 0,
          "second presented row");
    check(destination[12] == 0xCCu && destination[28] == 0xCCu,
          "destination pitch padding preserved");
    check(kWsExtraMax == 120, "widescreen compatibility limit");
}

static void test_register_adapter(void) {
    Snes instance = {0x42u};
    g_snes = &instance;
    recomp_write_internal_reg(0x4200u, 0x81u);
    check(last_register == 0x4200u && last_value == 0x81u,
          "internal-register write routing");
    check(recomp_read_internal_reg(0x4210u) == 0xA7u &&
              last_register == 0x4210u,
          "internal-register read routing");
}

int main(void) {
    test_cpu();
    test_widescreen_copy();
    test_register_adapter();
    return failures == 0 ? 0 : 1;
}
