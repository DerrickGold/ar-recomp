#include "action_effect_render.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

/* A spell's captured parts are quadrants of ONE fireball, not independent
 * fires. Rendering each part as its own self-contained glow and its own
 * outward particle spray is what made the first attempt read as four smudges
 * arranged in a square: the eye groups by silhouette, and there were four.
 *
 * So the unit of styling here is the BURST — the union of every visible part
 * of one spell. The burst carries the large soft body and the whole ember
 * plume; the individual parts contribute only small white-hot cores inside
 * it, which is what keeps the mass from looking like a featureless blob. */

typedef struct ActionEffectGlowStyle {
  float radius_x, radius_y;
  float ring_scale[kActionEffectGlowRings];  /* fraction of the outer radius */
  SDL_FColor centre;
  SDL_FColor ring[kActionEffectGlowRings];
  float flare;   /* outer-ring silhouette amplitude, fraction of radius */
  float rise;    /* aura offset toward -Y, fraction of radius: hot gas lifts */
  unsigned seed; /* silhouette phase; distinct per flame so they churn apart */
} ActionEffectGlowStyle;

/* The union of one spell's visible parts, expressed in the LOCAL space of the
 * anchor part so it can be fed straight back through the caller's projection
 * callback (which resolves local -> screen relative to a given instance). */
/* One contiguous body of fire: the merged extent of every captured part that
 * touches it, plus a representative part to seed its silhouette phase (so two
 * flames of the same spell churn independently rather than in lockstep). */
typedef struct ActionEffectFlame {
  ActionEffectLocalRect rect;
  const ActionEffectInstance *source;
} ActionEffectFlame;

typedef struct ActionEffectBurst {
  const ActionEffectInstance *anchor;
  float min_x, min_y, max_x, max_y;
  /* Parts CLUSTERED by overlap, not one entry per captured record. Magical
   * Fire's four parts are two vertically-stacked pairs; each pair meets
   * exactly at y 488 and is one flame wall, while the two walls are separated
   * by an authored gap the player stands in. Drawing a hot core per part put
   * two cores in each wall, stacked like a colon; clustering first is what
   * gives each wall the single core it should have. Both the glows and the
   * ember seeding work from these. */
  ActionEffectFlame flame[kActionEffectMaxInstances];
  float strength;
  unsigned flame_count;
  uint8_t phase;
} ActionEffectBurst;

/* Touching counts as overlapping: the stacked pairs share an exact edge. */
static bool RectsTouch(const ActionEffectLocalRect *a,
                       const ActionEffectLocalRect *b) {
  const float kSlack = 1.0f;
  return a->x0 <= b->x1 + kSlack && b->x0 <= a->x1 + kSlack &&
      a->y0 <= b->y1 + kSlack && b->y0 <= a->y1 + kSlack;
}

static void RectUnion(ActionEffectLocalRect *into,
                      const ActionEffectLocalRect *other) {
  into->x0 = fminf(into->x0, other->x0);
  into->y0 = fminf(into->y0, other->y0);
  into->x1 = fmaxf(into->x1, other->x1);
  into->y1 = fmaxf(into->y1, other->y1);
}

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

static uint32_t EffectHash(uint32_t value) {
  value ^= value >> 16;
  value *= 0x7FEB352Du;
  value ^= value >> 15;
  value *= 0x846CA68Bu;
  value ^= value >> 16;
  return value;
}

/* Hash -> 0..1. */
static float HashUnit(uint32_t value) {
  return (float)(EffectHash(value) & 0xFFFFu) / 65535.0f;
}

static bool Reserve(const ActionEffectRenderBatch *batch,
                    int vertices, int indices) {
  return batch && vertices >= 0 && indices >= 0 &&
      batch->vertex_count <= kActionEffectRenderMaxVertices - vertices &&
      batch->index_count <= kActionEffectRenderMaxIndices - indices;
}

/* Integer triangle wave over `period` ticks, 0..1. Identical phase and
 * amplitude on every libc/libm — the division is exact enough for
 * presentation and has no transcendental implementation variance. */
