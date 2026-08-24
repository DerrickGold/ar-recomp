#include "actraiser/actraiser_save_checksum.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "byte_order.h"
#include "cpu_65816_math.h"
#include "save_system.h"

extern uint8 *g_sram;
extern int g_sram_size;

enum {
  kSaveChecksumDpSum = 0x0014,
  kSaveChecksumDpXor = 0x0016,
  kChecksumComponentBits = sizeof(uint16_t) * CHAR_BIT,
};

static uint16_t AddPackedBcdWordsAfterClearCarry(uint16_t left,
                                                 uint16_t right) {
  return Cpu65816_Add16(left, right, false, true).value;
}

static uint32_t ComputePackedBcdChecksum(const uint8_t *sram) {
  uint16_t xor_value = 0;
  uint16_t sum = 0;
  for (size_t offset = 0; offset < kActRaiserSramChecksumOffset;
       offset += sizeof(uint16_t)) {
    const uint16_t word = ByteOrder_ReadLe16(sram + offset);
    sum = AddPackedBcdWordsAfterClearCarry(word, sum);
    xor_value ^= word;
  }
  return ((uint32_t)xor_value << kChecksumComponentBits) | sum;
}

/* $00:84F3 is a bounded, yield-free 4,086-word SRAM scan shared by save
 * validation and save writes. save_system.c already owns and tests the binary
 * checksum algorithm, so the ordinary game path calls that same core instead
 * of retaining another production implementation in generated code. The game
 * always enters with decimal mode clear, but the routine does not issue CLD;
 * the fallback preserves its valid packed-BCD behavior as well. */
RecompReturn ActRaiser_SaveAccumulateChecksum(CpuState *cpu) {
  if (!cpu) return RECOMP_RETURN_NORMAL;
  if (!g_sram || g_sram_size < kActRaiserSramSize) {
    fprintf(stderr,
            "FATAL: $00:84F3 HLE requires a complete %d-byte SRAM image\n",
            kActRaiserSramSize);
    abort();
  }

  cpu_mirrors_to_p(cpu);
  const uint8_t saved_p = cpu->P;
  const uint32_t checksum = cpu->_flag_D
      ? ComputePackedBcdChecksum(g_sram)
      : Save_ComputeChecksum(g_sram);
  const uint16_t sum = (uint16_t)checksum;
  const uint16_t xor_value =
      (uint16_t)(checksum >> kChecksumComponentBits);

  cpu_write16(cpu, kSnesLowWramBank,
              (uint16_t)(cpu->D + kSaveChecksumDpSum), sum);
  cpu_write16(cpu, kSnesLowWramBank,
              (uint16_t)(cpu->D + kSaveChecksumDpXor), xor_value);
  cpu->A = xor_value;
  cpu->X = sum;
  cpu->Y = xor_value;

  cpu->P = saved_p;
  cpu_p_to_mirrors(cpu);
  if (cpu->x_flag) {
    cpu->X &= UINT8_MAX;
    cpu->Y &= UINT8_MAX;
  }
  cpu->S = (uint16_t)(cpu->S + k65816RtlStackBytes);
  return RECOMP_RETURN_NORMAL;
}
