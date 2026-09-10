#include "localization/enhanced_text_settings.h"

#include <limits.h>
#include <stddef.h>

#define AR_MEMBER_END(type, member)                                             \
  (offsetof(type, member) + sizeof(((type *)0)->member))

void ArEnhancedTextSettings_Defaults(ArEnhancedTextSettings *settings) {
  if (!settings)
    return;
  *settings = (ArEnhancedTextSettings){
      .struct_size = sizeof(*settings),
      .abi_version = AR_ENHANCED_TEXT_SETTINGS_ABI_VERSION,
      .size_percent = 140,
      .sampling = kArEnhancedTextSampling_Crisp,
      .pixelation = kArEnhancedTextPixelation_Mosaic,
      .pixelation_size = 4,
  };
}

static bool ValidPixelationSize(int size) {
  return size == 2 || size == 4 || size == 6 || size == 8;
}

bool ArEnhancedTextSettings_IsValid(const ArEnhancedTextSettings *settings) {
  return settings &&
         settings->struct_size >=
             AR_MEMBER_END(ArEnhancedTextSettings, pixelation_size) &&
         settings->abi_version == AR_ENHANCED_TEXT_SETTINGS_ABI_VERSION &&
         settings->size_percent >= kArEnhancedTextMinimumSizePercent &&
         settings->size_percent <= kArEnhancedTextMaximumSizePercent &&
         (settings->size_percent - kArEnhancedTextMinimumSizePercent) %
                 kArEnhancedTextSizePercentStep ==
             0 &&
         settings->sampling >= kArEnhancedTextSampling_Crisp &&
         settings->sampling <= kArEnhancedTextSampling_Smooth &&
         settings->pixelation >= kArEnhancedTextPixelation_None &&
         settings->pixelation <= kArEnhancedTextPixelation_Mosaic &&
         ((settings->pixelation == kArEnhancedTextPixelation_None &&
           settings->pixelation_size == 0) ||
          (settings->pixelation != kArEnhancedTextPixelation_None &&
           ValidPixelationSize(settings->pixelation_size)));
}

int ArEnhancedTextSettings_ScaledPixels(const ArEnhancedTextSettings *settings,
                                        int base_pixels) {
  if (!ArEnhancedTextSettings_IsValid(settings) || base_pixels <= 0 ||
      base_pixels > (INT_MAX - 50) / settings->size_percent)
    return 0;
  const int scaled =
      (base_pixels * settings->size_percent + 50) / 100;
  return scaled > 0 ? scaled : 1;
}

bool ArEnhancedTextSettings_Apply(const ArEnhancedTextSettings *settings,
                                  int base_pixels, int minimum_base_pixels,
                                  ArTextRasterRequest *request) {
  if (!request || !ArTextRasterRequest_IsValid(request) ||
      minimum_base_pixels <= 0 || minimum_base_pixels > base_pixels)
    return false;
  const int pixels =
      ArEnhancedTextSettings_ScaledPixels(settings, base_pixels);
  const int minimum =
      ArEnhancedTextSettings_ScaledPixels(settings, minimum_base_pixels);
  if (!pixels || !minimum || minimum > pixels)
    return false;
  request->font_pixels = pixels;
  request->minimum_font_pixels = minimum;
  request->filter = settings->sampling == kArEnhancedTextSampling_Crisp
                        ? kArRenderFilter_Nearest
                        : kArRenderFilter_Linear;
  request->pixelation = (ArTextPixelation)settings->pixelation;
  request->pixelation_size = settings->pixelation_size;
  return ArTextRasterRequest_IsValid(request);
}