static float TriangleWave(unsigned ticks, unsigned period) {
  if (period < 2u) return 0.0f;
  unsigned half = period / 2u;
  unsigned cycle = ticks % period;
  unsigned triangle = cycle <= half ? cycle : period - cycle;
  return (float)triangle / (float)half;
}

/* Two beat rates rather than one. A single slow swell reads as a light being
 * dimmed; a fast flicker riding a slow swell reads as combustion. The rates
 * are deliberately coprime (7 and 30) so the pair does not resynchronise into
 * a visible loop for hundreds of ticks. */
static float DeterministicPulse(const ActionEffectInstance *effect) {
  unsigned seed = (unsigned)effect->visual * 2u;
  float swell = TriangleWave((unsigned)effect->phase_ticks + seed, 30u);
  float flicker = TriangleWave((unsigned)effect->phase_ticks + seed * 3u, 7u);
  return 0.82f + 0.12f * swell + 0.06f * flicker;
}

/* Outer-ring radius modulation in -1..+1, smooth around the circle and
 * animated. Three harmonics read off the same unit-circle table the rings
 * themselves use — the 1st, 3rd and 5th (3 and 5 are coprime with 32, so
 * stepping the index by them walks every residue and samples a true higher
 * harmonic). Each rotates at its own rate, so the silhouette writhes instead
 * of breathing uniformly, and the amplitudes sum to exactly 1 so the result
 * cannot exceed the caller's `flare` budget. No transcendental involved. */
static float FlameSilhouette(unsigned seed, unsigned ticks, int segment) {
  int i1 = (segment + (int)((ticks / 3u + seed) & 31u)) & 31;
  int i2 = (segment * 3 + (int)((ticks / 2u + seed * 5u) & 31u)) & 31;
  int i3 = (segment * 5 + (int)((ticks + seed * 11u) & 31u)) & 31;
  return 0.52f * kCircle32[i1][0] + 0.31f * kCircle32[i2][0] +
      0.17f * kCircle32[i3][0];
}

static SDL_FColor MixColor(SDL_FColor a, SDL_FColor b, float t) {
  return (SDL_FColor){
    a.r + (b.r - a.r) * t,
    a.g + (b.g - a.g) * t,
    a.b + (b.b - a.b) * t,
    a.a + (b.a - a.a) * t,
  };
}

/* ── Magical Fire palette ──────────────────────────────────────────────────
 * A real flame is not one hue faded out: it runs white-gold where it is
 * hottest, through orange, to a deep ember red at the cool edge. Ramping HUE
 * outward rather than only alpha is most of what separates "a glow" from "a
 * fire" — under SDL_BLENDMODE_ADD the stack then genuinely bleaches toward
 * white at the centre instead of merely getting more orange.
 *
 * Alphas are the tuning surface. The burst body and the part cores overlap by
 * construction, so each is set below what it could carry alone; raise them
 * together and the middle clips to flat white. */
/* Tier 1 — light spill. Wide, dim, deep red: the fire's effect on the air
 * around it rather than the fire itself. Kept low because it covers the most
 * pixels and is the first thing to muddy the frame if overdone. */
static const SDL_FColor kSpillCentre = { 1.00f, 0.45f, 0.14f, 0.13f };
static const SDL_FColor kSpillInner  = { 1.00f, 0.30f, 0.06f, 0.10f };
static const SDL_FColor kSpillBody   = { 0.90f, 0.15f, 0.01f, 0.05f };
static const SDL_FColor kSpillAura   = { 0.60f, 0.04f, 0.00f, 0.00f };

/* Tier 2 — the flame bodies, one per part. This is the fire proper, so it
 * carries the saturated orange and the strongest silhouette turbulence.
 * The near-white centre vertex IS the hot core — no separate glow tier is
 * needed for it — ramping out through saturated orange to a deep ember edge.
 *
 * That core lives per PART rather than at the burst centre for a reason
 * grounded in the art. Magical Fire is two flame WALLS flanking the caster,
 * not one ball: measured live, the four parts cover x 388..440 and 458..510
 * with an 18px gap at x 449, which is exactly where the player is standing.
 * A focal core at the union centre therefore put the brightest point in empty
 * space and read as a headlight between two lobes. Per-part cores instead
 * fuse vertically — the stacked pairs meet exactly at y 488 — giving one core
 * per wall, which is what the spell actually is. */
