#include "snes.h"

#include "../apu_sync.h"
#include "apu.h"
#include "cart.h"
#include "cpu.h"
#include "dma.h"
#include "ppu.h"
#include "saveload.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum {
    SNES_WRAM_SIZE = 0x20000,
    SNES_WRAM_MASK = SNES_WRAM_SIZE - 1,
    SNES_SCANLINE_MASTER_CYCLES = 1364,
    SNES_HBLANK_START = 1024,
    SNES_STATUS_READ_STEP = 64,
    SNES_APU_CATCHUP_LIMIT = 10000
};

int snes_frame_counter;
uint64_t g_apu_timer0_total_ticks;

SnesRdnmiReadHook *g_snes_rdnmi_read_hook;
SnesWramWriteHook *g_snes_wram_write_hook;
SnesRegisterWriteHook *g_snes_register_write_hook;
SnesHardwareReadHook *g_snes_hardware_read_hook;
SnesApuPortReadHook *g_snes_apu_port_read_hook;

static uint64_t s_catchup_calls;
static uint64_t s_catchup_cycles;

Snes *snes_init(uint8_t *ram) {
    if (ram == NULL) {
        return NULL;
    }
    Snes *snes = (Snes *)calloc(1u, sizeof(*snes));
    if (snes == NULL) {
        return NULL;
    }
    snes->ram = ram;
    snes->cpu = cpu_init();
    snes->apu = apu_init();
    snes->dma = dma_init(snes);
    snes->ppu = ppu_init();
    snes->cart = cart_init(snes);
    if (snes->cpu == NULL || snes->apu == NULL || snes->dma == NULL ||
        snes->ppu == NULL || snes->cart == NULL) {
        snes_free(snes);
        return NULL;
    }
    return snes;
}

void snes_free(Snes *snes) {
    if (snes == NULL) {
        return;
    }
    cpu_free(snes->cpu);
    apu_free(snes->apu);
    dma_free(snes->dma);
    ppu_free(snes->ppu);
    cart_free(snes->cart);
    free(snes);
}

void snes_saveload(Snes *snes, SaveLoadInfo *info) {
    if (snes == NULL || info == NULL || info->func == NULL) {
        return;
    }
    if (!info->portable) {
        cpu_saveload(snes->cpu, info);
        apu_saveload(snes->apu, info);
        dma_saveload(snes->dma, info);
        ppu_saveload(snes->ppu, info);
        cart_saveload(snes->cart, info);
        info->func(info, &snes->hPos, sizeof(*snes) - offsetof(Snes, hPos));
        info->func(info, snes->ram, SNES_WRAM_SIZE);
        info->func(info, &snes->ramAdr, sizeof(snes->ramAdr));
        snes->cpu->e = false;
        return;
    }
    cpu_saveload(snes->cpu, info);
    apu_saveload(snes->apu, info);
    dma_saveload(snes->dma, info);
    ppu_saveload(snes->ppu, info);
    cart_saveload(snes->cart, info);
    saveload_u16(info, &snes->hPos);
    saveload_u16(info, &snes->vPos);
    saveload_f64(info, &snes->apuCatchupCycles);
    saveload_bool(info, &snes->hIrqEnabled);
    saveload_bool(info, &snes->vIrqEnabled);
    saveload_bool(info, &snes->nmiEnabled);
    saveload_u16(info, &snes->hTimer);
    saveload_u16(info, &snes->vTimer);
    saveload_bool(info, &snes->inNmi);
    saveload_bool(info, &snes->forceNmi);
    saveload_bool(info, &snes->nmiAvail);
    saveload_u32(info, &snes->last4210Block);
    saveload_bool(info, &snes->inIrq);
    saveload_bool(info, &snes->inVblank);
    saveload_bool(info, &snes->autoJoyRead);
    saveload_u16(info, &snes->autoJoyTimer);
    saveload_bool(info, &snes->ppuLatch);
    saveload_u8(info, &snes->multiplyA);
    saveload_u16(info, &snes->multiplyResult);
    saveload_u16(info, &snes->divideA);
    saveload_u16(info, &snes->divideResult);
    saveload_bytes(info, snes->ram, SNES_WRAM_SIZE);
    saveload_u32(info, &snes->ramAdr);
    snes->cpu->e = false;
}

