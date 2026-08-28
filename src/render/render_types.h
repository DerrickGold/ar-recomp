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
  /* Source is already multiplied by source alpha. Useful for accumulating an
   * alpha-bearing effect target without dimming its RGB a second time. */
  kArRenderBlendMode_AddPremultiplied,
  /* Preserve destination RGB and multiply destination alpha by source alpha.
   * This intersects an accumulated silhouette with a sampled mask. */
  kArRenderBlendMode_DestinationAlphaMask,
  kArRenderBlendMode_Modulate,
  kArRenderBlendMode_Multiply,
} ArRenderBlendMode;

typedef enum ArRenderTextureAddressMode {
  /* Use the backend's ordinary sampling rule for the texture. */
  kArRenderTextureAddressMode_Auto,
  kArRenderTextureAddressMode_Clamp,
  kArRenderTextureAddressMode_Wrap,
} ArRenderTextureAddressMode;

typedef uint32_t ArRenderDrawStateFlags;
enum {
  /* Multiply sampled texture color/alpha for this draw only. Geometry already
   * carries per-vertex color, so texture modulation is valid only for texture
   * quad draws. */
  kArRenderDrawState_Tint = UINT32_C(1) << 0,
  /* Override the texture's descriptor blend mode for this draw only. */
  kArRenderDrawState_Blend = UINT32_C(1) << 1,
  /* Override U/V sampling outside [0, 1] for this draw only. Addressing is
   * meaningful only for textured submissions and requires TextureWrap when
   * either axis selects Wrap. */
  kArRenderDrawState_Address = UINT32_C(1) << 2,
};

typedef struct ArRenderDrawState {
  ArRenderDrawStateFlags flags;
  ArRenderColorF tint;
  ArRenderBlendMode blend;
  ArRenderTextureAddressMode address_u;
  ArRenderTextureAddressMode address_v;
} ArRenderDrawState;

typedef struct ArRenderTextureDesc {
  int width;
  int height;
  ArRenderPixelFormat format;
  ArRenderTextureUsage usage;
  ArRenderFilter filter;
  ArRenderBlendMode blend;
} ArRenderTextureDesc;

/* Portable state needed to enter a temporary render target without leaking
 * its viewport or clip into the caller. Backends capture native state into
 * these values and remain responsible for any target-specific details that do
 * not appear in the public contract. */
typedef struct ArRenderTargetState {
  ArRenderTexture target;
  ArRenderRectI viewport;
  ArRenderRectI clip;
  bool viewport_set;
  bool clip_enabled;
  bool valid;
} ArRenderTargetState;

typedef enum ArRenderTargetBeginResult {
  /* The target is bound with its full extent and clipping disabled. */
  kArRenderTargetBegin_Ready,
  /* State capture or target binding failed without losing caller state. */
  kArRenderTargetBegin_Omitted,
  /* Entering the target failed and the prior state could not be restored. */
  kArRenderTargetBegin_StateLost,
} ArRenderTargetBeginResult;

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
  /* Capture and restore a temporary render target together with its viewport
   * and clip. This is optional even when ordinary render targets exist. */
  kArRenderCapability_ScopedRenderTargets = UINT64_C(1) << 9,
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
