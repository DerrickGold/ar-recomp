#include "localization/text_rasterizer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bitmap effects rewritten for speed must produce the bytes the original
 * produced. The numeral slant below retains the full-page algorithm from before
 * it was restricted to numeral rows (2026-09-17), with the wrap-space classifier
 * correction applied to both versions. The rewrite is compared with it
 * on randomized pages, including failure cases and transparent pixels that
 * carry colour. */

static int s_failures;
#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

static bool ReferenceSlantAsciiNumerals(ArTextBitmap *b,
                                        const char *utf8, size_t bytes) {
  if (!b || !utf8 || !b->pixel_owners || !b->reveal_clusters ||
      !b->reveal_cluster_count || b->reveal_cluster_count > 65536 ||
      b->format != kArRenderPixelFormat_Rgba8888 || !b->pixels || b->width <= 0 ||
      b->width > INT32_MAX / 4 || b->pitch_bytes < b->width * 4 ||
      b->height <= 0 || (uint64_t)b->width * b->height > (UINT64_C(64) << 20) / 16)
    return false;
  const size_t area = (size_t)b->width * b->height;
  int *bottom = malloc((b->reveal_cluster_count + 1u) * sizeof(*bottom));
  uint32_t *pixels = calloc(area, sizeof(*pixels));
  uint32_t *owners = calloc(area, sizeof(*owners));
  if (!bottom || !pixels || !owners) {
    free(bottom); free(pixels); free(owners);
    return false;
  }
  bottom[0] = -1;
  size_t start = 0;
  for (size_t i = 0; i < b->reveal_cluster_count; ++i) {
    const size_t end = b->reveal_clusters[i].end_utf8_byte;
    bool number = start < end && end <= bytes;
    /* Omitted break whitespace does not belong to the next ink cluster. */
    static const char *const omitted[] = {
      " ", "\t", "\n", "\r", "\xc2\x85", "\xd8\x9c", "\xe1\x9a\x80",
      "\xe2\x80\x80", "\xe2\x80\x81", "\xe2\x80\x82", "\xe2\x80\x83",
      "\xe2\x80\x84", "\xe2\x80\x85", "\xe2\x80\x86", "\xe2\x80\x88",
      "\xe2\x80\x89", "\xe2\x80\x8a", "\xe2\x80\x8e", "\xe2\x80\x8f",
      "\xe2\x80\xa8", "\xe2\x80\xa9", "\xe2\x80\xaa", "\xe2\x80\xab",
      "\xe2\x80\xac", "\xe2\x80\xad", "\xe2\x80\xae", "\xe2\x81\x9f",
      "\xe2\x81\xa6", "\xe2\x81\xa7", "\xe2\x81\xa8", "\xe2\x81\xa9",
      "\xe3\x80\x80",
    };
    while (number && start < end) {
      size_t skipped = 0;
      for (size_t j = 0; j < sizeof(omitted) / sizeof(omitted[0]); ++j) {
        const size_t length = strlen(omitted[j]);
        if (length <= end - start && !memcmp(utf8 + start, omitted[j], length)) {
          skipped = length;
          break;
        }
      }
      if (!skipped) break;
      start += skipped;
    }
    number &= start < end;
    for (size_t j = start; number && j < end; ++j)
      number = utf8[j] >= '0' && utf8[j] <= '9';
    bottom[i + 1u] = number ? 0 : -1;
    start = end;
  }
  for (int y = 0; y < b->height; ++y)
    for (int x = 0; x < b->width; ++x) {
      const uint32_t owner = b->pixel_owners[(size_t)y * b->width + x];
      if (owner > b->reveal_cluster_count) {
        free(bottom); free(pixels); free(owners);
        return false;
      }
      if (owner && bottom[owner] >= 0) bottom[owner] = y;
    }
  /* Preserve the upright ink first. A slanted bearing may extend behind its
   * neighbour but may not overwrite that neighbour's letterform. */
  for (int pass = 0; pass < 2; ++pass)
    for (int y = 0; y < b->height; ++y)
      for (int x = 0; x < b->width; ++x) {
        const uint32_t owner = b->pixel_owners[(size_t)y * b->width + x];
        if (!owner || (bottom[owner] >= 0) != (pass != 0)) continue;
        const int target_x = x + (pass ? (bottom[owner] - y + 2) / 4 : 0);
        if (target_x >= b->width) {
          free(bottom); free(pixels); free(owners);
          return false;
        }
        const size_t at = (size_t)y * b->width + target_x;
        uint32_t pixel;
        memcpy(&pixel, (const uint8_t *)b->pixels +
            (size_t)y * b->pitch_bytes + (size_t)x * 4, 4);
        if (owners[at] && (bottom[owners[at]] < 0 ||
                          (pixels[at] & 255) >= (pixel & 255))) continue;
        pixels[at] = pixel;
        owners[at] = owner;
      }
  for (int y = 0; y < b->height; ++y)
    memcpy((uint8_t *)b->pixels + (size_t)y * b->pitch_bytes,
           pixels + (size_t)y * b->width, (size_t)b->width * 4);
  memcpy((void *)b->pixel_owners, owners, area * sizeof(*owners));
  free(bottom); free(pixels); free(owners);
  return true;
}


