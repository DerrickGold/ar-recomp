#include "actraiser/actraiser_lzss.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "quintet_lzss.h"

enum {
  kLzssDpSource = 0x00A5,
  kLzssDpSourceBank = 0x00A7,
  kLzssDpBitWindowLow = 0x00AD,
  kLzssDpControlMask = 0x00AE,
  kLzssDpDictionaryWrite = 0x00AF,
  kLzssDpDictionaryRead = 0x00B1,
  kLzssDpOutputSize = 0x00B3,
  kLzssDpOutputAddress = 0x00B5,
  kLzssDpShiftRegister = 0x00B7,
  kLzssDictionaryAddress = 0x2000,
  kLzssInitialControlMask = 1u << (CHAR_BIT - 1),
  kLzssAccumulatorHighByteMask = UINT16_MAX ^ UINT8_MAX,
};

typedef struct ActRaiserLzssReader {
  CpuState *cpu;
  uint16_t source_address;
  uint8_t source_bank;
} ActRaiserLzssReader;

static uint16_t ReadDp16(CpuState *cpu, uint16_t offset) {
  return cpu_read16(cpu, kSnesLowWramBank, (uint16_t)(cpu->D + offset));
}

static uint8_t ReadDp8(CpuState *cpu, uint16_t offset) {
  return cpu_read8(cpu, kSnesLowWramBank, (uint16_t)(cpu->D + offset));
}

static void WriteDp8(CpuState *cpu, uint16_t offset, uint8_t value) {
  cpu_write8(cpu, kSnesLowWramBank, (uint16_t)(cpu->D + offset), value);
}

static void WriteDp16(CpuState *cpu, uint16_t offset, uint16_t value) {
  cpu_write16(cpu, kSnesLowWramBank, (uint16_t)(cpu->D + offset), value);
}

static bool ReadMappedSourceByte(void *context, size_t offset, uint8_t *value) {
  ActRaiserLzssReader *reader = context;
  if (!reader || !reader->cpu || !value) return false;
  *value = cpu_read8(
      reader->cpu, reader->source_bank,
      (uint16_t)(reader->source_address + (uint16_t)offset));
  return true;
}

/* Reconstruct the accumulator value at the native epilogue. A match exits
 * from the copy loop with the remaining token count in B (A.high). A literal
 * exits immediately after $C639; its B value is the shifted $AD/$AE word that
 * helper leaves behind when it returns the literal in A.low. */
static uint16_t ReconstructFinalAccumulator(
    CpuState *cpu, const QuintetLzssState *state) {
  if (state->final_token_was_match)
    return (uint16_t)((uint16_t)state->final_match_remaining << CHAR_BIT |
                      state->last_output);

  const unsigned phase = state->last_byte_read_phase;
  const uint16_t control_mask =
      (uint16_t)(kLzssInitialControlMask >> phase);
  const uint16_t bit_window = (uint16_t)(
      ReadDp8(cpu, kLzssDpBitWindowLow) | (control_mask << CHAR_BIT));
  const uint16_t shifted_window =
      (uint16_t)(bit_window << (phase + 1));
  return (uint16_t)((shifted_window & kLzssAccumulatorHighByteMask) |
                    state->last_output);
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
  const uint8_t source_bank = ReadDp8(cpu, kLzssDpSourceBank);
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

  ActRaiserLzssReader reader = {
    .cpu = cpu,
    .source_address = source_address,
    .source_bank = source_bank,
  };
  QuintetLzssState state;
  if (!QuintetLzss_DecompressReader(
          ReadMappedSourceByte, &reader, output, output_size, &state)) {
    fprintf(stderr, "FATAL: $02:C5C9 HLE could not read its source stream\n");
    free(output);
    abort();
  }

  /* Native DB is $7E during both writes. X is 16-bit and wraps without
   * carrying into the bank, so preserve that address behavior explicitly. */
  for (uint32_t i = 0; i < output_size; i++)
    cpu_write8(cpu, kSnesLowWramBank,
               (uint16_t)(output_address + i), output[i]);
  for (unsigned i = 0; i < kQuintetLzssDictionaryBytes; i++)
    cpu_write8(cpu, kSnesLowWramBank,
               (uint16_t)(kLzssDictionaryAddress + i), state.dictionary[i]);
  free(output);

  const uint16_t source_end =
      (uint16_t)(source_address +
                 (uint16_t)(state.bits_consumed / CHAR_BIT));
  WriteDp16(cpu, kLzssDpSource, source_end);
  WriteDp8(cpu, kLzssDpControlMask, state.control_mask);
  WriteDp16(cpu, kLzssDpDictionaryWrite,
            (uint16_t)(kLzssDictionaryAddress | state.write_position));
  WriteDp16(cpu, kLzssDpDictionaryRead,
            (uint16_t)(kLzssDictionaryAddress | state.match_position));
  WriteDp16(cpu, kLzssDpShiftRegister, state.native_shift_register);
  cpu->A = ReconstructFinalAccumulator(cpu, &state);

  cpu->DB = saved_db;
  cpu->X = saved_x;
  cpu->Y = saved_y;
  cpu->P = saved_p;
  cpu_p_to_mirrors(cpu);
  if (cpu->x_flag) {
    cpu->X &= UINT8_MAX;
    cpu->Y &= UINT8_MAX;
  }
  cpu->S = (uint16_t)(cpu->S + k65816RtlStackBytes);
  return RECOMP_RETURN_NORMAL;
}
