#ifndef ACTION_EFFECT_RENDER_H
#define ACTION_EFFECT_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#include "action_effects.h"
#include "render/render_types.h"

enum {
  kActionEffectGlowSegments = 32,
  /* Segment count is a power of two, so animated table indices wrap with this
   * mask without repeating the literal 31 throughout the renderer. */
  kActionEffectGlowSegmentMask = kActionEffectGlowSegments - 1,
  /* Each glow is one multi-ring gradient mesh, not a stack of overlapping
   * discs: a hot core ring, a body ring, and a transparent aura ring, joined
   * by triangle strips. Overlapping discs would double-blend along every
   * shared edge under additive blending and band visibly; a single mesh gives
   * a continuous falloff with one colour stop per ring. */
  kActionEffectGlowRings = 3,
  kActionEffectGlowVertices =
      1 + kActionEffectGlowSegments * kActionEffectGlowRings,
  kActionEffectGlowIndices =
      kActionEffectGlowSegments * 3 +
      kActionEffectGlowSegments * 6 * (kActionEffectGlowRings - 1),

  /* One wide soft light-spill for the burst, plus one saturated flame body
   * per CLUSTER of touching parts. The worst case is every part landing in
   * its own cluster, hence +1 rather than +2 — the hot core is the flame
   * mesh's own centre vertex, not a glow of its own. */
  kActionEffectMaxGlows = kActionEffectMaxInstances + 1,
  /* Embers belong to the burst, not to a part, so this is a whole-effect
   * budget rather than a per-instance one — four quadrants must not each
   * throw their own separate spray. */
  /* 64, not 48: an impact splits this budget across every simultaneous burst
   * (Stardust detonates up to four at once), and a dozen sparks per blast is
   * too thin to read once half of them are still inside the source sprite. */
  kActionEffectMaxEmbers = 64,

  kActionEffectRenderMaxVertices =
      kActionEffectMaxGlows * kActionEffectGlowVertices +
      kActionEffectMaxEmbers * 4,
  kActionEffectRenderMaxIndices =
      kActionEffectMaxGlows * kActionEffectGlowIndices +
      kActionEffectMaxEmbers * 6,