static const SDL_FColor kFlameCentre = { 1.00f, 0.98f, 0.90f, 0.78f };
static const SDL_FColor kFlameInner  = { 1.00f, 0.76f, 0.30f, 0.44f };
static const SDL_FColor kFlameBody   = { 1.00f, 0.34f, 0.04f, 0.20f };
static const SDL_FColor kFlameAura   = { 0.85f, 0.10f, 0.00f, 0.00f };

static const SDL_FColor kEmberHot    = { 1.00f, 0.96f, 0.80f, 1.00f };
static const SDL_FColor kEmberCool   = { 0.95f, 0.22f, 0.01f, 0.00f };

static bool RectIsSane(const ActionEffectLocalRect *rect) {
  return isfinite(rect->x0) && isfinite(rect->y0) && isfinite(rect->x1) &&
      isfinite(rect->y1) && rect->x0 <= rect->x1 && rect->y0 <= rect->y1;
}

/* Project one local point plus the two unit-step neighbours, so a caller gets
 * both the screen anchor and the local->screen scale on each axis. The scale
 * is what keeps radii and ember sizes correct under the diorama's perspective
 * projection, where a pixel is not a pixel. */
static bool ProjectWithScale(const ActionEffectInstance *effect,
                             ActionEffectProjectPointFn project_point,
                             void *userdata, float local_x, float local_y,
                             SDL_FPoint *anchor,
                             float *scale_x, float *scale_y) {
  SDL_FPoint sample_x, sample_y;
  if (!isfinite(local_x) || !isfinite(local_y) ||
      !project_point(userdata, effect, local_x, local_y, anchor) ||
      !project_point(userdata, effect, local_x + 1.0f, local_y, &sample_x) ||
      !project_point(userdata, effect, local_x, local_y + 1.0f, &sample_y))
    return false;
  *scale_x = hypotf(sample_x.x - anchor->x, sample_x.y - anchor->y);
  *scale_y = hypotf(sample_y.x - anchor->x, sample_y.y - anchor->y);
  return isfinite(anchor->x) && isfinite(anchor->y) &&
      isfinite(*scale_x) && isfinite(*scale_y) &&
      *scale_x > 0.0f && *scale_y > 0.0f;
}

/* One ring-gradient glow centred on a local point. Shared by the burst body
 * and the per-part cores — they differ only in radius and palette. */
