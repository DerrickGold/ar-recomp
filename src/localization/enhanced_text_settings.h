#ifndef AR_LOCALIZATION_ENHANCED_TEXT_SETTINGS_H
#define AR_LOCALIZATION_ENHANCED_TEXT_SETTINGS_H

#include "localization/text_rasterizer.h"

#include <stdbool.h>
#include <stdint.h>

#define AR_ENHANCED_TEXT_SETTINGS_ABI_VERSION UINT32_C(1)

enum {
  kArEnhancedTextMinimumSizePercent = 80,
  kArEnhancedTextMaximumSizePercent = 140,
  kArEnhancedTextSizePercentStep = 5,
};

typedef enum ArEnhancedTextSampling {
  kArEnhancedTextSampling_Crisp = 0,
  kArEnhancedTextSampling_Smooth,
} ArEnhancedTextSampling;

typedef enum ArEnhancedTextPixelation {
  kArEnhancedTextPixelation_None = 0,
  kArEnhancedTextPixelation_LowResolution,
  kArEnhancedTextPixelation_Mosaic,
} ArEnhancedTextPixelation;

typedef struct ArEnhancedTextSettings {
  uint32_t struct_size;
  uint32_t abi_version;
  int32_t size_percent;
  ArEnhancedTextSampling sampling;
  ArEnhancedTextPixelation pixelation;
  int32_t pixelation_size;
} ArEnhancedTextSettings;

void ArEnhancedTextSettings_Defaults(ArEnhancedTextSettings *settings);
bool ArEnhancedTextSettings_IsValid(const ArEnhancedTextSettings *settings);
int ArEnhancedTextSettings_ScaledPixels(const ArEnhancedTextSettings *settings,
                                        int base_pixels);
bool ArEnhancedTextSettings_Apply(const ArEnhancedTextSettings *settings,
                                  int base_pixels, int minimum_base_pixels,
                                  ArTextRasterRequest *request);

#endif /* AR_LOCALIZATION_ENHANCED_TEXT_SETTINGS_H */
