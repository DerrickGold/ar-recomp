#include "actraiser/actraiser_save_checksum.h"

#include <stdio.h>
#include <stdlib.h>

#include "save_system.h"

extern uint8 *g_sram;
extern int g_sram_size;

enum {
  kChecksumDpSum = 0x0014,
  kChecksumDpXor = 0x0016,
};

static uint16_t ReadLe16(const uint8_t *bytes) {
  return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

/* The game always calls with decimal mode clear, but $84F3 does not issue
 * CLD: it PHP/REPs only M. Preserve the routine's valid D=1 behavior too so
 * the HLE remains a whole-body replacement rather than encoding a caller
 * assumption into the checksum result. */
static uint16_t AddPackedBcd16(uint16_t a, uint16_t b) {
  uint16_t result = 0;
  unsigned carry = 0;
  for (unsigned shift = 0; shift < 16; shift += 4) {
    unsigned digit = ((a >> shift) & 0x0Fu) +
                     ((b >> shift) & 0x0Fu) + carry;
    carry = digit > 9;
    if (carry) digit += 6;
    result |= (uint16_t)((digit & 0x0Fu) << shift);
  }
  return result;
}

static uint32_t ComputeDecimalChecksum(const uint8_t *sram) {
  uint16_t xored = 0;
  uint16_t summed = 0;
  for (int offset = 0; offset < kActRaiserSramChecksumOffset; offset += 2) {
    const uint16_t word = ReadLe16(sram + offset);
    summed = AddPackedBcd16(word, summed);
    xored ^= word;
  }
  return ((uint32_t)xored << 16) | summed;
}

/* $00:84F3 is a bounded, yield-free 4,086-word SRAM scan shared by save
 * validation and save writes. save_system.c already owns and tests the binary
 * checksum algorithm, so the ordinary game path calls that same core instead
 * of retaining another production implementation in generated code. */
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
      ? ComputeDecimalChecksum(g_sram)
      : Save_ComputeChecksum(g_sram);
  const uint16_t summed = (uint16_t)checksum;
  const uint16_t xored = (uint16_t)(checksum >> 16);

  cpu_write16(cpu, 0x7E, (uint16_t)(cpu->D + kChecksumDpSum), summed);
  cpu_write16(cpu, 0x7E, (uint16_t)(cpu->D + kChecksumDpXor), xored);
  cpu->A = xored;
  cpu->X = summed;
  cpu->Y = xored;

  cpu->P = saved_p;
  cpu_p_to_mirrors(cpu);
  if (cpu->x_flag) {
    cpu->X &= 0x00FF;
    cpu->Y &= 0x00FF;
  }
  cpu->S = (uint16_t)(cpu->S + 3);
  return RECOMP_RETURN_NORMAL;
}