void snes_reset(Snes *snes, bool hard) {
    if (snes == NULL) {
        return;
    }
    cart_reset(snes->cart);
    cpu_reset(snes->cpu);
    apu_reset(snes->apu);
    dma_reset(snes->dma);
    ppu_reset(snes->ppu);
    if (hard) {
        memset(snes->ram, 0, SNES_WRAM_SIZE);
    }
    snes->ramAdr = 0u;
    snes->hPos = 0u;
    snes->vPos = 0u;
    snes->apuCatchupCycles = 0.0;
    snes->hIrqEnabled = false;
    snes->vIrqEnabled = false;
    snes->nmiEnabled = false;
    snes->hTimer = 0x01ffu;
    snes->vTimer = 0x01ffu;
    snes->inNmi = false;
    snes->forceNmi = false;
    snes->nmiAvail = false;
    snes->last4210Block = 0u;
    snes->inIrq = false;
    snes->inVblank = false;
    snes->autoJoyRead = false;
    snes->autoJoyTimer = 0u;
    snes->ppuLatch = false;
    snes->multiplyA = 0xffu;
    snes->multiplyResult = 0xfe01u;
    snes->divideA = 0xffffu;
    snes->divideResult = 0x0101u;
}

void snes_catchupApu(Snes *snes) {
    if (snes == NULL || snes->apu == NULL) {
        return;
    }
    if (snes->apuCatchupCycles > SNES_APU_CATCHUP_LIMIT) {
        snes->apuCatchupCycles = SNES_APU_CATCHUP_LIMIT;
    }
    int cycles = (int)snes->apuCatchupCycles;
    if (cycles < 0) {
        cycles = 0;
    }
    for (int index = 0; index < cycles; ++index) {
        apu_cycle(snes->apu);
    }
    snes->apuCatchupCycles -= cycles;
    if (snes->apuCatchupCycles < 0.0) {
        snes->apuCatchupCycles = 0.0;
    }
    ++s_catchup_calls;
    s_catchup_cycles += (uint64_t)cycles;
}

void snes_catchup_stats(uint64_t *calls, uint64_t *cycles) {
    if (calls != NULL) *calls = s_catchup_calls;
    if (cycles != NULL) *cycles = s_catchup_cycles;
}

static void write_wram(Snes *snes, uint32_t offset, uint8_t value) {
    offset &= SNES_WRAM_MASK;
    const uint8_t old_value = snes->ram[offset];
    snes->ram[offset] = value;
    if (g_snes_wram_write_hook != NULL) {
        g_snes_wram_write_hook(offset, old_value, value);
    }
}

uint8_t snes_readBBus(Snes *snes, uint8_t address) {
    if (snes == NULL) return 0u;
    if (address < 0x40u) {
        return ppu_read(snes->ppu, address);
    }
    if (address < 0x80u) {
        RtlApuLock();
        rtl_accumulate_apu_catchup();
        snes_catchupApu(snes);
        const uint8_t port = (uint8_t)(address & 3u);
        const uint8_t value = snes->apu->outPorts[port];
        if (g_snes_apu_port_read_hook != NULL) {
            g_snes_apu_port_read_hook(snes, port, value);
        }
        RtlApuUnlock();
        return value;
    }
    if (address == 0x80u) {
        const uint8_t value = snes->ram[snes->ramAdr & SNES_WRAM_MASK];
        snes->ramAdr = (snes->ramAdr + 1u) & SNES_WRAM_MASK;
        return value;
    }
    return 0u;
}

