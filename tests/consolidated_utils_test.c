#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "byte_order.h"
#include "deterministic_hash.h"
#include "manifest_utils.h"
#include "snes_bgr555.h"
#include "text_parse_utils.h"

static int g_failures;

#define CHECK(condition) do { \
  if (!(condition)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #condition); \
    g_failures++; \
  } \
} while (0)

static void TestByteOrder(void) {
  uint8_t bytes[4] = {0};
  ByteOrder_WriteLe32(bytes, UINT32_C(0x78563412));
  CHECK(bytes[0] == 0x12 && bytes[1] == 0x34 &&
        bytes[2] == 0x56 && bytes[3] == 0x78);
  CHECK(ByteOrder_ReadLe16(bytes) == UINT16_C(0x3412));
  CHECK(ByteOrder_ReadLe32(bytes) == UINT32_C(0x78563412));

  ByteOrder_WriteBe32(bytes, UINT32_C(0x12345678));
  CHECK(bytes[0] == 0x12 && bytes[1] == 0x34 &&
        bytes[2] == 0x56 && bytes[3] == 0x78);
  CHECK(ByteOrder_ReadBe16(bytes) == UINT16_C(0x1234));
}

static void TestHashes(void) {
  static const char kHello[] = "hello";
  uint32_t hash32 = DETERMINISTIC_HASH_FNV1A32_OFFSET;
  for (size_t i = 0; i < sizeof(kHello) - 1; i++)
    hash32 = DeterministicHash_Fnv1a32Byte(hash32, (uint8_t)kHello[i]);
  CHECK(hash32 == UINT32_C(0x4F9F2CAB));

  CHECK(DeterministicHash_Fnv1a64(
            DETERMINISTIC_HASH_FNV1A64_OFFSET,
            kHello, sizeof(kHello) - 1) == UINT64_C(0xA430D84680AABD0B));
}

static void TestManifestUtilities(void) {
  char text[] = "\t  value \r\n";
  CHECK(!strcmp(Manifest_Trim(text), "value"));

  char path[128];
  Manifest_ResolvePath("packs/default/manifest.ini", "art/title.png",
                       path, sizeof(path));
  CHECK(!strcmp(path, "packs/default/art/title.png"));

  Manifest_ResolvePath("packs\\default\\manifest.ini", "art\\title.png",
                       path, sizeof(path));
  CHECK(!strcmp(path, "packs\\default/art\\title.png"));

  Manifest_ResolvePath("packs/manifest.ini", "/opt/art/title.png",
                       path, sizeof(path));
  CHECK(!strcmp(path, "/opt/art/title.png"));

  Manifest_ResolvePath("packs/manifest.ini", "C:\\art\\title.png",
                       path, sizeof(path));
  CHECK(!strcmp(path, "C:\\art\\title.png"));

  Manifest_ResolvePath("packs/manifest.ini", "\\\\server\\art\\title.png",
                       path, sizeof(path));
  CHECK(!strcmp(path, "\\\\server\\art\\title.png"));
}

static void TestTextParsing(void) {
  char line[] = " \t value  # comment\r\n";
  char *value = TextParse_TrimLeft(line);
  TextParse_StripInlineComment(value);
  CHECK(!strcmp(value, "value"));

  char literal[] = "value#suffix";
  TextParse_StripInlineComment(literal);
  CHECK(!strcmp(literal, "value#suffix"));
}

static void TestColorExpansion(void) {
  CHECK(ExpandColor5(0, 15) == 0);
  CHECK(ExpandColor5(31, 15) == 255);
  CHECK(ExpandColor5(63, 15) == 255);
  CHECK(ExpandColor5(31, 0) == 0);
}

int main(void) {
  TestByteOrder();
  TestHashes();
  TestManifestUtilities();
  TestTextParsing();
  TestColorExpansion();
  if (g_failures) {
    fprintf(stderr, "%d consolidated utility test(s) failed\n", g_failures);
    return 1;
  }
  puts("consolidated utility tests passed");
  return 0;
}
