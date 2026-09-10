#include "localization/enhanced_text_settings.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,         \
              #condition);                                                      \
      failures++;                                                               \
    }                                                                            \
  } while (0)

static ArTextRasterRequest Request(void) {
  static const char text[] = "Accented UTF-8: Élise";
  static const char stack[] = "test-stack";
  return (ArTextRasterRequest){
      .struct_size = sizeof(ArTextRasterRequest),
      .abi_version = AR_TEXT_RASTER_REQUEST_ABI_VERSION,
      .utf8 = text,
      .utf8_bytes = sizeof(text) - 1,
      .font_stack_id = stack,
      .font_stack_id_bytes = sizeof(stack) - 1,
      .source_revision = 1,
      .font_revision = 1,
      .style_id = kArTextStyle_RetailBlueWhiteBands,
      .flags = kArTextRasterFlag_WrapWords,
      .direction = kArTextDirection_LeftToRight,
      .alignment = kArTextHorizontalAlignment_Leading,
      .font_pixels = 10,
      .minimum_font_pixels = 8,
      .maximum_width = 640,
      .maximum_height = 480,
      .filter = kArRenderFilter_Linear,
  };
}

int main(void) {
  ArEnhancedTextSettings settings;
  ArEnhancedTextSettings_Defaults(&settings);
  CHECK(ArEnhancedTextSettings_IsValid(&settings));
  CHECK(settings.size_percent == 140);
  CHECK(settings.sampling == kArEnhancedTextSampling_Crisp);
  CHECK(settings.pixelation == kArEnhancedTextPixelation_Mosaic);
  CHECK(settings.pixelation_size == 2);
  CHECK(ArEnhancedTextSettings_ScaledPixels(&settings, 20) == 28);

  ArTextRasterRequest request = Request();
  CHECK(ArEnhancedTextSettings_Apply(&settings, 20, 15, &request));
  CHECK(request.font_pixels == 28);
  CHECK(request.minimum_font_pixels == 21);
  CHECK(request.filter == kArRenderFilter_Nearest);
  CHECK(request.pixelation == kArTextPixelation_Mosaic);
  CHECK(request.pixelation_size == 2);

  for (int percent = kArEnhancedTextMinimumSizePercent;
       percent <= kArEnhancedTextMaximumSizePercent;
       percent += kArEnhancedTextSizePercentStep) {
    settings.size_percent = percent;
    CHECK(ArEnhancedTextSettings_IsValid(&settings));
  }
  settings.size_percent = 141;
  CHECK(!ArEnhancedTextSettings_IsValid(&settings));
  settings.size_percent = 82;
  CHECK(!ArEnhancedTextSettings_IsValid(&settings));
  settings.size_percent = 140;

  const int pixel_sizes[] = {2, 4, 6, 8};
  for (size_t i = 0; i < sizeof(pixel_sizes) / sizeof(pixel_sizes[0]); i++) {
    settings.pixelation_size = pixel_sizes[i];
    CHECK(ArEnhancedTextSettings_IsValid(&settings));
  }
  settings.pixelation_size = 3;
  CHECK(!ArEnhancedTextSettings_IsValid(&settings));
  settings.pixelation = kArEnhancedTextPixelation_None;
  settings.pixelation_size = 0;
  settings.sampling = kArEnhancedTextSampling_Smooth;
  CHECK(ArEnhancedTextSettings_IsValid(&settings));
  request = Request();
  CHECK(ArEnhancedTextSettings_Apply(&settings, 20, 20, &request));
  CHECK(request.filter == kArRenderFilter_Linear);
  CHECK(request.pixelation == kArTextPixelation_None);

  if (failures)
    return 1;
  puts("enhanced text settings checks passed");
  return 0;
}
