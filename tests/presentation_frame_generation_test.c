#include <math.h>
#include <stdio.h>

#include "presentation_frame_generation.h"

static int failures;
#define CHECK(expression) do {                                            \
  if (!(expression)) {                                                    \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);     \
    failures++;                                                           \
  }                                                                       \
} while (0)

enum { kWidth = 24, kHeight = 16 };

static void TestIdentity(void) {
  uint32_t frame[kWidth * kHeight];
  for (int i = 0; i < kWidth * kHeight; i++)
    frame[i] = 0xff000000u | (uint32_t)(i * 977u);
  PresentationFrameGenerationMotionField field;
  CHECK(!PresentationFrameGeneration_Analyze(
      frame, frame, kWidth, kHeight, kWidth, kWidth,
      kPresentationFrameGenerationAnalysis_Blocks, &field));
  CHECK(!field.valid);
}

static void TestMovingPixels(void) {
  uint32_t previous[kWidth * kHeight] = {0};
  uint32_t current[kWidth * kHeight] = {0};
  for (int y = 4; y < 12; y++) {
    for (int x = 4; x < 12; x++) {
      const uint32_t color = 0xff000000u |
          (uint32_t)((x * 29 + y * 47) & 0x00ffffffu);
      previous[y * kWidth + x] = color;
      current[y * kWidth + x + 2] = color;
    }
  }
  PresentationFrameGenerationMotionField field;
  CHECK(PresentationFrameGeneration_Analyze(
      previous, current, kWidth, kHeight, kWidth, kWidth,
      kPresentationFrameGenerationAnalysis_Blocks, &field));
  CHECK(field.forward_dx[0] == 2 && field.forward_dy[0] == 0);
  CHECK(field.blocks_x == 2 && field.forward_dx[1] == 2);
  float dx = 0.0f, dy = 0.0f;
  PresentationFrameGeneration_MotionAt(&field, true, 8, 8, &dx, &dy);
  CHECK(fabsf(dx - 2.0f) < 0.0001f && fabsf(dy) < 0.0001f);
  PresentationFrameGeneration_MotionAt(&field, false, 10, 8, &dx, &dy);
  CHECK(fabsf(dx + 2.0f) < 0.0001f && fabsf(dy) < 0.0001f);
}

static void TestPairPhase(void) {
  CHECK(fabsf(PresentationFrameGeneration_PairPhase(0.5f, 1) - 0.5f) <
        0.0001f);
  CHECK(fabsf(PresentationFrameGeneration_PairPhase(0.5f, 2) - 0.75f) <
        0.0001f);
  CHECK(PresentationFrameGeneration_PairPhase(0.5f, 0) == 1.0f);
}

static void TestInvalidDimensions(void) {
  uint32_t pixel = 0;
  PresentationFrameGenerationMotionField field;
  CHECK(!PresentationFrameGeneration_Analyze(
      &pixel, &pixel, 0, 1, 1, 1,
      kPresentationFrameGenerationAnalysis_Blocks, &field));
  CHECK(!PresentationFrameGeneration_Analyze(
      &pixel, &pixel,
      kPresentationFrameGenerationMaximumWidth + 1, 1,
      kPresentationFrameGenerationMaximumWidth + 1,
      kPresentationFrameGenerationMaximumWidth + 1,
      kPresentationFrameGenerationAnalysis_Blocks, &field));
}

static void TestColorChangeIsNotMotion(void) {
  uint32_t previous[kWidth * kHeight];
  uint32_t current[kWidth * kHeight];
  for (int i = 0; i < kWidth * kHeight; i++) {
    previous[i] = 0xffff0000u;
    current[i] = 0xff0000ffu;
  }
  PresentationFrameGenerationMotionField field;
  CHECK(!PresentationFrameGeneration_Analyze(
      previous, current, kWidth, kHeight, kWidth, kWidth,
      kPresentationFrameGenerationAnalysis_Global, &field));
  CHECK(!field.valid);
}

static void TestMaximumSurface(void) {
  enum {
    kMaxWidth = kPresentationFrameGenerationMaximumWidth,
    kMaxHeight = kPresentationFrameGenerationMaximumHeight,
  };
  static uint32_t previous[kMaxWidth * kMaxHeight];
  static uint32_t current[kMaxWidth * kMaxHeight];
  for (int y = 0; y < kMaxHeight; y++) {
    for (int x = 0; x < kMaxWidth; x++) {
      uint32_t hash = (uint32_t)(x + 1) * 73856093u ^
          (uint32_t)(y + 1) * 19349663u;
      hash ^= hash >> 13;
      hash *= 0x5bd1e995u;
      const uint32_t color = 0xff000000u | (hash & 0x00ffffffu);
      previous[y * kMaxWidth + x] = color;
      current[y * kMaxWidth + x] = x >= 2
          ? previous[y * kMaxWidth + x - 2]
          : 0xff000000u;
    }
  }
  PresentationFrameGenerationMotionField field;
  CHECK(PresentationFrameGeneration_Analyze(
      previous, current, kMaxWidth, kMaxHeight,
      kMaxWidth, kMaxWidth,
      kPresentationFrameGenerationAnalysis_Global, &field));
  CHECK(field.uniform);
  CHECK(field.forward_dx[0] == 2 && field.forward_dy[0] == 0);
  CHECK(field.backward_dx[0] == -2 && field.backward_dy[0] == 0);
  /* Exercise the maximum field size without conflating the motion-analysis
   * contract with production's renderer-backed synthesis path. */
  CHECK(field.width == kMaxWidth && field.height == kMaxHeight);
  CHECK(PresentationFrameGeneration_Analyze(
      previous, current, kMaxWidth, kMaxHeight,
      kMaxWidth, kMaxWidth,
      kPresentationFrameGenerationAnalysis_Blocks, &field));
  CHECK(field.blocks_x == kPresentationFrameGenerationMaximumBlocksX);
  CHECK(field.blocks_y == kPresentationFrameGenerationMaximumBlocksY);
}

int main(void) {
  TestIdentity();
  TestMovingPixels();
  TestPairPhase();
  TestInvalidDimensions();
  TestColorChangeIsNotMotion();
  TestMaximumSurface();
  printf("presentation frame-generation tests: %s\n",
         failures ? "FAIL" : "pass");
  return failures ? 1 : 0;
}
