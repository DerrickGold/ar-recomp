#ifndef QUINTET_LZSS_H
#define QUINTET_LZSS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  kQuintetLzssDictionaryBytes = 256,
  kQuintetLzssDictionaryFill = 0x20,
  kQuintetLzssDictionaryStart = 0xEF,
};

/* A reader keeps the decoder independent of the source address space. The
 * immutable host users bind it to a byte slice; the $02:C5C9 HLE binds it to
 * the emulated 65816 bus so LoROM mapping and 16-bit address wrap stay native. */
typedef bool (*QuintetLzssReadByte)(void *context, size_t offset,
                                    uint8_t *value);

/* Final decoder state. Besides making the shared core observable in tests,
 * these fields are the state the native driver leaves in its direct-page
 * scratch and $7E:2000 dictionary. */
typedef struct QuintetLzssState {
  uint8_t dictionary[kQuintetLzssDictionaryBytes];
  size_t bits_consumed;
  uint16_t native_shift_register; /* native $B7/$B8 helper scratch */
  uint8_t write_position;         /* native $AF low byte */
  uint8_t match_position;         /* native $B1 low byte */
  uint8_t control_mask;           /* native $AE */
  uint8_t last_output;
  uint8_t last_read8_phase;
  uint8_t final_match_remaining;
  bool final_token_was_match;
} QuintetLzssState;

/* Decode exactly output_size bytes from an MSB-first raw token stream. */
bool QuintetLzss_DecompressReader(QuintetLzssReadByte read_byte,
                                  void *read_context,
                                  uint8_t *output, size_t output_size,
                                  QuintetLzssState *state);

/* Bounded byte-slice form of the raw-stream decoder. */
bool QuintetLzss_Decompress(const uint8_t *input, size_t input_size,
                            uint8_t *output, size_t output_size,
                            QuintetLzssState *state);

/* Stock asset form: little-endian output size followed by the raw stream.
 * expected_size must match the header, preventing a malformed asset from
 * overrunning a caller-owned output buffer. */
bool QuintetLzss_DecompressAsset(const uint8_t *packed, size_t packed_size,
                                 uint8_t *output, size_t expected_size,
                                 QuintetLzssState *state);

#endif /* QUINTET_LZSS_H */
