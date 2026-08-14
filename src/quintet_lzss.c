#include "quintet_lzss.h"

#include <limits.h>
#include <string.h>

typedef struct QuintetLzssBitReader {
  QuintetLzssReadByte read_byte;
  void *context;
  size_t bit;
  size_t cached_offset;
  uint8_t cached_value;
  bool have_cached_value;
} QuintetLzssBitReader;

typedef struct QuintetLzssBufferReader {
  const uint8_t *bytes;
  size_t size;
} QuintetLzssBufferReader;

static bool ReadBufferByte(void *context, size_t offset, uint8_t *value) {
  const QuintetLzssBufferReader *reader = context;
  if (!reader || !value || !reader->bytes || offset >= reader->size)
    return false;
  *value = reader->bytes[offset];
  return true;
}

static bool ReadSourceByte(QuintetLzssBitReader *reader, size_t offset,
                           uint8_t *value) {
  if (!reader || !value || !reader->read_byte) return false;
  if (!reader->have_cached_value || reader->cached_offset != offset) {
    if (!reader->read_byte(reader->context, offset, &reader->cached_value))
      return false;
    reader->cached_offset = offset;
    reader->have_cached_value = true;
  }
  *value = reader->cached_value;
  return true;
}

static bool ReadBits(QuintetLzssBitReader *reader, unsigned count,
                     unsigned *value) {
  if (!reader || !value || count > sizeof(*value) * CHAR_BIT ||
      reader->bit > SIZE_MAX - count)
    return false;
  unsigned result = 0;
  for (unsigned i = 0; i < count; i++) {
    uint8_t byte = 0;
    if (!ReadSourceByte(reader, reader->bit >> 3, &byte)) return false;
    result = (result << 1) |
        ((byte >> (7 - (reader->bit & 7))) & 1u);
    reader->bit++;
  }
  *value = result;
  return true;
}

/* $02:C639 always reads a 16-bit window even though it returns eight bits.
 * $02:C66C does the same only when its four-bit length crosses a byte. Keep
 * the resulting $B7/$B8 value so the CPU HLE can reproduce that scratch. A
 * missing look-ahead byte is treated as zero for metadata only; the ordinary
 * bit reads still reject a genuinely truncated input. */
static void CaptureNativeShiftRegister(QuintetLzssBitReader *reader,
                                       unsigned shift,
                                       QuintetLzssState *state) {
  uint8_t first = 0, second = 0;
  const size_t byte_offset = reader->bit >> 3;
  (void)ReadSourceByte(reader, byte_offset, &first);
  (void)ReadSourceByte(reader, byte_offset + 1, &second);
  state->native_shift_register =
      (uint16_t)(((uint16_t)((uint16_t)first << 8 | second) << shift) &
                 0xFFFFu);
}

static bool ReadNativeByte(QuintetLzssBitReader *reader, unsigned *value,
                           QuintetLzssState *state) {
  const unsigned phase = (unsigned)(reader->bit & 7);
  CaptureNativeShiftRegister(reader, phase, state);
  state->last_read8_phase = (uint8_t)phase;
  return ReadBits(reader, 8, value);
}

static bool ReadNativeLength(QuintetLzssBitReader *reader, unsigned *value,
                             QuintetLzssState *state) {
  const unsigned phase = (unsigned)(reader->bit & 7);
  if (phase >= 4)
    CaptureNativeShiftRegister(reader, phase - 4, state);
  return ReadBits(reader, 4, value);
}

bool QuintetLzss_DecompressReader(QuintetLzssReadByte read_byte,
                                  void *read_context,
                                  uint8_t *output, size_t output_size,
                                  QuintetLzssState *state_out) {
  if (!read_byte || !output || !output_size) return false;

  QuintetLzssState state;
  memset(&state, 0, sizeof(state));
  memset(state.dictionary, kQuintetLzssDictionaryFill,
         sizeof(state.dictionary));
  state.write_position = kQuintetLzssDictionaryStart;
  state.control_mask = 0x80;

  QuintetLzssBitReader reader = {
    .read_byte = read_byte,
    .context = read_context,
  };
  size_t produced = 0;
  while (produced < output_size) {
    unsigned literal = 0;
    if (!ReadBits(&reader, 1, &literal)) return false;
    if (literal) {
      unsigned value = 0;
      if (!ReadNativeByte(&reader, &value, &state)) return false;
      const uint8_t byte = (uint8_t)value;
      output[produced++] = byte;
      state.dictionary[state.write_position++] = byte;
      state.last_output = byte;
      state.final_token_was_match = false;
      state.final_match_remaining = 0;
      continue;
    }

    unsigned match = 0, length_code = 0;
    if (!ReadNativeByte(&reader, &match, &state) ||
        !ReadNativeLength(&reader, &length_code, &state))
      return false;
    unsigned remaining = length_code + 2;
    state.match_position = (uint8_t)match;
    while (remaining && produced < output_size) {
      const uint8_t byte = state.dictionary[state.match_position++];
      state.dictionary[state.write_position++] = byte;
      output[produced++] = byte;
      state.last_output = byte;
      state.final_token_was_match = true;
      state.final_match_remaining = (uint8_t)remaining;
      remaining--;
    }
  }

  state.bits_consumed = reader.bit;
  state.control_mask = (uint8_t)(0x80u >> (reader.bit & 7));
  if (state_out) *state_out = state;
  return true;
}

bool QuintetLzss_Decompress(const uint8_t *input, size_t input_size,
                            uint8_t *output, size_t output_size,
                            QuintetLzssState *state) {
  QuintetLzssBufferReader reader = { input, input_size };
  return QuintetLzss_DecompressReader(
      ReadBufferByte, &reader, output, output_size, state);
}

bool QuintetLzss_DecompressAsset(const uint8_t *packed, size_t packed_size,
                                 uint8_t *output, size_t expected_size,
                                 QuintetLzssState *state) {
  if (!packed || packed_size < 2) return false;
  const size_t output_size =
      (size_t)packed[0] | ((size_t)packed[1] << 8);
  if (output_size != expected_size) return false;
  return QuintetLzss_Decompress(
      packed + 2, packed_size - 2, output, output_size, state);
}