void snes_writeBBus(Snes *snes, uint8_t address, uint8_t value) {
    if (snes == NULL) return;
    if (address < 0x40u) {
        ppu_write(snes->ppu, address, value);
        return;
    }
    if (address < 0x80u) {
        RtlApuWrite((uint16_t)(0x2100u + address), value);
        return;
    }
    switch (address) {
        case 0x80u:
            write_wram(snes, snes->ramAdr, value);
            snes->ramAdr = (snes->ramAdr + 1u) & SNES_WRAM_MASK;
            break;
        case 0x81u:
            snes->ramAdr = (snes->ramAdr & 0x1ff00u) | value;
            break;
        case 0x82u:
            snes->ramAdr = (snes->ramAdr & 0x100ffu) | ((uint32_t)value << 8);
            break;
        case 0x83u:
            snes->ramAdr = (snes->ramAdr & 0x0ffffu) |
                           ((uint32_t)(value & 1u) << 16);
            break;
        default: break;
    }
}

uint16_t SwapInputBits(uint16_t value) {
    value = (uint16_t)(((value & 0x5555u) << 1) | ((value >> 1) & 0x5555u));
    value = (uint16_t)(((value & 0x3333u) << 2) | ((value >> 2) & 0x3333u));
    value = (uint16_t)(((value & 0x0f0fu) << 4) | ((value >> 4) & 0x0f0fu));
    return (uint16_t)((value << 8) | (value >> 8));
}

uint8_t snes_readReg(Snes *snes, uint16_t address) {
    if (snes == NULL) return 0u;
    switch (address) {
        case 0x4210u: {
            if (g_snes_rdnmi_read_hook != NULL) {
                const int value = g_snes_rdnmi_read_hook(snes);
                if (value >= 0) return (uint8_t)value;
            }
            const uint8_t value = (uint8_t)(0x02u |
                ((snes->inNmi || snes->forceNmi) ? 0x80u : 0u));
            snes->inNmi = false;
            return value;
        }
        case 0x4211u: {
            const uint8_t value = snes->inIrq ? 0x80u : 0u;
            snes->inIrq = false;
            return value;
        }
        case 0x4212u:
            snes->hPos = (uint16_t)((snes->hPos + SNES_STATUS_READ_STEP) %
                                    SNES_SCANLINE_MASTER_CYCLES);
            return (uint8_t)((snes->autoJoyTimer != 0u ? 1u : 0u) |
                             (snes->hPos >= SNES_HBLANK_START ? 0x40u : 0u) |
                             (snes->inVblank ? 0x80u : 0u));
        case 0x4213u: return snes->ppuLatch ? 0x80u : 0u;
        case 0x4214u: return (uint8_t)snes->divideResult;
        case 0x4215u: return (uint8_t)(snes->divideResult >> 8);
        case 0x4216u: return (uint8_t)snes->multiplyResult;
        case 0x4217u: return (uint8_t)(snes->multiplyResult >> 8);
        case 0x4016u:
        case 0x4017u: return 1u;
        case 0x4218u: return (uint8_t)SwapInputBits(snes->input1_currentState);
        case 0x4219u: return (uint8_t)(SwapInputBits(snes->input1_currentState) >> 8);
        case 0x421au: return (uint8_t)SwapInputBits(snes->input2_currentState);
        case 0x421bu: return (uint8_t)(SwapInputBits(snes->input2_currentState) >> 8);
        default: return 0u;
    }
}

