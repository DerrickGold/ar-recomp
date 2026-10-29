#ifndef AR_ACTION_STORM_H
#define AR_ACTION_STORM_H
/* Stateless storm channels and return strokes. Camera position never enters
 * the seed or clock; native rendering and editor scrubbing share this model. */
#include "action_effects.h"
#include "deterministic_hash.h"
#include "render/render_types.h"
#include <math.h>

enum {
  kActionStormMainSegments = 12,
  kActionStormForkSegments = 5,
  kActionStormMaxForks = 3,
  kActionStormGlowSegments = 16
};

typedef struct ActionStormFlash {
  uint32_t seed;
  float channel, cloud;
  float forks[kActionStormMaxForks];
} ActionStormFlash;

typedef struct ActionStormChannel {
  ArRenderPointF main[kActionStormMainSegments + 1];
  ArRenderPointF forks[kActionStormMaxForks][kActionStormForkSegments + 1];
} ActionStormChannel;

static inline bool ActionStorm_CustomParallax(const ActionEffectInstance *e) {
  return e->kind == kActionEffect_AuthoredStorm &&
      e->projection_plane == kActionEffectProjectionPlane_Bg2 &&
      e->field_style.storm.parallax_set;
}

static inline float ActionStorm_GlowHeight(const ActionEffectInstance *e) {
  const float height = e->geometry.data.rect.y1 - e->geometry.data.rect.y0;
  return e->field_style.storm.ground_colors
      ? fminf(height * .48f, 12 + 12 * (1 - e->field_style.softness)) : height * .48f;
}

/* Truncate at the first contact, keeping the crooked channel above it intact.
 * Equal tail points emit no triangles. Forks never resume below the ground. */
static inline void ActionStorm_Stop(ArRenderPointF *points, unsigned segments, float end) {
  if (end >= segments) return;
  const unsigned whole = (unsigned)fmaxf(0, floorf(end));
  const float t = fmaxf(0, end - whole);
  const ArRenderPointF hit = {points[whole].x + (points[whole + 1].x - points[whole].x) * t,
      points[whole].y + (points[whole + 1].y - points[whole].y) * t};
  for (unsigned i = whole + 1; i <= segments; ++i) points[i] = hit;
}

static inline float ActionStorm_Unit(uint32_t seed) {
  return (DeterministicHash_Mix32(seed) & 65535u) / 65535.f;
}

static inline float ActionStorm_Decay(float age, float decay) {
  return age < 0 ? 0 : expf(-age / decay);
}

static inline float ActionStorm_Pulse(float age, float hold, float decay) {
  return age < 0 ? 0 : ActionStorm_Decay(fmaxf(0, age - hold), decay);
}

/* Keep the origin inside its emitting cloud, slightly below the region top.
 * The editor displays this row as the cloud-attachment guide. */
static inline float ActionStorm_SourceY(const ActionEffectInstance *e) {
  const ActionEffectLocalRect *r = &e->geometry.data.rect;
  return r->y0 + (r->y1 - r->y0) * .12f;
}

/* A held discharge, a lingering channel, then a heavier return stroke. All
 * phases scale with the authored active duration, including the cloud glow.
 * The channel stays visible between pulses rather than blinking off. */
static inline bool ActionStorm_Flash(const ActionEffectInstance *e, ActionStormFlash *out) {
  const uint32_t ticks = e->pulse_ticks + e->particle_style.seed % 4096;
  const unsigned cycle = ticks / e->particle_lifetime;
  const uint32_t seed = DeterministicHash_Mix32(e->particle_style.seed ^ cycle * 0x9e3779b9u);
  const float duration = fmaxf(4, e->particle_lifetime * e->field_style.activity);
  const float start = floorf((e->particle_lifetime - duration) *
      (.15f + .7f * ActionStorm_Unit(seed ^ 31)));
  const float age = (ticks % e->particle_lifetime) - start;
  if (age < 0 || age >= duration) return false;
  const float delay = floorf(duration * (.30f + .06f * ActionStorm_Unit(seed ^ 131)));
  const float third_delay = delay + floorf(duration *
      (.24f + .06f * ActionStorm_Unit(seed ^ 251)));
  const bool third = duration >= 18 && ActionStorm_Unit(seed ^ 281) < .35f;
  const float first = ActionStorm_Pulse(age, duration * .08f, duration * .14f);
  const float second = ActionStorm_Pulse(age - delay, duration * .10f, duration * .18f);
  const float last = third ?
      ActionStorm_Pulse(age - third_delay, duration * .06f, duration * .13f) : 0;
  /* Feather the end instead of truncating a still-visible held return stroke. */
  const float end = fminf(1, (duration - age) / (duration * .2f));
  const float fade = end * end * (3 - 2 * end);
  const float residual = .08f * ActionStorm_Decay(age, duration * .42f);
  *out = (ActionStormFlash){
    .seed = seed,
    .channel = fminf(1, .85f * first + .95f * second + .55f * last + residual) * fade,
    .cloud = fminf(1, .48f * ActionStorm_Decay(age, duration * .27f) +
        .58f * ActionStorm_Decay(age - delay, duration * .30f) +
        (third ? .32f * ActionStorm_Decay(age - third_delay, duration * .25f) : 0)) * fade
  };
  for (unsigned i = 0; i < kActionStormMaxForks; ++i) {
    /* The first fork is the fading leader. Later forks become strongly
     * visible on the return stroke, rooted in exactly the same main channel. */
    const float initial = i ? 0 : .55f;
    const float returning = i ? .75f + .15f * ActionStorm_Unit(seed ^ (i * 43)) : .45f;
    out->forks[i] = (initial * first + returning * second + .4f * last + residual * .4f) * fade;
  }
  return true;
}

