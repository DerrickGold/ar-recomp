#ifndef AR_CPU_65816_MATH_H
#define AR_CPU_65816_MATH_H
/* Cpu65816 math: models the 65816's 16-bit ADC (binary and decimal) without
 * touching CpuState, for HLEs that must reproduce its flags.
 * Phase: pure.
 * Tests: tests/cpu_65816_math_test.c */

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

#endif /* AR_CPU_65816_MATH_H */