void snes_writeReg(Snes *snes, uint16_t address, uint8_t value) {
    if (snes == NULL) return;
    switch (address) {
        case 0x4200u:
            snes->autoJoyRead = (value & 1u) != 0u;
            if (!snes->autoJoyRead) snes->autoJoyTimer = 0u;
            snes->hIrqEnabled = (value & 0x10u) != 0u;
            snes->vIrqEnabled = (value & 0x20u) != 0u;
            snes->nmiEnabled = (value & 0x80u) != 0u;
            if (!snes->hIrqEnabled && !snes->vIrqEnabled) snes->inIrq = false;
            break;
        case 0x4201u:
            if ((value & 0x80u) == 0u && snes->ppuLatch) {
                (void)ppu_read(snes->ppu, 0x37u);
            }
            snes->ppuLatch = (value & 0x80u) != 0u;
            break;
        case 0x4202u: snes->multiplyA = value; break;
        case 0x4203u: snes->multiplyResult = (uint16_t)(snes->multiplyA * value); break;
        case 0x4204u: snes->divideA = (uint16_t)((snes->divideA & 0xff00u) | value); break;
        case 0x4205u: snes->divideA = (uint16_t)((snes->divideA & 0x00ffu) |
                                                 ((uint16_t)value << 8)); break;
        case 0x4206u:
            if (value == 0u) {
                snes->divideResult = 0xffffu;
                snes->multiplyResult = snes->divideA;
            } else {
                snes->divideResult = (uint16_t)(snes->divideA / value);
                snes->multiplyResult = (uint16_t)(snes->divideA % value);
            }
            break;
        case 0x4207u: snes->hTimer = (uint16_t)((snes->hTimer & 0x100u) | value); break;
        case 0x4208u: snes->hTimer = (uint16_t)((snes->hTimer & 0x0ffu) |
                                                ((uint16_t)(value & 1u) << 8)); break;
        case 0x4209u: snes->vTimer = (uint16_t)((snes->vTimer & 0x100u) | value); break;
        case 0x420au: snes->vTimer = (uint16_t)((snes->vTimer & 0x0ffu) |
                                                ((uint16_t)(value & 1u) << 8)); break;
        case 0x420bu:
            dma_startDma(snes->dma, value, false);
            dma_run_to_idle(snes->dma);
            break;
        case 0x420cu: dma_startDma(snes->dma, value, true); break;
        default: break;
    }
}

static bool is_system_bank(uint8_t bank) {
    return bank < 0x40u || (bank >= 0x80u && bank < 0xc0u);
}

uint8_t snes_read(Snes *snes, uint32_t address) {
    if (snes == NULL) return 0u;
    const uint8_t bank = (uint8_t)(address >> 16);
    const uint16_t offset = (uint16_t)address;
    if (bank == 0x7eu || bank == 0x7fu) {
        return snes->ram[((uint32_t)(bank & 1u) << 16) | offset];
    }
    if (is_system_bank(bank)) {
        uint8_t value;
        if (offset < 0x2000u) return snes->ram[offset];
        if (offset >= 0x2100u && offset < 0x2200u) {
            value = snes_readBBus(snes, (uint8_t)offset);
            if (g_snes_hardware_read_hook != NULL) g_snes_hardware_read_hook(offset, value);
            return value;
        }
        if (offset == 0x4016u || offset == 0x4017u) return 0u;
        if (offset >= 0x4200u && offset < 0x4220u) {
            value = snes_readReg(snes, offset);
            if (g_snes_hardware_read_hook != NULL) g_snes_hardware_read_hook(offset, value);
            return value;
        }
        if (offset >= 0x4300u && offset < 0x4380u) return dma_read(snes->dma, offset);
    }
    return cart_read(snes->cart, bank, offset);
}

void snes_write(Snes *snes, uint32_t address, uint8_t value) {
    if (snes == NULL) return;
    const uint8_t bank = (uint8_t)(address >> 16);
    const uint16_t offset = (uint16_t)address;
    if (bank == 0x7eu || bank == 0x7fu) {
        write_wram(snes, ((uint32_t)(bank & 1u) << 16) | offset, value);
        return;
    }
    if (is_system_bank(bank)) {
        if (offset < 0x2000u) {
            write_wram(snes, offset, value);
            return;
        }
        if (offset >= 0x2100u && offset < 0x2200u) {
            snes_writeBBus(snes, (uint8_t)offset, value);
            if (g_snes_register_write_hook != NULL) g_snes_register_write_hook(offset, value);
            return;
        }
        if (offset >= 0x4200u && offset < 0x4220u) {
            snes_writeReg(snes, offset, value);
            if (g_snes_register_write_hook != NULL) g_snes_register_write_hook(offset, value);
            return;
        }
        if (offset >= 0x4300u && offset < 0x4380u) {
            dma_write(snes->dma, offset, value);
            if (g_snes_register_write_hook != NULL) g_snes_register_write_hook(offset, value);
            return;
        }
    }
    cart_write(snes->cart, bank, offset, value);
}
