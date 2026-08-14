#include "actraiser/actraiser_lzss.h"

#include <stdio.h>
#include <stdlib.h>

#include "quintet_lzss.h"

enum {
  kLzssDpSource = 0x00A5,
  kLzssDpControlMask = 0x00AE,
  kLzssDpDictionaryWrite = 0x00AF,
  kLzssDpDictionaryRead = 0x00B1,
  kLzssDpOutputSize = 0x00B3,
  kLzssDpOutputAddress = 0x00B5,
  kLzssDpShiftRegister = 0x00B7,
  kLzssDictionaryAddress = 0x2000,
};

typedef struct ActRaiserLzssReader {
  CpuState *cpu;
  uint16_t address;
  uint8_t bank;
} ActRaiserLzssReader;

static uint16_t ReadDp16(CpuState *cpu, uint16_t offset) {
  return cpu_read16(cpu, 0x7E, (uint16_t)(cpu->D + offset));
}

static uint8_t ReadDp8(CpuState *cpu, uint16_t offset) {
  return cpu_read8(cpu, 0x7E, (uint16_t)(cpu->D + offset));
}

static void WriteDp8(CpuState *cpu, uint16_t offset, uint8_t value) {
  cpu_write8(cpu, 0x7E, (uint16_t)(cpu->D + offset), value);
}

static void WriteDp16(CpuState *cpu, uint16_t offset, uint16_t value) {
  cpu_write16(cpu, 0x7E, (uint16_t)(cpu->D + offset), value);
}

static bool ReadCpuSourceByte(void *context, size_t offset, uint8_t *value) {
  ActRaiserLzssReader *reader = context;
  if (!reader || !reader->cpu || !value) return false;
  *value = cpu_read8(reader->cpu, reader->bank,
                     (uint16_t)(reader->address + (uint16_t)offset));
  return true;
}

/* Reconstruct the accumulator value at the native epilogue. A match exits
 * from the copy loop with the remaining token count in B (A.high). A literal
 * exits immediately after $C639; its B value is the shifted $AD/$AE word that
 * helper leaves behind when it returns the literal in A.low. */
static uint16_t FinalAccumulator(CpuState *cpu,
                                 const QuintetLzssState *state) {
  if (state->final_token_was_match)
    return (uint16_t)((uint16_t)state->final_match_remaining << 8 |
                      state->last_output);

  const unsigned phase = state->last_read8_phase;
  const uint16_t mask = (uint16_t)(0x80u >> phase);
  const uint16_t ad_ae =
      (uint16_t)(ReadDp8(cpu, 0x00AD) | (mask << 8));
  const uint16_t shifted = (uint16_t)(ad_ae << (phase + 1));
  return (uint16_t)((shifted & 0xFF00u) | state->last_output);
}

/* Faithful whole-body HLE for the bounded, yield-free Quintet LZSS driver at
 * $02:C5C9. The five ROM callers supply a raw bitstream through $A5-$A7, a
 * 16-bit output count in $B3 and a bank-$7E destination in $B5. The native
 * routine owns $7E:2000-$20FF as its dictionary and exposes its final bit,
 * dictionary-pointer and shift-register scratch to the caller; preserve all
 * of it so the optimization is observationally equivalent, not just
 * byte-equivalent at the output.
 *
 * The routine restores P/DB/X/Y and ends in RTL. The generated hle_func shim
 * does not emulate that RTL, so this wrapper consumes its three-byte JSL
 * return frame explicitly. */
RecompReturn ActRaiser_LzssDecompress(CpuState *cpu) {
  if (!cpu) return RECOMP_RETURN_NORMAL;

  cpu_mirrors_to_p(cpu);
  const uint8_t saved_p = cpu->P;
  const uint8_t saved_db = cpu->DB;
  const uint16_t saved_x = cpu->X;
  const uint16_t saved_y = cpu->Y;
  const uint16_t source_address = ReadDp16(cpu, kLzssDpSource);
  const uint8_t source_bank = ReadDp8(cpu, kLzssDpSource + 2);
  const uint16_t output_size = ReadDp16(cpu, kLzssDpOutputSize);
  const uint16_t output_address = ReadDp16(cpu, kLzssDpOutputAddress);

  if (!output_size) {
    fprintf(stderr,
            "FATAL: $02:C5C9 HLE received a zero-byte output request\n");
    abort();
  }

  uint8_t *output = malloc(output_size);
  if (!output) {
    fprintf(stderr,
            "FATAL: $02:C5C9 HLE could not allocate %u output bytes\n",
            (unsigned)output_size);
    abort();
  }

  ActRaiserLzssReader reader = { cpu, source_address, source_bank };
  QuintetLzssState state;
  if (!QuintetLzss_DecompressReader(
          ReadCpuSourceByte, &reader, output, output_size, &state)) {
    fprintf(stderr, "FATAL: $02:C5C9 HLE could not read its source stream\n");
    free(output);
    abort();
  }

  /* Native DB is $7E during both writes. X is 16-bit and wraps without
   * carrying into the bank, so preserve that address behavior explicitly. */
  for (uint32_t i = 0; i < output_size; i++)
    cpu_write8(cpu, 0x7E, (uint16_t)(output_address + i), output[i]);
  for (unsigned i = 0; i < kQuintetLzssDictionaryBytes; i++)
    cpu_write8(cpu, 0x7E, (uint16_t)(kLzssDictionaryAddress + i),
               state.dictionary[i]);
  free(output);

  const uint16_t source_end =
      (uint16_t)(source_address + (uint16_t)(state.bits_consumed >> 3));
  WriteDp16(cpu, kLzssDpSource, source_end);
  WriteDp8(cpu, kLzssDpControlMask, state.control_mask);
  WriteDp16(cpu, kLzssDpDictionaryWrite,
            (uint16_t)(kLzssDictionaryAddress | state.write_position));
  WriteDp16(cpu, kLzssDpDictionaryRead,
            (uint16_t)(kLzssDictionaryAddress | state.match_position));
  WriteDp16(cpu, kLzssDpShiftRegister, state.native_shift_register);
  cpu->A = FinalAccumulator(cpu, &state);

  cpu->DB = saved_db;
  cpu->X = saved_x;
  cpu->Y = saved_y;
  cpu->P = saved_p;
  cpu_p_to_mirrors(cpu);
  if (cpu->x_flag) {
    cpu->X &= 0x00FF;
    cpu->Y &= 0x00FF;
  }
  cpu->S = (uint16_t)(cpu->S + 3);
  return RECOMP_RETURN_NORMAL;
}
