#ifndef AR_RENDER_TYPES_H
#define AR_RENDER_TYPES_H

/* Backend-neutral rendering vocabulary.
 *
 * Game and presentation code may include this header. Platform implementations
 * translate these values into SDL, Citro3D, GXM, or another native API. Keep
 * native graphics types and headers out of this boundary. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ArRenderTexture {
  uintptr_t value;
} ArRenderTexture;

static inline ArRenderTexture ArRenderTexture_Invalid(void) {
  return (ArRenderTexture){0};
}

static inline bool ArRenderTexture_IsValid(ArRenderTexture texture) {
  return texture.value != 0;
}

static inline bool ArRenderTexture_Equals(ArRenderTexture left,
                                          ArRenderTexture right) {
  return left.value == right.value;
}

typedef struct ArRenderRectI {
  int x;
  int y;
  int w;
  int h;
} ArRenderRectI;

typedef struct ArRenderRectF {
  float x;
  float y;
  float w;
  float h;
} ArRenderRectF;

typedef struct ArRenderPointF {
  float x;
  float y;
} ArRenderPointF;

typedef struct ArRenderColorF {
  float r;
  float g;
  float b;
  float a;
} ArRenderColorF;

/* Backends may consume the portable 2D geometry array directly when their
 * native layout is compatible or translate one batch at submission time. */
typedef struct ArRenderVertex2D {
  ArRenderPointF position;
  ArRenderColorF color;
  ArRenderPointF tex_coord;
} ArRenderVertex2D;

typedef enum ArRenderPixelFormat {
  /* Packed 32-bit pixel words. Component names run most- to least-significant
   * so ARGB8888 is 0xAARRGGBB and ABGR8888 is 0xAABBGGRR. */
  kArRenderPixelFormat_Argb8888,
  kArRenderPixelFormat_Abgr8888,
  kArRenderPixelFormat_Rgba8888,
  kArRenderPixelFormat_Rgb565,
  kArRenderPixelFormat_Rgba4444,
  kArRenderPixelFormat_A8,
} ArRenderPixelFormat;

typedef enum ArRenderTextureUsage {
  kArRenderTextureUsage_Static,
  kArRenderTextureUsage_Streaming,
  kArRenderTextureUsage_Target,
} ArRenderTextureUsage;

typedef enum ArRenderFilter {
  kArRenderFilter_Nearest,
  kArRenderFilter_Linear,
} ArRenderFilter;

typedef enum ArRenderBlendMode {
  kArRenderBlendMode_Opaque,
  kArRenderBlendMode_Alpha,
  kArRenderBlendMode_Add,
  kArRenderBlendMode_Modulate,
  kArRenderBlendMode_Multiply,
} ArRenderBlendMode;

typedef struct ArRenderTextureDesc {
  int width;
  int height;
  ArRenderPixelFormat format;
  ArRenderTextureUsage usage;
  ArRenderFilter filter;
  ArRenderBlendMode blend;
} ArRenderTextureDesc;

typedef uint64_t ArRenderCapabilityFlags;
enum {
  kArRenderCapability_StreamingTextures = UINT64_C(1) << 0,
  kArRenderCapability_RenderTargets = UINT64_C(1) << 1,
  kArRenderCapability_Geometry = UINT64_C(1) << 2,
  kArRenderCapability_Depth = UINT64_C(1) << 3,
  kArRenderCapability_CustomShaders = UINT64_C(1) << 4,
  kArRenderCapability_TextureWrap = UINT64_C(1) << 5,
  kArRenderCapability_BlendAdd = UINT64_C(1) << 6,
  kArRenderCapability_BlendModulate = UINT64_C(1) << 7,
  kArRenderCapability_BlendMultiply = UINT64_C(1) << 8,
};

typedef struct ArRenderCapabilities {
  ArRenderCapabilityFlags flags;
  int maximum_texture_width;
  int maximum_texture_height;
  int maximum_render_target_width;
  int maximum_render_target_height;
} ArRenderCapabilities;

static inline bool ArRenderCapabilities_Has(
    const ArRenderCapabilities *capabilities,
    ArRenderCapabilityFlags required) {
  return capabilities &&
      (capabilities->flags & required) == required;
}

#endif /* AR_RENDER_TYPES_H */
