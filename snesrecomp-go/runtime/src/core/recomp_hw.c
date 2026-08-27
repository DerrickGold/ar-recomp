#include "recomp_hw.h"

#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/generated_support.h"
#include "runner_state_internal.h"
#include "snes/snes.h"

void recomp_write_internal_reg(uint16_t register_address, uint8_t value) {
    snes_writeReg(g_snes, register_address, value);
}

uint8_t recomp_read_internal_reg(uint16_t register_address) {
    return snes_readReg(g_snes, register_address);
}
