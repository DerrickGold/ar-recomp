#include "snes/apu.h"
#include "snes/cart.h"
#include "snes/snes.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Ppu { uint8_t registers[0x40]; unsigned resets; };

static int failures;
static Snes *active_snes;
static unsigned lock_depth;
static unsigned accumulate_calls;
static unsigned apu_cycles;
static uint16_t last_host_apu_address;
static uint8_t last_host_apu_value;
static uint32_t last_wram_offset;
static uint8_t last_wram_old;
static uint8_t last_wram_new;
static uint16_t last_register_write;
static uint16_t last_hardware_read;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "runtime-next SNES contract failed: %s\n", message);
        ++failures;
    }
}

Apu *apu_init(void) { return (Apu *)calloc(1u, sizeof(Apu)); }
void apu_free(Apu *apu) { free(apu); }
void apu_reset(Apu *apu) { memset(apu, 0, sizeof(*apu)); }
void apu_cycle(Apu *apu) { ++apu->cycles; ++apu_cycles; }
uint8_t apu_cpuRead(Apu *apu, uint16_t address) { return apu->ram[address]; }
void apu_cpuWrite(Apu *apu, uint16_t address, uint8_t value) { apu->ram[address] = value; }
void apu_saveload(Apu *apu, SaveLoadInfo *info) { (void)apu; (void)info; }

Ppu *ppu_init(void) { return (Ppu *)calloc(1u, sizeof(Ppu)); }
void ppu_free(Ppu *ppu) { free(ppu); }
void ppu_reset(Ppu *ppu) { memset(ppu->registers, 0, sizeof(ppu->registers)); ++ppu->resets; }
uint8_t ppu_read(Ppu *ppu, uint8_t address) { return ppu->registers[address & 0x3fu]; }
void ppu_write(Ppu *ppu, uint8_t address, uint8_t value) { ppu->registers[address & 0x3fu] = value; }
void ppu_saveload(Ppu *ppu, SaveLoadInfo *info) { (void)ppu; (void)info; }

void RtlApuLock(void) { ++lock_depth; }
void RtlApuUnlock(void) { --lock_depth; }
void rtl_accumulate_apu_catchup(void) { ++accumulate_calls; }
void RtlApuWrite(uint16_t address, uint8_t value) {
    last_host_apu_address = address;
    last_host_apu_value = value;
    if (active_snes != NULL) active_snes->apu->inPorts[address & 3u] = value;
}

static void observe_wram(uint32_t offset, uint8_t old_value, uint8_t new_value) {
    last_wram_offset = offset;
    last_wram_old = old_value;
    last_wram_new = new_value;
}
static void observe_register(uint16_t address, uint8_t value) {
    (void)value;
    last_register_write = address;
}
static void observe_read(uint16_t address, uint8_t value) {
    (void)value;
    last_hardware_read = address;
}
static int override_rdnmi(Snes *snes) { (void)snes; return 0x91; }

static void test_reset(Snes *snes, uint8_t *ram) {
    memset(ram, 0xa5, 0x20000u);
    snes->forceNmi = true;
    snes->nmiAvail = true;
    snes_reset(snes, true);
    check(ram[0] == 0u && ram[0x1ffffu] == 0u, "hard reset clears all WRAM");
    check(!snes->forceNmi && !snes->nmiAvail && snes->last4210Block == 0u,
          "reset clears recomp pacing state");
    check(snes->hTimer == 0x1ffu && snes->divideResult == 0x101u,
          "reset initializes CPU I/O registers");
}

static void test_internal_registers(Snes *snes) {
    snes_writeReg(snes, 0x4202u, 13u);
    snes_writeReg(snes, 0x4203u, 7u);
    check(snes_readReg(snes, 0x4216u) == 91u, "multiplication registers");
    snes_writeReg(snes, 0x4204u, 0x34u);
    snes_writeReg(snes, 0x4205u, 0x12u);
    snes_writeReg(snes, 0x4206u, 0x10u);
    check(snes_readReg(snes, 0x4214u) == 0x23u &&
          snes_readReg(snes, 0x4216u) == 4u, "division quotient and remainder");
    snes_writeReg(snes, 0x4206u, 0u);
    check(snes->divideResult == 0xffffu && snes->multiplyResult == 0x1234u,
          "division by zero behavior");
    snes->inNmi = true;
    check(snes_readReg(snes, 0x4210u) == 0x82u && !snes->inNmi,
          "RDNMI reports and acknowledges NMI");
    g_snes_rdnmi_read_hook = override_rdnmi;
    check(snes_readReg(snes, 0x4210u) == 0x91u, "game pacing RDNMI hook");
    g_snes_rdnmi_read_hook = NULL;
    snes->input1_currentState = 0x0001u;
    check(SwapInputBits(0x0001u) == 0x8000u && snes_readReg(snes, 0x4219u) == 0x80u,
          "joypad bit order");
}

