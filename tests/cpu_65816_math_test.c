#include "cpu_65816_math.h"

#include <stddef.h>
#include <stdio.h>

static int failures;

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      failures++;                                                          \
    }                                                                      \
  } while (0)

typedef struct AddCase {
  uint16_t left;
  uint16_t right;
  bool carry_in;
  bool decimal;
  uint16_t expected_value;
  bool expected_carry;
  bool expected_overflow;
} AddCase;

static void CheckAdditionCases(void) {
  static const AddCase cases[] = {
    { 0x0000, 0x0000, false, false, 0x0000, false, false },
    { 0xFFFF, 0x0000, true,  false, 0x0000, true,  false },
    { 0x7FFF, 0x0000, true,  false, 0x8000, false, true  },
    { 0x8000, 0xFFFF, false, false, 0x7FFF, true,  true  },
    { 0x8000, 0x8000, false, false, 0x0000, true,  true  },
    { 0x0009, 0x0000, true,  true,  0x0010, false, false },
    { 0x0099, 0x0001, false, true,  0x0100, false, false },
    { 0x7999, 0x0001, false, true,  0x8000, false, true  },
    { 0x4999, 0x5001, false, true,  0x0000, true,  true  },
    { 0x9999, 0x0001, false, true,  0x0000, true,  false },
    { 0x8000, 0x8000, false, true,  0x6000, true,  true  },
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const AddCase *test = &cases[i];
    const Cpu65816Add16Result actual = Cpu65816_Add16(
        test->left, test->right, test->carry_in, test->decimal);
    CHECK(actual.value == test->expected_value);
    CHECK(actual.carry == test->expected_carry);
    CHECK(actual.overflow == test->expected_overflow);
  }
}

int main(void) {
  CheckAdditionCases();
  if (failures) {
    printf("65816 CPU math: %d failure(s)\n", failures);
    return 1;
  }
  printf("65816 CPU math: all checks passed\n");
  return 0;
}
