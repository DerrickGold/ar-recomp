#include "localization/unicode_grapheme.h"
#include "localization/interface_text.h"

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

static void TestInterfaceEditing(void) {
  char buffer[16] = "X";
  CHECK(ArInterfaceText_Append(buffer, sizeof(buffer), "é日", strlen("é日")) == 5);
  CHECK(!strcmp(buffer, "Xé日"));
  CHECK(ArInterfaceText_EraseLast(buffer, sizeof(buffer)));
  CHECK(!strcmp(buffer, "Xé"));
  CHECK(ArInterfaceText_EraseLast(buffer, sizeof(buffer)));
  CHECK(!strcmp(buffer, "X"));
  CHECK(!ArInterfaceText_Append(buffer, sizeof(buffer), "👩🏽‍💻", strlen("👩🏽‍💻")));
  CHECK(!strcmp(buffer, "X")); /* Can't fit the whole 15-byte cluster. */
  CHECK(ArInterfaceText_Append(buffer, sizeof(buffer), "e\u0301", 3) == 3);
  CHECK(ArInterfaceText_EraseLast(buffer, sizeof(buffer)));
  CHECK(!strcmp(buffer, "X"));
  CHECK(!ArInterfaceText_Append(buffer, sizeof(buffer), "A\xff", 2));
  CHECK(!ArInterfaceText_Append(buffer, sizeof(buffer), "A\0B", 3));
  CHECK(!strcmp(buffer, "X"));
  CHECK(!ArInterfaceText_Append(buffer, 4, "e\u0301", 3));
  CHECK(!strcmp(buffer, "X"));
  CHECK(ArInterfaceText_EraseLast(buffer, sizeof(buffer)));
  CHECK(!ArInterfaceText_EraseLast(buffer, sizeof(buffer)));
  char full[] = {'A','B'};
  CHECK(!ArInterfaceText_Append(full, sizeof(full), "C", 1));
  CHECK(!ArInterfaceText_EraseLast(full, sizeof(full)));
}

static void TestInterfaceWrapping(void) {
  const struct { const char *text, *line; size_t cells, bytes, consumed; } cases[] = {
    {"One two three", "One two", 7, 64, 8},
    {"One two three", "One", 6, 64, 4},
    {"longword", "lon", 3, 64, 3},
    {"日本語です", "日本", 2, 64, 6},
    {"日本語です", "日本", 9, 7, 6},
    {"éà Français", "éà", 2, 64, 5},
    {"e\u0301éZ", "e\u0301é", 2, 64, 5},
    {"👩🏽‍💻AB", "👩🏽‍💻", 1, 64, 15},
    {"Oui\nNon", "Oui", 20, 64, 4},
    {"Oui\r\nNon", "Oui", 3, 64, 5},
    {"\r\nNon", "", 20, 64, 2},
    {"One two\n\nNext", "One two", 7, 64, 8},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    ArInterfaceTextLine line;
    CHECK(ArInterfaceText_WrapLine(cases[i].text, strlen(cases[i].text), cases[i].cells, cases[i].bytes, &line));
    CHECK(line.bytes == strlen(cases[i].line));
    CHECK(!memcmp(cases[i].text, cases[i].line, line.bytes));
    CHECK(line.consumed == cases[i].consumed);
    CHECK(line.consumed > 0 && line.cells <= cases[i].cells);
  }
  ArInterfaceTextLine line;
  CHECK(!ArInterfaceText_WrapLine("👩🏽‍💻", 15, 20, 8, &line));
  CHECK(!line.bytes && !line.consumed);
  CHECK(!ArInterfaceText_WrapLine("", 0, 20, 64, &line));
  const char *paragraph = "日本語の説明です。\né\u0301 et des mots";
  size_t at = 0, bytes = strlen(paragraph), lines = 0;
  while (at < bytes) {
    if (!ArInterfaceText_WrapLine(paragraph + at, bytes - at, 3, 64, &line)) {
      CHECK(false); break;
    }
    CHECK(line.consumed <= bytes - at);
    size_t cluster = 0;
    while (cluster < line.bytes) {
      size_t next = cluster;
      bool valid = ArUnicodeGrapheme_Next(paragraph + at, line.bytes, cluster, NULL, &next);
      CHECK(valid);
      CHECK(next > cluster && next <= line.bytes);
      if (!valid || next <= cluster || next > line.bytes) break;
      cluster = next;
    }
    if (!line.consumed || line.consumed > bytes - at) break;
    at += line.consumed;
    ++lines;
  }
  CHECK(at == bytes && lines > 3);
}

int main(void) {
  TestInterfaceEditing();
  TestInterfaceWrapping();
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