static bool AppendGlow(ActionEffectRenderBatch *batch,
                       const ActionEffectInstance *effect,
                       const ActionEffectGlowStyle *style, float strength,
                       float local_x, float local_y,
                       ActionEffectProjectPointFn project_point,
                       void *userdata) {
  enum {
    kSegments = kActionEffectGlowSegments,
    kRings = kActionEffectGlowRings,
  };
  SDL_FPoint anchor;
  float scale_x, scale_y;
  if (!ProjectWithScale(effect, project_point, userdata, local_x, local_y,
                        &anchor, &scale_x, &scale_y))
    return true;
  if (!Reserve(batch, kActionEffectGlowVertices, kActionEffectGlowIndices))
    return false;

  int centre = batch->vertex_count;
  SDL_FColor centre_color = style->centre;
  centre_color.a *= strength;
  batch->vertices[batch->vertex_count++] = (SDL_Vertex){
    anchor, centre_color, { 0.0f, 0.0f },
  };

  float radius_x = fmaxf(3.0f, style->radius_x * scale_x);
  float radius_y = fmaxf(2.0f, style->radius_y * scale_y);

  int ring_base[kRings];
  for (int r = 0; r < kRings; r++) {
    ring_base[r] = batch->vertex_count;
    SDL_FColor color = style->ring[r];
    color.a *= strength;
    float scale = style->ring_scale[r];
    /* Only the outer rings writhe and lift. Perturbing the core would make
     * the bright middle jitter, which reads as a rendering fault rather than
     * as flame; the turbulence belongs where the fire meets the air.
     *
     * The weighting is LINEAR in outerness, not squared. Squared put almost
     * all of the movement on the outermost ring — which is the fully
     * transparent one, so the flame silhouette was being computed and then
     * not shown. The visible edge is the mid ring, and it has to move. */
    float outerness = (float)r / (float)(kRings - 1);
    float wobble = style->flare * outerness;
    float lift = style->rise * outerness * radius_y * scale;
    for (int s = 0; s < kSegments; s++) {
      float shape = 1.0f + wobble * FlameSilhouette(
          style->seed, (unsigned)effect->pulse_ticks, s);
      batch->vertices[batch->vertex_count++] = (SDL_Vertex){
        { anchor.x + kCircle32[s][0] * radius_x * scale * shape,
          anchor.y + kCircle32[s][1] * radius_y * scale * shape - lift },
        color, { 0.0f, 0.0f },
      };
    }
  }

  /* Centre fan out to ring 0. */
  for (int s = 0; s < kSegments; s++) {
    batch->indices[batch->index_count++] = centre;
    batch->indices[batch->index_count++] = ring_base[0] + s;
    batch->indices[batch->index_count++] = ring_base[0] + (s + 1) % kSegments;
  }
  /* Quad strips between consecutive rings, two triangles per segment. */
  for (int r = 0; r + 1 < kRings; r++) {
    for (int s = 0; s < kSegments; s++) {
      int next = (s + 1) % kSegments;
      int inner0 = ring_base[r] + s, inner1 = ring_base[r] + next;
      int outer0 = ring_base[r + 1] + s, outer1 = ring_base[r + 1] + next;
      batch->indices[batch->index_count++] = inner0;
      batch->indices[batch->index_count++] = outer0;
      batch->indices[batch->index_count++] = outer1;
      batch->indices[batch->index_count++] = inner0;
      batch->indices[batch->index_count++] = outer1;
      batch->indices[batch->index_count++] = inner1;
    }
  }
  return true;
}

/* One ember's offset from its birth point at normalised age `t`, in local
 * units. Sampled twice by the emitter and differenced, which is what lets the
 * spark be drawn as a streak aligned with its own direction of travel. */
static void EmberOffset(float drift, float t, float *x, float *y) {
  /* Buoyancy is quadratic in age: hot gas ACCELERATES upward, so the plume
   * flares open at the top rather than drifting in a straight line. This is
   * the single biggest cue that the particles belong to a fire — and it is
   * shared by every ember, which is what makes them read as one plume rather
   * than as four quadrant sprays.
   *
   * The travel is deliberately larger than the burst is tall. Embers that
   * stay inside the glow read as speckle ON the fire; the plume only appears
   * once a good share of them have climbed clear of it. */
  *y = -(2.0f + 11.0f * t + 48.0f * t * t);
  /* Lateral spread widens with height, for the same reason. */
  *x = drift * (0.5f + 2.6f * t);
}

