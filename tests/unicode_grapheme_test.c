#include "localization/unicode_grapheme.h"

#include <stdio.h>
#include <string.h>

static int s_failures;
#define CHECK(expression) do { \
  if (!(expression)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #expression); \
    ++s_failures; \
  } \
} while (0)

static void CheckBoundaries(const char *name, const char *text,
                            const size_t *expected, size_t expected_count) {
  const size_t length = strlen(text);
  size_t offset = 0;
  size_t count = 0;
  while (offset < length) {
    size_t next = 0;
    uint32_t first = 0;
    if (!ArUnicodeGrapheme_Next(text, length, offset, &first, &next)) {
      fprintf(stderr, "%s: failed to decode cluster at byte %zu\n",
              name, offset);
      ++s_failures;
      return;
    }
    if (count >= expected_count || next != expected[count]) {
      fprintf(stderr,
              "%s: boundary %zu was %zu, expected %zu\n", name, count,
              next, count < expected_count ? expected[count] : length + 1u);
      ++s_failures;
      return;
    }
    CHECK(next > offset);
    offset = next;
    ++count;
  }
  if (count != expected_count) {
    fprintf(stderr, "%s: found %zu clusters, expected %zu\n",
            name, count, expected_count);
    ++s_failures;
  }
}

#define CHECK_BOUNDARIES(name, text, ...) do { \
  const size_t expected_[] = {__VA_ARGS__}; \
  CheckBoundaries((name), (text), expected_, \
                  sizeof(expected_) / sizeof(expected_[0])); \
} while (0)

int main(void) {
  CHECK_BOUNDARIES("ASCII", "abc", 1u, 2u, 3u);
  CHECK_BOUNDARIES("CRLF", "\r\na", 2u, 3u);
  CHECK_BOUNDARIES("decomposed accent", "e\xCC\x81x", 3u, 4u);
  CHECK_BOUNDARIES("Arabic mark", u8"نَص", 4u, 6u);
  CHECK_BOUNDARIES("Hangul Jamo", u8"각x", 9u, 10u);
  CHECK_BOUNDARIES("spacing mark", u8"काx", 6u, 7u);
  CHECK_BOUNDARIES("prepend", u8"؀نx", 4u, 5u);
  CHECK_BOUNDARIES("Indic conjunct", u8"क्षx", 9u, 10u);
  CHECK_BOUNDARIES("emoji ZWJ", u8"👩‍👩‍👧x", 18u, 19u);
  CHECK_BOUNDARIES("emoji modifier ZWJ", u8"👩🏽‍💻x", 15u, 16u);
  CHECK_BOUNDARIES("regional indicators", u8"🇨🇦🇫🇷x", 8u, 16u, 17u);
  CHECK_BOUNDARIES("keycap", u8"1️⃣x", 7u, 8u);

  const char overlong[] = {(char)0xC0, (char)0xAF};
  size_t next = 99;
  CHECK(!ArUnicodeGrapheme_Next(
      overlong, sizeof(overlong), 0, NULL, &next));
  const char surrogate[] = {
    (char)0xED, (char)0xA0, (char)0x80,
  };
  CHECK(!ArUnicodeGrapheme_Next(
      surrogate, sizeof(surrogate), 0, NULL, &next));
  const char truncated[] = {(char)0xF0, (char)0x9F, (char)0x91};
  CHECK(!ArUnicodeGrapheme_Next(
      truncated, sizeof(truncated), 0, NULL, &next));

  if (s_failures) return 1;
  puts("Unicode grapheme checks passed");
  return 0;
}