  /* Scene actors remain independent light sources. Two gradient meshes give
   * each a soft environmental spill plus a tighter luminous body; the small
   * fixed spark budget keeps the worst-case batch bounded and deterministic. */
  kActionSceneEffectGlowsPerInstance = 2,
  kActionSceneEffectParticlesPerInstance = 12,
  kActionSceneEffectMaxGlows =
      kActionSceneEffectMaxInstances * kActionSceneEffectGlowsPerInstance,
  kActionSceneEffectMaxParticles =
      kActionSceneEffectMaxInstances * kActionSceneEffectParticlesPerInstance,
  /* Act 2's widest camera can see several hundred source pixels of one lava
   * lip. A denser fixed field keeps ambient sparks from collapsing to two or
   * three visible motes, while an explicit four-lake ceiling retains a hard
   * geometry bound for malformed frames. */
  kActionSceneEffectLavaReservoirParticleCount = 32,
  kActionSceneEffectMaxLavaReservoirs = 4,
  kActionSceneEffectLavaReservoirParticleExtraVertices =
      kActionSceneEffectMaxLavaReservoirs *
      (kActionSceneEffectLavaReservoirParticleCount -
       kActionSceneEffectParticlesPerInstance) * 4,
  kActionSceneEffectLavaReservoirParticleExtraIndices =
      kActionSceneEffectMaxLavaReservoirs *
      (kActionSceneEffectLavaReservoirParticleCount -
       kActionSceneEffectParticlesPerInstance) * 6,
  /* A single ellipse over a 640px lake becomes transparent at its ends. Keep
   * the light local with overlapping <=96px lip segments; twelve covers the
   * measured maximum camera-local set while retaining a hard malformed-frame
   * ceiling. The ordinary per-instance budget already reserves two glows, so
   * the conservative extra scratch remains explicitly bounded. */
  kActionSceneEffectLavaGlowSpanPixels = 96,
  kActionSceneEffectMaxLavaGlowSegments = 12,
  kActionSceneEffectLavaReservoirGlowExtraVertices =
      kActionSceneEffectMaxLavaGlowSegments * 2 *
      kActionEffectGlowVertices,
  kActionSceneEffectLavaReservoirGlowExtraIndices =
      kActionSceneEffectMaxLavaGlowSegments * 2 *
      kActionEffectGlowIndices,
  /* The wheel composition authors twelve distinct 16x16 fireball parts. The
   * ordinary two-glow budget still covers its core/spill; these are the exact
   * extra flame shells anchored to the rim. Only one root wheel is legal. */
  kActionSceneEffectMaxFlamingWheels = 1,
  kActionSceneEffectFlamingWheelFireballs = 12,
  kActionSceneEffectFlamingWheelExtraVertices =
      kActionSceneEffectFlamingWheelFireballs *
      kActionEffectGlowVertices,
  kActionSceneEffectFlamingWheelExtraIndices =
      kActionSceneEffectFlamingWheelFireballs *
      kActionEffectGlowIndices,
  /* Bottom waterfall atmosphere uses four tiers of six low-poly cloud puffs
   * plus thirty-two deterministic foam/mist motes. A dedicated 12-segment
   * primitive keeps the volume dense without paying the generic 32-segment
   * light-glow cost for every puff. */
  kActionSceneEffectWaterfallMistCloudCount = 24,
  kActionSceneEffectWaterfallMistCloudSegments = 12,
  kActionSceneEffectWaterfallMistCloudVertices =
      1 + kActionSceneEffectWaterfallMistCloudSegments *
          kActionEffectGlowRings,
  kActionSceneEffectWaterfallMistCloudIndices =
      kActionSceneEffectWaterfallMistCloudSegments * 3 +
      kActionSceneEffectWaterfallMistCloudSegments * 6 *
          (kActionEffectGlowRings - 1),
  kActionSceneEffectWaterfallMistParticleCount = 32,
  /* The boss strike adds two screen-space filament layers over at most 24
   * authored OAM-row segments: a broad amber corona and a narrow white-gold
   * core.
   * The mapped room has one boss and its lifecycle can publish only one active
   * strike (the separate floor child is Impact), so keep that cardinality in
   * the bounded contract rather than inflating every scene slot by 80 verts. */
  kActionSceneEffectMaxLightningFilaments = 1,
  kActionSceneEffectLightningSegments = 24,
  kActionSceneEffectLightningLayers = 2,
  kActionSceneEffectLightningVerticesPerInstance =
      kActionSceneEffectLightningSegments * 4 *
      kActionSceneEffectLightningLayers,
  kActionSceneEffectLightningIndicesPerInstance =
      kActionSceneEffectLightningSegments * 6 *
      kActionSceneEffectLightningLayers,
  /* Marahna's connector emitter authors at most five simultaneous links.
   * Each $4AA1/$4B82 composition contains ten tiles along the horizontal or
   * vertical chord, so the enhancement reserves two ten-segment ribbons per
   * link and rejects impossible sixth-link input. */
  kActionSceneEffectMaxMarahnaLightningLinks = 5,
  kActionSceneEffectMarahnaLightningSegments = 10,
  kActionSceneEffectMarahnaLightningLayers = 2,
  kActionSceneEffectMarahnaLightningVerticesPerInstance =
      kActionSceneEffectMarahnaLightningSegments * 4 *
      kActionSceneEffectMarahnaLightningLayers,
  kActionSceneEffectMarahnaLightningIndicesPerInstance =
      kActionSceneEffectMarahnaLightningSegments * 6 *
      kActionSceneEffectMarahnaLightningLayers,
  /* A charge pose can overlap its one launched diagonal bolt. Only the
   * diagonal stage adds filament geometry beyond the ordinary two-glow/
   * particle budget; its post-impact ground charge uses that ordinary budget. */
  kActionSceneEffectMaxMarahnaBossLightningBolts = 1,
  kActionSceneEffectMarahnaBossLightningSegments = 8,
  kActionSceneEffectMarahnaBossLightningLayers = 2,
  kActionSceneEffectMarahnaBossLightningVerticesPerInstance =
      kActionSceneEffectMarahnaBossLightningSegments * 4 *
      kActionSceneEffectMarahnaBossLightningLayers,
  kActionSceneEffectMarahnaBossLightningIndicesPerInstance =
      kActionSceneEffectMarahnaBossLightningSegments * 6 *
      kActionSceneEffectMarahnaBossLightningLayers,
  /* One player crescent can coexist with the Aitos boss's two-child diagonal
   * volley. Their shared magical path uses fixed crossed stars (8 vertices/
   * 12 indices) plus two wake quads. Reserve only the measured three-stream
   * peak above an ordinary scene actor's 12-quad particle budget. */
  kActionSceneEffectMaxSwordStreams = 3,
  kActionSceneEffectSwordStarCount = 48,
  kActionSceneEffectSwordWakeLayers = 2,
  kActionSceneEffectSwordVerticesPerInstance =
      kActionSceneEffectSwordStarCount * 8 +
      kActionSceneEffectSwordWakeLayers * 4,
  kActionSceneEffectSwordIndicesPerInstance =
      kActionSceneEffectSwordStarCount * 12 +
      kActionSceneEffectSwordWakeLayers * 6,
  kActionSceneEffectSwordExtraVertices =
      kActionSceneEffectSwordVerticesPerInstance -
      kActionSceneEffectParticlesPerInstance * 4,
  kActionSceneEffectSwordExtraIndices =
      kActionSceneEffectSwordIndicesPerInstance -
      kActionSceneEffectParticlesPerInstance * 6,
  /* One camera-wide waterfall veil is admitted only after exact platform
   * splash signatures identify the section. Its denser field is the only
   * scene style that exceeds the ordinary 12-particle actor budget. */
  kActionSceneEffectMaxWaterfallVeils = 1,
  /* Eight rows across sixteen stable lanes preserve the original veil density
   * now that its projection continues through the 224-row overflow repeat. */
  kActionSceneEffectWaterfallParticleCount = 128,
  kActionSceneEffectWaterfallExtraVertices =
      (kActionSceneEffectWaterfallParticleCount -
       kActionSceneEffectParticlesPerInstance) * 4,
  kActionSceneEffectWaterfallExtraIndices =
      (kActionSceneEffectWaterfallParticleCount -
       kActionSceneEffectParticlesPerInstance) * 6,
  kActionSceneEffectWaterfallMistExtraVertices =
      kActionSceneEffectWaterfallMistCloudCount *
          kActionSceneEffectWaterfallMistCloudVertices -
      kActionSceneEffectGlowsPerInstance * kActionEffectGlowVertices +
      (kActionSceneEffectWaterfallMistParticleCount -
       kActionSceneEffectParticlesPerInstance) * 4,
  kActionSceneEffectWaterfallMistExtraIndices =
      kActionSceneEffectWaterfallMistCloudCount *
          kActionSceneEffectWaterfallMistCloudIndices -
      kActionSceneEffectGlowsPerInstance * kActionEffectGlowIndices +
      (kActionSceneEffectWaterfallMistParticleCount -
       kActionSceneEffectParticlesPerInstance) * 6,
  kActionSceneEffectRenderMaxVertices =
      kActionSceneEffectMaxGlows * kActionEffectGlowVertices +
      kActionSceneEffectMaxParticles * 4 +
      kActionSceneEffectMaxLightningFilaments *
          kActionSceneEffectLightningVerticesPerInstance +
      kActionSceneEffectMaxMarahnaLightningLinks *
          kActionSceneEffectMarahnaLightningVerticesPerInstance +
      kActionSceneEffectMaxMarahnaBossLightningBolts *
          kActionSceneEffectMarahnaBossLightningVerticesPerInstance +
      kActionSceneEffectMaxSwordStreams *
          kActionSceneEffectSwordExtraVertices +
      kActionSceneEffectMaxWaterfallVeils *
          (kActionSceneEffectWaterfallExtraVertices +
           kActionSceneEffectWaterfallMistExtraVertices) +
      kActionSceneEffectLavaReservoirParticleExtraVertices +
      kActionSceneEffectLavaReservoirGlowExtraVertices +
      kActionSceneEffectMaxFlamingWheels *
          kActionSceneEffectFlamingWheelExtraVertices,
  kActionSceneEffectRenderMaxIndices =
      kActionSceneEffectMaxGlows * kActionEffectGlowIndices +
      kActionSceneEffectMaxParticles * 6 +
      kActionSceneEffectMaxLightningFilaments *
          kActionSceneEffectLightningIndicesPerInstance +
      kActionSceneEffectMaxMarahnaLightningLinks *
          kActionSceneEffectMarahnaLightningIndicesPerInstance +
      kActionSceneEffectMaxMarahnaBossLightningBolts *
          kActionSceneEffectMarahnaBossLightningIndicesPerInstance +
      kActionSceneEffectMaxSwordStreams *
          kActionSceneEffectSwordExtraIndices +
      kActionSceneEffectMaxWaterfallVeils *
          (kActionSceneEffectWaterfallExtraIndices +
           kActionSceneEffectWaterfallMistExtraIndices) +
      kActionSceneEffectLavaReservoirParticleExtraIndices +
      kActionSceneEffectLavaReservoirGlowExtraIndices +
      kActionSceneEffectMaxFlamingWheels *
          kActionSceneEffectFlamingWheelExtraIndices,

