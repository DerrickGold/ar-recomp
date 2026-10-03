#ifndef AR_ACTION_EFFECT_RENDER_INTERNAL_H
#define AR_ACTION_EFFECT_RENDER_INTERNAL_H
/* ActionEffectRender internals: the geometry writer, glow styles and scene
 * particle clocks the effect builders share, and the helpers one builder calls
 * in another. Not a public API; only the action effect renderers include it.
 * Phase: pure. */
#include "action/action_effect_render.h"
#include "action/action_effect_source.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "deterministic_hash.h"




static const float kCircle32[kActionEffectGlowSegments][2] = {
  { 1.000000f, 0.000000f }, { 0.980785f, 0.195090f },
  { 0.923880f, 0.382683f }, { 0.831470f, 0.555570f },
  { 0.707107f, 0.707107f }, { 0.555570f, 0.831470f },
  { 0.382683f, 0.923880f }, { 0.195090f, 0.980785f },
  { 0.000000f, 1.000000f }, { -0.195090f, 0.980785f },
  { -0.382683f, 0.923880f }, { -0.555570f, 0.831470f },
  { -0.707107f, 0.707107f }, { -0.831470f, 0.555570f },
  { -0.923880f, 0.382683f }, { -0.980785f, 0.195090f },
  { -1.000000f, 0.000000f }, { -0.980785f, -0.195090f },
  { -0.923880f, -0.382683f }, { -0.831470f, -0.555570f },
  { -0.707107f, -0.707107f }, { -0.555570f, -0.831470f },
  { -0.382683f, -0.923880f }, { -0.195090f, -0.980785f },
  { 0.000000f, -1.000000f }, { 0.195090f, -0.980785f },
  { 0.382683f, -0.923880f }, { 0.555570f, -0.831470f },
  { 0.707107f, -0.707107f }, { 0.831470f, -0.555570f },
  { 0.923880f, -0.382683f }, { 0.980785f, -0.195090f },
};

/* Capacity-aware view over either public batch type. Geometry builders write
 * directly into their final destination; no spell-sized scene scratch batch,
 * index rebasing, or capacity coupling is required. Counts are committed to
 * the public batch only after the complete build succeeds, so any overflow
 * leaves a zero-count, fail-closed output. */
typedef struct ActionSceneryShadow {
  uint8_t *pixels;
  float x, y, step;
  bool valid;
} ActionSceneryShadow;
typedef struct ActionEffectGeometryWriter {
  ArRenderVertex2D *vertices;
  int32_t *indices;
  int vertex_count;
  int index_count;
  int vertex_capacity;
  int index_capacity;
  const ActionSceneryShadow *shadow;
  ActionEffectSourceBatch *source;
} ActionEffectGeometryWriter;
bool ActionSceneryShadow_Prepare(ActionSceneryShadow *shadow, ActionSceneryShadowCache *cache,
                                 const ActionMoonlightOcclusion *occlusion,
                                 ActionEffectProjectPointFn project, ActionEffectClipBoundsFn clip,
                                 void *context);
void ActionSceneryShadow_Apply(ActionEffectGeometryWriter *writer, int begin,
                               const ActionEffectInstance *effect, float x, float y, bool multiply,
                               ActionEffectProjectPointFn project, void *context);

typedef struct SceneParticleClock {
  uint32_t seed;
  float t, previous_t;
} SceneParticleClock;

typedef struct SceneParticleLifetime {
  unsigned minimum, seed_shift, seed_mask;
} SceneParticleLifetime;

static const SceneParticleLifetime kSceneEmberLifetime = {21, 5, 15};

/* ---- defined in action_effect_render.c ---- */
float HashUnit(uint32_t value);
ActionEffectGeometryWriter GeometryWriter(
    ArRenderVertex2D *vertices, int vertex_capacity,
    int32_t *indices, int index_capacity);
bool Reserve(const ActionEffectGeometryWriter *writer,
                    int vertices, int indices);
float TriangleWave(unsigned ticks, unsigned period);
unsigned EffectVisualTicks(const ActionEffectInstance *effect,
                                  unsigned ticks);
float DeterministicPulse(const ActionEffectInstance *effect);
float FlameSilhouette(unsigned seed, unsigned ticks, int segment);
ArRenderColorF MixColor(ArRenderColorF a, ArRenderColorF b, float t);
bool RectIsSane(const ActionEffectLocalRect *rect);
bool ProjectWithScale(const ActionEffectInstance *effect,
                             ActionEffectProjectPointFn project_point,
                             void *userdata, float local_x, float local_y,
                             ArRenderPointF *anchor,
                             float *scale_x, float *scale_y);