static void test_bus(Snes *snes, uint8_t *ram) {
    g_snes_wram_write_hook = observe_wram;
    g_snes_register_write_hook = observe_register;
    g_snes_hardware_read_hook = observe_read;
    snes_write(snes, 0x7f2345u, 0x66u);
    check(ram[0x12345u] == 0x66u && last_wram_offset == 0x12345u &&
          last_wram_old == 0u && last_wram_new == 0x66u, "WRAM bank and observer");
    check(snes_read(snes, 0x002345u) == 0u, "unmapped system-bank read");
    snes_write(snes, 0x000123u, 0x77u);
    check(snes_read(snes, 0x800123u) == 0x77u, "low WRAM bank mirror");

    snes_write(snes, 0x00210fu, 0x44u);
    check(snes_read(snes, 0x00210fu) == 0x44u && last_register_write == 0x210fu &&
          last_hardware_read == 0x210fu, "PPU B-bus routing and observers");

    snes_writeBBus(snes, 0x81u, 0x34u);
    snes_writeBBus(snes, 0x82u, 0x12u);
    snes_writeBBus(snes, 0x83u, 1u);
    snes_writeBBus(snes, 0x80u, 0x9au);
    check(ram[0x11234u] == 0x9au && snes->ramAdr == 0x11235u,
          "WRAM data port address and increment");
}

static void test_apu_bus(Snes *snes) {
    snes->apu->outPorts[2] = 0x5au;
    snes->apuCatchupCycles = 3.75;
    check(snes_readBBus(snes, 0x42u) == 0x5au, "APU output-port read");
    check(lock_depth == 0u && accumulate_calls == 1u && apu_cycles == 3u,
          "APU read synchronization and bounded catchup");
    check(snes->apuCatchupCycles > 0.7 && snes->apuCatchupCycles < 0.8,
          "fractional APU catchup retained");
    snes_writeBBus(snes, 0x43u, 0xa6u);
    check(last_host_apu_address == 0x2143u && last_host_apu_value == 0xa6u &&
          snes->apu->inPorts[3] == 0xa6u, "APU write scheduler routing");
}

static void test_synchronous_dma_register(Snes *snes, uint8_t *ram) {
    ram[0x1000u] = 0x5au;
    ram[0x1001u] = 0xa5u;
    dma_write(snes->dma, 0x4300u, 0x01u);
    dma_write(snes->dma, 0x4301u, 0x18u);
    dma_write(snes->dma, 0x4302u, 0x00u);
    dma_write(snes->dma, 0x4303u, 0x10u);
    dma_write(snes->dma, 0x4304u, 0x7eu);
    dma_write(snes->dma, 0x4305u, 2u);
    dma_write(snes->dma, 0x4306u, 0u);
    snes_writeReg(snes, 0x420bu, 1u);
    check(snes->ppu->registers[0x18u] == 0x5au &&
          snes->ppu->registers[0x19u] == 0xa5u,
          "$420B drains PPU DMA synchronously");
    check(!snes->dma->dmaBusy && snes->dma->dmaTimer == 0u &&
          snes->dma->channel[0].aAdr == 0x1002u,
          "$420B leaves completed DMA state");

    ram[0x1010u] = 0x6cu;
    dma_write(snes->dma, 0x4300u, 0x00u);
    dma_write(snes->dma, 0x4301u, 0x40u);
    dma_write(snes->dma, 0x4302u, 0x10u);
    dma_write(snes->dma, 0x4303u, 0x10u);
    dma_write(snes->dma, 0x4305u, 1u);
    snes_writeReg(snes, 0x420bu, 1u);
    check(last_host_apu_address == 0x2140u &&
          last_host_apu_value == 0x6cu && snes->apu->inPorts[0] == 0x6cu,
          "$420B retains DMA-to-APU-port routing");
}

static void test_cartridge(Snes *snes) {
    uint8_t rom[0x8000u];
    memset(rom, 0, sizeof(rom));
    rom[0] = 0xabu;
    cart_load(snes->cart, 1, rom, (int)sizeof(rom), 0x800u);
    check(snes_read(snes, 0x008000u) == 0xabu, "cartridge read fallback");
    snes_write(snes, 0x700010u, 0x55u);
    check(snes_read(snes, 0x700010u) == 0x55u, "cartridge SRAM write fallback");
}

int main(void) {
    uint8_t *ram = (uint8_t *)malloc(0x20000u);
    check(ram != NULL, "WRAM fixture allocation");
    if (ram == NULL) return 1;
    Snes *snes = snes_init(ram);
    check(snes != NULL, "SNES allocation");
    if (snes == NULL) { free(ram); return 1; }
    active_snes = snes;
    test_reset(snes, ram);
    test_internal_registers(snes);
    test_bus(snes, ram);
    test_apu_bus(snes);
    test_synchronous_dma_register(snes, ram);
    test_cartridge(snes);
    active_snes = NULL;
    snes_free(snes);
    free(ram);
    return failures == 0 ? 0 : 1;
}