static bool AppendEmbers(ActionEffectRenderBatch *batch,
                         const ActionEffectBurst *burst, unsigned count,
                         ActionEffectProjectPointFn project_point,
                         void *userdata) {
  const ActionEffectInstance *anchor = burst->anchor;
  if (!burst->flame_count) return true;

  for (uint32_t i = 0; i < count; i++) {
    uint32_t seed = EffectHash(
        burst->anchor->pulse_generation * 0x9E3779B9u ^
        (uint32_t)burst->anchor->record_address * 0x85EBCA6Bu ^
        i * 0xC2B2AE35u);

    /* Pick a flame body, then a point inside it. Round-robin by index rather
     * than by hash, so the plume stays evenly shared between the walls no
     * matter how few embers are alive at once. */
    const ActionEffectLocalRect *part = &burst->flame[i % burst->flame_count].rect;
    float span_x = part->x1 - part->x0;
    float span_y = part->y1 - part->y0;
    if (span_x < 1.0f) span_x = 1.0f;
    if (span_y < 1.0f) span_y = 1.0f;
    float mid_x = (part->x0 + part->x1) * 0.5f;

    /* Births hug the lower body and pull in from the edges: fire feeds its
     * plume from its hottest part, and seeding the footprint uniformly
     * produced an even confetti of sparks with no sense of flow. */
    float birth_u = HashUnit(seed ^ 0x1Bu);
    float birth_v = HashUnit(seed ^ 0x2Fu);
    float birth_x = part->x0 + (0.15f + 0.70f * birth_u) * span_x;
    float birth_y = part->y0 + (0.45f + 0.55f * birth_v) * span_y;

    unsigned lifetime = 24u + ((seed >> 5) & 15u);
    unsigned age = ((unsigned)anchor->pulse_ticks + seed % lifetime) %
        lifetime;
    float t = (float)age / (float)(lifetime - 1u);

    /* Outward component follows the ember's own offset from the burst's
     * centreline, so the plume fans out from the whole mass instead of every
     * spark sharing one direction. */
    float drift = (birth_x - mid_x) / (span_x * 0.5f);
    drift = drift * 3.0f + (HashUnit(seed ^ 0x77u) - 0.5f) * 2.5f;

    float ox, oy, prev_x, prev_y;
    EmberOffset(drift, t, &ox, &oy);
    EmberOffset(drift, fmaxf(0.0f, t - 0.12f), &prev_x, &prev_y);

    SDL_FPoint position, previous;
    float scale_x, scale_y;
    if (!ProjectWithScale(anchor, project_point, userdata, birth_x + ox,
                          birth_y + oy, &position, &scale_x, &scale_y) ||
        !project_point(userdata, anchor, birth_x + prev_x, birth_y + prev_y,
                       &previous))
      continue;
    float output_scale = (scale_x + scale_y) * 0.5f;
    if (output_scale < 0.5f) output_scale = 0.5f;

    /* Motion-aligned axes. A newborn ember has barely moved, so the
     * difference degenerates; fall back to straight up, which is where it is
     * headed anyway. */
    float dir_x = position.x - previous.x, dir_y = position.y - previous.y;
    float length = hypotf(dir_x, dir_y);
    if (length < 0.001f) { dir_x = 0.0f; dir_y = -1.0f; }
    else { dir_x /= length; dir_y /= length; }

    /* Embers cool, shrink and stretch as they slow: the streak lengthens with
     * age while its width collapses, so the plume ends in thin sparks rather
     * than in a scatter of identical lozenges. Small — an ember that reads as
     * a shape rather than as a point of light looks like debris. */
    float width = (0.95f - 0.60f * t) * output_scale;
    if (width < 0.40f) width = 0.40f;
    float reach = width * (1.6f + 4.2f * t);

    /* Per-ember brightness flicker, decorrelated from the body pulse so the
     * plume shimmers instead of blinking with the glow. */
    float flicker = 0.65f + 0.35f * HashUnit(
        seed ^ (((unsigned)anchor->pulse_ticks / 2u + i) * 0x27D4EB2Du));

    SDL_FColor color = MixColor(kEmberHot, kEmberCool, t);
    /* Fade in over the first few ticks of life. Without it an ember pops into
     * existence at full brightness inside the flame body, which reads as a
     * flicker artifact rather than as a spark being thrown. */
    float birth_fade = t * 8.0f;
    if (birth_fade > 1.0f) birth_fade = 1.0f;
    color.a *= burst->strength * flicker * birth_fade;

    static const int kQuad[] = { 0, 1, 2, 0, 2, 3 };
    if (!Reserve(batch, 4, 6)) return false;
    int base = batch->vertex_count;
    batch->vertices[batch->vertex_count++] = (SDL_Vertex){
      { position.x + dir_x * reach, position.y + dir_y * reach },
      color, { 0.0f, 0.0f } };
    batch->vertices[batch->vertex_count++] = (SDL_Vertex){
      { position.x - dir_y * width, position.y + dir_x * width },
      color, { 0.0f, 0.0f } };
    batch->vertices[batch->vertex_count++] = (SDL_Vertex){
      { position.x - dir_x * reach, position.y - dir_y * reach },
      color, { 0.0f, 0.0f } };
    batch->vertices[batch->vertex_count++] = (SDL_Vertex){
      { position.x + dir_y * width, position.y - dir_x * width },
      color, { 0.0f, 0.0f } };
    for (int n = 0; n < 6; n++)
      batch->indices[batch->index_count++] = base + kQuad[n];
  }
  return true;
}

