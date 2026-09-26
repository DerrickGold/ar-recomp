#ifndef RENDER_CAPABILITIES_H
#define RENDER_CAPABILITIES_H

#include <stdbool.h>
#include <stdint.h>

/* Application feature support resolved before gameplay. These are semantic
 * features, not native API/device identifiers or runner ABI fields. */
typedef uint32_t RenderFeatureMask;
enum {
  kRenderFeature_Depth = 1u << 0,
  kRenderFeature_ConnectedGlobe = 1u << 1,
  kRenderFeature_DioramaBlur = 1u << 2,
  kRenderFeature_DioramaRim = 1u << 3,
  kRenderFeature_DioramaDof = 1u << 4,
  kRenderFeature_Crt = 1u << 5,
  kRenderFeature_SimRim = 1u << 6,
  kRenderFeature_Effects = 1u << 7,
  kRenderFeature_SimSoftShadows = 1u << 8,
  kRenderFeature_All = (1u << 9) - 1,
};

/* Read-only renderer capability boundary used by Settings availability gates.
 * The main-thread presentation path owns detection and reset; callers must not
 * mutate the underlying latches or depend on which backend operation rejected
 * them. */
bool Present_SimRimMaskSupported(void);
bool Present_EffectRendererSupported(void);

#endif
