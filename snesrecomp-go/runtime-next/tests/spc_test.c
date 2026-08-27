#include "snes/apu.h"
#include "snes/saveload.h"
#include "snes/spc.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static unsigned patch_calls;
static unsigned cycle_calls;
static void *save_address;
static size_t save_size;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "runtime-next SPC contract failed: %s\n", message);
        ++failures;
    }
}

uint8_t apu_cpuRead(Apu *apu, uint16_t address) { return apu->ram[address]; }
void apu_cpuWrite(Apu *apu, uint16_t address, uint8_t value) { apu->ram[address] = value; }
uint64_t snes_apu_cycle_count(void) { return 0u; }

static Spc *fixture(Apu *apu, uint16_t pc) {
    memset(apu, 0, sizeof(*apu));
    apu->ram[0xfffeu] = (uint8_t)pc;
    apu->ram[0xffffu] = (uint8_t)(pc >> 8);
    Spc *spc = spc_init(apu);
    if (spc != NULL) spc_reset(spc);
    return spc;
}

static void test_all_opcodes(void) {
    Apu apu;
    for (unsigned opcode = 0; opcode < 256u; ++opcode) {
        Spc *spc = fixture(&apu, 0x0200u);
        check(spc != NULL, "opcode fixture allocation");
        if (spc == NULL) return;
        apu.ram[0x0200u] = (uint8_t)opcode;
        apu.ram[0x0201u] = 0u;
        apu.ram[0x0202u] = 2u;
        apu.ram[0xffdeu] = 0u;
        apu.ram[0xffdfu] = 2u;
        spc->sp = 0xefu;
        const int cycles = spc_runOpcode(spc);
        check(cycles > 0 && cycles <= 14, "every opcode has bounded timing");
        spc_free(spc);
    }
}

static void test_arithmetic_and_memory(void) {
    Apu apu;
    Spc *spc = fixture(&apu, 0x0200u);
    check(spc != NULL, "arithmetic fixture");
    if (spc == NULL) return;
    const uint8_t program[] = {
        0xe8u, 0x10u,       /* MOV A,#$10 */
        0x88u, 0x05u,       /* ADC A,#$05 */
        0xc4u, 0x20u,       /* MOV $20,A */
        0x28u, 0x0fu,       /* AND A,#$0F */
        0x68u, 0x05u,       /* CMP A,#$05 */
    };
    memcpy(apu.ram + 0x0200u, program, sizeof(program));
    for (unsigned index = 0; index < 5u; ++index) (void)spc_runOpcode(spc);
    check(apu.ram[0x20u] == 0x15u && spc->a == 5u,
          "ALU rows and direct-page store");
    check(spc->z && spc->c && !spc->n, "compare flags");
    spc_free(spc);
}

static void test_call_word_and_bit_ops(void) {
    Apu apu;
    Spc *spc = fixture(&apu, 0x0200u);
    check(spc != NULL, "call fixture");
    if (spc == NULL) return;
    apu.ram[0x0200u] = 0x3fu; apu.ram[0x0201u] = 0x00u; apu.ram[0x0202u] = 0x03u;
    apu.ram[0x0300u] = 0xe8u; apu.ram[0x0301u] = 0x42u; apu.ram[0x0302u] = 0x6fu;
    spc->sp = 0xefu;
    (void)spc_runOpcode(spc); (void)spc_runOpcode(spc); (void)spc_runOpcode(spc);
    check(spc->a == 0x42u && spc->pc == 0x0203u && spc->sp == 0xefu,
          "CALL/RET stack order");

    spc->pc = 0x0400u;
    apu.ram[0x10u] = 0x34u; apu.ram[0x11u] = 0x12u;
    apu.ram[0x12u] = 2u; apu.ram[0x13u] = 1u;
    apu.ram[0x0400u] = 0xbau; apu.ram[0x0401u] = 0x10u;
    apu.ram[0x0402u] = 0x7au; apu.ram[0x0403u] = 0x12u;
    (void)spc_runOpcode(spc); (void)spc_runOpcode(spc);
    check(spc->a == 0x36u && spc->y == 0x13u, "MOVW and ADDW");

    spc->pc = 0x0500u; apu.ram[0x20u] = 0u;
    apu.ram[0x0500u] = 0x02u; apu.ram[0x0501u] = 0x20u;
    apu.ram[0x0502u] = 0x03u; apu.ram[0x0503u] = 0x20u; apu.ram[0x0504u] = 2u;
    (void)spc_runOpcode(spc); (void)spc_runOpcode(spc);
    check(apu.ram[0x20u] == 1u && spc->pc == 0x0507u,
          "SET1 and taken BBS relative branch");
    spc_free(spc);
}

static void patch_hook(Spc *spc, uint16_t pc) { (void)pc; ++patch_calls; spc->a = 7u; }
static int cycle_hook(Spc *spc, uint16_t pc, int cycles) {
    (void)spc; (void)pc; ++cycle_calls; return cycles + 1;
}
static void capture_save(SaveLoadInfo *info, void *data, size_t size) {
    (void)info; save_address = data; save_size = size;
}

static void test_hooks_and_saveload(void) {
    Apu apu;
    Spc *spc = fixture(&apu, 0x0200u);
    check(spc != NULL, "hook fixture");
    if (spc == NULL) return;
    apu.ram[0x0200u] = 0xbcu; /* INC A */
    g_spc_opcode_patch_hook = patch_hook;
    g_spc_opcode_cycle_hook = cycle_hook;
    check(spc_runOpcode(spc) == 3 && spc->a == 8u,
          "patch/cycle hook ordering");
    check(patch_calls == 1u && cycle_calls == 1u,
          "opcode patch and cycle hooks called");
    SaveLoadInfo info = {capture_save};
    spc_saveload(spc, &info);
    check(save_address == &spc->a &&
          save_size == offsetof(Spc, cyclesUsed) - offsetof(Spc, a),
          "SPC register save span");
    spc_free(spc);
}

int main(void) {
    test_all_opcodes();
    test_arithmetic_and_memory();
    test_call_word_and_bit_ops();
    test_hooks_and_saveload();
    return failures == 0 ? 0 : 1;
}