/* Collect every visible Magical Fire part into one burst, in the local space
 * of the first such part. Returns false when the frame carries no renderable
 * fire at all. */
static bool BuildBurst(const ActionEffectFrame *frame,
                       ActionEffectBurst *burst) {
  memset(burst, 0, sizeof(*burst));
  for (uint8_t i = 0; i < frame->effect_count; i++) {
    const ActionEffectInstance *effect = &frame->effects[i];
    if (!(effect->flags & kActionEffectFlag_Visible) ||
        effect->kind != kActionEffect_MagicalFire ||
        effect->geometry.kind != kActionEffectGeometry_Rect ||
        effect->obj_priority >= kActionEffectObjPriorityCount ||
        effect->render_layer != kActionEffectRenderLayer_WorldOverlay ||
        !RectIsSane(&effect->geometry.data.rect))
      continue;
    if (effect->phase != kActionEffectPhase_FireIgnition &&
        effect->phase != kActionEffectPhase_FireBloom)
      continue;

    /* Re-express this part's rect in the anchor's local frame. */
    float shift_x = 0.0f, shift_y = 0.0f;
    if (!burst->anchor) {
      burst->anchor = effect;
      burst->phase = effect->phase;
      burst->min_x = burst->max_x = effect->geometry.data.rect.x0;
      burst->min_y = burst->max_y = effect->geometry.data.rect.y0;
    } else {
      shift_x = (float)(effect->world_x - burst->anchor->world_x);
      shift_y = (float)(effect->world_y - burst->anchor->world_y);
      /* Bloom wins: if any part has opened, the whole burst has. */
      if (effect->phase == kActionEffectPhase_FireBloom)
        burst->phase = kActionEffectPhase_FireBloom;
    }
    ActionEffectLocalRect local = {
      shift_x + effect->geometry.data.rect.x0,
      shift_y + effect->geometry.data.rect.y0,
      shift_x + effect->geometry.data.rect.x1,
      shift_y + effect->geometry.data.rect.y1,
    };
    burst->min_x = fminf(burst->min_x, local.x0);
    burst->max_x = fmaxf(burst->max_x, local.x1);
    burst->min_y = fminf(burst->min_y, local.y0);
    burst->max_y = fmaxf(burst->max_y, local.y1);
    burst->flame[burst->flame_count].rect = local;
    burst->flame[burst->flame_count].source = effect;
    burst->flame_count++;
  }
  if (!burst->anchor) return false;

  /* Merge every touching pair until nothing more merges. At most seven parts
   * exist per spell, so the repeated O(n^2) sweep is a handful of float
   * compares and needs no union-find bookkeeping to justify itself. */
  for (bool merged = true; merged; ) {
    merged = false;
    for (unsigned a = 0; a < burst->flame_count && !merged; a++) {
      for (unsigned b = a + 1; b < burst->flame_count && !merged; b++) {
        if (!RectsTouch(&burst->flame[a].rect, &burst->flame[b].rect))
          continue;
        RectUnion(&burst->flame[a].rect, &burst->flame[b].rect);
        burst->flame[b] = burst->flame[--burst->flame_count];
        merged = true;
      }
    }
  }

  burst->strength = DeterministicPulse(burst->anchor);
  return isfinite(burst->min_x) && isfinite(burst->max_x) &&
      isfinite(burst->min_y) && isfinite(burst->max_y);
}

