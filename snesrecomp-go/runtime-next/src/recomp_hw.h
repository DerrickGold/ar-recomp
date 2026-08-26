#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void recomp_write_internal_reg(uint16_t register_address, uint8_t value);
uint8_t recomp_read_internal_reg(uint16_t register_address);

#ifdef __cplusplus
}
#endif
