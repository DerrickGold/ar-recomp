#include "cpu_65816_math.h"

#include <limits.h>

enum {
  kWordBits = sizeof(uint16_t) * CHAR_BIT,
  kWordSignBit = 1u << (kWordBits - 1),
  kPackedBcdDigitBits = 4,
  kPackedBcdDigitMask = (1u << kPackedBcdDigitBits) - 1,
  kPackedBcdMaximumDigit = 9,
  kPackedBcdCarryCorrection = 6,
  kPackedBcdHighDigitShift = kWordBits - kPackedBcdDigitBits,
  kPackedBcdCarryIntoHighDigitShift =
      kWordBits - 2 * kPackedBcdDigitBits,
  kPackedBcdHighDigitMask = kPackedBcdDigitMask << kPackedBcdHighDigitShift,
};

Cpu65816Add16Result Cpu65816_Add16(uint16_t left, uint16_t right,
                                  bool carry_in, bool decimal) {
  Cpu65816Add16Result result = { 0 };
  if (!decimal) {
    const uint32_t sum = (uint32_t)left + right + carry_in;
    result.value = (uint16_t)sum;
    result.carry = sum > UINT16_MAX;
    result.overflow =
        ((left ^ result.value) & (right ^ result.value) & kWordSignBit) != 0;
    return result;
  }

  uint32_t adjusted = 0;
  unsigned carry = carry_in;
  unsigned carry_into_high_digit = 0;
  for (unsigned shift = 0; shift < kWordBits;
       shift += kPackedBcdDigitBits) {
    unsigned digit = ((left >> shift) & kPackedBcdDigitMask) +
                     ((right >> shift) & kPackedBcdDigitMask) + carry;
    carry = digit > kPackedBcdMaximumDigit;
    if (shift == kPackedBcdCarryIntoHighDigitShift)
      carry_into_high_digit = carry;
    if (carry) digit += kPackedBcdCarryCorrection;
    adjusted |= (uint32_t)(digit & kPackedBcdDigitMask) << shift;
  }

  result.value = (uint16_t)adjusted;
  result.carry = carry != 0;
  const uint32_t visible_high_sum =
      (left & kPackedBcdHighDigitMask) +
      (right & kPackedBcdHighDigitMask) +
      (carry_into_high_digit << kPackedBcdHighDigitShift);
  result.overflow =
      ((left ^ visible_high_sum) & (right ^ visible_high_sum) &
       kWordSignBit) != 0;
  return result;
}