static inline bool ActionStorm_HasSkyWeather(const ActionSceneEffectFrame *frame) {
  if (!frame || frame->authored_count>kActionAuthoredMaxInstances) return false;
  for (unsigned i=0;i<frame->authored_count;++i) {
    const ActionEffectInstance *e=&frame->authored[i];
    ActionStormFlash flash;
    if (e->kind==kActionEffect_AuthoredStorm && (e->flags&kActionEffectFlag_Visible) &&
        e->render_layer==kActionEffectRenderLayer_Bg2Weather && e->particle_lifetime>=120 &&
        ActionStorm_Flash(e,&flash)) return true;
  }
  return false;
}

/* Split a coarse run into crooked unequal steps. Offset in both dimensions,
 * rather than alternating X on a regular Y ladder. At a kink this can briefly
 * turn sideways or back upwards before continuing towards the endpoint. */
static inline void ActionStorm_Subdivide(ArRenderPointF a, ArRenderPointF b,
    float bend, uint32_t seed, unsigned segments, ArRenderPointF *points) {
  points[0] = a;
  points[segments] = b;
  for (unsigned i = 1; i < segments; ++i) {
    const uint32_t joint = seed ^ (i * 0x85ebca6bu);
    const float t = (i + .55f * (ActionStorm_Unit(joint ^ 17) - .5f)) / segments;
    points[i] = (ArRenderPointF){
      a.x + (b.x - a.x) * t + (ActionStorm_Unit(joint ^ 53) - .5f) * bend,
      a.y + (b.y - a.y) * t + (ActionStorm_Unit(joint ^ 97) - .5f) * bend * .8f
    };
  }
}

static inline void ActionStorm_Path(const ActionEffectInstance *e, uint32_t seed,
    ActionStormChannel *out) {
  const ActionEffectLocalRect *r = &e->geometry.data.rect;
  const float width = r->x1 - r->x0, height = r->y1 - r->y0;
  const float x = r->x0 + width * (.2f + .6f * ActionStorm_Unit(seed ^ 53));
  const float y = ActionStorm_SourceY(e);
  const float length = e->field_style.storm.ground_colors ? r->y1 - y :
      height * (.55f + .3f * ActionStorm_Unit(seed ^ 97));
  const float lean = (ActionStorm_Unit(seed ^ 17) - .5f) * width * .45f;
  const float bend = e->field_style.amplitude;
  ArRenderPointF spine[4] = {{x, y}};
  for (unsigned i = 1; i < 4; ++i) {
    const float t = i / 3.f;
    spine[i] = (ArRenderPointF){
      x + lean * t + (i < 3 ? (ActionStorm_Unit(seed ^ (i * 127)) - .5f) * bend * 2 : 0),
      y + length * (t + (i < 3 ? (ActionStorm_Unit(seed ^ (i * 157)) - .5f) * .18f : 0))
    };
    ActionStorm_Subdivide(spine[i - 1], spine[i], bend * .7f,
        seed ^ (i * 173), kActionStormMainSegments / 3, out->main + (i - 1) * 4);
  }
  for (unsigned i = 0; i + 1 < e->field_style.strands; ++i) {
    const uint32_t fork = DeterministicHash_Mix32(seed ^ ((i + 1) * 0xc2b2ae35u));
    /* Separate junctions and opposite first/return forks keep new growth
     * readable even when the entire distant strike is only a few pixels. */
    const unsigned joint = 2 + i * 3 + fork % 2;
    const ArRenderPointF root = out->main[joint];
    const float side = (((seed >> 8) + i) & 1) ? 1.f : -1.f;
    const float reach = fminf(width * .19f, length * .65f + bend);
    const ArRenderPointF tip = {
      root.x + side * reach * (.55f + .45f * ActionStorm_Unit(fork ^ 13)),
      root.y + length * (.04f + .23f * ActionStorm_Unit(fork ^ 23))
    };
    const ArRenderPointF elbow = {
      root.x + (tip.x - root.x) * (.3f + .25f * ActionStorm_Unit(fork ^ 37)),
      root.y + (tip.y - root.y) * .4f + (ActionStorm_Unit(fork ^ 47) - .5f) * bend
    };
    ActionStorm_Subdivide(root, elbow, bend * .45f, fork, 2, out->forks[i]);
    ActionStorm_Subdivide(elbow, tip, bend * .35f, fork ^ 67,
        kActionStormForkSegments - 2, out->forks[i] + 2);
    if (e->storm_contact.valid)
      ActionStorm_Stop(out->forks[i], kActionStormForkSegments,
          joint >= e->storm_contact.end[0] ? 0 : e->storm_contact.end[i + 1]);
  }
  if (e->storm_contact.valid)
    ActionStorm_Stop(out->main, kActionStormMainSegments, e->storm_contact.end[0]);
}

/* Four clipped triangles per segment; clipping can yield seven vertices.
 * Return strokes reuse the channel, so they do not multiply the draw budget. */
static inline unsigned ActionStorm_VertexBudget(unsigned strands) {
  return 7 * kActionStormGlowSegments +
      28 * (kActionStormMainSegments + kActionStormForkSegments * (strands - 1));
}

static inline ActionEffectLocalRect ActionStorm_Bounds(const ActionEffectInstance *e) {
  const ActionEffectLocalRect *r = &e->geometry.data.rect;
  const float width = r->x1 - r->x0, height = r->y1 - r->y0;
  const float bend = e->field_style.amplitude;
  const float pad = width * .28f + bend * 2 + 3;
  /* The cloud flash is centred on the source, not below it in open sky. */
  return (ActionEffectLocalRect){r->x0 - pad, r->y0 - height * .36f - bend - 3,
      r->x1 + pad, r->y1 + height * .1f + bend + 3};
}
#endif
