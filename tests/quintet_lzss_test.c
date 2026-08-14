#include "quintet_lzss.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      failures++;                                                          \
    }                                                                      \
  } while (0)

static void PutBits(uint8_t *bytes, size_t *bit, unsigned value,
                    unsigned count) {
  for (unsigned i = 0; i < count; i++) {
    const unsigned shift = count - 1 - i;
    if ((value >> shift) & 1u)
      bytes[*bit >> 3] |= (uint8_t)(1u << (7 - (*bit & 7)));
    (*bit)++;
  }
}

static void PutLiteral(uint8_t *bytes, size_t *bit, uint8_t value) {
  PutBits(bytes, bit, 1, 1);
  PutBits(bytes, bit, value, 8);
}

static void PutMatch(uint8_t *bytes, size_t *bit, uint8_t source,
                     unsigned length) {
  PutBits(bytes, bit, 0, 1);
  PutBits(bytes, bit, source, 8);
  PutBits(bytes, bit, length - 2, 4);
}

static size_t BytesForBits(size_t bit) { return (bit + 7) / 8; }

static void TestLiteralState(void) {
  uint8_t packed[8] = {0};
  uint8_t output[3] = {0};
  size_t bit = 0;
  PutLiteral(packed, &bit, 'A');
  PutLiteral(packed, &bit, 'B');
  PutLiteral(packed, &bit, 'C');

  QuintetLzssState state;
  CHECK(QuintetLzss_Decompress(
      packed, BytesForBits(bit), output, sizeof(output), &state));
  CHECK(!memcmp(output, "ABC", sizeof(output)));
  CHECK(state.bits_consumed == 27);
  CHECK(state.control_mask == 0x10);
  CHECK(state.write_position == 0xF2);
  CHECK(state.match_position == 0x00);
  CHECK(state.last_output == 'C');
  CHECK(state.last_read8_phase == 3);
  CHECK(!state.final_token_was_match);
  CHECK(state.dictionary[0xEF] == 'A');
  CHECK(state.dictionary[0xF0] == 'B');
  CHECK(state.dictionary[0xF1] == 'C');
  CHECK(state.dictionary[0x00] == kQuintetLzssDictionaryFill);
}

static void TestOverlappingMatchAndOutputTruncation(void) {
  uint8_t packed[8] = {0};
  size_t bit = 0;
  PutLiteral(packed, &bit, 'A');
  PutMatch(packed, &bit, 0xEF, 5);

  uint8_t full[6] = {0};
  QuintetLzssState full_state;
  CHECK(QuintetLzss_Decompress(
      packed, BytesForBits(bit), full, sizeof(full), &full_state));
  CHECK(!memcmp(full, "AAAAAA", sizeof(full)));
  CHECK(full_state.bits_consumed == 22);
  CHECK(full_state.control_mask == 0x02);
  CHECK(full_state.write_position == 0xF5);
  CHECK(full_state.match_position == 0xF4);
  CHECK(full_state.final_token_was_match);
  CHECK(full_state.final_match_remaining == 1);

  uint8_t truncated_output[3] = {0};
  QuintetLzssState truncated_state;
  CHECK(QuintetLzss_Decompress(
      packed, BytesForBits(bit), truncated_output, sizeof(truncated_output),
      &truncated_state));
  CHECK(!memcmp(truncated_output, "AAA", sizeof(truncated_output)));
  CHECK(truncated_state.bits_consumed == full_state.bits_consumed);
  CHECK(truncated_state.write_position == 0xF2);
  CHECK(truncated_state.match_position == 0xF1);
  CHECK(truncated_state.final_match_remaining == 4);

  CHECK(!QuintetLzss_Decompress(
      packed, BytesForBits(bit) - 1, full, sizeof(full), NULL));
}

static void TestStraddlingLengthState(void) {
  uint8_t packed[8] = {0};
  uint8_t output[5] = {0};
  size_t bit = 0;
  PutLiteral(packed, &bit, 'X');
  PutLiteral(packed, &bit, 'Y');
  PutLiteral(packed, &bit, 'Z');
  PutMatch(packed, &bit, 0xEF, 2); /* length starts at bit phase four */

  QuintetLzssState state;
  CHECK(QuintetLzss_Decompress(
      packed, BytesForBits(bit), output, sizeof(output), &state));
  CHECK(!memcmp(output, "XYZXY", sizeof(output)));
  CHECK(state.bits_consumed == 40);
  CHECK(state.control_mask == 0x80);
  CHECK(state.native_shift_register ==
        (uint16_t)((uint16_t)packed[4] << 8 | packed[5]));
  CHECK(state.final_token_was_match);
  CHECK(state.final_match_remaining == 1);
}

static void TestAssetHeaderAndDictionaryWrap(void) {
  uint8_t asset[64] = {0};
  uint8_t output[20] = {0};
  asset[0] = sizeof(output);
  size_t bit = 16;
  for (unsigned i = 0; i < sizeof(output); i++)
    PutLiteral(asset, &bit, (uint8_t)i);

  QuintetLzssState state;
  CHECK(QuintetLzss_DecompressAsset(
      asset, BytesForBits(bit), output, sizeof(output), &state));
  for (unsigned i = 0; i < sizeof(output); i++)
    CHECK(output[i] == i);
  CHECK(state.write_position == 0x03);
  CHECK(state.dictionary[0xEF] == 0);
  CHECK(state.dictionary[0xFF] == 16);
  CHECK(state.dictionary[0x00] == 17);
  CHECK(state.dictionary[0x02] == 19);
  CHECK(!QuintetLzss_DecompressAsset(
      asset, BytesForBits(bit), output, sizeof(output) - 1, NULL));
  CHECK(!QuintetLzss_DecompressAsset(asset, 1, output, sizeof(output), NULL));
}

int main(void) {
  TestLiteralState();
  TestOverlappingMatchAndOutputTruncation();
  TestStraddlingLengthState();
  TestAssetHeaderAndDictionaryWrap();
  if (failures) {
    printf("quintet lzss: %d failure(s)\n", failures);
    return 1;
  }
  printf("quintet lzss: all checks passed\n");
  return 0;
}