  /* One textured mesh draw resolves the already-composited lava room through
   * this small refraction grid. It is the portable renderer equivalent of a
   * heat shader: fixed cost, no per-pixel CPU work or backend-specific blob. */
  kActionHeatMeshColumns = 16,
  kActionHeatMeshRows = 14,
  kActionHeatMeshVertices =
      (kActionHeatMeshColumns + 1) * (kActionHeatMeshRows + 1),
  kActionHeatMeshIndices =
      kActionHeatMeshColumns * kActionHeatMeshRows * 6,
};

typedef bool (*ActionEffectProjectPointFn)(
    void *userdata, const ActionEffectInstance *effect,
    float local_x, float local_y, ArRenderPointF *point);

/* Renderer-independent output. The game-specific module owns spell styles,
 * clocks and geometry while present.c owns only the backend submission. */
typedef struct ActionEffectRenderBatch {
  ArRenderVertex2D vertices[kActionEffectRenderMaxVertices];
  int32_t indices[kActionEffectRenderMaxIndices];
  int vertex_count;
  int index_count;
} ActionEffectRenderBatch;

typedef struct ActionSceneEffectRenderBatch {
  ArRenderVertex2D vertices[kActionSceneEffectRenderMaxVertices];
  int32_t indices[kActionSceneEffectRenderMaxIndices];
  int vertex_count;
  int index_count;
} ActionSceneEffectRenderBatch;

