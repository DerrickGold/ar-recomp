#include "action_effect_render.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "deterministic_hash.h"

/* A spell's captured parts are not independent effects. Rendering each one as
 * its own self-contained glow with its own outward spray made Magical Fire
 * read as four smudges arranged in a square: the eye groups by silhouette,
 * and there were four.
 *
 * So there are two tiers. One wide, dim LIGHT SPILL spans the whole burst and
 * is what makes several parts read as a single object. Then one saturated
 * BODY per cluster of touching same-role parts, whose own centre vertex is
 * its hot core — no separate glow for that. Clustering is what turns Fire's
 * four quadrants into its two actual walls, and keeps Magical Light's centre
 * flare from being swallowed by its beam columns.
 *
 * Per-part styling is resolved from that part's PHASE, not the burst's: a
 * spell routinely runs several stages at once (Stardust flies and detonates
 * simultaneously), so anything read once for the whole burst must be
 * genuinely burst-wide. */

typedef struct ActionEffectGlowStyle {
  float radius_x, radius_y;
  float ring_scale[kActionEffectGlowRings];  /* fraction of the outer radius */
  SDL_FColor centre;
  SDL_FColor ring[kActionEffectGlowRings];
  float flare;   /* outer-ring silhouette amplitude, fraction of radius */
  float rise;    /* aura offset along (lift_x,lift_y), fraction of radius */
  /* Unit vector for the ellipse's local +X. (1,0) leaves the body screen-
   * aligned, which is right for a fire standing in place. A projectile must
   * instead be oriented along its own heading, or its glow reads as a
   * screen-axis blob stuck to a sprite that is plainly travelling diagonally. */
  float axis_x, axis_y;
  /* Unit vector the outer rings shift toward. (0,-1) is screen-up, i.e. hot
   * gas rising; an aligned body instead trails backwards along its heading. */
  float lift_x, lift_y;
  unsigned seed; /* silhouette phase; distinct per flame so they churn apart */
} ActionEffectGlowStyle;

/* One contiguous body: the merged extent of every captured part that touches
 * it, plus a representative part to seed its silhouette phase (so two flames
 * of the same spell churn independently rather than in lockstep) and to carry
 * the phase its own styling is resolved from. */
typedef struct ActionEffectFlame {
  ActionEffectLocalRect rect;
  const ActionEffectInstance *source;
  uint8_t role;
  uint8_t phase;
} ActionEffectFlame;

/* The union of one spell's visible parts, expressed in the LOCAL space of the
 * anchor part so it can be fed straight back through the caller's projection
 * callback (which resolves local -> screen relative to a given instance). */
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
  /* Deliberately NO burst-wide phase field. It existed, was set from the
   * anchor, and drove styling for every part — which is the defect this
   * structure now prevents by construction. Anything burst-wide is derived
   * from the flames at the point of use. */
} ActionEffectBurst;

/* Touching counts as overlapping: the stacked pairs share an exact edge.
 * Only same-ROLE parts are ever tested — Magical Light's centre flare sits
 * between its two beam columns and would otherwise swallow them into one
 * enormous rect, destroying the very distinction the capture preserved. */
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

/* Mist is a mass of overlapping puffs rather than a precision light halo.
 * Twelve segments keep each silhouette round at SNES presentation scale while
 * making a dense 24-puff volume cheaper than six generic 32-segment glows. */
static const float
kMistCircle12[kActionSceneEffectWaterfallMistCloudSegments][2] = {
  { 1.000000f, 0.000000f }, { 0.866025f, 0.500000f },
  { 0.500000f, 0.866025f }, { 0.000000f, 1.000000f },
  {-0.500000f, 0.866025f }, {-0.866025f, 0.500000f },
  {-1.000000f, 0.000000f }, {-0.866025f,-0.500000f },
  {-0.500000f,-0.866025f }, { 0.000000f,-1.000000f },
  { 0.500000f,-0.866025f }, { 0.866025f,-0.500000f },
};

/* Hash -> 0..1. */
static float HashUnit(uint32_t value) {
  return (float)(DeterministicHash_Mix32(value) & 0xFFFFu) / 65535.0f;
}

/* Capacity-aware view over either public batch type. Geometry builders write
 * directly into their final destination; no spell-sized scene scratch batch,
 * index rebasing, or capacity coupling is required. Counts are committed to
 * the public batch only after the complete build succeeds, so any overflow
 * leaves a zero-count, fail-closed output. */
typedef struct ActionEffectGeometryWriter {
  SDL_Vertex *vertices;
  int *indices;
  int vertex_count;
  int index_count;
  int vertex_capacity;
  int index_capacity;
} ActionEffectGeometryWriter;

static ActionEffectGeometryWriter GeometryWriter(
    SDL_Vertex *vertices, int vertex_capacity,
    int *indices, int index_capacity) {
  return (ActionEffectGeometryWriter) {
    .vertices = vertices,
    .indices = indices,
    .vertex_capacity = vertex_capacity,
    .index_capacity = index_capacity,
  };
}