bool ActionEffectRender_Build(const ActionEffectFrame *frame,
                              bool lighting_enabled,
                              bool particles_enabled,
                              ActionEffectProjectPointFn project_point,
                              void *project_userdata,
                              ActionEffectRenderBatch *batch) {
  if (!batch) return false;
  memset(batch, 0, sizeof(*batch));
  if (!frame || frame->effect_count > kActionEffectMaxInstances)
    return false;
  if (!lighting_enabled && !particles_enabled) return true;
  if (!project_point) return false;

  ActionEffectBurst burst;
  if (!BuildBurst(frame, &burst)) return true;

  float half_x = (burst.max_x - burst.min_x) * 0.5f;
  float half_y = (burst.max_y - burst.min_y) * 0.5f;
  float mid_x = (burst.min_x + burst.max_x) * 0.5f;
  float mid_y = (burst.min_y + burst.max_y) * 0.5f;

  bool bloom = burst.phase == kActionEffectPhase_FireBloom;
  float phase_strength = bloom ? 1.0f : 0.78f;
  float strength = burst.strength * phase_strength;

  if (lighting_enabled) {
    /* Tier 1: light spill. Sized off the real union extents rather than a
     * constant, so a wide-open bloom reads wide and an ignition spark reads
     * small; the floors keep a not-yet-grown rect from collapsing it. */
    const ActionEffectGlowStyle spill = {
      .radius_x = fmaxf(24.0f, half_x * (bloom ? 1.55f : 1.20f)),
      .radius_y = fmaxf(20.0f, half_y * (bloom ? 2.10f : 1.55f)),
      /* Weighted toward the outside: a narrow hot middle with a long, soft
       * falloff is what reads as luminous. Even spacing reads as a disc. */
      .ring_scale = { 0.34f, 0.66f, 1.0f },
      .centre = kSpillCentre,
      .ring = { kSpillInner, kSpillBody, kSpillAura },
      .flare = bloom ? 0.20f : 0.12f,
      .rise = bloom ? 0.26f : 0.14f,
      .seed = (unsigned)burst.anchor->record_address >> 4,
    };
    if (!AppendGlow(batch, burst.anchor, &spill, strength, mid_x, mid_y,
                    project_point, project_userdata))
      return false;

    /* Tier 2: one flame body per CLUSTER — per wall, not per captured part.
     * Its radii cover the merged extent, so each wall is a single continuous
     * flame with one white core, and the strong flare gives it a turbulent
     * outline rather than a clean ellipse. */
    for (unsigned i = 0; i < burst.flame_count; i++) {
      const ActionEffectLocalRect *rect = &burst.flame[i].rect;
      const ActionEffectGlowStyle flame = {
        .radius_x = fmaxf(8.0f, (rect->x1 - rect->x0) * 0.44f),
        /* Taller than wide: flame climbs. A body sized symmetrically to its
         * rect reads as a lamp, whatever colour it is. */
        .radius_y = fmaxf(7.0f, (rect->y1 - rect->y0) * 0.80f),
        /* Tight, so the white core stays a highlight: a wide white centre
         * just bleaches the whole body to a flat disc. */
        .ring_scale = { 0.22f, 0.55f, 1.0f },
        .centre = kFlameCentre,
        .ring = { kFlameInner, kFlameBody, kFlameAura },
        .flare = bloom ? 0.38f : 0.22f,
        .rise = 0.34f,
        .seed = (unsigned)burst.flame[i].source->record_address >> 5,
      };
      if (!AppendGlow(batch, burst.anchor, &flame, strength,
                      (rect->x0 + rect->x1) * 0.5f,
                      (rect->y0 + rect->y1) * 0.5f,
                      project_point, project_userdata))
        return false;
    }
  }

  if (particles_enabled) {
    /* Scale the plume with how much fire there actually is, but keep it a
     * whole-burst budget so the count does not multiply with part count. */
    unsigned embers = bloom ? kActionEffectMaxEmbers
                            : kActionEffectMaxEmbers / 2u;
    if (embers > kActionEffectMaxEmbers) embers = kActionEffectMaxEmbers;
    if (!AppendEmbers(batch, &burst, embers, project_point, project_userdata))
      return false;
  }
  return true;
}
