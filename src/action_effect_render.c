#include "action_effect_render.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

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
    /* Rotation by the style's axis. Because the axis arrives as a UNIT vector
     * it is already (cos, sin) — the whole orientation path stays free of
     * trigonometry, and so keeps the bit-identical determinism the batch is
     * tested for. */
    float ax = style->axis_x, ay = style->axis_y;
    for (int s = 0; s < kSegments; s++) {
      float shape = 1.0f + wobble * FlameSilhouette(
          style->seed, (unsigned)effect->pulse_ticks, s);
      float ux = kCircle32[s][0] * radius_x * scale * shape;
      float uy = kCircle32[s][1] * radius_y * scale * shape;
      batch->vertices[batch->vertex_count++] = (SDL_Vertex){
        { anchor.x + ux * ax - uy * ay + style->lift_x * lift,
          anchor.y + ux * ay + uy * ax + style->lift_y * lift },
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
static bool AppendEmbers(ActionEffectRenderBatch *batch,
                         const ActionEffectBurst *burst, unsigned count,
                         const SpellVisual *visual,
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
      const float *dir = kCircle32[(i * 7u + (seed >> 11)) & 31u];
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
    if (!AppendGlow(batch, burst.anchor, &spill, strength, mid_x, mid_y,
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
      if (!AppendGlow(batch, burst.anchor, &body,
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
        !AppendEmbers(batch, &burst, embers, visual, project_point,
                      project_userdata))
      return false;
  }
  return true;
}
