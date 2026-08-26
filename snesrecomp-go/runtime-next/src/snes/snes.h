#ifndef SNES_H
#define SNES_H

#include <stdbool.h>
#include <stdint.h>

#include "apu.h"
#include "cart.h"
#include "cpu.h"
#include "dma.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Apu Apu;
typedef struct Cart Cart;
typedef struct Cpu Cpu;
typedef struct Dma Dma;
typedef struct Ppu Ppu;
typedef struct SaveLoadInfo SaveLoadInfo;
typedef struct Snes Snes;

struct Snes {
    Cpu *cpu;
    Apu *apu;
    Ppu *ppu;
    Dma *dma;
    Cart *cart;
    uint16_t input1_currentState;
    uint16_t input2_currentState;
    bool disableRender;
    uint32_t ramAdr;
    uint8_t *ram;
    uint16_t hPos;
    uint16_t vPos;
    double apuCatchupCycles;
    bool hIrqEnabled;
    bool vIrqEnabled;
    bool nmiEnabled;
    uint16_t hTimer;
    uint16_t vTimer;
    bool inNmi;
    bool forceNmi;
    bool nmiAvail;
    uint32_t last4210Block;
    bool inIrq;
    bool inVblank;
    bool autoJoyRead;
    uint16_t autoJoyTimer;
    bool ppuLatch;
    uint8_t multiplyA;
    uint16_t multiplyResult;
    uint16_t divideA;
    uint16_t divideResult;
};

typedef int SnesRdnmiReadHook(Snes *snes);
typedef void SnesWramWriteHook(uint32_t offset, uint8_t old_value,
                               uint8_t new_value);
typedef void SnesRegisterWriteHook(uint16_t address, uint8_t value);
typedef void SnesHardwareReadHook(uint16_t address, uint8_t value);
typedef void SnesApuPortReadHook(Snes *snes, uint8_t port, uint8_t value);

extern SnesRdnmiReadHook *g_snes_rdnmi_read_hook;
extern SnesWramWriteHook *g_snes_wram_write_hook;
extern SnesRegisterWriteHook *g_snes_register_write_hook;
extern SnesHardwareReadHook *g_snes_hardware_read_hook;
extern SnesApuPortReadHook *g_snes_apu_port_read_hook;

Snes *snes_init(uint8_t *ram);
void snes_free(Snes *snes);
void snes_reset(Snes *snes, bool hard);
uint8_t snes_readBBus(Snes *snes, uint8_t address);
void snes_writeBBus(Snes *snes, uint8_t address, uint8_t value);
uint8_t snes_read(Snes *snes, uint32_t address);
void snes_write(Snes *snes, uint32_t address, uint8_t value);
uint8_t snes_readReg(Snes *snes, uint16_t address);
void snes_writeReg(Snes *snes, uint16_t address, uint8_t value);
uint16_t SwapInputBits(uint16_t value);
bool snes_loadRom(Snes *snes, const uint8_t *data, int length);
void snes_saveload(Snes *snes, SaveLoadInfo *info);
void snes_catchupApu(Snes *snes);
void snes_catchup_stats(uint64_t *calls, uint64_t *cycles);

extern int snes_frame_counter;
extern uint64_t g_apu_timer0_total_ticks;

#ifdef __cplusplus
}
#endif

#endif