static uint32_t Random(uint32_t *state) {
  *state ^= *state << 13;
  *state ^= *state >> 17;
  *state ^= *state << 5;
  return *state;
}

typedef struct Page {
  uint8_t *pixels;
  uint32_t *owners;
  ArTextRevealCluster clusters[16];
  char utf8[64];
  size_t bytes;
  ArTextBitmap bitmap;
} Page;

/* `padding` trailing columns stay unowned, like the rasterizer's slant
 * padding, so most pages have room for their numerals to lean. */
static void BuildPage(Page *page, uint32_t *state, int width, int height,
                      int pitch, int padding, size_t clusters,
                      bool valid_owners) {
  memset(page, 0, sizeof(*page));
  page->pixels = malloc((size_t)pitch * (size_t)height);
  page->owners = malloc((size_t)width * (size_t)height * sizeof(uint32_t));
  /* Inferred ranges include digits, letters and omitted break whitespace. */
  static const char kAlphabet[] = "0123456789AB9 \t\r\n";
  size_t at = 0;
  for (size_t i = 0; i < clusters; ++i) {
    const size_t length = 1 + Random(state) % 3;
    for (size_t j = 0; j < length && at < sizeof(page->utf8) - 1; ++j)
      page->utf8[at++] = kAlphabet[Random(state) % (sizeof(kAlphabet) - 1)];
    page->clusters[i].end_utf8_byte = at;
  }
  page->bytes = at;
  for (size_t i = 0; i < (size_t)pitch * (size_t)height; ++i)
    page->pixels[i] = (uint8_t)Random(state);
  for (int i = 0; i < width * height; ++i) {
    const uint32_t roll = Random(state) % 4;
    page->owners[i] = roll == 0 || i % width >= width - padding
        ? 0 : 1 + Random(state) % (uint32_t)clusters;
    if (!valid_owners && Random(state) % 97 == 0)
      page->owners[i] = (uint32_t)clusters + 1;
  }
  page->bitmap = (ArTextBitmap){
    .pixels = page->pixels, .width = width, .height = height,
    .pitch_bytes = pitch, .format = kArRenderPixelFormat_Rgba8888,
    .reveal_clusters = page->clusters, .reveal_cluster_count = clusters,
    .pixel_owners = page->owners,
  };
}

static void TestSlantMatchesFullPageReference(void) {
  uint32_t state = 0x9e3779b9u;
  int compared = 0, succeeded = 0, failed = 0;
  for (int trial = 0; trial < 6000; ++trial) {
    const int width = 1 + (int)(Random(&state) % 48);
    const int height = 1 + (int)(Random(&state) % 40);
    const int pitch = width * 4 + (int)(Random(&state) % 8);
    const size_t clusters = 1 + Random(&state) % 12;
    const bool valid_owners = Random(&state) % 10 != 0;
    const int padding = Random(&state) % 5 ? (height + 2) / 4 : 0;
    Page reference, candidate;
    uint32_t seed = state;
    BuildPage(&reference, &seed, width, height, pitch, padding, clusters,
              valid_owners);
    seed = state;
    BuildPage(&candidate, &seed, width, height, pitch, padding, clusters,
              valid_owners);
    state = seed;
    const bool expected = ReferenceSlantAsciiNumerals(
        &reference.bitmap, reference.utf8, reference.bytes);
    const bool actual = ArTextBitmap_SlantAsciiNumerals(
        &candidate.bitmap, candidate.utf8, candidate.bytes);
    CHECK(expected == actual);
    CHECK(!memcmp(reference.pixels, candidate.pixels, (size_t)pitch * (size_t)height));
    CHECK(!memcmp(reference.owners, candidate.owners,
                  (size_t)width * (size_t)height * sizeof(uint32_t)));
    ++compared;
    if (expected) ++succeeded; else ++failed;
    free(reference.pixels); free(reference.owners);
    free(candidate.pixels); free(candidate.owners);
  }
  /* Both outcomes must actually be exercised, or equality proves little. */
  CHECK(compared == 6000 && succeeded > 1000 && failed > 100);
  printf("slant oracle: %d pages, %d applied, %d rejected\n", compared, succeeded, failed);
}

int main(void) {
  TestSlantMatchesFullPageReference();
  if (s_failures) {
    fprintf(stderr, "text_bitmap_effects_test: %d failure(s)\n", s_failures);
    return 1;
  }
  printf("text_bitmap_effects_test: OK\n");
  return 0;
}
