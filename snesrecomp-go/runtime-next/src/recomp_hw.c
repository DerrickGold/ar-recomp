#include "recomp_hw.h"

typedef struct Snes Snes;

extern Snes *g_snes;
extern void snes_writeReg(Snes *snes, uint16_t address, uint8_t value);
extern uint8_t snes_readReg(Snes *snes, uint16_t address);

void recomp_write_internal_reg(uint16_t register_address, uint8_t value) {
    snes_writeReg(g_snes, register_address, value);
}

uint8_t recomp_read_internal_reg(uint16_t register_address) {
    return snes_readReg(g_snes, register_address);
}