typedef struct ActionHeatRenderMesh {
  ArRenderVertex2D vertices[kActionHeatMeshVertices];
  int32_t indices[kActionHeatMeshIndices];
  int vertex_count;
  int index_count;
} ActionHeatRenderMesh;

/* Unknown effect/phase/geometry/layer values are ignored (fail closed).
 * False reports malformed input or an internal capacity violation. Output
 * counts are always initialized; only the vertex/index prefixes they name are
 * defined and safe to submit. Storage beyond those counts is unspecified. */
bool ActionEffectRender_Build(const ActionEffectFrame *frame,
                              bool lighting_enabled,
                              bool particles_enabled,
                              ActionEffectProjectPointFn project_point,
                              void *project_userdata,
                              ActionEffectRenderBatch *batch);

bool ActionSceneEffectRender_Build(const ActionSceneEffectFrame *frame,
                                   bool lighting_enabled,
                                   bool particles_enabled,
                                   ActionEffectProjectPointFn project_point,
                                   void *project_userdata,
                                   ActionSceneEffectRenderBatch *batch);

/* Builds only map-derived decorations assigned to `render_layer`. Keeping
 * this separate from the actor builder preserves both capture and scratch
 * capacity when a camera window contains many authored structures. */
bool ActionSceneDecorationRender_Build(
    const ActionSceneEffectFrame *frame, uint8_t render_layer,
    bool lighting_enabled, bool particles_enabled,
    ActionEffectProjectPointFn project_point, void *project_userdata,
    ActionSceneEffectRenderBatch *batch);

/* Builds output-space positions and normalized UVs into a viewport-sized
 * source texture for subtle lower-screen heat refraction. The outer edge is
 * pinned exactly, so the mesh cannot sample beyond the scene target or pull
 * letterbox pixels into the game image. */
bool ActionHeatRender_Build(uint16_t game_frame,
                            ArRenderRectI output_viewport,
                            int target_width, int target_height,
                            int source_width,
                            ActionHeatRenderMesh *mesh);

#endif  /* ACTION_EFFECT_RENDER_H */