bool AppendGlowAtTicks(ActionEffectGeometryWriter *, const ActionEffectInstance *,
    const ActionEffectGlowStyle *, float, float, float, ActionEffectProjectPointFn, void *, unsigned);
bool AppendGlow(ActionEffectGeometryWriter *writer,
                       const ActionEffectInstance *effect,
                       const ActionEffectGlowStyle *style, float strength,
                       float local_x, float local_y,
                       ActionEffectProjectPointFn project_point,
                       void *userdata);

/* ---- defined in action_scene_effect_render.c ---- */
bool AppendSceneSoftCloud(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    float local_x, float local_y, float radius_x, float radius_y,
    ArRenderColorF tint, float opacity, unsigned seed,
    ActionEffectProjectPointFn project_point, void *userdata);
bool SceneActorHeading(const ActionEffectInstance *effect,
                              float *x, float *y);
bool AppendAuthoredField(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,bool lighting,bool particles,
    ActionEffectProjectPointFn project,ActionEffectClipBoundsFn clip,void *context);
bool AppendAuthoredEnvironment(ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect, const ActionEffectFloorField *floor, bool lighting, bool particles,
    ActionEffectProjectPointFn project, ActionEffectClipBoundsFn clip, void *context);
bool AppendSceneParticle(ActionEffectGeometryWriter *writer,
                                const ActionEffectInstance *effect,
                                float x, float y, float previous_x,
                                float previous_y, float width, float reach,
                                ArRenderColorF color,
                                ActionEffectProjectPointFn project_point,
                                void *userdata);
bool AppendSceneStarParticle(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    float local_x, float local_y, float size, ArRenderColorF color,
    ActionEffectProjectPointFn project_point, void *userdata);
SceneParticleClock
SceneParticleClockAt(const ActionEffectInstance *effect, unsigned visual_ticks,
                     unsigned index, SceneParticleLifetime timing);

ArRenderColorF SceneParticleColor(ArRenderColorF hot,
                                         ArRenderColorF cool,
                                         SceneParticleClock clock);

/* Small shared per-vertex operation stays inline across stage modules. */
static inline float SceneSoftFalloff(float distance) {
  const float t = fmaxf(0, 1-distance*distance);
  return t*t;
}

/* ---- defined in action_environment_geometry.c ---- */
bool AppendSourceBillboard(ActionEffectGeometryWriter *, const ActionEffectInstance *,
    float, float, float, float, float, float, unsigned, const float (*)[4], const ArRenderColorF *);
void TintEffectMember(ActionEffectGeometryWriter *, const ActionNativeMember *, int, int, bool);
bool AppendSceneClippedTriangle(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ArRenderVertex2D *source, int *mapped, const int *triangle,
    const ActionEffectLocalRect *clip, ActionEffectProjectPointFn project_point, void *userdata);
bool AppendSceneSoftPatch(ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect,
    const ActionEffectLocalRect *clip, float x, float y, float rx, float ry,
    ArRenderColorF color, float lean, ActionEffectProjectPointFn project_point, void *userdata);

/* ---- defined in action_ray_field_render.c ---- */
bool AppendForestRays(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
                      const ActionRayField *field, const ActionNativeMembers *members, bool foreground,
                      ActionEffectProjectPointFn project_point,
                      ActionEffectClipBoundsFn clip_bounds, void *userdata);
bool AppendForestMotes(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
                       const ActionRayField *field, const ActionNativeMembers *members, ActionEffectProjectPointFn project_point,
                       void *userdata);
bool AppendForestLeaves(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
                        const ActionRayField *field, const ActionNativeMembers *members,
                        ActionEffectProjectPointFn project_point, void *userdata);

/* ---- defined in action_castle_effect_render.c ---- */
bool AppendCastleEnvironment(const ActionCastleField *f,ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
                             const ActionNativeMembers *members, bool lighting, bool particles,
                             ActionEffectProjectPointFn project_point,
                             ActionEffectClipBoundsFn clip_bounds, void *userdata);

/* ---- defined in action_bloodpool_effect_render.c ---- */
typedef struct BloodpoolMoonProjection {
  ArRenderPointF origin, axis, vertical;
  float orientation;
  const ActionMoonField *field;
} BloodpoolMoonProjection;
bool BloodpoolMoonProjection_Init(const ActionMoonField *f, BloodpoolMoonProjection *projection,
    const ActionEffectInstance *moon, ActionEffectProjectPointFn project_point, void *userdata);
