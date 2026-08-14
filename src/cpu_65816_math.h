#ifndef CPU_65816_MATH_H
#define CPU_65816_MATH_H

#include <stdbool.h>
#include <stdint.h>

typedef struct Cpu65816Add16Result {
  uint16_t value;
  bool carry;
  bool overflow;
} Cpu65816Add16Result;

/* Model a 16-bit 65816 ADC without mutating CpuState. Decimal mode retains
 * the processor's binary overflow behavior alongside the BCD-adjusted result. */
Cpu65816Add16Result Cpu65816_Add16(uint16_t left, uint16_t right,
                                  bool carry_in, bool decimal);

#endif /* CPU_65816_MATH_H */