static bool Reserve(const ActionEffectGeometryWriter *writer,
                    int vertices, int indices) {
  return writer && writer->vertices && writer->indices &&
      vertices >= 0 && indices >= 0 &&
      vertices <= writer->vertex_capacity &&
      indices <= writer->index_capacity &&
      writer->vertex_count <= writer->vertex_capacity - vertices &&
      writer->index_count <= writer->index_capacity - indices;
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

/* The captured clock remains authentic 60 Hz lifecycle time. Animated BG
 * fire turns over much faster than the original generic light pulse, so its
 * presentation samples run at 2x. Unsigned wrap is intentional: all visual
 * functions below are periodic and deterministic across the wrap. */
static unsigned EffectVisualTicks(const ActionEffectInstance *effect,
                                  unsigned ticks) {
  return effect &&
      (effect->kind == kActionEffect_WallTorch ||
       effect->kind == kActionEffect_AitosLavaPit ||
       effect->kind == kActionEffect_AitosStatueFire ||
       effect->kind == kActionEffect_AitosLavaReservoir ||
       effect->kind == kActionEffect_AitosWaterSplash)
      ? ticks * 2u : ticks;
}

/* Two beat rates rather than one. A single slow swell reads as a light being
 * dimmed; a fast flicker riding a slow swell reads as combustion. The rates
 * are deliberately coprime (7 and 30) so the pair does not resynchronise into
 * a visible loop for hundreds of ticks. */
static float DeterministicPulse(const ActionEffectInstance *effect) {
  unsigned seed = (unsigned)effect->visual * 2u;
  const unsigned ticks = EffectVisualTicks(
      effect, (unsigned)effect->phase_ticks);
  float swell = TriangleWave(ticks + seed, 30u);
  float flicker = TriangleWave(ticks + seed * 3u, 7u);
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
  int i1 = (segment + (int)((ticks / 3u + seed) &
                            kActionEffectGlowSegmentMask)) &
      kActionEffectGlowSegmentMask;
  int i2 = (segment * 3 + (int)((ticks / 2u + seed * 5u) &
                                kActionEffectGlowSegmentMask)) &
      kActionEffectGlowSegmentMask;
  int i3 = (segment * 5 + (int)((ticks + seed * 11u) &
                                kActionEffectGlowSegmentMask)) &
      kActionEffectGlowSegmentMask;
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
/* How a spell's particles move. Rise is a fire sitting in place; Trail is a
 * projectile dragging its own combustion behind it. The distinction matters
 * because a trail must follow the ACTOR's heading, which only the captured
 * per-instance velocity knows — a buoyant plume on a star crossing the screen
 * reads as smoke hanging in the air rather than as something in flight. */
typedef enum EmberMode {
  kEmberMode_Rise = 0,
  kEmberMode_Trail,
  /* A detonation: particles fan radially out of the impact point and clear
   * the sprite that caused it. Distinct from Rise because an impact's spray
   * has no preferred direction until the blast has spent itself. */
  kEmberMode_Burst,
} EmberMode;

/* Radial spray from an impact. The reach term t*(2-t) expands fast and then
 * decelerates, which is what a blast front does — linear travel reads as a
 * ring of drifting dots. `reach` deliberately exceeds the source sprite's own
 * radius, because particles that die inside the opaque graphic that spawned
 * them are invisible however bright they are. */
static void EmberOffsetBurst(float dir_x, float dir_y, float t,
                             float *x, float *y) {
  float reach = 42.0f * (t * (2.0f - t));
  float lift = 11.0f * t * t;   /* hot gas still climbs once the blast spends */
  *x = dir_x * reach;
  *y = dir_y * reach - lift;
}

/* The actor's unit heading, or false when it is not travelling.
 *
 * The FLIP BITS are applied to the velocity, on the reading that they are how
 * the ROM mirrors one authored direction into several — Magical Fire's four
 * walls sweep outward from one stored velocity plus four flip combinations,
 * and reading the raw velocity for every slot would point them all the same
 * way.
 *
 * This is not a guess: docs/ram-map.md's entry for slot +06/+08 already records
 * that the per-tick velocity is decoded by $00:8E2F and that "flip bits mirror
 * the authored deltas". Direct per-slot capture is still absent — only slot
 * $06A0's bytes survive (pinned in action_effects_test.c: velocity (4,2), no
 * flips) — so if a Fire cast ever shows all four walls leaning the same way,
 * this is the line to suspect. Measured Stardust carries no flips, so the
 * mirroring is a no-op there and cannot mask the question either way. */
static bool ActorHeading(const ActionEffectInstance *effect,
                         float *hx, float *hy) {
  float vx = (float)effect->velocity_x;
  float vy = (float)effect->velocity_y;
  if (effect->flags & kActionEffectFlag_FlipHorizontal) vx = -vx;
  if (effect->flags & kActionEffectFlag_FlipVertical) vy = -vy;
  float speed = hypotf(vx, vy);
  if (speed < 0.001f) return false;
  *hx = vx / speed;
  *hy = vy / speed;
  return true;
}

/* ── Per-spell visual table ────────────────────────────────────────────────
 * One entry per ActionEffectKind. Every spell reuses the same two-tier
 * machinery — a wide dim light spill over the whole burst, plus a saturated
 * body per cluster whose centre vertex is its hot core — and differs only in
 * palette, proportion and turbulence. That is deliberate: the shapes come
 * from the ROM's own extents, so a spell is characterised by its COLOUR and
 * its behaviour, not by bespoke geometry code per spell.
 *
 * Fire's numbers are tuned against real captured WRAM. The other three are
 * first-pass values chosen from the artwork description in the investigation
 * (Stardust: cold white-blue sparkle; Aura: green-gold orbiting halo; Light:
 * pale holy white-cyan) and have never been seen in motion. They are a
 * starting point to react to, not a finished look. */
typedef struct SpellVisual {
  SDL_FColor spill[1 + kActionEffectGlowRings];  /* centre, then rings */
  SDL_FColor body[1 + kActionEffectGlowRings];
  SDL_FColor ember_hot, ember_cool;
  float spill_scale_x, spill_scale_y;   /* multiples of the burst half-extent */
  float body_scale_x, body_scale_y;     /* multiples of the cluster extent */
  float flare, rise;
  uint8_t embers;
} SpellVisual;

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

static const SpellVisual kSpellVisuals[kActionEffect_KindCount] = {
  [kActionEffect_MagicalFire] = {
    .spill = { kSpillCentre, kSpillInner, kSpillBody, kSpillAura },
    .body = { kFlameCentre, kFlameInner, kFlameBody, kFlameAura },
    .ember_hot = kEmberHot, .ember_cool = kEmberCool,
    .spill_scale_x = 1.55f, .spill_scale_y = 2.10f,
    /* Elongated ALONG the heading, not up the screen: the wall sweeps
     * sideways, and the old taller-than-wide body was only correct while the
     * ellipse could not rotate. Vertical lick still comes from `rise`, which
     * stays screen-up for a fire (see the lift rule in Build). */
    .body_scale_x = 0.56f, .body_scale_y = 0.58f,
    .flare = 0.38f, .rise = 0.34f, .embers = kActionEffectMaxEmbers,
  },
  /* Stardust — cold, hard sparkle. Four staggered actors that relaunch, so
   * the read should be scattered points of light rather than a mass: a tight
   * body, little turbulence, and most of the energy in the embers. */
  [kActionEffect_MagicalStardust] = {
    .spill = { { 0.62f, 0.78f, 1.00f, 0.10f }, { 0.50f, 0.68f, 1.00f, 0.08f },
               { 0.32f, 0.48f, 0.95f, 0.04f }, { 0.16f, 0.24f, 0.70f, 0.00f } },
    .body = { { 0.96f, 0.99f, 1.00f, 0.74f }, { 0.72f, 0.88f, 1.00f, 0.40f },
              { 0.42f, 0.62f, 1.00f, 0.16f }, { 0.20f, 0.32f, 0.85f, 0.00f } },
    .ember_hot = { 0.95f, 0.99f, 1.00f, 1.00f },
    .ember_cool = { 0.35f, 0.55f, 1.00f, 0.00f },
    .spill_scale_x = 1.30f, .spill_scale_y = 1.30f,
    .body_scale_x = 0.50f, .body_scale_y = 0.50f,
    .flare = 0.16f, .rise = 0.06f, .embers = kActionEffectMaxEmbers,
  },
  /* Aura — a moving green-gold halo. Broad soft spill (it is a protective
   * shell, not a point source) and gentle churn; its motion comes from the
   * ROM moving the slots, so the style itself stays calm. */
  [kActionEffect_MagicalAura] = {
    .spill = { { 0.55f, 1.00f, 0.62f, 0.13f }, { 0.42f, 0.92f, 0.48f, 0.10f },
               { 0.24f, 0.70f, 0.30f, 0.05f }, { 0.10f, 0.40f, 0.14f, 0.00f } },
    .body = { { 0.92f, 1.00f, 0.86f, 0.60f }, { 0.68f, 1.00f, 0.62f, 0.36f },
              { 0.38f, 0.86f, 0.36f, 0.15f }, { 0.16f, 0.50f, 0.16f, 0.00f } },
    .ember_hot = { 0.90f, 1.00f, 0.78f, 0.85f },
    .ember_cool = { 0.30f, 0.75f, 0.32f, 0.00f },
    .spill_scale_x = 1.70f, .spill_scale_y = 1.70f,
    .body_scale_x = 0.58f, .body_scale_y = 0.58f,
    .flare = 0.22f, .rise = 0.10f, .embers = kActionEffectMaxEmbers / 2u,
  },
  /* Light — pale holy white-cyan. The columns are 16x224, so the body scale
   * is driven almost entirely by the captured rect; keeping the multipliers
   * near 0.5 makes the beam read as a shaft rather than a bloom. */
  [kActionEffect_MagicalLight] = {
    .spill = { { 0.85f, 0.97f, 1.00f, 0.14f }, { 0.72f, 0.92f, 1.00f, 0.11f },
               { 0.48f, 0.74f, 0.95f, 0.05f }, { 0.24f, 0.44f, 0.70f, 0.00f } },
    .body = { { 1.00f, 1.00f, 1.00f, 0.70f }, { 0.88f, 0.96f, 1.00f, 0.42f },
              { 0.58f, 0.80f, 1.00f, 0.17f }, { 0.28f, 0.50f, 0.80f, 0.00f } },
    .ember_hot = { 1.00f, 1.00f, 0.98f, 0.90f },
    .ember_cool = { 0.50f, 0.72f, 1.00f, 0.00f },
    .spill_scale_x = 1.40f, .spill_scale_y = 1.60f,
    .body_scale_x = 0.52f, .body_scale_y = 0.52f,
    .flare = 0.18f, .rise = 0.12f, .embers = kActionEffectMaxEmbers / 3u,
  },
};

/* Per-phase modulation, applied on top of the spell's palette. Keeps the
 * "opening/steady/fading" shape of a cast out of the colour table. */
typedef struct PhaseModifier {
  float strength, size, embers, flare;
  uint8_t ember_mode;
  /* Orient the body to the actor's own heading. Only meaningful for a stage
   * that travels: Stardust's stars descend at a measured 45 degrees on a
   * SQUARE 16x16 extent, so the diagonal lives in the art, not the extents —
   * an axis-aligned ellipse cannot express it however it is scaled. */
  bool align_to_heading;
  /* Optional per-phase palette overrides. NULL keeps the spell's own colours;
   * a stage that is materially a different substance (a star in flight is a
   * burning projectile, not the cold sparkle it detonates into) can restate
   * them without needing a whole second spell entry. */
  const SDL_FColor *body;          /* 1 + kActionEffectGlowRings entries */
  const SDL_FColor *ember_pair;    /* hot, then cool */
} PhaseModifier;

/* Warm flame palette shared by any stage that should read as combustion
 * rather than as its spell's own element. */
static const SDL_FColor kFlightBody[1 + kActionEffectGlowRings] = {
  { 1.00f, 0.96f, 0.82f, 0.76f },
  { 1.00f, 0.74f, 0.32f, 0.42f },
  { 1.00f, 0.38f, 0.06f, 0.18f },
  { 0.85f, 0.12f, 0.00f, 0.00f },
};
static const SDL_FColor kImpactBody[1 + kActionEffectGlowRings] = {
  { 1.00f, 0.98f, 0.90f, 0.70f },
  { 1.00f, 0.80f, 0.38f, 0.40f },
  { 1.00f, 0.40f, 0.07f, 0.17f },
  { 0.88f, 0.14f, 0.00f, 0.00f },
};
static const SDL_FColor kFlightEmbers[2] = {
  { 1.00f, 0.90f, 0.62f, 0.95f },
  { 0.90f, 0.20f, 0.01f, 0.00f },
};

static const PhaseModifier kPhaseModifiers[kActionEffectPhase_Count] = {
  /* Designated fields throughout: this struct has grown twice, and positional
   * initializers silently reinterpret every entry when a member is inserted
   * in the middle. */
  [kActionEffectPhase_None] = { 0 },
  /* Deliberately styleless, and stated rather than left to fall out of the
   * zero-initialiser: a Stardust actor that has not launched is sitting ON THE
   * PLAYER, so anything drawn here appears at the player's feet. That is
   * precisely the "stardust spawning in the ground" report. ModifierFor()
   * rejects a zero strength, so BuildBurst skips the instance entirely.
   * Do not give this a style. */
  [kActionEffectPhase_StardustPreLaunch] = { 0 },
  /* Fire's walls sweep sideways — measured heading 27 degrees off horizontal
   * on a 2.08:1 box — so the body orients to that. Its lift stays screen-up:
   * a fire still rises while it travels. */
  [kActionEffectPhase_FireIgnition] = {
    .strength = 0.78f, .size = 0.74f, .embers = 0.50f, .flare = 0.58f,
    .ember_mode = kEmberMode_Rise, .align_to_heading = true },
  [kActionEffectPhase_FireBloom] = {
    .strength = 1.00f, .size = 1.00f, .embers = 1.00f, .flare = 1.00f,
    .ember_mode = kEmberMode_Rise, .align_to_heading = true },
  /* The star IN FLIGHT. It previously had no rule at all, so a projectile was
   * invisible to the effect until it detonated. It now carries a real glow, a
   * comet body turned to its measured 45-degree descent, and a flame trail
   * streaming off that same heading — while the burst keeps Stardust's cold
   * sparkle, so the two stages read as ignition and detonation. */
  [kActionEffectPhase_StardustLaunch] = {
    .strength = 0.95f, .size = 1.05f, .embers = 0.85f, .flare = 0.55f,
    .ember_mode = kEmberMode_Trail, .body = kFlightBody,
    .ember_pair = kFlightEmbers, .align_to_heading = true },
  /* IMPACT. The burst body used to be sized at ~0.5x its own 32x32 extent —
   * exactly the radius of the ROM's circular explosion sprite — so the entire
   * glow sat underneath the opaque graphic and could not be seen. The size
   * here is what pushes a flame corona OUT past the sprite edge, where there
   * is background left to add to; the radial ember mode does the same for the
   * spray. Warm palette with a white-hot core: the comet is burning, so its
   * impact is a fireball, but it keeps a star's centre rather than becoming a
   * second Magical Fire. */
  [kActionEffectPhase_StardustBurst] = {
    .strength = 1.05f, .size = 1.95f, .embers = 1.00f, .flare = 2.60f,
    .ember_mode = kEmberMode_Burst, .body = kImpactBody,
    .ember_pair = kFlightEmbers },
  [kActionEffectPhase_AuraOrb] = {
    .strength = 1.00f, .size = 1.00f, .embers = 1.00f, .flare = 1.00f },
  [kActionEffectPhase_LightFlare] = {
    .strength = 1.00f, .size = 1.00f, .embers = 1.00f, .flare = 1.00f },
  /* The investigation is explicit that the 24-tick pre-beam visual must not
   * receive full intensity, or the spell appears to fire before it does. */
  [kActionEffectPhase_LightBeamCharge] = {
    .strength = 0.35f, .size = 0.60f, .embers = 0.25f, .flare = 0.30f },
  [kActionEffectPhase_LightBeam] = {
    .strength = 1.00f, .size = 1.00f, .embers = 0.60f, .flare = 0.50f },
};

static const SpellVisual *VisualFor(uint8_t kind) {
  if (kind <= kActionEffect_None || kind >= kActionEffect_KindCount)
    return NULL;
  const SpellVisual *visual = &kSpellVisuals[kind];
  /* A kind with no authored palette must draw nothing rather than black
   * geometry: fail closed the same way the capture does. */
  return visual->body_scale_x > 0.0f ? visual : NULL;
}

static const PhaseModifier *ModifierFor(uint8_t phase) {
  if (phase <= kActionEffectPhase_None || phase >= kActionEffectPhase_Count)
    return NULL;
  const PhaseModifier *modifier = &kPhaseModifiers[phase];
  return modifier->strength > 0.0f ? modifier : NULL;
}

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
static bool AppendGlow(ActionEffectGeometryWriter *writer,
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
  if (!Reserve(writer, kActionEffectGlowVertices, kActionEffectGlowIndices))
    return false;

  int centre = writer->vertex_count;
  SDL_FColor centre_color = style->centre;
  centre_color.a *= strength;
  writer->vertices[writer->vertex_count++] = (SDL_Vertex){
    anchor, centre_color, { 0.0f, 0.0f },
  };

  float radius_x = fmaxf(3.0f, style->radius_x * scale_x);
  float radius_y = fmaxf(2.0f, style->radius_y * scale_y);

  int ring_base[kRings];
  for (int r = 0; r < kRings; r++) {
    ring_base[r] = writer->vertex_count;
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
    /* Rotation by the style's axis. Because the axis arrives as a UNIT vector
     * it is already (cos, sin) — the whole orientation path stays free of
     * trigonometry, and so keeps the bit-identical determinism the batch is
     * tested for. */
    float ax = style->axis_x, ay = style->axis_y;
    for (int s = 0; s < kSegments; s++) {
      float shape = 1.0f + wobble * FlameSilhouette(
          style->seed,
          EffectVisualTicks(effect, (unsigned)effect->pulse_ticks), s);
      float ux = kCircle32[s][0] * radius_x * scale * shape;
      float uy = kCircle32[s][1] * radius_y * scale * shape;
      writer->vertices[writer->vertex_count++] = (SDL_Vertex){
        { anchor.x + ux * ax - uy * ay + style->lift_x * lift,
          anchor.y + ux * ay + uy * ax + style->lift_y * lift },
        color, { 0.0f, 0.0f },
      };
    }
  }

  /* Centre fan out to ring 0. */
  for (int s = 0; s < kSegments; s++) {
    writer->indices[writer->index_count++] = centre;
    writer->indices[writer->index_count++] = ring_base[0] + s;
    writer->indices[writer->index_count++] =
        ring_base[0] + (s + 1) % kSegments;
  }
  /* Quad strips between consecutive rings, two triangles per segment. */
  for (int r = 0; r + 1 < kRings; r++) {
    for (int s = 0; s < kSegments; s++) {
      int next = (s + 1) % kSegments;
      int inner0 = ring_base[r] + s, inner1 = ring_base[r] + next;
      int outer0 = ring_base[r + 1] + s, outer1 = ring_base[r + 1] + next;
      writer->indices[writer->index_count++] = inner0;
      writer->indices[writer->index_count++] = outer0;
      writer->indices[writer->index_count++] = outer1;
      writer->indices[writer->index_count++] = inner0;
      writer->indices[writer->index_count++] = outer1;
      writer->indices[writer->index_count++] = inner1;
    }
  }
  return true;
}

/* One source-alpha cloud puff. Unlike AppendGlow this is not a light and is
 * never submitted additively: its opaque-ish core actually conceals the hard
 * BG2/skybox seam, while two soft rings feather it into neighbouring puffs.
 * Per-puff radius, tint and opacity let four overlapping tiers imply volume
 * without a backend-specific 3D-noise shader. */
static bool AppendWaterfallMistCloud(
    ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect,
    float local_x, float local_y, float local_radius_x, float local_radius_y,
    SDL_FColor tint, float opacity, unsigned seed,
    ActionEffectProjectPointFn project_point, void *userdata) {
  enum {
    kSegments = kActionSceneEffectWaterfallMistCloudSegments,
    kRings = kActionEffectGlowRings,
  };
  SDL_FPoint anchor;
  float scale_x, scale_y;
  if (!ProjectWithScale(effect, project_point, userdata, local_x, local_y,
                        &anchor, &scale_x, &scale_y))
    return true;
  if (!Reserve(writer,
               kActionSceneEffectWaterfallMistCloudVertices,
               kActionSceneEffectWaterfallMistCloudIndices))
    return false;

  const SDL_FColor white = {1.0f, 1.0f, 1.0f, opacity};
  SDL_FColor centre_color = MixColor(tint, white, 0.42f);
  centre_color.a = opacity;
  const int centre = writer->vertex_count;
  writer->vertices[writer->vertex_count++] = (SDL_Vertex){
    anchor, centre_color, {0.0f, 0.0f},
  };

  const float radius_x = fmaxf(4.0f, local_radius_x * scale_x);
  const float radius_y = fmaxf(3.0f, local_radius_y * scale_y);
  static const float kRingScale[kRings] = {0.24f, 0.70f, 1.0f};
  SDL_FColor ring_color[kRings] = {
    MixColor(tint, white, 0.24f), tint, tint,
  };
  ring_color[0].a = opacity * 0.84f;
  ring_color[1].a = opacity * 0.34f;
  ring_color[2].a = 0.0f;
  const unsigned ticks = EffectVisualTicks(
      effect, (unsigned)effect->pulse_ticks) / 4u;

  int ring_base[kRings];
  for (int ring = 0; ring < kRings; ring++) {
    ring_base[ring] = writer->vertex_count;
    const float wobble = 0.035f + 0.055f * (float)ring;
    for (int segment = 0; segment < kSegments; segment++) {
      const int silhouette_segment =
          segment * kActionEffectGlowSegments / kSegments;
      const float shape = 1.0f + wobble * FlameSilhouette(
          seed, ticks, silhouette_segment);
      writer->vertices[writer->vertex_count++] = (SDL_Vertex){
        {
          anchor.x + kMistCircle12[segment][0] * radius_x *
              kRingScale[ring] * shape,
          anchor.y + kMistCircle12[segment][1] * radius_y *
              kRingScale[ring] * shape,
        },
        ring_color[ring], {0.0f, 0.0f},
      };
    }
  }

  for (int segment = 0; segment < kSegments; segment++) {
    const int next = (segment + 1) % kSegments;
    writer->indices[writer->index_count++] = centre;
    writer->indices[writer->index_count++] = ring_base[0] + segment;
    writer->indices[writer->index_count++] = ring_base[0] + next;
  }
  for (int ring = 0; ring + 1 < kRings; ring++) {
    for (int segment = 0; segment < kSegments; segment++) {
      const int next = (segment + 1) % kSegments;
      const int inner0 = ring_base[ring] + segment;
      const int inner1 = ring_base[ring] + next;
      const int outer0 = ring_base[ring + 1] + segment;
      const int outer1 = ring_base[ring + 1] + next;
      writer->indices[writer->index_count++] = inner0;
      writer->indices[writer->index_count++] = outer0;
      writer->indices[writer->index_count++] = outer1;
      writer->indices[writer->index_count++] = inner0;
      writer->indices[writer->index_count++] = outer1;
      writer->indices[writer->index_count++] = inner1;
    }
  }
  return true;
}

static bool AppendWaterfallMistCloudVolume(
    ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect, float pulse,
    ActionEffectProjectPointFn project_point, void *userdata) {
  enum { kColumns = 6, kTiers = 4 };
  _Static_assert(kColumns * kTiers ==
                     kActionSceneEffectWaterfallMistCloudCount,
                 "waterfall cloud layout must fill its bounded budget");
  static const float kTierY[kTiers] = {48.0f, 20.0f, -24.0f, -5.0f};
  static const float kTierRadiusX[kTiers] = {108.0f, 92.0f, 76.0f, 62.0f};
  static const float kTierRadiusY[kTiers] = {88.0f, 70.0f, 54.0f, 32.0f};
  static const float kTierOpacity[kTiers] = {0.090f, 0.115f, 0.135f, 0.205f};
  static const float kTierOffsetX[kTiers] = {0.0f, 28.0f, -20.0f, 12.0f};
  static const SDL_FColor kTierTint[kTiers] = {
    {0.58f, 0.76f, 0.88f, 1.0f},
    {0.68f, 0.85f, 0.94f, 1.0f},
    {0.78f, 0.91f, 0.98f, 1.0f},
    {0.90f, 0.97f, 1.00f, 1.0f},
  };
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float span = rect->x1 - rect->x0;
  const unsigned ticks = EffectVisualTicks(
      effect, (unsigned)effect->pulse_ticks);

  /* Back to front: broad cool banks establish depth, rising mid-tier puffs
   * break their upper silhouette, and the compact bright tier reads as fresh
   * foam boiling where the waterfall meets the cloud. */
  for (unsigned cloud = 0;
       cloud < kActionSceneEffectWaterfallMistCloudCount; cloud++) {
    const unsigned tier = cloud / kColumns;
    const unsigned column = cloud % kColumns;
    const unsigned seed = DeterministicHash_Mix32(
        (uint32_t)effect->pulse_generation ^
        ((uint32_t)cloud + 1u) * 0x9E3779B9u);
    const unsigned x_period = 132u + tier * 17u + (seed & 15u);
    const unsigned y_period = 96u + tier * 13u + ((seed >> 4) & 15u);
    float x_wave = TriangleWave(ticks + seed % x_period, x_period) * 2.0f - 1.0f;
    float y_wave = TriangleWave(
        ticks + (seed >> 8) % y_period, y_period);
    if (cloud & 1u) x_wave = -x_wave;
    const float lane = ((float)column + 0.5f) / (float)kColumns;
    const float jitter_x = (HashUnit(seed ^ 0x53u) - 0.5f) * 28.0f;
    const float jitter_y = (HashUnit(seed ^ 0xB5u) - 0.5f) *
        (tier == 3u ? 14.0f : 28.0f);
    float x = rect->x0 + span * lane + kTierOffsetX[tier] +
        jitter_x + x_wave * (6.0f + 2.5f * (float)tier);
    float y = kTierY[tier] + jitter_y -
        y_wave * (5.0f + 1.5f * (float)tier);
    /* A fixed first anchor gives the production projection regression one
     * stable point while the other 23 puffs drift independently around it. */
    if (cloud == 0u) {
      x = rect->x0 + span * lane;
      y = kTierY[tier];
    }
    const float size_jitter = 0.88f + 0.24f * HashUnit(seed ^ 0x71u);
    const float breathe = 0.94f + 0.08f * TriangleWave(
        ticks + (seed >> 16) % 113u, 113u);
    const float opacity = kTierOpacity[tier] *
        (0.90f + 0.10f * pulse) *
        (0.92f + 0.08f * HashUnit(seed ^ 0xA7u));
    if (!AppendWaterfallMistCloud(
            writer, effect, x, y,
            kTierRadiusX[tier] * size_jitter * breathe,
            kTierRadiusY[tier] * size_jitter / breathe,
            kTierTint[tier], opacity, seed,
            project_point, userdata))
      return false;
  }
  return true;
}

/* One ember's offset from its birth point at normalised age `t`, in local
 * units. Sampled twice by the emitter and differenced, which is what lets the
 * spark be drawn as a streak aligned with its own direction of travel. */
/* Streams backwards along the actor's heading and widens with age, so the
 * tail is a cone rather than a line. (dir_x, dir_y) is the unit vector
 * OPPOSITE travel; `spread` is the ember's signed lateral share. */
static void EmberOffsetTrail(float dir_x, float dir_y, float spread, float t,
                             float *x, float *y) {
  float distance = 2.0f + 30.0f * t;
  float lateral = spread * (1.2f + 4.5f * t);
  /* Perpendicular of the heading, so the cone opens across the direction of
   * travel rather than along a fixed screen axis. */
  *x = dir_x * distance - dir_y * lateral;
  *y = dir_y * distance + dir_x * lateral;
}

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

/* Ember styling is resolved PER FLAME, not once for the burst. A spell can run
 * several stages at the same moment — every measured Stardust snapshot had
 * flying stars and detonating ones alive together — so taking the mode and
 * palette from the burst's anchor gave bursting stars a flight trail (and
 * flying stars a detonation spray) depending only on which part happened to be
 * first in the frame. */
static bool AppendEmbers(ActionEffectGeometryWriter *writer,
                         const ActionEffectBurst *burst, unsigned count,
                         const SpellVisual *visual,
                         ActionEffectProjectPointFn project_point,
                         void *userdata) {
  const ActionEffectInstance *anchor = burst->anchor;
  if (!burst->flame_count) return true;

  for (uint32_t i = 0; i < count; i++) {
    uint32_t seed = DeterministicHash_Mix32(
        burst->anchor->pulse_generation * 0x9E3779B9u ^
        (uint32_t)burst->anchor->record_address * 0x85EBCA6Bu ^
        i * 0xC2B2AE35u);

    /* Pick a flame body, then a point inside it. Round-robin by index rather
     * than by hash, so the plume stays evenly shared between the walls no
     * matter how few embers are alive at once. */
    const ActionEffectFlame *flame = &burst->flame[i % burst->flame_count];
    /* A beam column is a shaft of light, not a fire: it has nothing to throw.
     * Skipping it here rather than zeroing the spell's ember count keeps
     * Light's centre flare sparking while its columns stay clean. */
    if (flame->role == kActionEffectRole_Column) continue;
    const PhaseModifier *flame_phase = ModifierFor(flame->phase);
    if (!flame_phase || flame_phase->embers <= 0.0f) continue;
    uint8_t mode = flame_phase->ember_mode;
    SDL_FColor ember_hot = flame_phase->ember_pair ? flame_phase->ember_pair[0]
                                                   : visual->ember_hot;
    SDL_FColor ember_cool = flame_phase->ember_pair ? flame_phase->ember_pair[1]
                                                    : visual->ember_cool;
    const ActionEffectLocalRect *part = &flame->rect;
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
    /* A trail is emitted FROM the projectile, so its particles are born at
     * the body centre and stream away; scattering their births across the
     * rect the way a fire does would smear the source into a cloud. */
    if (mode == kEmberMode_Trail || mode == kEmberMode_Burst) {
      birth_x = mid_x + (birth_u - 0.5f) * span_x * 0.25f;
      birth_y = (part->y0 + part->y1) * 0.5f + (birth_v - 0.5f) * span_y * 0.25f;
    }

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
    if (mode == kEmberMode_Burst) {
      /* Fan by INDEX rather than by hash so the spray covers the circle
       * instead of clumping: 7 is coprime with 32, so stepping the table by
       * it walks every direction before repeating. */
      const float *dir = kCircle32[
          (i * 7u + (seed >> 11)) & kActionEffectGlowSegmentMask];
      EmberOffsetBurst(dir[0], dir[1], t, &ox, &oy);
      EmberOffsetBurst(dir[0], dir[1], fmaxf(0.0f, t - 0.12f),
                       &prev_x, &prev_y);
    } else if (mode == kEmberMode_Trail) {
      /* Backwards along the actor's own heading. A stationary or not-yet-
       * moving actor has no heading to trail from, so fall back to rising:
       * that is the correct look for a star that has stopped. */
      float vx = (float)flame->source->velocity_x;
      float vy = (float)flame->source->velocity_y;
      float speed = hypotf(vx, vy);
      if (speed < 0.001f) {
        EmberOffset(drift, t, &ox, &oy);
        EmberOffset(drift, fmaxf(0.0f, t - 0.12f), &prev_x, &prev_y);
      } else {
        float dir_x = -vx / speed, dir_y = -vy / speed;
        float spread = (HashUnit(seed ^ 0x5Du) - 0.5f) * 2.0f;
        EmberOffsetTrail(dir_x, dir_y, spread, t, &ox, &oy);
        EmberOffsetTrail(dir_x, dir_y, spread, fmaxf(0.0f, t - 0.12f),
                         &prev_x, &prev_y);
      }
    } else {
      EmberOffset(drift, t, &ox, &oy);
      EmberOffset(drift, fmaxf(0.0f, t - 0.12f), &prev_x, &prev_y);
    }

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

    SDL_FColor color = MixColor(ember_hot, ember_cool, t);
    /* Fade in over the first few ticks of life. Without it an ember pops into
     * existence at full brightness inside the flame body, which reads as a
     * flicker artifact rather than as a spark being thrown. */
    float birth_fade = t * 8.0f;
    if (birth_fade > 1.0f) birth_fade = 1.0f;
    color.a *= burst->strength * flicker * birth_fade;

    static const int kQuad[] = { 0, 1, 2, 0, 2, 3 };
    if (!Reserve(writer, 4, 6)) return false;
    int base = writer->vertex_count;
    writer->vertices[writer->vertex_count++] = (SDL_Vertex){
      { position.x + dir_x * reach, position.y + dir_y * reach },
      color, { 0.0f, 0.0f } };
    writer->vertices[writer->vertex_count++] = (SDL_Vertex){
      { position.x - dir_y * width, position.y + dir_x * width },
      color, { 0.0f, 0.0f } };
    writer->vertices[writer->vertex_count++] = (SDL_Vertex){
      { position.x - dir_x * reach, position.y - dir_y * reach },
      color, { 0.0f, 0.0f } };
    writer->vertices[writer->vertex_count++] = (SDL_Vertex){
      { position.x + dir_y * width, position.y - dir_x * width },
      color, { 0.0f, 0.0f } };
    for (int n = 0; n < 6; n++)
      writer->indices[writer->index_count++] = base + kQuad[n];
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
        effect->geometry.kind != kActionEffectGeometry_Rect ||
        effect->obj_priority >= kActionEffectObjPriorityCount ||
        effect->render_layer != kActionEffectRenderLayer_WorldOverlay ||
        !RectIsSane(&effect->geometry.data.rect))
      continue;
    /* Any spell the tables describe. A kind or phase with no authored entry
     * draws nothing rather than defaulting to Fire's look — a wrong-coloured
     * spell is a worse bug than a missing one, and the capture already fails
     * closed the same way. */
    if (!VisualFor(effect->kind) || !ModifierFor(effect->phase)) continue;
    /* One cast is one spell: parts of a different kind cannot share a burst
     * (the controller only ever runs one spell at a time, so this is a
     * consistency guard rather than a real mixing case). */
    if (burst->anchor && effect->kind != burst->anchor->kind) continue;

    /* Re-express this part's rect in the anchor's local frame. */
    float shift_x = 0.0f, shift_y = 0.0f;
    if (!burst->anchor) {
      burst->anchor = effect;
      burst->min_x = burst->max_x = effect->geometry.data.rect.x0;
      burst->min_y = burst->max_y = effect->geometry.data.rect.y0;
    } else {
      shift_x = (float)(effect->world_x - burst->anchor->world_x);
      shift_y = (float)(effect->world_y - burst->anchor->world_y);
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
    burst->flame[burst->flame_count].role = effect->role;
    burst->flame[burst->flame_count].phase = effect->phase;
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
        if (burst->flame[a].role != burst->flame[b].role ||
            !RectsTouch(&burst->flame[a].rect, &burst->flame[b].rect))
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
  /* Only the populated prefixes are part of the batch contract. Clearing the
   * complete fixed-capacity arrays wrote about 50 KB even for an empty list. */
  batch->vertex_count = 0;
  batch->index_count = 0;
  if (!frame || frame->effect_count > kActionEffectMaxInstances)
    return false;
  if (!lighting_enabled && !particles_enabled) return true;
  if (!project_point) return false;
  ActionEffectGeometryWriter writer = GeometryWriter(
      batch->vertices, kActionEffectRenderMaxVertices,
      batch->indices, kActionEffectRenderMaxIndices);

  ActionEffectBurst burst;
  if (!BuildBurst(frame, &burst)) return true;

  float half_x = (burst.max_x - burst.min_x) * 0.5f;
  float half_y = (burst.max_y - burst.min_y) * 0.5f;
  float mid_x = (burst.min_x + burst.max_x) * 0.5f;
  float mid_y = (burst.min_y + burst.max_y) * 0.5f;

  const SpellVisual *visual = VisualFor(burst.anchor->kind);
  /* The burst-wide spill takes the STRONGEST live stage, not the anchor's.
   * The anchor is simply the first visible part, so it changes as slots
   * retire — driving the spill from it made the whole-burst glow jump between
   * stage styles from frame to frame while the spell itself was steady. */
  const PhaseModifier *phase = NULL;
  for (unsigned i = 0; i < burst.flame_count; i++) {
    const PhaseModifier *candidate = ModifierFor(burst.flame[i].phase);
    if (candidate && (!phase || candidate->strength > phase->strength))
      phase = candidate;
  }
  if (!visual || !phase) return true;   /* BuildBurst already vetted these */
  float strength = burst.strength * phase->strength;

  if (lighting_enabled) {
    /* Tier 1: light spill, one per burst. Sized off the real union extents
     * rather than a constant, so a wide-open cast reads wide and an opening
     * one reads small; the floors keep a not-yet-grown rect from collapsing
     * it. The spill is what ties several parts into one object visually, so
     * it spans the union even when the parts do not touch. */
    const ActionEffectGlowStyle spill = {
      .radius_x = fmaxf(24.0f, half_x * visual->spill_scale_x * phase->size),
      .radius_y = fmaxf(20.0f, half_y * visual->spill_scale_y * phase->size),
      /* Weighted toward the outside: a narrow hot middle with a long, soft
       * falloff is what reads as luminous. Even spacing reads as a disc. */
      .ring_scale = { 0.34f, 0.66f, 1.0f },
      .centre = visual->spill[0],
      .ring = { visual->spill[1], visual->spill[2], visual->spill[3] },
      .flare = visual->flare * 0.55f * phase->flare,
      .rise = visual->rise * 0.75f * phase->size,
      /* The spill spans the whole burst and has no single heading, so it
       * stays screen-aligned with hot gas rising. */
      .axis_x = 1.0f, .axis_y = 0.0f,
      .lift_x = 0.0f, .lift_y = -1.0f,
      .seed = (unsigned)burst.anchor->record_address >> 4,
    };
    if (!AppendGlow(&writer, burst.anchor, &spill, strength, mid_x, mid_y,
                    project_point, project_userdata))
      return false;

    /* Tier 2: one body per CLUSTER of touching same-role parts, not per
     * captured part. Its radii cover the merged extent, so a wall of fire or
     * a beam column is a single continuous body with one hot core, and the
     * flare gives it a turbulent outline rather than a clean ellipse. */
    for (unsigned i = 0; i < burst.flame_count; i++) {
      const ActionEffectLocalRect *rect = &burst.flame[i].rect;
      const PhaseModifier *part_phase = ModifierFor(burst.flame[i].phase);
      if (!part_phase) continue;
      /* A beam column is a shaft, not a plume: it neither climbs nor writhes,
       * so the rise and most of the flare are suppressed for that role and
       * the captured 16x224 rect is left to speak for itself. */
      bool column = burst.flame[i].role == kActionEffectRole_Column;
      /* A stage may restate the spell's body colours (see PhaseModifier): a
       * star in flight burns, the burst it becomes does not. */
      const SDL_FColor *body_palette =
          part_phase->body ? part_phase->body : visual->body;
      /* Orientation. Screen-aligned by default; a stage that travels turns
       * its body to its own heading so the glow agrees with art that is
       * plainly drawn along the direction of motion (Fire's walls sweep
       * horizontally, Stardust's stars descend at 45 degrees).
       *
       * The LIFT is a separate question from the AXIS. A projectile's aura
       * should trail backwards behind it, but a fire still rises even while
       * its wall sweeps sideways — so only a trailing stage redirects the
       * lift, and everything else keeps hot gas going up the screen. */
      float axis_x = 1.0f, axis_y = 0.0f;
      float lift_x = 0.0f, lift_y = -1.0f;
      float along = 1.0f, across = 1.0f;
      float heading_x, heading_y;
      if (part_phase->align_to_heading &&
          ActorHeading(burst.flame[i].source, &heading_x, &heading_y)) {
        axis_x = heading_x;
        axis_y = heading_y;
        if (part_phase->ember_mode == kEmberMode_Trail) {
          lift_x = -heading_x;
          lift_y = -heading_y;
          /* A projectile reads as a comet: long along travel, tight across.
           * Its captured extent is a SQUARE 16x16, so without this the
           * rotation would be mathematically applied and visually invisible. */
          along = 1.9f;
          across = 0.7f;
        }
      }
      const ActionEffectGlowStyle body = {
        .radius_x = fmaxf(8.0f, (rect->x1 - rect->x0) *
                              visual->body_scale_x * part_phase->size * along),
        .radius_y = fmaxf(7.0f, (rect->y1 - rect->y0) *
                              visual->body_scale_y * part_phase->size * across),
        /* Tight, so the hot core stays a highlight: a wide white centre just
         * bleaches the whole body to a flat disc. */
        .ring_scale = { 0.22f, 0.55f, 1.0f },
        .centre = body_palette[0],
        .ring = { body_palette[1], body_palette[2], body_palette[3] },
        .flare = visual->flare * part_phase->flare * (column ? 0.25f : 1.0f),
        .rise = column ? 0.0f : visual->rise,
        .axis_x = axis_x, .axis_y = axis_y,
        .lift_x = lift_x, .lift_y = lift_y,
        .seed = (unsigned)burst.flame[i].source->record_address >> 5,
      };
      if (!AppendGlow(&writer, burst.anchor, &body,
                      burst.strength * part_phase->strength,
                      (rect->x0 + rect->x1) * 0.5f,
                      (rect->y0 + rect->y1) * 0.5f,
                      project_point, project_userdata))
        return false;
    }
  }

  if (particles_enabled && visual->embers) {
    /* A whole-burst budget, so the count does not multiply with part count. */
    unsigned embers = (unsigned)((float)visual->embers * phase->embers);
    if (embers > kActionEffectMaxEmbers) embers = kActionEffectMaxEmbers;
    if (embers &&
        !AppendEmbers(&writer, &burst, embers, visual, project_point,
                      project_userdata))
      return false;
  }
  batch->vertex_count = writer.vertex_count;
  batch->index_count = writer.index_count;
  return true;
}

/* ── Independent scene lights ─────────────────────────────────────────────
 *
 * Scene effects intentionally reuse the same ring-gradient geometry and SDL
 * additive submission as spells. No renderer shader or backend-specific
 * uniform path is involved: GLSL/SPIR-V/MSL support therefore does not gate
 * these accents, and SDL's software/D3D renderers receive the same batch.
 * Unlike a spell cast, each scene record remains an independent light centre. */

static bool SceneActorHeading(const ActionEffectInstance *effect,
                              float *x, float *y) {
  if (!effect || !x || !y) return false;
  const float vx = (float)effect->velocity_x;
  const float vy = (float)effect->velocity_y;
  const float speed = hypotf(vx, vy);
  if (speed < 0.001f) return false;
  *x = vx / speed;
  *y = vy / speed;
  return true;
}

static void SceneFireballHeading(const ActionEffectInstance *effect,
                                 float *x, float *y) {
  if (!x || !y) return;
  *x = 1.0f;
  *y = 0.0f;
  if (SceneActorHeading(effect, x, y)) return;
  if (effect && effect->kind == kActionEffect_AitosLavaFireball) {
    /* Its reset frame sits above the pit before relaunch; retain the rising
     * shot's downward wake rather than snapping horizontally while stopped. */
    *x = 0.0f;
    *y = -1.0f;
  } else if (effect && effect->kind == kActionEffect_MarahnaFireball &&
             effect->phase == kActionEffectPhase_MarahnaFireballOrb) {
    /* `$E047` deliberately pauses twice in its left/right animation. A still
     * ball remains a flame: let heat climb instead of inventing a rightward
     * trail for the two zero-velocity entries. */
    *x = 0.0f;
    *y = 1.0f;
  }
}

/* Centres of the twelve 16x16 fireball parts in the wheel's four measured
 * full-ring compositions ($5276/$5398/$54BA/$55DC). The compositions rotate
 * tile art and flip selection, but this symmetric set of authored centres is
 * invariant. Keeping the literal OAM-local anchors prevents a procedural
 * circle from drifting away from the authentic square-round silhouette. */
static const float
kFlamingWheelFireballAnchors[kActionSceneEffectFlamingWheelFireballs][2] = {
  {-24.0f, -24.0f}, {-8.0f, -24.0f}, {8.0f, -24.0f}, {24.0f, -24.0f},
  {-24.0f,  -8.0f}, {24.0f,  -8.0f},
  {-24.0f,   8.0f}, {24.0f,   8.0f},
  {-24.0f,  24.0f}, {-8.0f,  24.0f}, {8.0f,  24.0f}, {24.0f,  24.0f},
};

static bool FlamingWheelHasAuthoredFireballRing(
    const ActionEffectInstance *effect) {
  if (!effect || effect->kind != kActionEffect_FlamingWheel) return false;
  return effect->composition == 0x5276 || effect->composition == 0x5398 ||
      effect->composition == 0x54BA || effect->composition == 0x55DC;
}

static bool AppendFlamingWheelFireballLighting(
    ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect, float pulse,
    ActionEffectProjectPointFn project_point, void *userdata) {
  if (!FlamingWheelHasAuthoredFireballRing(effect)) return true;
  for (unsigned i = 0; i < kActionSceneEffectFlamingWheelFireballs; i++) {
    const ActionEffectGlowStyle flame = {
      .radius_x = 10.5f, .radius_y = 12.5f,
      .ring_scale = {0.20f, 0.60f, 1.0f},
      .centre = {1.00f, 1.00f, 0.78f, 0.72f},
      .ring = {{1.00f, 0.70f, 0.16f, 0.42f},
               {1.00f, 0.22f, 0.01f, 0.15f},
               {0.72f, 0.04f, 0.00f, 0.00f}},
      .flare = 0.34f, .rise = 0.38f,
      .axis_x = 1.0f, .lift_y = -1.0f,
      .seed = (unsigned)effect->pulse_generation + i * 0x45D9u,
    };
    if (!AppendGlow(writer, effect, &flame, pulse,
                    kFlamingWheelFireballAnchors[i][0],
                    kFlamingWheelFireballAnchors[i][1],
                    project_point, userdata))
      return false;
  }
  return true;
}

static unsigned LavaReservoirGlowSegmentCount(
    const ActionEffectInstance *effect) {
  if (!effect || effect->kind != kActionEffect_AitosLavaReservoir ||
      effect->geometry.kind != kActionEffectGeometry_Rect)
    return 0;
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float width = rect->x1 - rect->x0;
  if (!isfinite(width) || width <= 0.0f) return 0;
  if (width > (float)(kActionSceneEffectMaxLavaGlowSegments *
                      kActionSceneEffectLavaGlowSpanPixels))
    return kActionSceneEffectMaxLavaGlowSegments + 1u;
  return (unsigned)ceilf(
      width / (float)kActionSceneEffectLavaGlowSpanPixels);
}

/* Long Act-2 lakes cannot use one reservoir-wide radial gradient: its outer
 * ring is intentionally transparent, so the ends look unlit until the camera
 * approaches the lake centre. Overlapping bounded emitters keep every part of
 * the lip locally hot and also follow Diorama perspective more faithfully. */
static bool AppendLavaReservoirLighting(
    ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect, float pulse,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const unsigned segments = LavaReservoirGlowSegmentCount(effect);
  if (!segments || segments > kActionSceneEffectMaxLavaGlowSegments)
    return false;
  const float segment_width = (rect->x1 - rect->x0) / (float)segments;
  for (unsigned i = 0; i < segments; i++) {
    const float centre_x = rect->x0 + ((float)i + 0.5f) * segment_width;
    const ActionEffectGlowStyle spill = {
      .radius_x = fmaxf(38.0f, segment_width * 0.72f + 12.0f),
      .radius_y = 42.0f,
      .ring_scale = {0.18f, 0.68f, 1.0f},
      .centre = {1.00f, 0.43f, 0.04f, 0.13f},
      .ring = {{1.00f, 0.28f, 0.01f, 0.10f},
               {0.82f, 0.07f, 0.00f, 0.04f},
               {0.48f, 0.01f, 0.00f, 0.00f}},
      .flare = 0.07f, .rise = 0.10f,
      .axis_x = 1.0f, .lift_y = -1.0f,
      .seed = (unsigned)effect->generation + i * 0x5BD1u,
    };
    const ActionEffectGlowStyle body = {
      .radius_x = fmaxf(30.0f, segment_width * 0.58f + 7.0f),
      .radius_y = 9.0f,
      .ring_scale = {0.15f, 0.78f, 1.0f},
      .centre = {1.00f, 1.00f, 0.72f, 0.62f},
      .ring = {{1.00f, 0.68f, 0.10f, 0.36f},
               {1.00f, 0.20f, 0.01f, 0.13f},
               {0.72f, 0.04f, 0.00f, 0.00f}},
      .flare = 0.12f, .rise = 0.18f,
      .axis_x = 1.0f, .lift_y = -1.0f,
      .seed = (unsigned)effect->pulse_generation + i * 0x7A4Du,
    };
    if (!AppendGlow(writer, effect, &spill, pulse, centre_x, -10.0f,
                    project_point, userdata) ||
        !AppendGlow(writer, effect, &body, pulse, centre_x, 0.0f,
                    project_point, userdata))
      return false;
  }
  return true;
}

static bool AppendSceneParticle(ActionEffectGeometryWriter *writer,
                                const ActionEffectInstance *effect,
                                float x, float y, float previous_x,
                                float previous_y, float width, float reach,
                                SDL_FColor color,
                                ActionEffectProjectPointFn project_point,
                                void *userdata) {
  SDL_FPoint position, previous;
  float scale_x, scale_y;
  if (!ProjectWithScale(effect, project_point, userdata, x, y, &position,
                        &scale_x, &scale_y) ||
      !project_point(userdata, effect, previous_x, previous_y, &previous))
    return true;
  const float output_scale = fmaxf(0.5f, (scale_x + scale_y) * 0.5f);
  float dx = position.x - previous.x;
  float dy = position.y - previous.y;
  const float length = hypotf(dx, dy);
  if (length < 0.001f) {
    dx = 0.0f;
    dy = -1.0f;
  } else {
    dx /= length;
    dy /= length;
  }
  width *= output_scale;
  reach *= output_scale;
  if (!Reserve(writer, 4, 6)) return false;
  const int base = writer->vertex_count;
  writer->vertices[writer->vertex_count++] = (SDL_Vertex){
    {position.x + dx * reach, position.y + dy * reach}, color, {0.0f, 0.0f}};
  writer->vertices[writer->vertex_count++] = (SDL_Vertex){
    {position.x - dy * width, position.y + dx * width}, color, {0.0f, 0.0f}};
  writer->vertices[writer->vertex_count++] = (SDL_Vertex){
    {position.x - dx * reach, position.y - dy * reach}, color, {0.0f, 0.0f}};
  writer->vertices[writer->vertex_count++] = (SDL_Vertex){
    {position.x + dy * width, position.y - dx * width}, color, {0.0f, 0.0f}};
  static const int kQuad[6] = {0, 1, 2, 0, 2, 3};
  for (int i = 0; i < 6; i++)
    writer->indices[writer->index_count++] = base + kQuad[i];
  return true;
}

/* A sword-beam sparkle is two crossed additive diamonds. Forty-eight fixed
 * glints independently materialize along the magical path rather than moving
 * backward like fire embers. The bounded extra capacity is explicit in
 * action_effect_render.h.
 * Projected local unit vectors keep the cross on the OBJ plane in Diorama
 * mode instead of leaving it screen-axis-aligned. */
static bool AppendSceneStarParticle(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    float local_x, float local_y, float size, SDL_FColor color,
    ActionEffectProjectPointFn project_point, void *userdata) {
  SDL_FPoint centre, sample_x, sample_y;
  if (!project_point(userdata, effect, local_x, local_y, &centre) ||
      !project_point(userdata, effect, local_x + 1.0f, local_y, &sample_x) ||
      !project_point(userdata, effect, local_x, local_y + 1.0f, &sample_y))
    return true;
  float xx = sample_x.x - centre.x, xy = sample_x.y - centre.y;
  float yx = sample_y.x - centre.x, yy = sample_y.y - centre.y;
  const float x_length = hypotf(xx, xy), y_length = hypotf(yx, yy);
  if (x_length < 0.001f || y_length < 0.001f) return true;
  xx /= x_length;
  xy /= x_length;
  yx /= y_length;
  yy /= y_length;
  const float long_x = size * x_length;
  const float long_y = size * 1.35f * y_length;
  const float thin_x = fmaxf(0.45f, size * 0.23f * x_length);
  const float thin_y = fmaxf(0.45f, size * 0.23f * y_length);
  if (!Reserve(writer, 8, 12)) return false;
  const int base = writer->vertex_count;
  const SDL_FPoint points[] = {
    {centre.x + xx * long_x, centre.y + xy * long_x},
    {centre.x + yx * thin_y, centre.y + yy * thin_y},
    {centre.x - xx * long_x, centre.y - xy * long_x},
    {centre.x - yx * thin_y, centre.y - yy * thin_y},
    {centre.x + yx * long_y, centre.y + yy * long_y},
    {centre.x + xx * thin_x, centre.y + xy * thin_x},
    {centre.x - yx * long_y, centre.y - yy * long_y},
    {centre.x - xx * thin_x, centre.y - xy * thin_x},
  };
  for (unsigned i = 0; i < 8; i++)
    writer->vertices[writer->vertex_count++] =
        (SDL_Vertex){points[i], color, {0.0f, 0.0f}};
  static const int kDiamonds[12] = {
    0, 1, 2, 0, 2, 3,
    4, 5, 6, 4, 6, 7,
  };
  for (unsigned i = 0; i < 12; i++)
    writer->indices[writer->index_count++] = base + kDiamonds[i];
  return true;
}

/* Visuals $00-$05 are not frames of one generic bolt. The `$7E:5000`
 * compositions author two different centre lines (vertical and diagonal),
 * each clipped to long/medium/short lengths. These are the per-row centroids
 * of the real 8x8 OAM parts at $5346/$5401/$5492 and $54F2/$55C2/$5661.
 * Following them fixes both endpoint placement and every bend angle; a coarse
 * interpolation across the culling rect cannot recover this information. */
static const int8_t kBossLightningVerticalX[] = {
  4, 4, 4, 4, 4, 4, 4, 4, 4, 3, 6, 7, 6, 5, 7, 7, 2, 5, 4,
  4, 4, 4, 6, 6, 4,
};
static const int8_t kBossLightningDiagonalX[] = {
  0, -4, -5, -5, -6, -12, -10, -11, -16, -20, -20, -22, -26,
  -30, -30, -32, -30, -29, -29, -33, -36, -36, -40, -44, -44,
};

static bool BossLightningPathFor(const ActionEffectInstance *effect,
                                 const int8_t **path_x,
                                 unsigned *joint_count) {
  if (!effect || !joint_count || effect->visual > 5u)
    return false;
  const unsigned family_visual = effect->visual % 3u;
  if (path_x) {
    *path_x = effect->visual < 3u ? kBossLightningVerticalX
                                 : kBossLightningDiagonalX;
  }
  *joint_count = family_visual == 0u ? 25u
      : (family_visual == 1u ? 19u : 13u);
  return true;
}

static bool BossLightningPathPoint(const ActionEffectInstance *effect,
                                   unsigned joint, float *x, float *y) {
  const int8_t *path_x = NULL;
  unsigned joint_count = 0;
  if (!BossLightningPathFor(effect, &path_x, &joint_count) ||
      joint >= joint_count || !x || !y)
    return false;
  *x = (float)path_x[joint];
  if (effect->flags & kActionEffectFlag_FlipHorizontal) *x = -*x;
  /* `$8D68`'s action-OBJ emitter stores Y with one extra draw-bias pixel
   * after the camera-origin bias cancels. Subtract it here so the filament
   * runs through the emitted tile centres, not one row below them. */
  *y = -80.0f + (float)joint * 8.0f;
  return true;
}

static bool BossLightningPathSample(const ActionEffectInstance *effect,
                                    float along, float *x, float *y) {
  unsigned joint_count = 0;
  if (!BossLightningPathFor(effect, NULL, &joint_count) || !x || !y ||
      !isfinite(along))
    return false;
  along = fmaxf(0.0f, fminf(1.0f, along));
  const float scaled = along * (float)(joint_count - 1u);
  unsigned segment = (unsigned)scaled;
  if (segment >= joint_count - 1u)
    return BossLightningPathPoint(effect, joint_count - 1u, x, y);
  float x0, y0, x1, y1;
  if (!BossLightningPathPoint(effect, segment, &x0, &y0) ||
      !BossLightningPathPoint(effect, segment + 1u, &x1, &y1))
    return false;
  const float t = scaled - (float)segment;
  *x = x0 + (x1 - x0) * t;
  *y = y0 + (y1 - y0) * t;
  return true;
}

static bool AppendProjectedRibbonSegments(
    ActionEffectGeometryWriter *writer, const SDL_FPoint *points,
    const float *scales, unsigned joint_count, float half_width,
    SDL_FColor color) {
  if (!writer || !points || !scales || joint_count < 2u) return true;
  const unsigned segment_count = joint_count - 1u;
  if (!Reserve(writer, (int)segment_count * 4, (int)segment_count * 6))
    return false;

  for (unsigned i = 0; i < segment_count; i++) {
    float dx = points[i + 1].x - points[i].x;
    float dy = points[i + 1].y - points[i].y;
    const float length = hypotf(dx, dy);
    if (length < 0.001f) continue;
    dx /= length;
    dy /= length;
    const float taper0 = i == 0 ? 0.55f : 1.0f;
    const float taper1 = i + 1 == segment_count
        ? 0.55f : 1.0f;
    const float width0 = half_width * scales[i] * taper0;
    const float width1 = half_width * scales[i + 1] * taper1;
    const int base = writer->vertex_count;
    writer->vertices[writer->vertex_count++] = (SDL_Vertex){
      {points[i].x - dy * width0, points[i].y + dx * width0},
      color, {0.0f, 0.0f},
    };
    writer->vertices[writer->vertex_count++] = (SDL_Vertex){
      {points[i].x + dy * width0, points[i].y - dx * width0},
      color, {0.0f, 0.0f},
    };
    writer->vertices[writer->vertex_count++] = (SDL_Vertex){
      {points[i + 1].x + dy * width1, points[i + 1].y - dx * width1},
      color, {0.0f, 0.0f},
    };
    writer->vertices[writer->vertex_count++] = (SDL_Vertex){
      {points[i + 1].x - dy * width1, points[i + 1].y + dx * width1},
      color, {0.0f, 0.0f},
    };
    static const int kQuad[6] = {0, 1, 2, 0, 2, 3};
    for (int j = 0; j < 6; j++)
      writer->indices[writer->index_count++] = base + kQuad[j];
  }
  return true;
}

static bool AppendBossLightningRibbonLayer(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    float half_width, SDL_FColor color,
    ActionEffectProjectPointFn project_point, void *userdata) {
  enum { kJoints = kActionSceneEffectLightningSegments + 1 };
  SDL_FPoint points[kJoints];
  float scales[kJoints];
  unsigned joint_count = 0;
  if (!BossLightningPathFor(effect, NULL, &joint_count)) return true;
  for (unsigned i = 0; i < joint_count; i++) {
    float local_x, local_y, scale_x, scale_y;
    if (!BossLightningPathPoint(effect, i, &local_x, &local_y) ||
        !ProjectWithScale(effect, project_point, userdata, local_x, local_y,
                          &points[i], &scale_x, &scale_y))
      return true;
    scales[i] = fmaxf(0.5f, (scale_x + scale_y) * 0.5f);
  }
  return AppendProjectedRibbonSegments(writer, points, scales, joint_count,
                                       half_width, color);
}

static bool AppendBossLightningRibbon(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  if (effect->phase != kActionEffectPhase_BossLightningStrike) return true;
  const float pulse = DeterministicPulse(effect);
  SDL_FColor corona = {1.00f, 0.54f, 0.04f, 0.17f * pulse};
  SDL_FColor filament = {1.00f, 0.98f, 0.68f, 0.88f * pulse};
  return AppendBossLightningRibbonLayer(
             writer, effect, 4.8f, corona, project_point, userdata) &&
      AppendBossLightningRibbonLayer(
             writer, effect, 1.15f, filament, project_point, userdata);
}

static bool AppendMarahnaLightningRibbonLayer(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    float half_width, SDL_FColor color,
    ActionEffectProjectPointFn project_point, void *userdata) {
  enum { kJoints = kActionSceneEffectMarahnaLightningSegments + 1 };
  SDL_FPoint points[kJoints];
  float scales[kJoints];
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const bool horizontal = rect->x1 - rect->x0 >= rect->y1 - rect->y0;
  const float mid_x = (rect->x0 + rect->x1) * 0.5f;
  const float mid_y = (rect->y0 + rect->y1) * 0.5f;
  const unsigned ticks = EffectVisualTicks(
      effect, (unsigned)effect->phase_ticks);
  for (unsigned i = 0; i < kJoints; i++) {
    const float along = (float)i / (float)(kJoints - 1u);
    float x = horizontal ? rect->x0 + (rect->x1 - rect->x0) * along
                         : mid_x;
    float y = horizontal ? mid_y
                         : rect->y0 + (rect->y1 - rect->y0) * along;
    if (i && i + 1u < kJoints) {
      const uint32_t seed = DeterministicHash_Mix32(
          effect->generation * 0x9E3779B9u ^ i * 0x85EBCA6Bu);
      const float static_bend = HashUnit(seed) - 0.5f;
      const float animated_bend =
          TriangleWave(ticks + i * 5u, 13u) - 0.5f;
      const float bend = static_bend * 3.0f + animated_bend * 5.0f;
      if (horizontal) y += bend;
      else x += bend;
    }
    float scale_x, scale_y;
    if (!ProjectWithScale(effect, project_point, userdata, x, y, &points[i],
                          &scale_x, &scale_y))
      return true;
    scales[i] = fmaxf(0.5f, (scale_x + scale_y) * 0.5f);
  }
  return AppendProjectedRibbonSegments(writer, points, scales, kJoints,
                                       half_width, color);
}

static bool AppendMarahnaLightningRibbon(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  if (effect->kind != kActionEffect_MarahnaLightningLink ||
      effect->phase != kActionEffectPhase_MarahnaLightningActive)
    return true;
  const float pulse = DeterministicPulse(effect);
  const SDL_FColor corona = {0.24f, 0.34f, 1.00f, 0.24f * pulse};
  const SDL_FColor filament = {0.90f, 0.98f, 1.00f, 0.94f * pulse};
  return AppendMarahnaLightningRibbonLayer(
             writer, effect, 4.0f, corona, project_point, userdata) &&
      AppendMarahnaLightningRibbonLayer(
             writer, effect, 1.05f, filament, project_point, userdata);
}

static void MarahnaBossBoltEndpoints(const ActionEffectInstance *effect,
                                     float *x0, float *y0,
                                     float *x1, float *y1) {
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const bool left = effect->velocity_x < 0;
  *x0 = left ? rect->x1 : rect->x0;
  *y0 = rect->y0;
  *x1 = left ? rect->x0 : rect->x1;
  *y1 = rect->y1;
}

static bool AppendMarahnaBossLightningRibbonLayer(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    float half_width, SDL_FColor color,
    ActionEffectProjectPointFn project_point, void *userdata) {
  enum { kJoints = kActionSceneEffectMarahnaBossLightningSegments + 1 };
  SDL_FPoint points[kJoints];
  float scales[kJoints];
  float x0, y0, x1, y1;
  MarahnaBossBoltEndpoints(effect, &x0, &y0, &x1, &y1);
  const float dx = x1 - x0;
  const float dy = y1 - y0;
  const float length = hypotf(dx, dy);
  if (length < 0.001f) return true;
  const float normal_x = -dy / length;
  const float normal_y = dx / length;
  const unsigned ticks = EffectVisualTicks(
      effect, (unsigned)effect->phase_ticks);
  for (unsigned i = 0; i < kJoints; i++) {
    const float along = (float)i / (float)(kJoints - 1u);
    float x = x0 + dx * along;
    float y = y0 + dy * along;
    if (i && i + 1u < kJoints) {
      const uint32_t seed = DeterministicHash_Mix32(
          effect->generation * 0x9E3779B9u ^ i * 0x85EBCA6Bu);
      const float bend = (HashUnit(seed) - 0.5f) * 3.0f +
          (TriangleWave(ticks + i * 5u, 11u) - 0.5f) * 5.0f;
      x += normal_x * bend;
      y += normal_y * bend;
    }
    float scale_x, scale_y;
    if (!ProjectWithScale(effect, project_point, userdata, x, y, &points[i],
                          &scale_x, &scale_y))
      return true;
    scales[i] = fmaxf(0.5f, (scale_x + scale_y) * 0.5f);
  }
  return AppendProjectedRibbonSegments(writer, points, scales, kJoints,
                                       half_width, color);
}

static bool AppendMarahnaBossLightningRibbon(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  if (effect->kind != kActionEffect_MarahnaBossLightning ||
      effect->phase != kActionEffectPhase_MarahnaBossLightningBolt)
    return true;
  const float pulse = DeterministicPulse(effect);
  const SDL_FColor corona = {0.22f, 0.48f, 1.00f, 0.27f * pulse};
  const SDL_FColor filament = {0.92f, 0.99f, 1.00f, 0.96f * pulse};
  return AppendMarahnaBossLightningRibbonLayer(
             writer, effect, 4.4f, corona, project_point, userdata) &&
      AppendMarahnaBossLightningRibbonLayer(
             writer, effect, 1.10f, filament, project_point, userdata);
}

static bool AppendSwordBeamTrailLayer(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    float length, float head_half_width, float tail_half_width,
    SDL_FColor head_color, SDL_FColor tail_color,
    ActionEffectProjectPointFn project_point, void *userdata) {
  float hx = 1.0f, hy = 0.0f;
  if (!SceneActorHeading(effect, &hx, &hy)) return true;
  const float px = -hy, py = hx;
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float centre_x = (rect->x0 + rect->x1) * 0.5f;
  const float centre_y = (rect->y0 + rect->y1) * 0.5f;
  const float tail_x = centre_x - hx * length;
  const float tail_y = centre_y - hy * length;
  const float local_x[] = {
    centre_x + px * head_half_width,
    centre_x - px * head_half_width,
    tail_x - px * tail_half_width,
    tail_x + px * tail_half_width,
  };
  const float local_y[] = {
    centre_y + py * head_half_width,
    centre_y - py * head_half_width,
    tail_y - py * tail_half_width,
    tail_y + py * tail_half_width,
  };
  SDL_FPoint points[4];
  for (unsigned i = 0; i < 4; i++)
    if (!project_point(userdata, effect, local_x[i], local_y[i], &points[i]))
      return true;
  if (!Reserve(writer, 4, 6)) return false;
  const int base = writer->vertex_count;
  writer->vertices[writer->vertex_count++] =
      (SDL_Vertex){points[0], head_color, {0.0f, 0.0f}};
  writer->vertices[writer->vertex_count++] =
      (SDL_Vertex){points[1], head_color, {0.0f, 0.0f}};
  writer->vertices[writer->vertex_count++] =
      (SDL_Vertex){points[2], tail_color, {0.0f, 0.0f}};
  writer->vertices[writer->vertex_count++] =
      (SDL_Vertex){points[3], tail_color, {0.0f, 0.0f}};
  static const int kQuad[6] = {0, 1, 2, 0, 2, 3};
  for (unsigned i = 0; i < 6; i++)
    writer->indices[writer->index_count++] = base + kQuad[i];
  return true;
}

static bool AppendSwordBeamTrail(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  if (effect->kind != kActionEffect_SwordBeam ||
      effect->phase != kActionEffectPhase_SwordBeamFlight)
    return true;
  const float pulse = DeterministicPulse(effect);
  SDL_FColor outer_head = {0.32f, 0.78f, 1.00f, 0.19f * pulse};
  SDL_FColor outer_tail = {0.05f, 0.22f, 0.82f, 0.00f};
  SDL_FColor inner_head = {0.78f, 0.98f, 1.00f, 0.27f * pulse};
  SDL_FColor inner_tail = {0.10f, 0.42f, 1.00f, 0.00f};
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float crescent_half_height = (rect->y1 - rect->y0) * 0.5f;
  const float outer_head_width = fmaxf(6.0f, crescent_half_height * 0.95f);
  const float inner_head_width = fmaxf(2.75f,
                                       crescent_half_height * 0.58f);
  /* Both layers now meet nearly the full decoded crescent height. They remain
   * low-alpha and taper immediately, avoiding the detached headlight while
   * fixing the centreline-only attachment captured in 20260810-190729. */
  return AppendSwordBeamTrailLayer(
             writer, effect, 80.0f, outer_head_width, 2.0f,
             outer_head, outer_tail, project_point, userdata) &&
      AppendSwordBeamTrailLayer(
             writer, effect, 56.0f, inner_head_width, 1.0f,
             inner_head, inner_tail, project_point, userdata);
}

static bool AppendSceneParticles(ActionEffectGeometryWriter *writer,
                                 const ActionEffectInstance *effect,
                                 ActionEffectProjectPointFn project_point,
                                 void *userdata) {
  unsigned count = 0;
  SDL_FColor hot = {0}, cool = {0};
  switch (effect->kind) {
    case kActionEffect_WallTorch:
      count = 7;
      hot = (SDL_FColor){1.00f, 0.92f, 0.55f, 0.88f};
      cool = (SDL_FColor){0.95f, 0.18f, 0.01f, 0.00f};
      break;
    case kActionEffect_EnemyFireball:
    case kActionEffect_MarahnaFireball:
    case kActionEffect_AitosLavaFireball:
      count = kActionSceneEffectParticlesPerInstance;
      hot = (SDL_FColor){1.00f, 0.97f, 0.78f, 0.96f};
      cool = (SDL_FColor){1.00f, 0.10f, 0.00f, 0.00f};
      break;
    case kActionEffect_AitosStatueFire:
      count = kActionSceneEffectParticlesPerInstance;
      hot = (SDL_FColor){1.00f, 0.96f, 0.62f, 0.96f};
      cool = (SDL_FColor){0.98f, 0.08f, 0.00f, 0.00f};
      break;
    case kActionEffect_AitosLavaPit:
      count = kActionSceneEffectParticlesPerInstance;
      hot = (SDL_FColor){1.00f, 0.91f, 0.38f, 0.92f};
      cool = (SDL_FColor){0.90f, 0.06f, 0.00f, 0.00f};
      break;
    case kActionEffect_AitosLavaReservoir:
      count = kActionSceneEffectLavaReservoirParticleCount;
      hot = (SDL_FColor){1.00f, 0.94f, 0.48f, 0.94f};
      cool = (SDL_FColor){0.94f, 0.08f, 0.00f, 0.00f};
      break;
    case kActionEffect_AitosMoltenRock:
      count = kActionSceneEffectParticlesPerInstance;
      hot = (SDL_FColor){1.00f, 0.92f, 0.48f, 0.86f};
      cool = (SDL_FColor){0.94f, 0.12f, 0.00f, 0.00f};
      break;
    case kActionEffect_AitosWaterSplash:
      count = kActionSceneEffectParticlesPerInstance;
      hot = (SDL_FColor){0.92f, 1.00f, 1.00f, 0.86f};
      cool = (SDL_FColor){0.12f, 0.48f, 1.00f, 0.00f};
      break;
    case kActionEffect_AitosWaterfall:
      count = kActionSceneEffectWaterfallParticleCount;
      hot = (SDL_FColor){0.82f, 0.98f, 1.00f, 0.46f};
      cool = (SDL_FColor){0.08f, 0.42f, 0.82f, 0.00f};
      break;
    case kActionEffect_AitosWaterfallMist:
      count = kActionSceneEffectWaterfallMistParticleCount;
      hot = (SDL_FColor){0.96f, 1.00f, 1.00f, 0.54f};
      cool = (SDL_FColor){0.34f, 0.70f, 0.92f, 0.00f};
      break;
    case kActionEffect_SwordBeam:
      count = kActionSceneEffectSwordStarCount;
      hot = (SDL_FColor){0.96f, 1.00f, 1.00f, 1.00f};
      cool = (SDL_FColor){0.10f, 0.48f, 1.00f, 0.55f};
      break;
    case kActionEffect_LightningTrap:
      count = kActionSceneEffectParticlesPerInstance;
      hot = (SDL_FColor){0.98f, 1.00f, 1.00f, 0.92f};
      cool = (SDL_FColor){0.18f, 0.48f, 1.00f, 0.00f};
      break;
    case kActionEffect_MarahnaLightningLink:
      count = kActionSceneEffectParticlesPerInstance;
      hot = (SDL_FColor){0.98f, 1.00f, 1.00f, 0.96f};
      cool = (SDL_FColor){0.20f, 0.18f, 1.00f, 0.00f};
      break;
    case kActionEffect_MarahnaBossLightning:
      count = kActionSceneEffectParticlesPerInstance;
      hot = (SDL_FColor){0.98f, 1.00f, 1.00f, 0.98f};
      cool = (SDL_FColor){0.10f, 0.32f, 1.00f, 0.00f};
      break;
    case kActionEffect_BloodpoolBossLightning:
      count = kActionSceneEffectParticlesPerInstance;
      hot = (SDL_FColor){1.00f, 1.00f, 0.82f, 0.98f};
      cool = (SDL_FColor){1.00f, 0.34f, 0.01f, 0.00f};
      break;
    case kActionEffect_MinotaurAxe:
      count = kActionSceneEffectParticlesPerInstance;
      hot = (SDL_FColor){1.00f, 1.00f, 0.90f, 0.92f};
      cool = (SDL_FColor){1.00f, 0.48f, 0.06f, 0.00f};
      break;
    case kActionEffect_FlamingWheel:
      /* Transitional boss compositions do not contain the complete rim.
       * Keep their body light, but do not attach sparks to parts that are not
       * authored in that frame. */
      count = FlamingWheelHasAuthoredFireballRing(effect)
          ? kActionSceneEffectParticlesPerInstance : 0;
      hot = (SDL_FColor){1.00f, 0.96f, 0.58f, 0.96f};
      cool = (SDL_FColor){0.96f, 0.08f, 0.00f, 0.00f};
      break;
    case kActionEffect_FlamingWheelProjectile:
      count = kActionSceneEffectParticlesPerInstance;
      hot = (SDL_FColor){0.88f, 1.00f, 1.00f, 0.96f};
      cool = (SDL_FColor){0.02f, 0.58f, 0.86f, 0.00f};
      break;
    case kActionEffect_IceDragonIceBall:
      count = kActionSceneEffectParticlesPerInstance;
      hot = (SDL_FColor){0.96f, 1.00f, 1.00f, 0.94f};
      cool = (SDL_FColor){0.10f, 0.55f, 1.00f, 0.00f};
      break;
    case kActionEffect_TanzaraProjectile:
      count = kActionSceneEffectParticlesPerInstance;
      hot = (SDL_FColor){1.00f, 0.94f, 1.00f, 0.94f};
      cool = (SDL_FColor){0.62f, 0.08f, 1.00f, 0.00f};
      break;
    default:
      return true;
  }

  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const unsigned visual_ticks = EffectVisualTicks(
      effect, (unsigned)effect->pulse_ticks);
  float heading_x = 1.0f, heading_y = 0.0f;
  if (effect->kind == kActionEffect_EnemyFireball ||
      effect->kind == kActionEffect_MarahnaFireball ||
      effect->kind == kActionEffect_AitosLavaFireball)
    SceneFireballHeading(effect, &heading_x, &heading_y);
  else
    SceneActorHeading(effect, &heading_x, &heading_y);
  for (unsigned i = 0; i < count; i++) {
    const uint32_t seed = DeterministicHash_Mix32(
        effect->pulse_generation * 0x9E3779B9u ^
        (uint32_t)effect->record_address * 0x85EBCA6Bu ^
        i * 0xC2B2AE35u);
    const unsigned lifetime =
        (effect->kind == kActionEffect_LightningTrap ||
         effect->kind == kActionEffect_MarahnaLightningLink ||
         effect->kind == kActionEffect_MarahnaBossLightning ||
         effect->kind == kActionEffect_BloodpoolBossLightning)
        ? 11u + ((seed >> 6) & 7u)
        : effect->kind == kActionEffect_AitosLavaReservoir
            ? 31u + ((seed >> 5) & 21u)
            : 21u + ((seed >> 5) & 15u);
    const unsigned birth_phase = seed % lifetime;
    const unsigned age = (visual_ticks + birth_phase) % lifetime;
    const float t = (float)age / (float)(lifetime - 1u);
    const float previous_t = fmaxf(0.0f, t - 0.14f);
    float x = 0.0f, y = 0.0f, previous_x = 0.0f, previous_y = 0.0f;
    float width = 0.55f, reach = 1.8f + 2.2f * t;
    float sword_path_t = 0.0f;

    if (effect->kind == kActionEffect_AitosLavaPit) {
      const float half_width = (rect->x1 - rect->x0) * 0.5f;
      const float birth = (HashUnit(seed ^ 0x71u) * 2.0f - 1.0f) *
          fmaxf(1.0f, half_width - 3.0f);
      const float drift = (HashUnit(seed ^ 0x37u) - 0.5f) * 10.0f;
      x = birth + drift * t * t;
      /* The captured rectangle covers the full bubbly volume. Its geometric
       * centre still reads too low in the isometric mouth: the apparent
       * surface sits one quarter-height above it. Keep births in a narrow
       * band around that authored plane; the fraction scales correctly for
       * both one- and two-row pits. */
      const float source_surface_y = (rect->y0 + rect->y1) * 0.5f -
          (rect->y1 - rect->y0) * 0.25f;
      const float source_y = source_surface_y +
          (HashUnit(seed ^ 0xB5u) - 0.5f) * 3.0f;
      y = source_y - 5.0f * t - 13.0f * t * t;
      previous_x = birth + drift * previous_t * previous_t;
      previous_y = source_y - 5.0f * previous_t -
          13.0f * previous_t * previous_t;
      width = 0.50f + 0.42f * (1.0f - t);
    } else if (effect->kind == kActionEffect_AitosLavaReservoir) {
      const float half_width = (rect->x1 - rect->x0) * 0.5f;
      const float birth_x = (HashUnit(seed ^ 0x71u) * 2.0f - 1.0f) *
          fmaxf(1.0f, half_width - 2.0f);
      const float drift = (HashUnit(seed ^ 0x37u) - 0.5f) * 22.0f;
      const float source_y = rect->y0 + 1.5f +
          (HashUnit(seed ^ 0xB5u) - 0.5f) * 2.0f;
      x = birth_x + drift * t * t;
      y = source_y - 9.0f * t - 34.0f * t * t;
      previous_x = birth_x + drift * previous_t * previous_t;
      previous_y = source_y - 9.0f * previous_t -
          34.0f * previous_t * previous_t;
      width = 0.42f + 0.48f * (1.0f - t);
      reach = 1.8f + 4.2f * t;
    } else if (effect->kind == kActionEffect_AitosMoltenRock) {
      /* Close sparks tumble off a hot solid surface. They do not align into a
       * directional flame wake, which is what distinguishes molten rock from
       * the actual `$CF9E` lava fireballs. */
      const float *direction = kCircle32[
          (i * 11u + (seed >> 10)) & kActionEffectGlowSegmentMask];
      const float distance = 6.0f + 10.0f * t;
      const float old_distance = 6.0f + 10.0f * previous_t;
      x = direction[0] * distance + heading_x * 2.0f * t;
      y = direction[1] * distance + 10.0f * t * t;
      previous_x = direction[0] * old_distance +
          heading_x * 2.0f * previous_t;
      previous_y = direction[1] * old_distance +
          10.0f * previous_t * previous_t;
      width = 0.44f + 0.34f * (1.0f - t);
      reach = 1.5f + 2.2f * (1.0f - t);
    } else if (effect->kind == kActionEffect_AitosWaterSplash) {
      const float half_width = (rect->x1 - rect->x0) * 0.5f;
      const float birth_x = (HashUnit(seed ^ 0x71u) * 2.0f - 1.0f) *
          fmaxf(1.0f, half_width - 2.0f);
      const bool drip = (i & 1u) != 0;
      const float drift = (HashUnit(seed ^ 0x37u) - 0.5f) * 5.0f;
      x = birth_x + drift * t;
      previous_x = birth_x + drift * previous_t;
      if (drip) {
        y = 2.0f + 6.0f * t + 25.0f * t * t;
        previous_y = 2.0f + 6.0f * previous_t +
            25.0f * previous_t * previous_t;
      } else {
        y = -8.0f - 12.0f * t + 18.0f * t * t;
        previous_y = -8.0f - 12.0f * previous_t +
            18.0f * previous_t * previous_t;
      }
      width = 0.38f + 0.30f * (1.0f - t);
      reach = 1.8f + 3.0f * t;
    } else if (effect->kind == kActionEffect_AitosWaterfall) {
      /* Stable lanes, staggered by identity, provide a slow translucent flow
       * over the fast two-frame source cycle. The varying alpha and length
       * break horizontal bands without blurring away the pixel art. */
      const unsigned columns = 16u;
      _Static_assert(kActionSceneEffectWaterfallParticleCount % 16u == 0u,
                     "waterfall veil must fill complete lane rows");
      const unsigned column = i % columns;
      const unsigned row = i / columns;
      const float lane = ((float)column + 0.5f) / (float)columns;
      const float left = rect->x0 + 10.0f;
      const float width_span = rect->x1 - rect->x0 - 20.0f;
      const float y_span = rect->y1 - rect->y0 + 48.0f;
      const float phase = (HashUnit(seed ^ 0x29u) +
          (float)visual_ticks / (84.0f + (float)(row * 11u)));
      const float wrapped = phase - floorf(phase);
      x = left + width_span * lane +
          (HashUnit(seed ^ 0x53u) - 0.5f) * 10.0f;
      y = rect->y0 - 24.0f + y_span * wrapped;
      previous_x = x + (HashUnit(seed ^ 0x37u) - 0.5f) * 1.5f;
      previous_y = y - (8.0f + 7.0f * HashUnit(seed ^ 0xB5u));
      width = 0.45f + 0.42f * HashUnit(seed ^ 0x71u);
      reach = 6.0f + 10.0f * HashUnit(seed ^ 0xA7u);
    } else if (effect->kind == kActionEffect_AitosWaterfallMist) {
      /* Foam boils along the lower waterfall edge while lighter droplets
       * drift upward into the fog banks. Horizontal phase offsets avoid a
       * static bright seam over the gap. */
      const float span = rect->x1 - rect->x0;
      const float lane = ((float)i + HashUnit(seed ^ 0x53u)) /
          (float)count;
      const float drift = (HashUnit(seed ^ 0x37u) - 0.5f) * 18.0f;
      const float base_y = 1.0f +
          (HashUnit(seed ^ 0xB5u) - 0.5f) * 12.0f;
      x = rect->x0 + span * lane + drift * t;
      y = base_y - 5.0f * t - 17.0f * t * t;
      previous_x = rect->x0 + span * lane + drift * previous_t;
      previous_y = base_y - 5.0f * previous_t -
          17.0f * previous_t * previous_t;
      width = 0.62f + 0.68f * (1.0f - t);
      reach = 2.4f + 4.4f * t;
    } else if (effect->kind == kActionEffect_WallTorch) {
      const float drift = (HashUnit(seed ^ 0x37u) - 0.5f) * 7.0f;
      const float birth = (HashUnit(seed ^ 0x71u) - 0.5f) * 5.0f;
      x = birth + drift * t * t;
      y = -2.0f - 9.0f * t - 25.0f * t * t;
      previous_x = birth + drift * previous_t * previous_t;
      previous_y = -2.0f - 9.0f * previous_t -
          25.0f * previous_t * previous_t;
      width = 0.45f + 0.35f * (1.0f - t);
    } else if (effect->kind == kActionEffect_AitosStatueFire) {
      /* Seed throughout the authored horizontal plume, then let hot flecks
       * continue a little in the facing direction while buoyancy pulls them
       * upward. This reads as a sustained breath rather than a projectile
       * wake, and scales naturally across the $1C-$1F pillar frames. */
      const float direction =
          (effect->flags & kActionEffectFlag_FlipHorizontal) ? -1.0f : 1.0f;
      const float span = fmaxf(8.0f, rect->x1 - rect->x0);
      const float lane = HashUnit(seed ^ 0x71u);
      const float birth_x = rect->x0 + span * lane;
      const float source_y = (rect->y0 + rect->y1) * 0.5f +
          (HashUnit(seed ^ 0xB5u) - 0.5f) * 10.0f;
      const float forward = direction *
          (3.0f + 10.0f * HashUnit(seed ^ 0x53u));
      const float curl = (HashUnit(seed ^ 0x37u) - 0.5f) * 7.0f;
      x = birth_x + forward * t + curl * t * t;
      y = source_y - 5.0f * t - 17.0f * t * t;
      previous_x = birth_x + forward * previous_t +
          curl * previous_t * previous_t;
      previous_y = source_y - 5.0f * previous_t -
          17.0f * previous_t * previous_t;
      width = 0.52f + 0.48f * (1.0f - t);
      reach = 2.0f + 4.0f * t;
    } else if (effect->kind == kActionEffect_EnemyFireball ||
               effect->kind == kActionEffect_MarahnaFireball ||
               effect->kind == kActionEffect_AitosLavaFireball) {
      const float side = (HashUnit(seed ^ 0x53u) - 0.5f) *
          (4.0f + 14.0f * t);
      /* Start beyond the 16px source art instead of hiding the youngest
       * sparks inside its painted red tail. The longer cone makes the host
       * enhancement legible without bleaching the authentic projectile. */
      const float distance = 10.0f + 44.0f * t;
      const float previous_distance = 10.0f + 44.0f * previous_t;
      x = -heading_x * distance - heading_y * side;
      y = -heading_y * distance + heading_x * side;
      previous_x = -heading_x * previous_distance - heading_y * side;
      previous_y = -heading_y * previous_distance + heading_x * side;
      width = 0.80f + 0.60f * (1.0f - t);
      reach = 3.0f + 5.0f * t;
    } else if (effect->kind == kActionEffect_FlamingWheel) {
      const float *anchor = kFlamingWheelFireballAnchors[
          i % kActionSceneEffectFlamingWheelFireballs];
      const float birth_x = anchor[0] +
          (HashUnit(seed ^ 0x53u) - 0.5f) * 3.0f;
      const float base_y = anchor[1] - 1.0f +
          (HashUnit(seed ^ 0x71u) - 0.5f) * 2.0f;
      const float sway = (HashUnit(seed ^ 0x37u) - 0.5f) * 10.0f;
      x = birth_x + sway * t * t;
      y = base_y - 8.0f * t - 24.0f * t * t;
      previous_x = birth_x + sway * previous_t * previous_t;
      previous_y = base_y - 8.0f * previous_t -
          24.0f * previous_t * previous_t;
      width = 0.65f + 0.50f * (1.0f - t);
      reach = 2.5f + 4.0f * t;
    } else if (effect->kind == kActionEffect_FlamingWheelProjectile) {
      const float side = (HashUnit(seed ^ 0x53u) - 0.5f) *
          (4.0f + 13.0f * t);
      const float distance = 5.0f + 36.0f * t;
      const float old_distance = 5.0f + 36.0f * previous_t;
      x = -heading_x * distance - heading_y * side;
      y = -heading_y * distance + heading_x * side;
      previous_x = -heading_x * old_distance - heading_y * side;
      previous_y = -heading_y * old_distance + heading_x * side;
      width = 0.48f + 0.46f * (1.0f - t);
      reach = 2.0f + 3.8f * t;
    } else if (effect->kind == kActionEffect_MinotaurAxe ||
               effect->kind == kActionEffect_IceDragonIceBall ||
               effect->kind == kActionEffect_TanzaraProjectile) {
      const float spread = effect->kind == kActionEffect_IceDragonIceBall
          ? 16.0f : 11.0f;
      const float length = effect->kind == kActionEffect_MinotaurAxe
          ? 32.0f : 42.0f;
      const float side = (HashUnit(seed ^ 0x53u) - 0.5f) *
          (4.0f + spread * t);
      const float distance = 5.0f + length * t;
      const float old_distance = 5.0f + length * previous_t;
      x = -heading_x * distance - heading_y * side;
      y = -heading_y * distance + heading_x * side;
      previous_x = -heading_x * old_distance - heading_y * side;
      previous_y = -heading_y * old_distance + heading_x * side;
      if (effect->kind == kActionEffect_IceDragonIceBall) {
        y += 5.0f * t * t;
        previous_y += 5.0f * previous_t * previous_t;
      }
      width = 0.50f + 0.48f * (1.0f - t);
      reach = 2.0f + 4.0f * t;
    } else if (effect->kind == kActionEffect_SwordBeam) {
      /* Sixteen fixed cross-sections span the path, each with top/centre/
       * bottom lanes. Position depends only on identity; the materialization
       * clock below changes alpha and size without pushing stars backward. */
      const unsigned lane_count = 3u;
      const unsigned cross_section_count = count / lane_count;
      const unsigned cross_section = i / lane_count;
      sword_path_t = cross_section_count > 1u
          ? (float)cross_section / (float)(cross_section_count - 1u)
          : 0.0f;
      const float centre_x = (rect->x0 + rect->x1) * 0.5f;
      const float centre_y = (rect->y0 + rect->y1) * 0.5f;
      const float half_height = (rect->y1 - rect->y0) * 0.5f;
      const float lane = (float)(i % lane_count) - 1.0f;
      const float lane_half_span = fmaxf(2.0f, half_height - 2.5f) *
          (1.0f - 0.35f * sword_path_t);
      const float jitter = (HashUnit(seed ^ 0x53u) - 0.5f) * 1.5f;
      const float side = lane * lane_half_span + jitter;
      const float distance = 4.0f + 84.0f * sword_path_t;
      x = centre_x - heading_x * distance - heading_y * side;
      y = centre_y - heading_y * distance + heading_x * side;
    } else if (effect->kind == kActionEffect_MarahnaLightningLink) {
      const bool horizontal =
          rect->x1 - rect->x0 >= rect->y1 - rect->y0;
      const float mid_x = (rect->x0 + rect->x1) * 0.5f;
      const float mid_y = (rect->y0 + rect->y1) * 0.5f;
      if (i >= count * 3u / 4u) {
        const bool at_end = (i & 1u) != 0;
        const float endpoint_x = horizontal
            ? (at_end ? rect->x1 : rect->x0) : mid_x;
        const float endpoint_y = horizontal
            ? mid_y : (at_end ? rect->y1 : rect->y0);
        const float *direction = kCircle32[
            (i * 7u + (seed >> 12)) & kActionEffectGlowSegmentMask];
        const float distance = 2.0f + 15.0f * t;
        const float old_distance = 2.0f + 15.0f * previous_t;
        x = endpoint_x + direction[0] * distance;
        y = endpoint_y + direction[1] * distance;
        previous_x = endpoint_x + direction[0] * old_distance;
        previous_y = endpoint_y + direction[1] * old_distance;
      } else {
        const float along = HashUnit(seed ^ 0x29u);
        const float jitter = (HashUnit(
            seed ^ (visual_ticks * 0x27D4EB2Du)) - 0.5f) * 9.0f;
        const float old_jitter = -jitter * 0.45f;
        if (horizontal) {
          x = rect->x0 + (rect->x1 - rect->x0) * along;
          y = mid_y + jitter;
          previous_x = x;
          previous_y = mid_y + old_jitter;
        } else {
          x = mid_x + jitter;
          y = rect->y0 + (rect->y1 - rect->y0) * along;
          previous_x = mid_x + old_jitter;
          previous_y = y;
        }
      }
      width = 0.42f + 0.28f * (1.0f - t);
      reach = 2.0f + 2.5f * (1.0f - t);
    } else if (effect->kind == kActionEffect_MarahnaBossLightning) {
      if (effect->phase == kActionEffectPhase_MarahnaBossLightningBolt) {
        float x0, y0, x1, y1;
        MarahnaBossBoltEndpoints(effect, &x0, &y0, &x1, &y1);
        const float along = HashUnit(seed ^ 0x29u);
        const float old_along = fmaxf(0.0f, along - 0.18f);
        const float jitter = (HashUnit(seed ^
            (visual_ticks * 0x27D4EB2Du)) - 0.5f) * 8.0f;
        const float dx = x1 - x0, dy = y1 - y0;
        const float length = fmaxf(0.001f, hypotf(dx, dy));
        x = x0 + dx * along - dy / length * jitter;
        y = y0 + dy * along + dx / length * jitter;
        previous_x = x0 + dx * old_along + dy / length * jitter * 0.4f;
        previous_y = y0 + dy * old_along - dx / length * jitter * 0.4f;
      } else if (effect->phase ==
                 kActionEffectPhase_MarahnaBossLightningGroundCharge) {
        /* The post-impact charge slides horizontally along the floor. Eight
         * sparks form a low wake behind it; four short-lived contacts jump
         * around the leading orb so the effect still reads on its compact
         * first animation frame. All coordinates remain in OBJ-local space
         * and therefore share the production flat/Diorama projection. */
        if (i < count * 2u / 3u) {
          const float side = (HashUnit(seed ^ 0x53u) - 0.5f) *
              (7.0f + 9.0f * t);
          const float distance = 5.0f + 35.0f * t;
          const float old_distance = 5.0f + 35.0f * previous_t;
          x = -heading_x * distance - heading_y * side;
          y = -heading_y * distance + heading_x * side;
          previous_x = -heading_x * old_distance - heading_y * side;
          previous_y = -heading_y * old_distance + heading_x * side;
        } else {
          const float *direction = kCircle32[
              (i * 7u + (seed >> 12)) & kActionEffectGlowSegmentMask];
          const float distance = 3.0f + 18.0f * t;
          const float old_distance = 3.0f + 18.0f * previous_t;
          x = heading_x * 3.0f + direction[0] * distance;
          y = direction[1] * distance * 0.44f;
          previous_x = heading_x * 3.0f + direction[0] * old_distance;
          previous_y = direction[1] * old_distance * 0.44f;
        }
        width = 0.48f + 0.36f * (1.0f - t);
        reach = 2.4f + 3.4f * (1.0f - t);
      } else {
        const float centre_y = -24.0f;
        const float angle_x = HashUnit(seed ^ 0x29u) * 2.0f - 1.0f;
        const float angle_y = HashUnit(seed ^ 0x71u) * 2.0f - 1.0f;
        const float radius = effect->phase ==
            kActionEffectPhase_MarahnaBossLightningOrb ? 24.0f : 46.0f;
        x = angle_x * radius;
        y = centre_y + angle_y * radius * 0.46f;
        previous_x = x * 0.82f;
        previous_y = centre_y + (y - centre_y) * 0.82f;
      }
      width = 0.45f + 0.34f * (1.0f - t);
      reach = 2.2f + 3.2f * (1.0f - t);
    } else if (effect->kind == kActionEffect_LightningTrap) {
      /* Most sparks crawl across the full bolt; the last quarter burst away
       * from its lower impact so the strike has both a shaft and a contact. */
      if (i >= count * 3u / 4u) {
        const float *direction = kCircle32[
            (i * 7u + (seed >> 12)) & kActionEffectGlowSegmentMask];
        const float distance = 3.0f + 20.0f * t;
        const float old_distance = 3.0f + 20.0f * previous_t;
        x = (rect->x0 + rect->x1) * 0.5f + direction[0] * distance;
        y = rect->y1 + direction[1] * distance * 0.55f;
        previous_x = (rect->x0 + rect->x1) * 0.5f +
            direction[0] * old_distance;
        previous_y = rect->y1 + direction[1] * old_distance * 0.55f;
      } else {
        const float along = HashUnit(seed ^ 0x29u);
        const float base_y = rect->y0 + (rect->y1 - rect->y0) * along;
        const float jitter = (HashUnit(seed ^
            (visual_ticks * 0x27D4EB2Du)) - 0.5f) * 12.0f;
        const float old_jitter = -jitter * 0.55f;
        x = (rect->x0 + rect->x1) * 0.5f + jitter;
        y = base_y + (t - 0.5f) * 5.0f;
        previous_x = (rect->x0 + rect->x1) * 0.5f + old_jitter;
        previous_y = base_y + (previous_t - 0.5f) * 5.0f;
      }
      width = 0.42f + 0.28f * (1.0f - t);
      reach = 2.0f + 2.5f * (1.0f - t);
    } else if (effect->phase == kActionEffectPhase_BossLightningStrike) {
      float endpoint_x = (rect->x0 + rect->x1) * 0.5f;
      float endpoint_y = rect->y1;
      BossLightningPathSample(effect, 1.0f, &endpoint_x, &endpoint_y);
      if (i >= count * 3u / 4u) {
        const float *direction = kCircle32[
            (i * 9u + (seed >> 11)) & kActionEffectGlowSegmentMask];
        const float distance = 4.0f + 24.0f * t;
        const float old_distance = 4.0f + 24.0f * previous_t;
        x = endpoint_x + direction[0] * distance;
        y = endpoint_y + direction[1] * distance * 0.48f;
        previous_x = endpoint_x + direction[0] * old_distance;
        previous_y = endpoint_y +
            direction[1] * old_distance * 0.48f;
      } else {
        const float birth = HashUnit(seed ^ 0x29u);
        float along = birth + 0.34f * t;
        float previous_along = birth + 0.34f * previous_t;
        if (along > 1.0f) along -= 1.0f;
        if (previous_along > 1.0f) previous_along -= 1.0f;
        if (!BossLightningPathSample(effect, along, &x, &y) ||
            !BossLightningPathSample(
                effect, previous_along, &previous_x, &previous_y))
          continue;
      }
      width = 0.55f + 0.38f * (1.0f - t);
      reach = 2.5f + 3.5f * (1.0f - t);
    } else {
      /* The linked state-$09 child expands around its own floor artwork. */
      const float *direction = kCircle32[
          (i * 11u + (seed >> 13)) & kActionEffectGlowSegmentMask];
      const float distance = 2.0f + 26.0f * t;
      const float old_distance = 2.0f + 26.0f * previous_t;
      x = (rect->x0 + rect->x1) * 0.5f + direction[0] * distance;
      y = rect->y1 - 2.0f + direction[1] * distance * 0.38f;
      previous_x = (rect->x0 + rect->x1) * 0.5f +
          direction[0] * old_distance;
      previous_y = rect->y1 - 2.0f +
          direction[1] * old_distance * 0.38f;
      width = 0.50f + 0.32f * (1.0f - t);
      reach = 2.0f + 2.8f * (1.0f - t);
    }

    SDL_FColor color = MixColor(
        hot, cool, effect->kind == kActionEffect_SwordBeam ? sword_path_t : t);
    if (effect->kind == kActionEffect_SwordBeam) {
      float materialize = TriangleWave(visual_ticks + i * 7u, 18u);
      materialize = materialize * materialize *
          (3.0f - 2.0f * materialize);
      color.a *= materialize * (1.0f - 0.35f * sword_path_t) *
          (0.82f + 0.18f * HashUnit(seed ^ 0xA7u));
      const float base_size = 1.15f + 1.30f * (1.0f - sword_path_t) *
          (0.78f + 0.22f * HashUnit(seed ^ 0xD3u));
      const float star_size = base_size * (0.70f + 0.50f * materialize);
      if (!AppendSceneStarParticle(writer, effect, x, y, star_size, color,
                                   project_point, userdata))
        return false;
      continue;
    }
    const float birth_fade = fminf(1.0f, t * 8.0f);
    const float retirement_fade = 1.0f - t;
    color.a *= birth_fade * retirement_fade *
        (0.82f + 0.18f * HashUnit(seed ^ 0xA7u));
    if (!AppendSceneParticle(writer, effect, x, y, previous_x, previous_y,
                             width, reach, color, project_point, userdata))
      return false;
  }
  return true;
}

static bool AppendSceneLighting(ActionEffectGeometryWriter *writer,
                                const ActionEffectInstance *effect,
                                ActionEffectProjectPointFn project_point,
                                void *userdata) {
  const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
  const float mid_x = (rect->x0 + rect->x1) * 0.5f;
  const float mid_y = (rect->y0 + rect->y1) * 0.5f;
  const float pulse = DeterministicPulse(effect);
  ActionEffectGlowStyle spill = {0}, body = {0};
  float spill_x = mid_x, spill_y = mid_y;
  float body_x = mid_x, body_y = mid_y;

  if (effect->kind == kActionEffect_AitosLavaReservoir)
    return AppendLavaReservoirLighting(
        writer, effect, pulse, project_point, userdata);

  switch (effect->kind) {
    case kActionEffect_WallTorch:
      spill = (ActionEffectGlowStyle){
        .radius_x = 30.0f, .radius_y = 25.0f,
        .ring_scale = {0.25f, 0.62f, 1.0f},
        .centre = {1.00f, 0.48f, 0.10f, 0.14f},
        .ring = {{1.00f, 0.34f, 0.05f, 0.10f},
                 {0.84f, 0.12f, 0.01f, 0.045f},
                 {0.55f, 0.03f, 0.00f, 0.00f}},
        .flare = 0.12f, .rise = 0.16f,
        .axis_x = 1.0f, .lift_y = -1.0f,
        .seed = (unsigned)effect->generation,
      };
      body = (ActionEffectGlowStyle){
        .radius_x = 8.5f, .radius_y = 14.0f,
        .ring_scale = {0.20f, 0.52f, 1.0f},
        .centre = {1.00f, 0.98f, 0.82f, 0.72f},
        .ring = {{1.00f, 0.72f, 0.22f, 0.40f},
                 {1.00f, 0.27f, 0.02f, 0.16f},
                 {0.78f, 0.07f, 0.00f, 0.00f}},
        .flare = 0.34f, .rise = 0.42f,
        .axis_x = 1.0f, .lift_y = -1.0f,
        .seed = (unsigned)effect->pulse_generation,
      };
      body_y = -3.0f;
      break;
    case kActionEffect_AitosStatueFire: {
      const float half_width = (rect->x1 - rect->x0) * 0.5f;
      const float half_height = (rect->y1 - rect->y0) * 0.5f;
      spill = (ActionEffectGlowStyle){
        .radius_x = fmaxf(28.0f, half_width + 18.0f),
        .radius_y = fmaxf(21.0f, half_height + 13.0f),
        .ring_scale = {0.24f, 0.66f, 1.0f},
        .centre = {1.00f, 0.48f, 0.07f, 0.23f},
        .ring = {{1.00f, 0.31f, 0.02f, 0.16f},
                 {0.86f, 0.08f, 0.00f, 0.06f},
                 {0.48f, 0.01f, 0.00f, 0.00f}},
        .flare = 0.15f, .rise = 0.18f,
        .axis_x = 1.0f, .lift_y = -1.0f,
        .seed = (unsigned)effect->record_address,
      };
      body = (ActionEffectGlowStyle){
        .radius_x = fmaxf(11.0f, half_width + 4.0f),
        .radius_y = fmaxf(8.0f, half_height + 2.0f),
        .ring_scale = {0.18f, 0.56f, 1.0f},
        .centre = {1.00f, 1.00f, 0.76f, 0.84f},
        .ring = {{1.00f, 0.68f, 0.13f, 0.50f},
                 {1.00f, 0.22f, 0.01f, 0.20f},
                 {0.72f, 0.03f, 0.00f, 0.00f}},
        .flare = 0.28f, .rise = 0.30f,
        .axis_x = 1.0f, .lift_y = -1.0f,
        .seed = (unsigned)effect->pulse_generation,
      };
      body_y -= 2.0f;
      break;
    }
    case kActionEffect_AitosLavaPit: {
      const float half_width = (rect->x1 - rect->x0) * 0.5f;
      const float half_height = (rect->y1 - rect->y0) * 0.5f;
      spill = (ActionEffectGlowStyle){
        .radius_x = fmaxf(34.0f, half_width + 14.0f),
        .radius_y = fmaxf(23.0f, half_height + 13.0f),
        .ring_scale = {0.22f, 0.70f, 1.0f},
        .centre = {1.00f, 0.42f, 0.04f, 0.17f},
        .ring = {{1.00f, 0.28f, 0.02f, 0.13f},
                 {0.80f, 0.08f, 0.00f, 0.055f},
                 {0.45f, 0.01f, 0.00f, 0.00f}},
        .flare = 0.10f, .rise = 0.12f,
        .axis_x = 1.0f, .lift_y = -1.0f,
        .seed = (unsigned)effect->generation,
      };
      body = (ActionEffectGlowStyle){
        .radius_x = fmaxf(28.0f, half_width + 3.0f),
        .radius_y = fmaxf(8.0f, half_height + 2.0f),
        .ring_scale = {0.16f, 0.78f, 1.0f},
        .centre = {1.00f, 0.98f, 0.66f, 0.78f},
        .ring = {{1.00f, 0.66f, 0.10f, 0.46f},
                 {1.00f, 0.20f, 0.01f, 0.18f},
                 {0.72f, 0.04f, 0.00f, 0.00f}},
        .flare = 0.19f, .rise = 0.24f,
        .axis_x = 1.0f, .lift_y = -1.0f,
        .seed = (unsigned)effect->pulse_generation,
      };
      spill_y = -half_height * 0.30f;
      body_y = 0.0f;
      break;
    }
    case kActionEffect_AitosMoltenRock:
      spill = (ActionEffectGlowStyle){
        .radius_x = 25.0f, .radius_y = 23.0f,
        .ring_scale = {0.24f, 0.66f, 1.0f},
        .centre = {1.00f, 0.44f, 0.05f, 0.18f},
        .ring = {{1.00f, 0.28f, 0.02f, 0.13f},
                 {0.78f, 0.08f, 0.00f, 0.05f},
                 {0.42f, 0.01f, 0.00f, 0.00f}},
        .flare = 0.035f, .rise = 0.03f,
        .axis_x = 1.0f, .lift_y = 1.0f,
        .seed = (unsigned)effect->record_address,
      };
      body = (ActionEffectGlowStyle){
        .radius_x = 10.5f, .radius_y = 10.5f,
        .ring_scale = {0.18f, 0.62f, 1.0f},
        .centre = {1.00f, 1.00f, 0.70f, 0.86f},
        .ring = {{1.00f, 0.67f, 0.12f, 0.52f},
                 {1.00f, 0.20f, 0.01f, 0.18f},
                 {0.72f, 0.03f, 0.00f, 0.00f}},
        .flare = 0.025f, .rise = 0.02f,
        .axis_x = 1.0f, .lift_y = 1.0f,
        .seed = (unsigned)effect->pulse_generation,
      };
      break;
    case kActionEffect_AitosWaterSplash: {
      const float half_width = (rect->x1 - rect->x0) * 0.5f;
      spill = (ActionEffectGlowStyle){
        .radius_x = fmaxf(22.0f, half_width + 10.0f),
        .radius_y = 19.0f,
        .ring_scale = {0.22f, 0.70f, 1.0f},
        .centre = {0.52f, 0.90f, 1.00f, 0.12f},
        .ring = {{0.28f, 0.70f, 1.00f, 0.08f},
                 {0.08f, 0.32f, 0.78f, 0.03f},
                 {0.02f, 0.10f, 0.38f, 0.00f}},
        .flare = 0.025f, .rise = 0.04f,
        .axis_x = 1.0f, .lift_y = 1.0f,
        .seed = (unsigned)effect->generation,
      };
      body = (ActionEffectGlowStyle){
        .radius_x = fmaxf(14.0f, half_width + 2.0f),
        .radius_y = 6.0f,
        .ring_scale = {0.18f, 0.72f, 1.0f},
        .centre = {0.94f, 1.00f, 1.00f, 0.50f},
        .ring = {{0.54f, 0.92f, 1.00f, 0.28f},
                 {0.16f, 0.58f, 1.00f, 0.08f},
                 {0.03f, 0.18f, 0.54f, 0.00f}},
        .flare = 0.02f, .axis_x = 1.0f, .lift_y = 1.0f,
        .seed = (unsigned)effect->pulse_generation,
      };
      spill_y = 4.0f;
      body_y = -4.0f;
      break;
    }
    case kActionEffect_AitosWaterfall:
      /* Two broad, low-alpha meshes act as a soft water veil. The
       * underlying tiles remain the image; this only lowers the perceived
       * contrast of their short animation cycle and supplies cool depth. The
       * first live waterfall acceptance showed the original 0.01-alpha veil
       * disappeared under CRT scaling, so these remain restrained but no
       * longer sub-perceptual. */
      spill = (ActionEffectGlowStyle){
        .radius_x = 330.0f, .radius_y = 282.0f,
        .ring_scale = {0.12f, 0.88f, 1.0f},
        .centre = {0.30f, 0.70f, 1.00f, 0.042f},
        .ring = {{0.22f, 0.62f, 1.00f, 0.034f},
                 {0.10f, 0.42f, 0.78f, 0.014f},
                 {0.03f, 0.14f, 0.30f, 0.00f}},
        .flare = 0.008f, .axis_x = 1.0f, .lift_y = 1.0f,
        .seed = (unsigned)effect->generation,
      };
      body = (ActionEffectGlowStyle){
        .radius_x = 282.0f, .radius_y = 230.0f,
        .ring_scale = {0.12f, 0.92f, 1.0f},
        .centre = {0.56f, 0.88f, 1.00f, 0.052f},
        .ring = {{0.38f, 0.78f, 1.00f, 0.040f},
                 {0.16f, 0.52f, 0.92f, 0.017f},
                 {0.04f, 0.18f, 0.42f, 0.00f}},
        .flare = 0.006f, .axis_x = 1.0f, .lift_y = 1.0f,
        .seed = (unsigned)effect->pulse_generation,
      };
      break;
    case kActionEffect_AitosWaterfallMist:
      return AppendWaterfallMistCloudVolume(
          writer, effect, pulse, project_point, userdata);
    case kActionEffect_EnemyFireball:
    case kActionEffect_MarahnaFireball:
    case kActionEffect_AitosLavaFireball: {
      float hx = 1.0f, hy = 0.0f;
      SceneFireballHeading(effect, &hx, &hy);
      spill = (ActionEffectGlowStyle){
        .radius_x = 38.0f, .radius_y = 27.0f,
        .ring_scale = {0.28f, 0.65f, 1.0f},
        .centre = {1.00f, 0.52f, 0.09f, 0.24f},
        .ring = {{1.00f, 0.35f, 0.03f, 0.17f},
                 {0.92f, 0.11f, 0.00f, 0.075f},
                 {0.55f, 0.02f, 0.00f, 0.00f}},
        .flare = 0.13f, .rise = 0.18f,
        .axis_x = hx, .axis_y = hy,
        .lift_x = -hx, .lift_y = -hy,
        .seed = (unsigned)effect->record_address,
      };
      body = (ActionEffectGlowStyle){
        .radius_x = 25.0f, .radius_y = 11.0f,
        .ring_scale = {0.20f, 0.50f, 1.0f},
        .centre = {1.00f, 0.99f, 0.88f, 0.90f},
        .ring = {{1.00f, 0.78f, 0.28f, 0.60f},
                 {1.00f, 0.30f, 0.02f, 0.30f},
                 {0.82f, 0.07f, 0.00f, 0.00f}},
        .flare = 0.27f, .rise = 0.34f,
        .axis_x = hx, .axis_y = hy,
        .lift_x = -hx, .lift_y = -hy,
        .seed = (unsigned)effect->pulse_generation,
      };
      /* Pull the enhanced body into the wake so its brightest region remains
       * visible beside the painted core instead of disappearing underneath. */
      body_x = mid_x - hx * 6.0f;
      body_y = mid_y - hy * 6.0f;
      break;
    }
    case kActionEffect_FlamingWheel: {
      const float half_width = (rect->x1 - rect->x0) * 0.5f;
      const float half_height = (rect->y1 - rect->y0) * 0.5f;
      spill = (ActionEffectGlowStyle){
        .radius_x = fmaxf(42.0f, half_width + 18.0f),
        .radius_y = fmaxf(38.0f, half_height + 18.0f),
        .ring_scale = {0.24f, 0.68f, 1.0f},
        .centre = {1.00f, 0.48f, 0.05f, 0.25f},
        .ring = {{1.00f, 0.29f, 0.02f, 0.17f},
                 {0.86f, 0.08f, 0.00f, 0.065f},
                 {0.48f, 0.01f, 0.00f, 0.00f}},
        .flare = 0.20f, .rise = 0.24f,
        .axis_x = 1.0f, .lift_y = -1.0f,
        .seed = (unsigned)effect->record_address,
      };
      body = (ActionEffectGlowStyle){
        .radius_x = fmaxf(22.0f, half_width + 4.0f),
        .radius_y = fmaxf(22.0f, half_height + 4.0f),
        .ring_scale = {0.18f, 0.60f, 1.0f},
        .centre = {1.00f, 1.00f, 0.72f, 0.82f},
        .ring = {{1.00f, 0.67f, 0.12f, 0.52f},
                 {1.00f, 0.19f, 0.01f, 0.20f},
                 {0.70f, 0.03f, 0.00f, 0.00f}},
        .flare = 0.31f, .rise = 0.34f,
        .axis_x = 1.0f, .lift_y = -1.0f,
        .seed = (unsigned)effect->pulse_generation,
      };
      body_y -= 3.0f;
      break;
    }
    case kActionEffect_FlamingWheelProjectile: {
      float hx = 1.0f, hy = 0.0f;
      SceneActorHeading(effect, &hx, &hy);
      spill = (ActionEffectGlowStyle){
        .radius_x = 29.0f, .radius_y = 18.0f,
        .ring_scale = {0.22f, 0.66f, 1.0f},
        .centre = {0.62f, 0.95f, 1.00f, 0.22f},
        .ring = {{0.22f, 0.76f, 1.00f, 0.14f},
                 {0.03f, 0.36f, 0.82f, 0.05f},
                 {0.01f, 0.12f, 0.36f, 0.00f}},
        .flare = 0.12f, .rise = 0.08f,
        .axis_x = hx, .axis_y = hy,
        .lift_x = -hx, .lift_y = -hy,
        .seed = (unsigned)effect->record_address,
      };
      body = (ActionEffectGlowStyle){
        .radius_x = 15.0f, .radius_y = 8.0f,
        .ring_scale = {0.16f, 0.58f, 1.0f},
        .centre = {0.94f, 1.00f, 1.00f, 0.88f},
        .ring = {{0.52f, 0.94f, 1.00f, 0.52f},
                 {0.06f, 0.62f, 0.94f, 0.18f},
                 {0.01f, 0.18f, 0.48f, 0.00f}},
        .flare = 0.18f, .rise = 0.12f,
        .axis_x = hx, .axis_y = hy,
        .lift_x = -hx, .lift_y = -hy,
        .seed = (unsigned)effect->pulse_generation,
      };
      spill_x = mid_x - hx * 5.0f;
      spill_y = mid_y - hy * 5.0f;
      body_x = mid_x - hx * 2.0f;
      body_y = mid_y - hy * 2.0f;
      break;
    }
    case kActionEffect_MinotaurAxe:
    case kActionEffect_IceDragonIceBall:
    case kActionEffect_TanzaraProjectile: {
      float hx = 1.0f, hy = 0.0f;
      SceneActorHeading(effect, &hx, &hy);
      const bool ice = effect->kind == kActionEffect_IceDragonIceBall;
      const bool axe = effect->kind == kActionEffect_MinotaurAxe;
      const SDL_FColor spill_centre = ice
          ? (SDL_FColor){0.42f, 0.84f, 1.00f, 0.22f}
          : (axe ? (SDL_FColor){1.00f, 0.70f, 0.22f, 0.19f}
                 : (SDL_FColor){0.76f, 0.28f, 1.00f, 0.22f});
      const SDL_FColor spill_mid = ice
          ? (SDL_FColor){0.18f, 0.55f, 1.00f, 0.13f}
          : (axe ? (SDL_FColor){1.00f, 0.43f, 0.06f, 0.12f}
                 : (SDL_FColor){0.54f, 0.12f, 1.00f, 0.14f});
      const SDL_FColor body_mid = ice
          ? (SDL_FColor){0.56f, 0.91f, 1.00f, 0.52f}
          : (axe ? (SDL_FColor){1.00f, 0.78f, 0.31f, 0.48f}
                 : (SDL_FColor){0.88f, 0.54f, 1.00f, 0.50f});
      spill = (ActionEffectGlowStyle){
        .radius_x = ice ? 35.0f : 31.0f,
        .radius_y = ice ? 22.0f : 20.0f,
        .ring_scale = {0.24f, 0.68f, 1.0f},
        .centre = spill_centre,
        .ring = {spill_mid,
                 ice ? (SDL_FColor){0.06f, 0.22f, 0.76f, 0.045f}
                     : (axe ? (SDL_FColor){0.76f, 0.15f, 0.01f, 0.04f}
                            : (SDL_FColor){0.28f, 0.04f, 0.70f, 0.05f}),
                 {0.03f, 0.02f, 0.25f, 0.00f}},
        .flare = ice ? 0.07f : 0.12f, .rise = 0.06f,
        .axis_x = hx, .axis_y = hy,
        .lift_x = -hx, .lift_y = -hy,
        .seed = (unsigned)effect->record_address,
      };
      body = (ActionEffectGlowStyle){
        .radius_x = ice ? 19.0f : 17.0f,
        .radius_y = ice ? 9.0f : 10.0f,
        .ring_scale = {0.17f, 0.60f, 1.0f},
        .centre = {1.00f, 1.00f, 1.00f, ice ? 0.86f : 0.78f},
        .ring = {body_mid,
                 ice ? (SDL_FColor){0.15f, 0.55f, 1.00f, 0.17f}
                     : (axe ? (SDL_FColor){1.00f, 0.30f, 0.03f, 0.15f}
                            : (SDL_FColor){0.58f, 0.12f, 1.00f, 0.18f}),
                 {0.05f, 0.02f, 0.30f, 0.00f}},
        .flare = ice ? 0.08f : 0.16f, .rise = 0.09f,
        .axis_x = hx, .axis_y = hy,
        .lift_x = -hx, .lift_y = -hy,
        .seed = (unsigned)effect->pulse_generation,
      };
      spill_x = mid_x - hx * 5.0f;
      spill_y = mid_y - hy * 5.0f;
      body_x = mid_x - hx * 2.0f;
      body_y = mid_y - hy * 2.0f;
      break;
    }
    case kActionEffect_MarahnaLightningLink: {
      const bool horizontal =
          rect->x1 - rect->x0 >= rect->y1 - rect->y0;
      const float axis_x = horizontal ? 1.0f : 0.0f;
      const float axis_y = horizontal ? 0.0f : 1.0f;
      spill = (ActionEffectGlowStyle){
        .radius_x = 48.0f, .radius_y = 16.0f,
        .ring_scale = {0.22f, 0.72f, 1.0f},
        .centre = {0.62f, 0.82f, 1.00f, 0.18f},
        .ring = {{0.40f, 0.62f, 1.00f, 0.12f},
                 {0.18f, 0.28f, 0.96f, 0.05f},
                 {0.07f, 0.08f, 0.58f, 0.00f}},
        .flare = 0.08f, .axis_x = axis_x, .axis_y = axis_y,
        .lift_y = -1.0f,
        .seed = (unsigned)effect->record_address,
      };
      body = (ActionEffectGlowStyle){
        .radius_x = 42.0f, .radius_y = 6.0f,
        .ring_scale = {0.14f, 0.84f, 1.0f},
        .centre = {0.98f, 1.00f, 1.00f, 0.74f},
        .ring = {{0.76f, 0.92f, 1.00f, 0.42f},
                 {0.30f, 0.46f, 1.00f, 0.15f},
                 {0.10f, 0.12f, 0.72f, 0.00f}},
        .flare = 0.15f, .axis_x = axis_x, .axis_y = axis_y,
        .lift_y = -1.0f,
        .seed = (unsigned)effect->pulse_generation,
      };
      break;
    }
    case kActionEffect_MarahnaBossLightning: {
      if (effect->phase == kActionEffectPhase_MarahnaBossLightningBolt) {
        float x0, y0, x1, y1;
        MarahnaBossBoltEndpoints(effect, &x0, &y0, &x1, &y1);
        spill_x = body_x = (x0 + x1) * 0.5f;
        spill_y = body_y = (y0 + y1) * 0.5f;
        const float dx = x1 - x0, dy = y1 - y0;
        const float length = fmaxf(0.001f, hypotf(dx, dy));
        spill = (ActionEffectGlowStyle){
          .radius_x = 38.0f, .radius_y = 16.0f,
          .ring_scale = {0.22f, 0.72f, 1.0f},
          .centre = {0.66f, 0.88f, 1.00f, 0.21f},
          .ring = {{0.38f, 0.68f, 1.00f, 0.14f},
                   {0.12f, 0.30f, 1.00f, 0.055f},
                   {0.03f, 0.09f, 0.64f, 0.00f}},
          .flare = 0.09f, .axis_x = dx / length, .axis_y = dy / length,
          .lift_y = -1.0f, .seed = (unsigned)effect->record_address,
        };
        body = (ActionEffectGlowStyle){
          .radius_x = 29.0f, .radius_y = 6.0f,
          .ring_scale = {0.14f, 0.82f, 1.0f},
          .centre = {0.98f, 1.00f, 1.00f, 0.82f},
          .ring = {{0.74f, 0.94f, 1.00f, 0.46f},
                   {0.24f, 0.54f, 1.00f, 0.17f},
                   {0.06f, 0.14f, 0.76f, 0.00f}},
          .flare = 0.16f, .axis_x = dx / length, .axis_y = dy / length,
          .lift_y = -1.0f, .seed = (unsigned)effect->pulse_generation,
        };
      } else if (effect->phase ==
                 kActionEffectPhase_MarahnaBossLightningGroundCharge) {
        float hx = 1.0f, hy = 0.0f;
        SceneActorHeading(effect, &hx, &hy);
        spill_x = mid_x - hx * 4.0f;
        spill_y = mid_y - hy * 4.0f;
        body_x = mid_x;
        body_y = mid_y;
        spill = (ActionEffectGlowStyle){
          .radius_x = 43.0f, .radius_y = 19.0f,
          .ring_scale = {0.22f, 0.70f, 1.0f},
          .centre = {0.62f, 0.86f, 1.00f, 0.24f},
          .ring = {{0.34f, 0.64f, 1.00f, 0.16f},
                   {0.10f, 0.26f, 0.98f, 0.06f},
                   {0.02f, 0.07f, 0.58f, 0.00f}},
          .flare = 0.10f, .rise = 0.06f,
          .axis_x = hx, .axis_y = hy,
          .lift_x = -hx, .lift_y = -hy,
          .seed = (unsigned)effect->record_address,
        };
        body = (ActionEffectGlowStyle){
          .radius_x = 22.0f, .radius_y = 13.0f,
          .ring_scale = {0.15f, 0.76f, 1.0f},
          .centre = {0.98f, 1.00f, 1.00f, 0.88f},
          .ring = {{0.72f, 0.94f, 1.00f, 0.52f},
                   {0.22f, 0.50f, 1.00f, 0.18f},
                   {0.04f, 0.12f, 0.72f, 0.00f}},
          .flare = 0.16f, .rise = 0.08f,
          .axis_x = hx, .axis_y = hy,
          .lift_x = -hx, .lift_y = -hy,
          .seed = (unsigned)effect->pulse_generation,
        };
      } else {
        const bool orb = effect->phase ==
            kActionEffectPhase_MarahnaBossLightningOrb;
        spill = (ActionEffectGlowStyle){
          .radius_x = orb ? 46.0f : 64.0f,
          .radius_y = orb ? 38.0f : 31.0f,
          .ring_scale = {0.22f, 0.68f, 1.0f},
          .centre = {0.62f, 0.84f, 1.00f, orb ? 0.26f : 0.18f},
          .ring = {{0.34f, 0.66f, 1.00f, orb ? 0.17f : 0.12f},
                   {0.11f, 0.27f, 0.96f, 0.055f},
                   {0.03f, 0.07f, 0.55f, 0.00f}},
          .flare = 0.10f, .axis_x = 1.0f, .lift_y = -1.0f,
          .seed = (unsigned)effect->record_address,
        };
        body = (ActionEffectGlowStyle){
          .radius_x = orb ? 26.0f : 50.0f,
          .radius_y = orb ? 26.0f : 15.0f,
          .ring_scale = {0.16f, 0.78f, 1.0f},
          .centre = {0.98f, 1.00f, 1.00f, orb ? 0.86f : 0.64f},
          .ring = {{0.70f, 0.92f, 1.00f, orb ? 0.50f : 0.34f},
                   {0.22f, 0.48f, 1.00f, 0.15f},
                   {0.05f, 0.12f, 0.70f, 0.00f}},
          .flare = 0.15f, .axis_x = 1.0f, .lift_y = -1.0f,
          .seed = (unsigned)effect->pulse_generation,
        };
        spill_y = body_y = -24.0f;
      }
      break;
    }
    case kActionEffect_SwordBeam: {
      float hx = 1.0f, hy = 0.0f;
      SceneActorHeading(effect, &hx, &hy);
      spill = (ActionEffectGlowStyle){
        .radius_x = 24.0f, .radius_y = 22.0f,
        .ring_scale = {0.25f, 0.64f, 1.0f},
        .centre = {0.52f, 0.88f, 1.00f, 0.13f},
        .ring = {{0.26f, 0.70f, 1.00f, 0.09f},
                 {0.08f, 0.34f, 0.98f, 0.035f},
                 {0.02f, 0.10f, 0.60f, 0.00f}},
        .flare = 0.035f, .rise = 0.11f,
        .axis_x = hx, .axis_y = hy,
        .lift_x = -hx, .lift_y = -hy,
        .seed = (unsigned)effect->record_address,
      };
      body = (ActionEffectGlowStyle){
        .radius_x = 10.0f, .radius_y = 18.0f,
        .ring_scale = {0.20f, 0.58f, 1.0f},
        .centre = {0.94f, 1.00f, 1.00f, 0.52f},
        .ring = {{0.48f, 0.92f, 1.00f, 0.30f},
                 {0.12f, 0.56f, 1.00f, 0.10f},
                 {0.03f, 0.18f, 0.72f, 0.00f}},
        .flare = 0.025f, .rise = 0.10f,
        .axis_x = hx, .axis_y = hy,
        .lift_x = -hx, .lift_y = -hy,
        .seed = (unsigned)effect->pulse_generation,
      };
      /* Anchor the restrained halo to the decoded OAM rectangle. Only the
       * outer spill leans slightly into the wake; the luminous core remains
       * on the painted crescent. */
      spill_x = mid_x - hx * 2.0f;
      spill_y = mid_y - hy * 2.0f;
      body_x = mid_x;
      body_y = mid_y;
      break;
    }
    case kActionEffect_LightningTrap:
      spill = (ActionEffectGlowStyle){
        .radius_x = 31.0f,
        .radius_y = fmaxf(46.0f, (rect->y1 - rect->y0) * 0.59f),
        /* Ring 2 is transparent. Keep the last COLOURED ring at the captured
         * electrode endpoints; the old 0.62 scale made the visible aura die
         * near the middle even though its transparent geometry was full-size. */
        .ring_scale = {0.24f, 0.86f, 1.0f},
        .centre = {0.68f, 0.90f, 1.00f, 0.13f},
        .ring = {{0.48f, 0.76f, 1.00f, 0.09f},
                 {0.22f, 0.48f, 1.00f, 0.04f},
                 {0.08f, 0.18f, 0.72f, 0.00f}},
        .flare = 0.07f, .axis_x = 1.0f, .lift_y = -1.0f,
        .seed = (unsigned)effect->record_address,
      };
      body = (ActionEffectGlowStyle){
        .radius_x = 9.0f,
        .radius_y = fmaxf(38.0f, (rect->y1 - rect->y0) * 0.55f),
        .ring_scale = {0.16f, 0.92f, 1.0f},
        .centre = {1.00f, 1.00f, 1.00f, 0.62f},
        .ring = {{0.82f, 0.96f, 1.00f, 0.34f},
                 {0.34f, 0.68f, 1.00f, 0.13f},
                 {0.10f, 0.28f, 0.84f, 0.00f}},
        .flare = 0.13f, .axis_x = 1.0f, .lift_y = -1.0f,
        .seed = (unsigned)effect->pulse_generation,
      };
      break;
    case kActionEffect_BloodpoolBossLightning:
      if (effect->phase == kActionEffectPhase_BossLightningStrike) {
        BossLightningPathSample(effect, 0.5f, &spill_x, &spill_y);
        body_x = spill_x;
        body_y = spill_y;
        spill = (ActionEffectGlowStyle){
          .radius_x = fmaxf(42.0f, (rect->x1 - rect->x0) * 1.15f),
          .radius_y = fmaxf(58.0f, (rect->y1 - rect->y0) * 0.64f),
          .ring_scale = {0.24f, 0.72f, 1.0f},
          .centre = {1.00f, 0.72f, 0.17f, 0.19f},
          .ring = {{1.00f, 0.48f, 0.06f, 0.13f},
                   {0.86f, 0.18f, 0.01f, 0.055f},
                   {0.52f, 0.04f, 0.00f, 0.00f}},
          .flare = 0.10f, .axis_x = 1.0f, .lift_y = -1.0f,
          .seed = (unsigned)effect->record_address,
        };
        body = (ActionEffectGlowStyle){
          .radius_x = 15.0f,
          .radius_y = fmaxf(45.0f, (rect->y1 - rect->y0) * 0.53f),
          .ring_scale = {0.16f, 0.78f, 1.0f},
          .centre = {1.00f, 1.00f, 0.86f, 0.70f},
          .ring = {{1.00f, 0.88f, 0.34f, 0.42f},
                   {1.00f, 0.47f, 0.05f, 0.16f},
                   {0.80f, 0.10f, 0.00f, 0.00f}},
          .flare = 0.16f, .axis_x = 1.0f, .lift_y = -1.0f,
          .seed = (unsigned)effect->pulse_generation,
        };
        float start_x, start_y, end_x, end_y;
        if (BossLightningPathSample(effect, 0.0f, &start_x, &start_y) &&
            BossLightningPathSample(effect, 1.0f, &end_x, &end_y)) {
          const float path_x = end_x - start_x;
          const float path_y = end_y - start_y;
          const float path_length = hypotf(path_x, path_y);
          if (path_length > 0.001f) {
            /* Local +Y is the ellipse's long axis; rotate it onto the
             * authored start-to-end chord. The ribbon retains the individual
             * OAM bends while its surrounding body agrees with their angle. */
            spill.axis_x = body.axis_x = path_y / path_length;
            spill.axis_y = body.axis_y = -path_x / path_length;
          }
        }
      } else {
        spill = (ActionEffectGlowStyle){
          .radius_x = 37.0f,
          .radius_y = 15.0f,
          .ring_scale = {0.24f, 0.66f, 1.0f},
          .centre = {1.00f, 0.82f, 0.35f, 0.18f},
          .ring = {{1.00f, 0.60f, 0.12f, 0.13f},
                   {0.92f, 0.28f, 0.02f, 0.055f},
                   {0.58f, 0.05f, 0.00f, 0.00f}},
          .flare = 0.08f, .axis_x = 1.0f, .lift_y = -1.0f,
          .seed = (unsigned)effect->record_address,
        };
        body = (ActionEffectGlowStyle){
          .radius_x = 18.0f,
          .radius_y = 8.0f,
          .ring_scale = {0.18f, 0.58f, 1.0f},
          .centre = {1.00f, 1.00f, 0.84f, 0.68f},
          .ring = {{1.00f, 0.86f, 0.32f, 0.38f},
                   {1.00f, 0.42f, 0.04f, 0.14f},
                   {0.76f, 0.08f, 0.00f, 0.00f}},
          .flare = 0.12f, .axis_x = 1.0f, .lift_y = -1.0f,
          .seed = (unsigned)effect->pulse_generation,
        };
        spill_y = body_y = rect->y1 - 2.0f;
      }
      break;
    default:
      return true;
  }

  if (!AppendGlow(writer, effect, &spill, pulse, spill_x, spill_y,
                  project_point, userdata))
    return false;
  if (!AppendGlow(writer, effect, &body, pulse, body_x, body_y,
                  project_point, userdata))
    return false;
  return AppendFlamingWheelFireballLighting(
             writer, effect, pulse, project_point, userdata) &&
      AppendBossLightningRibbon(writer, effect, project_point, userdata) &&
      AppendMarahnaLightningRibbon(
             writer, effect, project_point, userdata) &&
      AppendMarahnaBossLightningRibbon(
             writer, effect, project_point, userdata) &&
      AppendSwordBeamTrail(writer, effect, project_point, userdata);
}

static bool SceneEffectStyleKnown(const ActionEffectInstance *effect) {
  if (!effect) return false;
  switch (effect->kind) {
    case kActionEffect_WallTorch:
      return effect->phase == kActionEffectPhase_WallTorch;
    case kActionEffect_EnemyFireball:
      return effect->phase == kActionEffectPhase_EnemyFireballFlight;
    case kActionEffect_MarahnaFireball:
      return (effect->phase == kActionEffectPhase_MarahnaFireballOrb &&
              effect->visual >= 0x05u && effect->visual <= 0x08u) ||
          (effect->phase == kActionEffectPhase_MarahnaFireballSplit &&
           (effect->visual == 0x32u || effect->visual == 0x33u)) ||
          (effect->phase == kActionEffectPhase_MarahnaSnakeFireballShot &&
           (effect->visual == 0x1Du || effect->visual == 0x1Eu));
    case kActionEffect_AitosLavaPit:
      return effect->phase == kActionEffectPhase_AitosLavaPit;
    case kActionEffect_AitosLavaReservoir:
      return effect->phase == kActionEffectPhase_AitosLavaReservoir;
    case kActionEffect_AitosLavaFireball:
      return effect->phase == kActionEffectPhase_AitosLavaFireballFlight;
    case kActionEffect_AitosStatueFire:
      return effect->phase == kActionEffectPhase_AitosStatueFireBreath &&
          effect->visual >= 0x1Cu && effect->visual <= 0x1Fu;
    case kActionEffect_AitosMoltenRock:
      return effect->phase == kActionEffectPhase_AitosMoltenRockFlight &&
          effect->visual == 0x2Bu;
    case kActionEffect_AitosWaterSplash:
      return effect->phase == kActionEffectPhase_AitosWaterSplash;
    case kActionEffect_AitosWaterfall:
      return effect->phase == kActionEffectPhase_AitosWaterfallFlow;
    case kActionEffect_AitosWaterfallMist:
      return effect->phase == kActionEffectPhase_AitosWaterfallMist;
    case kActionEffect_MarahnaLightningLink:
      return effect->phase == kActionEffectPhase_MarahnaLightningActive &&
          ((effect->visual == 0x2Eu &&
            effect->animation_state == 0x27u) ||
           (effect->visual == 0x31u &&
            effect->animation_state == 0x28u));
    case kActionEffect_MarahnaBossLightning:
      return (effect->phase ==
                  kActionEffectPhase_MarahnaBossLightningCharge &&
              (effect->visual == 0x07u || effect->visual == 0x08u)) ||
          (effect->phase == kActionEffectPhase_MarahnaBossLightningOrb &&
           effect->visual == 0x0Au) ||
          (effect->phase == kActionEffectPhase_MarahnaBossLightningBolt &&
           effect->visual == 0x11u) ||
          (effect->phase ==
               kActionEffectPhase_MarahnaBossLightningGroundCharge &&
           effect->visual >= 0x12u && effect->visual <= 0x14u);
    case kActionEffect_LightningTrap:
      return effect->phase == kActionEffectPhase_LightningActive;
    case kActionEffect_BloodpoolBossLightning:
      return (effect->phase == kActionEffectPhase_BossLightningStrike &&
              effect->visual <= 5u) ||
          (effect->phase == kActionEffectPhase_BossLightningImpact &&
           effect->visual >= 8u && effect->visual <= 10u);
    case kActionEffect_SwordBeam:
      return effect->phase == kActionEffectPhase_SwordBeamFlight &&
          (effect->visual == 0x20u || effect->visual == 0x21u ||
           effect->visual == 0x30u || effect->visual == 0x31u);
    case kActionEffect_MinotaurAxe:
      return effect->phase == kActionEffectPhase_MinotaurAxeFlight &&
          (effect->visual <= 0x07u || effect->visual == 0x10u);
    case kActionEffect_FlamingWheel:
      return effect->phase == kActionEffectPhase_FlamingWheelBody;
    case kActionEffect_FlamingWheelProjectile:
      return effect->phase ==
                 kActionEffectPhase_FlamingWheelProjectileFlight &&
          effect->visual <= 0x03u;
    case kActionEffect_IceDragonIceBall:
      return effect->phase == kActionEffectPhase_IceDragonIceBallFlight &&
          effect->visual >= 0x12u && effect->visual <= 0x19u;
    case kActionEffect_TanzaraProjectile:
      return effect->phase == kActionEffectPhase_TanzaraProjectileFlight;
    default:
      return false;
  }
}

static bool BuildSceneEffectList(
    const ActionEffectInstance *effects, uint8_t effect_count,
    uint8_t capacity, bool overflow, uint8_t render_layer,
    bool lighting_enabled, bool particles_enabled,
    ActionEffectProjectPointFn project_point, void *project_userdata,
    ActionSceneEffectRenderBatch *batch) {
  if (!batch) return false;
  /* Submitters consume only [0, count). The scene capacity is deliberately
   * large, so zeroing its unused tail would touch hundreds of KiB per build. */
  batch->vertex_count = 0;
  batch->index_count = 0;
  if (!effects || effect_count > capacity ||
      render_layer >= kActionEffectRenderLayer_Count)
    return false;
  if (overflow || (!lighting_enabled && !particles_enabled)) return true;
  if (!project_point) return false;
  ActionEffectGeometryWriter writer = GeometryWriter(
      batch->vertices, kActionSceneEffectRenderMaxVertices,
      batch->indices, kActionSceneEffectRenderMaxIndices);
  unsigned lightning_filaments = 0;
  unsigned marahna_lightning_links = 0;
  unsigned marahna_boss_lightning_bolts = 0;
  unsigned sword_streams = 0;
  unsigned waterfall_veils = 0;
  unsigned waterfall_mists = 0;
  unsigned lava_reservoirs = 0;
  unsigned lava_glow_segments = 0;
  unsigned flaming_wheels = 0;

  for (uint8_t i = 0; i < effect_count; i++) {
    const ActionEffectInstance *effect = &effects[i];
    if (!(effect->flags & kActionEffectFlag_Visible) ||
        effect->geometry.kind != kActionEffectGeometry_Rect ||
        !RectIsSane(&effect->geometry.data.rect) ||
        !SceneEffectStyleKnown(effect) ||
        effect->obj_priority >= kActionEffectObjPriorityCount ||
        effect->render_layer != render_layer ||
        effect->projection_plane > kActionEffectProjectionPlane_Bg1High)
      continue;
    if (effect->kind == kActionEffect_BloodpoolBossLightning &&
        effect->phase == kActionEffectPhase_BossLightningStrike &&
        ++lightning_filaments > kActionSceneEffectMaxLightningFilaments)
      return false;
    if (effect->kind == kActionEffect_MarahnaLightningLink &&
        ++marahna_lightning_links >
            kActionSceneEffectMaxMarahnaLightningLinks)
      return false;
    if (effect->kind == kActionEffect_MarahnaBossLightning &&
        effect->phase == kActionEffectPhase_MarahnaBossLightningBolt &&
        ++marahna_boss_lightning_bolts >
            kActionSceneEffectMaxMarahnaBossLightningBolts)
      return false;
    if (effect->kind == kActionEffect_SwordBeam &&
        ++sword_streams > kActionSceneEffectMaxSwordStreams)
      return false;
    if (effect->kind == kActionEffect_AitosWaterfall &&
        ++waterfall_veils > kActionSceneEffectMaxWaterfallVeils)
      return false;
    if (effect->kind == kActionEffect_AitosWaterfallMist &&
        ++waterfall_mists > kActionSceneEffectMaxWaterfallVeils)
      return false;
    if (effect->kind == kActionEffect_AitosLavaReservoir) {
      if (++lava_reservoirs > kActionSceneEffectMaxLavaReservoirs)
        return false;
      const unsigned segments = LavaReservoirGlowSegmentCount(effect);
      if (!segments ||
          segments > kActionSceneEffectMaxLavaGlowSegments -
              lava_glow_segments)
        return false;
      lava_glow_segments += segments;
    }
    if (effect->kind == kActionEffect_FlamingWheel &&
        ++flaming_wheels > kActionSceneEffectMaxFlamingWheels)
      return false;

    if (lighting_enabled &&
        !AppendSceneLighting(&writer, effect, project_point,
                             project_userdata))
      return false;
    if (particles_enabled &&
        !AppendSceneParticles(&writer, effect, project_point,
                              project_userdata))
      return false;
  }
  batch->vertex_count = writer.vertex_count;
  batch->index_count = writer.index_count;
  return true;
}

bool ActionSceneEffectRender_Build(const ActionSceneEffectFrame *frame,
                                   bool lighting_enabled,
                                   bool particles_enabled,
                                   ActionEffectProjectPointFn project_point,
                                   void *project_userdata,
                                   ActionSceneEffectRenderBatch *batch) {
  if (!frame) {
    if (batch) {
      batch->vertex_count = 0;
      batch->index_count = 0;
    }
    return false;
  }
  return BuildSceneEffectList(
      frame->effects, frame->effect_count, kActionSceneEffectMaxInstances,
      frame->overflow, kActionEffectRenderLayer_WorldOverlay,
      lighting_enabled, particles_enabled, project_point, project_userdata,
      batch);
}

bool ActionSceneDecorationRender_Build(
    const ActionSceneEffectFrame *frame, uint8_t render_layer,
    bool lighting_enabled, bool particles_enabled,
    ActionEffectProjectPointFn project_point, void *project_userdata,
    ActionSceneEffectRenderBatch *batch) {
  if (!frame) {
    if (batch) {
      batch->vertex_count = 0;
      batch->index_count = 0;
    }
    return false;
  }
  return BuildSceneEffectList(
      frame->decorations, frame->decoration_count,
      kActionSceneDecorationMaxInstances, frame->decoration_overflow,
      render_layer, lighting_enabled, particles_enabled, project_point,
      project_userdata, batch);
}

bool ActionHeatRender_Build(uint16_t game_frame, SDL_Rect output_viewport,
                            int target_width, int target_height,
                            int source_width,
                            ActionHeatRenderMesh *mesh) {
  if (mesh) {
    mesh->vertex_count = 0;
    mesh->index_count = 0;
  }
  if (!mesh || output_viewport.x < 0 || output_viewport.y < 0 ||
      output_viewport.w <= 0 || output_viewport.h <= 0 ||
      target_width <= 0 || target_height <= 0 || source_width <= 0)
    return false;

  const float source_pixel_scale =
      (float)output_viewport.w / (float)source_width;
  const float target_x_per_output =
      (float)target_width / (float)output_viewport.w;
  const float target_y_per_output =
      (float)target_height / (float)output_viewport.h;
  /* Scale in authentic pixels before capping in output pixels. The old
   * 3.25-output-pixel cap reduced a 3420px presentation to less than half an
   * authentic pixel of displacement, which made the haze appear to switch on
   * only where high-contrast art happened to reveal it. This remains below
   * one authentic pixel at ordinary scales and is bounded at 6.5px. */
  const float amplitude = fminf(6.50f, fmaxf(0.75f,
      source_pixel_scale * 0.82f));
  const float phase = (float)game_frame * 0.117f;
  const SDL_FColor white = {1.0f, 1.0f, 1.0f, 1.0f};
  for (int row = 0; row <= kActionHeatMeshRows; row++) {
    const float ny = (float)row / (float)kActionHeatMeshRows;
    const float y = (float)output_viewport.y +
        (float)output_viewport.h * ny;
    /* sin(pi*y) pins top/bottom; weighting toward the floor keeps the HUD-
     * free upper room readable while the lava half visibly shimmers. */
    const float edge_y = sinf(ny * 3.141592654f) * (0.28f + 0.72f * ny);
    for (int column = 0; column <= kActionHeatMeshColumns; column++) {
      const float nx = (float)column / (float)kActionHeatMeshColumns;
      const float x = (float)output_viewport.x +
          (float)output_viewport.w * nx;
      const float edge_x = sinf(nx * 3.141592654f);
      const float envelope = edge_x * edge_y;
      const float wave = sinf(nx * 10.681f + ny * 20.420f + phase) +
          0.42f * sinf(nx * 23.562f - ny * 11.938f - phase * 1.37f);
      const float cross_wave =
          sinf(nx * 16.336f + ny * 8.168f - phase * 0.73f);
      const float sample_x = (float)target_width * nx +
          amplitude * target_x_per_output * envelope * wave;
      const float sample_y = (float)target_height * ny +
          amplitude * target_y_per_output * 0.24f * envelope * cross_wave;
      mesh->vertices[mesh->vertex_count++] = (SDL_Vertex){
        {x, y}, white,
        {sample_x / (float)target_width,
         sample_y / (float)target_height},
      };
    }
  }
  for (int row = 0; row < kActionHeatMeshRows; row++) {
    for (int column = 0; column < kActionHeatMeshColumns; column++) {
      const int a = row * (kActionHeatMeshColumns + 1) + column;
      const int b = a + 1;
      const int c = a + kActionHeatMeshColumns + 1;
      const int d = c + 1;
      mesh->indices[mesh->index_count++] = a;
      mesh->indices[mesh->index_count++] = c;
      mesh->indices[mesh->index_count++] = d;
      mesh->indices[mesh->index_count++] = a;
      mesh->indices[mesh->index_count++] = d;
      mesh->indices[mesh->index_count++] = b;
    }
  }
  return mesh->vertex_count == kActionHeatMeshVertices &&
      mesh->index_count == kActionHeatMeshIndices;
}