float BloodpoolMoonProjection_Light(
    const BloodpoolMoonProjection *projection, ArRenderPointF point);
bool AppendBloodpoolTimberMoonlight(const ActionMoonField *f, const ActionMarshField *m, ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect, const ActionEffectInstance *moon,
    const ActionBloodpoolDetails *details, const ActionMoonlightOcclusion *occlusion,
    ActionMoonlightRenderScratch *scratch, ActionEffectProjectPointFn project_point,
    ActionEffectClipBoundsFn clip_bounds, void *userdata);
bool AppendBloodpoolEnvironment(const ActionMoonField *f, const ActionMarshField *m,
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, ActionEffectClipBoundsFn clip_bounds, void *userdata);
bool AppendBloodpoolSkyRays(const ActionMoonField *f,float gain,ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect, ActionEffectProjectPointFn project_point,
    ActionEffectClipBoundsFn clip_bounds, void *userdata);
bool AppendBloodpoolMoonlight(const ActionMoonField *f,
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    const ActionMoonlightOcclusion *occlusion, ActionMoonlightRenderScratch *scratch,
    ActionEffectProjectPointFn project_point,
    ActionEffectClipBoundsFn clip_bounds, void *userdata);
bool AppendBloodpoolWaterMoonlight(const ActionMoonField *f, const ActionMarshField *m,
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *water,
    const ActionEffectInstance *moon, const ActionMoonlightOcclusion *occlusion,
    ActionMoonlightRenderScratch *scratch, ActionEffectProjectPointFn project_point,
    ActionEffectClipBoundsFn clip_bounds, void *userdata);
bool AppendBloodpoolWaveCaps(const ActionMoonField *f, ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    const ActionBloodpoolDetails *details, ActionEffectProjectPointFn project_point,
    ActionEffectClipBoundsFn clip_bounds, void *userdata);

/* ---- defined in action_bloodpool_detail_render.c ---- */
float BloodpoolCloudTransmission(const ActionMoonField *f, uint16_t ticks);
bool AppendBloodpoolCloud(const ActionMoonField *f, ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, ActionEffectClipBoundsFn clip_bounds, void *userdata);
bool AppendBloodpoolDetailParticles(const ActionMoonField *f, const ActionMarshField *m, ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect, const ActionEffectInstance *moon,
    const ActionBloodpoolDetails *details, ActionEffectProjectPointFn project_point,
    ActionEffectClipBoundsFn clip_bounds, void *userdata);

/* ---- defined in action_cave_effect_render.c ---- */
bool AppendCaveEnvironment(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
                           const ActionWaterField *definition, const ActionAtmosphereField *atmosphere, const ActionNativeMembers *members,
                           ActionEffectProjectPointFn project_point,
                           ActionEffectClipBoundsFn clip_bounds, void *userdata);

/* ---- defined in action_scene_lightning_render.c ---- */
bool AppendSwordBeamParticles(ActionEffectGeometryWriter *writer,
                                     const ActionEffectInstance *effect,
                                     ActionEffectProjectPointFn project_point,
                                     void *userdata);
bool AppendSwordBeamLighting(ActionEffectGeometryWriter *writer,
                                    const ActionEffectInstance *effect,
                                    ActionEffectProjectPointFn project_point,
                                    void *userdata);
bool AppendMarahnaLightningLinkParticles(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata);
bool AppendMarahnaLightningLinkLighting(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata);
bool AppendMarahnaBossLightningParticles(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata);
bool AppendMarahnaBossLightningLighting(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata);
bool AppendLightningTrapParticles(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect, const ActionArcField *field,
    ActionEffectProjectPointFn project_point, void *userdata);
bool AppendLightningTrapLighting(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect, const ActionArcField *field,
    ActionEffectProjectPointFn project_point, void *userdata);
bool AppendBossLightningParticles(ActionEffectGeometryWriter *writer,
                                  const ActionEffectInstance *effect, const ActionArcField *field,
                                  ActionEffectProjectPointFn project_point, void *userdata);
bool AppendBossLightningLighting(ActionEffectGeometryWriter *writer,
                                 const ActionEffectInstance *effect, const ActionArcField *field,
                                 ActionEffectProjectPointFn project_point, void *userdata);

#endif  /* AR_ACTION_EFFECT_RENDER_INTERNAL_H */
