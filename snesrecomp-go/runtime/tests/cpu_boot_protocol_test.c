/* Synthetic CPU-to-SPC handoff: no commercial ROM or IPL bytes. */
#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
const DispatchEntry g_dispatch_table[] = {{0u, {NULL, NULL, NULL, NULL}}};
const unsigned g_dispatch_table_count = 0u;
static void unused_frame(void) {}
static int failures;
static int wait_port(unsigned port, uint8 value) {
    for (unsigned n = 0; n < 100000u; ++n)
        if (cpu_read8(&g_cpu, 0u, (uint16)(0x2140u + port)) == value)
            return 1;
    fprintf(stderr, "CPU/SPC handoff: port %u never observed %02X\n", port, value);
    ++failures;
    return 0;
}
int main(void) {
    static const RtlGameIdentity identity = {
        .struct_size = RTL_GAME_IDENTITY_V1_SIZE, .game_id = "synthetic-boot",
        .display_name = "Synthetic boot", .save_name_prefix = "synthetic-boot"};
    static const RtlGameExecutionApi execution = {
        .struct_size = RTL_GAME_EXECUTION_API_V2_SIZE, .run_frame = unused_frame};
    static const RtlGameModule module = {
        .abi_version = RTL_GAME_MODULE_ABI_VERSION, .struct_size = RTL_GAME_MODULE_V2_SIZE,
        .capabilities = RTL_GAME_MODULE_CAP_IDENTITY | RTL_GAME_MODULE_CAP_EXECUTION,
        .identity = &identity, .execution = &execution};
    uint8 *rom = calloc(1u, 0x10000u);
    if (rom == NULL) return 2;
    memcpy(rom + 0xffc0u, "SYNTHETIC BOOT TEST   ", 21u);
    rom[0xffd5u] = 0x21u; rom[0xffd7u] = 6u;
    rom[0xffdcu] = rom[0xffddu] = 0xffu;
    rom[0xfffdu] = 0x80u;
    if (RtlRegisterGame(&module) != SR_RESULT_OK || !SnesInit(rom, 0x10000)) {
        free(rom); return 2;
    }
    cpu_state_init(&g_cpu, g_ram);
    if (!wait_port(1u, 0xbbu)) goto done;
    cpu_write16(&g_cpu, 0u, 0x2142u, 0x0200u);
    cpu_write16(&g_cpu, 0u, 0x2140u, 0x01ccu);
    if (!wait_port(0u, 0xccu)) goto done;
    /* Immediately replace port 0 after execution starts. The CPU must see
     * the firmware's execute acknowledgement before this program clears it. */
    static const uint8 program[] = {0x8f, 0x00, 0xf4, 0x2f, 0xfe};
    for (unsigned i = 0u; i < sizeof(program); ++i) {
        cpu_write8(&g_cpu, 0u, 0x2141u, program[i]);
        cpu_write8(&g_cpu, 0u, 0x2140u, (uint8)i);
        if (!wait_port(0u, (uint8)i)) goto done;
    }
    cpu_write16(&g_cpu, 0u, 0x2140u, 0x0007u);
    if (wait_port(0u, 7u)) (void)wait_port(0u, 0u);
done:
    SnesShutdown(); free(rom);
    return failures ? 1 : 0;
}
