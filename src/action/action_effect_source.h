#ifndef AR_ACTION_EFFECT_SOURCE_H
#define AR_ACTION_EFFECT_SOURCE_H

/* Deferred effect primitives. Recipes still run on the CPU, but clipping,
 * projection, billboard sizing and scenery shadows consume resident motion
 * on the GPU. This packet owns no renderer objects and borrows no game state. */
#include "action_effect_projection.h"

enum {
  kActionSourceTriangle = 0,
  kActionSourceParticle = 1,
  kActionSourceLeaf = 2,
  kActionSourceQuad = 3,
  kActionSourceStar = 4,
  kActionSourceGlowTriangle = 5,
  kActionSourceEmber = 6,
  kActionSourceRibbon = 7,
  kActionSourceLitTriangle = 8,
  kActionSourceFloorTriangle = 9,
  kActionSourceMaxLightJobs = 8,
  kActionSourceMaxLightPoints = 129 * 17,
  kActionSourceMaximumPrimitives = 16384,
  kActionSourceVerticesPerPrimitive = 15,
  kActionSourcePacketSlots = kActionEffectRenderLayer_Count + 2,
};

/* Thirteen 16-byte words; mirrored by action_effect_project.comp.glsl. */
typedef struct ActionEffectSourcePrimitive {
  uint32_t meta[4]; /* kind, projection plane, object priority, shadow enabled */
  float origin[4];  /* capture XY; flags: atmosphere=1, static=2, clipped=4, grid=8 */
  float clip[4];    /* effect-local authored bounds, before motion */
  float points[6][4]; /* Grid triangles: points[5] = world X, cell X, spacing. */
  float colors[3][4];
  float extra[4]; /* particle width/reach OR local shadow origin XY */
} ActionEffectSourcePrimitive;

/* One finite moon transport field. Each job owns its sample coordinates; the
 * short-lived occlusion pointer is copied to GPU storage before submission. */
typedef struct ActionEffectSourceLightData {
  float meta[4]; /* sample count, kind: haze/water/timber/insect, slope limit, reserved */
  float light[4], receiver[4]; /* capture origin XY, plane, static-anchor flag */
  float clip[4];
  float filter[4]; /* receiver world X, optional culling margins */
  float transport[4], surface[4];
  float rays[6][4], sources[9][4];
  float points[kActionSourceMaxLightPoints][4];
} ActionEffectSourceLightData;
typedef struct ActionEffectSourceLightJob {
  ActionEffectSourceLightData data;
  const ActionMoonlightOcclusion *occlusion;
} ActionEffectSourceLightJob;

typedef struct ActionEffectSourceBatch {
  ActionEffectProjectionContext context;
  ActionEffectSourcePrimitive *primitives;
  unsigned count, capacity;
  ActionEffectSourceLightJob *lights;
  unsigned light_count, light_capacity;
  bool lit_triangles;
  float light_cap, light_sample, light_base;
  bool failed;
  /* Optional immutable packet identity. Zero disables retention. A changed
   * capture (primitives and occluders) must receive a new revision, including
   * after CPU reset. */
  uint64_t revision;
  unsigned packet_slot;
} ActionEffectSourceBatch;

/* A marker callback: supported helpers emit primitives instead of projecting
 * points. An unexpected recipe use fails the entire batch, never drops art. */
bool ActionEffectSource_ProjectPoint(void *, const ActionEffectInstance *, float, float,
                                     ArRenderPointF *);
bool ActionEffectSource_ClipBounds(void *, const ActionEffectInstance *, ActionEffectLocalRect *);
bool ActionEffectSource_Triangle(ActionEffectSourceBatch *, const ActionEffectInstance *,
                                 const ArRenderVertex2D *, const int *,
                                 const ActionEffectLocalRect *);
bool ActionEffectSource_Particle(ActionEffectSourceBatch *, const ActionEffectInstance *, float,
                                 float, float, float, float, float, ArRenderColorF);
bool ActionEffectSource_Leaf(ActionEffectSourceBatch *, const ActionEffectInstance *,
                             const ArRenderPointF[6], ArRenderColorF, ArRenderColorF);
bool ActionEffectSource_FloorTriangle(ActionEffectSourceBatch *, const ActionEffectInstance *,
    const ActionEffectLocalRect *, const ArRenderPointF[3], float, float, float, float, unsigned, ArRenderColorF);
bool ActionEffectSource_Quad(ActionEffectSourceBatch *, const ActionEffectInstance *,
                             const ArRenderPointF[4], const ArRenderColorF[4]);
bool ActionEffectSource_Ribbon(ActionEffectSourceBatch *, const ActionEffectInstance *,
    const ArRenderPointF *, unsigned, float, float, ArRenderColorF);
bool ActionEffectSource_Star(ActionEffectSourceBatch *, const ActionEffectInstance *,
                             float, float, float, ArRenderColorF);
/* Each triangle corner stores the X/Y coefficients of the two projected radii.
 * Animation and palette stay in the shared recipe; only billboard sizing is deferred. */
bool ActionEffectSource_GlowTriangle(ActionEffectSourceBatch *, const ActionEffectInstance *,
    float, float, float, float, const float [3][4], const ArRenderColorF [3]);
ActionEffectSourceLightJob *ActionEffectSource_BeginLight(ActionEffectSourceBatch *,
    const ActionEffectInstance *, const ActionEffectInstance *, const ActionMoonField *,
    const ActionMoonlightOcclusion *, unsigned, const ActionEffectLocalRect *);
void ActionEffectSource_Shadow(ActionEffectSourceBatch *, unsigned begin, float, float, bool);
void ActionEffectSource_TintRange(ActionEffectSourceBatch *, unsigned begin, unsigned end, uint32_t, float, bool);
void ActionEffectSource_Tint(ActionEffectSourceBatch *, unsigned begin, uint32_t, float, bool);

#endif
