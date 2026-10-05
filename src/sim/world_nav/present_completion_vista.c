#include "sim/world_nav/present_world_nav_internal.h"
#include "sim/world_nav/completion_vista_backend.h"
#include "sim/world_nav/sim_completion_cherubs.h"

static ArRenderTexture s_sparkles;
static ArRenderTexture s_cherubs;
static uint32_t s_cherub_revision;
static ArRenderTexture s_water_pattern;
static uint32_t s_water_pattern_revision;
static ArRenderTexture s_feathers;
static float Random(unsigned id);

static bool EnsureWaterPattern(ArRenderDevice *device) {
  uint32_t frames[kWorldWaterFrameCount][64], pixels[kWorldWaterFrameCount * 64];
  if (!SimWorldMap_CopyWaterFrames(frames)) return false;
  uint32_t hash = UINT32_C(2166136261);
  unsigned darkest = 765, brightest = 0;
  for (unsigned f = 0; f < kWorldWaterFrameCount; f++)
    for (unsigned p = 0; p < 64; p++) {
      const uint32_t color = frames[f][p];
      const unsigned value = ((color >> 16) & 255) + ((color >> 8) & 255) + (color & 255);
      if (value < darkest) darkest = value;
      if (value > brightest) brightest = value;
      hash = (hash ^ color) * UINT32_C(16777619);
    }
  if (darkest == brightest) return false;
  if (!ArRenderTexture_IsValid(s_water_pattern)) {
    const ArRenderTextureDesc desc = {kWorldWaterFrameCount * 8,     8,
                                      kArRenderPixelFormat_Argb8888, kArRenderTextureUsage_Static,
                                      kArRenderFilter_Nearest,       kArRenderBlendMode_Alpha};
    if (!ArRenderDevice_CreateTexture(device, &desc, &s_water_pattern)) return false;
    s_water_pattern_revision = 0;
  }
  if (s_water_pattern_revision == (hash ? hash : 1)) return true;
  /* Keep genuine wavelet coverage, independent of the old blue palette.
   * Current ocean lighting supplies the tint; these texels add only relief. */
  for (unsigned f = 0; f < kWorldWaterFrameCount; f++)
    for (unsigned p = 0; p < 64; p++) {
      const uint32_t color = frames[f][p];
      const unsigned value = ((color >> 16) & 255) + ((color >> 8) & 255) + (color & 255);
      const unsigned ink = (value - darkest) * 255 / (brightest - darkest);
      pixels[(p / 8) * kWorldWaterFrameCount * 8 + f * 8 + p % 8] =
          UINT32_C(0xff000000) | ink * UINT32_C(0x010101);
    }
  if (!ArRenderDevice_UpdateTexture(device, s_water_pattern, NULL, pixels,
                                    kWorldWaterFrameCount * 8 * 4))
    return false;
  s_water_pattern_revision = hash ? hash : 1;
  Sim3DPerformance_AddUpload(sizeof(pixels));
  return true;
}

static void DrawCherubs(ArRenderDevice *device, ArRenderRectI viewport, float time) {
  uint32_t pixels[kSimCompletionCherubAtlasWidth * kSimCompletionCherubAtlasHeight], revision;
  if (!SimCompletionCherubs_Copy(pixels, &revision)) return;
  if (!ArRenderTexture_IsValid(s_cherubs)) {
    const ArRenderTextureDesc desc = {
        kSimCompletionCherubAtlasWidth, kSimCompletionCherubAtlasHeight,
        kArRenderPixelFormat_Argb8888,  kArRenderTextureUsage_Static,
        kArRenderFilter_Nearest,        kArRenderBlendMode_Alpha};
    if (!ArRenderDevice_CreateTexture(device, &desc, &s_cherubs)) return;
    s_cherub_revision = 0;
  }
  if (s_cherub_revision != revision) {
    if (!ArRenderDevice_UpdateTexture(device, s_cherubs, NULL, pixels,
                                      kSimCompletionCherubAtlasWidth * 4))
      return;
    s_cherub_revision = revision;
    Sim3DPerformance_AddUpload(sizeof(pixels));
  }
  static const struct {
    float x, y, height, opacity;
  } cherubs[] = {
      {.380f, .080f, 19, .65f}, {.465f, .055f, 14, .40f}, {.535f, .118f, 22, .68f},
      {.615f, .073f, 16, .50f}, {.565f, .030f, 12, .32f},
  };
  enum { kCount = sizeof(cherubs) / sizeof(cherubs[0]) };
  ArRenderVertex2D vertices[kCount * 4];
  int32_t indices[kCount * 6];
  const float scale = viewport.h / 448.0f;
  for (int i = 0; i < kCount; i++) {
    const float flight = sinf(time * (.075f + i * .012f) + i * 1.7f);
    const float x = viewport.x + (cherubs[i].x + flight * (i & 1 ? .028f : .014f)) * viewport.w;
    const float y =
        viewport.y + cherubs[i].y * viewport.h + sinf(time * .70f + i * 2.3f) * 1.8f * scale;
    const float height = cherubs[i].height * scale * (1 + flight * .09f);
    const float width = height * kSimCompletionCherubWidth / kSimCompletionCherubHeight;
    const int frame = (int)floorf(fmodf(time * 4 + i, kSimCompletionCherubFrames));
    const float u0 = frame / (float)kSimCompletionCherubFrames;
    const float u1 = (frame + 1) / (float)kSimCompletionCherubFrames;
    const ArRenderColorF tint = {.18f, .24f, .36f, cherubs[i].opacity * (.94f + flight * .06f)};
    const ArRenderVertex2D quad[4] = {
        {{x - width * .5f, y - height * .5f}, tint, {u0, 0}},
        {{x + width * .5f, y - height * .5f}, tint, {u1, 0}},
        {{x + width * .5f, y + height * .5f}, tint, {u1, 1}},
        {{x - width * .5f, y + height * .5f}, tint, {u0, 1}},
    };
    memcpy(vertices + i * 4, quad, sizeof(quad));
    const int32_t faces[6] = {i * 4, i * 4 + 1, i * 4 + 2, i * 4, i * 4 + 2, i * 4 + 3};
    memcpy(indices + i * 6, faces, sizeof(faces));
  }
  /* Draw before the cloud volumes and native HUD. The backdrop's existing
   * master fade covers this art too; shader/particle foreground fades once. */
  const ArRenderDrawState draw = {.flags = kArRenderDrawState_Blend,
                                  .blend = kArRenderBlendMode_Alpha};
  if (ArRenderDevice_DrawGeometryWithState(device, s_cherubs, vertices, kCount * 4, indices,
                                           kCount * 6, &draw))
    Sim3DPerformance_AddDraw(kCount * 4, kCount * 6);
}

static bool DrawFeathers(ArRenderDevice *device, ArRenderRectI viewport,
                         const CompletionVistaParams *p) {
  if (!(p->detail_flags & kDeathHeimCompletion_Feathers) || p->brightness <= 0) return true;
  if (!ArRenderTexture_IsValid(s_feathers)) {
    /* Two tiny hand-shaped pixel poses; nearest sampling keeps the feathers
     * in the same visual vocabulary as the native angel silhouettes. */
    static const char *const art[2][12] = {
        {"....o...", "...wwo..", "..wwwwo.", "..wwswo.", "..wswwo.", "..wswo..", "..swwo..",
         "..swo...", "..so....", "..o.....", ".o......", "........"},
        {"....o...", "....wo..", "...wwo..", "...swo..", "...swo..", "..swo...", "..swo...",
         "..so....", "..so....", "..o.....", ".o......", "........"},
    };
    uint32_t pixels[16 * 12] = {0};
    for (int f = 0; f < 2; f++)
      for (int y = 0; y < 12; y++)
        for (int x = 0; x < 8; x++) {
          const char ink = art[f][y][x];
          pixels[y * 16 + f * 8 + x] = ink == 'w'   ? UINT32_C(0xfff4f2e4)
                                       : ink == 's' ? UINT32_C(0xffd5dfef)
                                       : ink == 'o' ? UINT32_C(0x887c9bb7)
                                                    : 0;
        }
    const ArRenderTextureDesc desc = {16,
                                      12,
                                      kArRenderPixelFormat_Argb8888,
                                      kArRenderTextureUsage_Static,
                                      kArRenderFilter_Nearest,
                                      kArRenderBlendMode_Alpha};
    if (!ArRenderDevice_CreateTexture(device, &desc, &s_feathers)) return false;
    if (!ArRenderDevice_UpdateTexture(device, s_feathers, NULL, pixels, 16 * 4)) {
      ArRenderDevice_DestroyTexture(device, s_feathers);
      s_feathers = ArRenderTexture_Invalid();
      return false;
    }
    Sim3DPerformance_AddUpload(sizeof(pixels));
  }
  enum { kCount = 12 };
  ArRenderVertex2D vertices[kCount * 4];
  int32_t indices[kCount * 6];
  const float scale = viewport.h / 448.0f;
  for (int i = 0; i < kCount; i++) {
    const float seed = Random(i + 901), depth = Random(i + 933);
    const float life = 15 + seed * 10;
    const float age = (float)fmod((double)p->time + seed * life, life) / life;
    const float phase = p->time * (.55f + depth * .30f) + i * 2.4f;
    const float x = .30f + seed * .40f + sinf(phase) * (.012f + age * .030f);
    const float y = .065f + age * fmaxf(.1f, p->horizon - .10f);
    const float h = (6 + depth * 5) * (1 + age * .25f) * scale, w = h * 8 / 12;
    const float angle = sinf(phase) * .55f;
    const float alpha = sinf(age * kPi) * sinf(age * kPi) * (.28f + depth * .35f) * p->brightness;
    const float frame = cosf(phase) > .15f ? 0 : .5f;
    const ArRenderColorF tint = {1, 1, 1, alpha};
    for (int v = 0; v < 4; v++) {
      const float dx = (v == 1 || v == 2 ? .5f : -.5f) * w, dy = (v >= 2 ? .5f : -.5f) * h;
      vertices[i * 4 + v] =
          (ArRenderVertex2D){{viewport.x + x * viewport.w + dx * cosf(angle) - dy * sinf(angle),
                              viewport.y + y * viewport.h + dx * sinf(angle) + dy * cosf(angle)},
                             tint,
                             {frame + (v == 1 || v == 2 ? .5f : 0), v >= 2 ? 1 : 0}};
    }
    const int32_t quad[6] = {i * 4, i * 4 + 1, i * 4 + 2, i * 4, i * 4 + 2, i * 4 + 3};
    memcpy(indices + i * 6, quad, sizeof(quad));
  }
  const bool drawn =
      ArRenderDevice_DrawGeometry(device, s_feathers, vertices, kCount * 4, indices, kCount * 6);
  if (drawn) Sim3DPerformance_AddDraw(kCount * 4, kCount * 6);
  return drawn;
}

static bool EnsureSparkles(ArRenderDevice *device) {
  if (ArRenderTexture_IsValid(s_sparkles)) return true;
  uint32_t pixels[64 * 32];
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 64; x++) {
      const float px = ((x % 32) + .5f - 16) / 16, py = (y + .5f - 16) / 16;
      const float round = expf(-(px * px + py * py) * 9);
      const float cross = expf(-px * px * 160 - py * py * 8) + expf(-py * py * 160 - px * px * 8);
      const float alpha = fminf(1, x < 32 ? cross * .65f + round * .20f : round);
      pixels[y * 64 + x] = ((uint32_t)lroundf(alpha * 255) << 24) | UINT32_C(0xffffff);
    }
  const ArRenderTextureDesc desc = {64,
                                    32,
                                    kArRenderPixelFormat_Argb8888,
                                    kArRenderTextureUsage_Static,
                                    kArRenderFilter_Linear,
                                    kArRenderBlendMode_Add};
  if (!ArRenderDevice_CreateTexture(device, &desc, &s_sparkles)) return false;
  if (ArRenderDevice_UpdateTexture(device, s_sparkles, NULL, pixels, 64 * 4)) {
    Sim3DPerformance_AddUpload(sizeof(pixels));
    return true;
  }
  ArRenderDevice_DestroyTexture(device, s_sparkles);
  s_sparkles = ArRenderTexture_Invalid();
  return false;
}
static float Random(unsigned id) {
  uint32_t n = id * UINT32_C(747796405) + UINT32_C(2891336453);
  n = ((n >> ((n >> 28) + 4)) ^ n) * UINT32_C(277803737);
  return ((n >> 22) ^ n) / (float)UINT32_MAX;
}

typedef struct VistaVector {
  float x, y, z;
} VistaVector;
static VistaVector Vector(const float v[3]) { return (VistaVector){v[0], v[1], v[2]}; }
static VistaVector Add(VistaVector a, VistaVector b) {
  return (VistaVector){a.x + b.x, a.y + b.y, a.z + b.z};
}
static VistaVector Scale(VistaVector v, float scale) {
  return (VistaVector){v.x * scale, v.y * scale, v.z * scale};
}
static float Dot(VistaVector a, VistaVector b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static VistaVector Unit(VistaVector v) { return Scale(v, 1 / fmaxf(.00001f, sqrtf(Dot(v, v)))); }
static VistaVector Cross(VistaVector a, VistaVector b) {
  return (VistaVector){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static VistaVector Origin(const CompletionVistaParams *p) {
  return Add(Vector(p->eye), (VistaVector){0, 0, p->radius});
}
static VistaVector Ray(const CompletionVistaParams *p, ArRenderPointF uv) {
  return Unit(Add(Vector(p->forward), Add(Scale(Vector(p->right), (2 * uv.x - 1) * p->right_scale),
                                          Scale(Vector(p->up), (1 - 2 * uv.y) * p->up_scale))));
}
static bool WaterHit(const CompletionVistaParams *p, VistaVector origin, VistaVector ray,
                     VistaVector *hit) {
  const float radius = p->radius * .9975f, b = Dot(origin, ray);
  const float discriminant = b * b - Dot(origin, origin) + radius * radius;
  if (discriminant < 0 || b >= 0) return false;
  const float distance = -b - sqrtf(discriminant);
  if (distance <= 0) return false;
  *hit = Add(origin, Scale(ray, distance));
  return true;
}
static bool PrepareLights(CompletionVistaParams *p) {
  const VistaVector origin = Origin(p), forward = Vector(p->forward);
  /* Give the native hero a world-space anchor at the water depth under the
   * platform. The light passes through the torso and continues onto the sea. */
  const ArRenderPointF feet = {fminf(1, fmaxf(0, p->player.x)),
                               fminf(.96f, fmaxf(p->horizon + .08f, p->player.y + .10f))};
  VistaVector water;
  /* Free camera can move the actor outside the globe silhouette. Borrow a
   * visible water depth while preserving the actor's actual projected ray. */
  if (!WaterHit(p, origin, Ray(p, feet), &water) &&
      !WaterHit(p, origin, Ray(p, (ArRenderPointF){.5f, .92f}), &water))
    return false;
  const float depth = Dot(Add(water, Scale(origin, -1)), forward);
  static const struct {
    ArRenderPointF aperture;
    float spread, water_offset, source_depth, reach, slope, strength;
  } beams[] = {
      {{.50f, .035f}, 0, 0, 1.15f, .70f, .18f, .90f},
      /* Stagger sources and ocean receivers in depth. Ordered image-space
       * origins and spreads keep the axes diverging at every image row,
       * including where the nearer shafts continue past distant receivers. */
      {{.286f, .060f}, -.36f, .075f, 1.80f, .13f, .017f, .38f},
      {{.332f, .060f}, -.29f, .300f, .72f, .20f, .032f, .82f},
      {{.366f, .060f}, -.23f, .130f, 1.25f, .38f, .020f, .54f},
      {{.416f, .060f}, -.18f, .240f, .88f, .42f, .055f, .75f},
      {{.447f, .060f}, -.13f, .055f, 2.10f, .45f, .024f, .44f},
      {{.482f, .060f}, -.075f, .180f, 1.40f, .55f, .036f, .58f},
      {{.523f, .060f}, .075f, .065f, 1.90f, .42f, .030f, .52f},
      {{.558f, .060f}, .13f, .300f, .70f, .35f, .040f, .82f},
      {{.589f, .060f}, .18f, .130f, 1.30f, .48f, .022f, .58f},
      {{.626f, .060f}, .23f, .230f, 1.00f, .40f, .048f, .72f},
      {{.676f, .060f}, .29f, .055f, 2.15f, .13f, .021f, .42f},
      {{.706f, .060f}, .36f, .180f, 1.30f, .18f, .025f, .56f},
  };
  _Static_assert(sizeof(beams) / sizeof(beams[0]) == kCompletionVistaLightCount,
                 "Every completion light needs a beam layout");
  for (int i = 0; i < kCompletionVistaLightCount; i++) {
    const VistaVector source_ray = Ray(p, beams[i].aperture);
    const VistaVector source =
        Add(origin, Scale(source_ray,
                          depth * beams[i].source_depth / fmaxf(.05f, Dot(source_ray, forward))));
    VistaVector target;
    if (!i) {
      const VistaVector hero_ray = Ray(p, p->player);
      target = Add(origin, Scale(hero_ray, depth / fmaxf(.05f, Dot(hero_ray, forward))));
    } else {
      const float y = fminf(.96f, p->horizon + beams[i].water_offset);
      const ArRenderPointF uv = {beams[i].aperture.x + beams[i].spread * (y - beams[i].aperture.y),
                                 y};
      if (!WaterHit(p, origin, Ray(p, uv), &target)) target = water;
    }
    /* Short cones dissolve toward the camera. The broad central cone still
     * reaches the hero; ambient sky illumination keeps the entire sea lit. */
    target = Add(source, Scale(Add(target, Scale(source, -1)), beams[i].reach));
    CompletionVistaLight *light = &p->lights[i];
    light->source[0] = source.x;
    light->source[1] = source.y;
    light->source[2] = source.z;
    light->target[0] = target.x;
    light->target[1] = target.y;
    light->target[2] = target.z;
    light->cone_slope = beams[i].slope;
    light->strength = beams[i].strength;
  }
  return true;
}
static ArRenderPointF Project(const CompletionVistaParams *p, VistaVector point) {
  const VistaVector relative = Add(point, Scale(Origin(p), -1));
  const float depth = fmaxf(.001f, Dot(relative, Vector(p->forward)));
  return (ArRenderPointF){.5f + .5f * Dot(relative, Vector(p->right)) / (depth * p->right_scale),
                          .5f - .5f * Dot(relative, Vector(p->up)) / (depth * p->up_scale)};
}

static bool DrawSunGlints(ArRenderDevice *device, ArRenderRectI viewport,
                          const CompletionVistaParams *p) {
  if (!(p->detail_flags & kDeathHeimCompletion_SunGlints) || p->brightness <= 0) return true;
  if (!EnsureSparkles(device)) return false;
  enum { kSlots = 96 };
  ArRenderVertex2D vertices[kSlots * 4];
  int32_t indices[kSlots * 6];
  const float scale = viewport.h / 448.0f;
  int count = 0;
  for (unsigned i = 0; i < kSlots; i++) {
    /* Re-seed every short appearance instead of repeating one star field.
     * The clock and slot hash are presentation-only: retained frames agree,
     * and no gameplay RNG, emitter updates or simulation objects are used. */
    const float offset = Random(i + 1401), life = 2.8f + offset * 3.5f;
    const float clock = p->time + offset * 37;
    const uint32_t cycle = (uint32_t)floorf(clock / life);
    const unsigned seed = i * 19u + cycle * UINT32_C(2654435761) + 1601u;
    const float age = clock - cycle * life, duration = .45f + Random(seed + 1) * .65f;
    if (age >= duration) continue;
    const float depth = powf(Random(seed + 2), 1.15f);
    ArRenderPointF uv = {.02f + .96f * Random(seed + 3),
                         p->horizon + .018f + depth * fmaxf(0, .96f - p->horizon)};
    /* Trace the actual globe surface so wide-view sky/limb pixels never
     * receive stars. A little wave drift keeps the glints attached to water. */
    if (p->waves) uv.x += sinf(p->time * 1.7f + Random(seed + 6) * 2 * kPi) * .0015f * depth;
    VistaVector point;
    if (!WaterHit(p, Origin(p), Ray(p, uv), &point)) continue;
    uv = Project(p, point);
    const float size = (.65f + depth * 4.8f + Random(seed + 4) * .50f) * scale;
    if (uv.y * viewport.h - size <= p->horizon * viewport.h ||
        uv.y * viewport.h + size >= viewport.h)
      continue;
    const float pulse = sinf(age / duration * kPi);
    const float alpha =
        pulse * pulse * (.60f + Random(seed + 5) * .40f) * (.22f + depth * .78f) * p->brightness;
    const ArRenderColorF tint = {1, .97f, .90f, alpha};
    const float x = viewport.x + uv.x * viewport.w, y = viewport.y + uv.y * viewport.h;
    const ArRenderVertex2D quad[4] = {
        {{x - size, y - size}, tint, {0, 0}},
        {{x + size, y - size}, tint, {.5f, 0}},
        {{x + size, y + size}, tint, {.5f, 1}},
        {{x - size, y + size}, tint, {0, 1}},
    };
    memcpy(vertices + count * 4, quad, sizeof(quad));
    const int32_t faces[6] = {count * 4, count * 4 + 1, count * 4 + 2,
                              count * 4, count * 4 + 2, count * 4 + 3};
    memcpy(indices + count * 6, faces, sizeof(faces));
    count++;
  }
  if (!count) return true;
  const ArRenderDrawState draw = {.flags = kArRenderDrawState_Blend,
                                  .blend = kArRenderBlendMode_Add};
  const bool drawn = ArRenderDevice_DrawGeometryWithState(device, s_sparkles, vertices, count * 4,
                                                          indices, count * 6, &draw);
  if (drawn) Sim3DPerformance_AddDraw(count * 4, count * 6);
  return drawn;
}

/* Fixed populations, analytic birth/death envelopes, no gameplay RNG or
 * simulation objects. Retained presents use their weather clock; no emitter
 * or update loop exists outside B. */
static bool DrawParticles(ArRenderDevice *device, ArRenderRectI viewport,
                          const CompletionVistaParams *p) {
  if (p->brightness <= 0 || (p->stage == kCompletionVistaStage_Ocean && !p->waves) ||
      (p->stage == kCompletionVistaStage_Shafts &&
       !(p->detail_flags & kDeathHeimCompletion_Shafts)))
    return true;
  if (!EnsureSparkles(device)) return false;
  enum { kCount = 128, kGlints = 96 };
  ArRenderVertex2D vertices[kCount * 4];
  int32_t indices[kCount * 6];
  const bool water = p->stage == kCompletionVistaStage_Ocean;
  const int count = water ? kCount : 48;
  const float scale = viewport.h / 448.0f;
  struct ParticleLight {
    VistaVector source, axis, mote_side, mote_up, footprint, side, along;
    float length, width, footprint_length, water_fade;
    bool valid;
  } frames[kCompletionVistaLightCount];
  for (int j = 0; j < kCompletionVistaLightCount; j++) {
    const CompletionVistaLight *light = &p->lights[j];
    struct ParticleLight *f = &frames[j];
    memset(f, 0, sizeof(*f));
    f->source = Vector(light->source);
    const VistaVector delta = Add(Vector(light->target), Scale(f->source, -1));
    f->axis = Unit(delta);
    f->length = sqrtf(Dot(delta, delta));
    f->mote_side = Unit(Cross(f->axis, (VistaVector){0, 0, 1}));
    f->mote_up = Cross(f->axis, f->mote_side);
    f->valid = WaterHit(p, f->source, f->axis, &f->footprint);
    const VistaVector normal = Unit(f->footprint);
    f->side = Unit(Cross(f->axis, normal));
    f->along = Unit(Cross(normal, f->side));
    const VistaVector water_delta = Add(f->footprint, Scale(f->source, -1));
    f->valid = f->valid && Dot(water_delta, water_delta) < 4 * f->length * f->length;
    const float fade =
        fminf(1, fmaxf(0, (sqrtf(Dot(water_delta, water_delta)) / fmaxf(.001f, f->length) - 1.30f) /
                              .70f));
    f->water_fade = 1 - fade * fade * (3 - 2 * fade);
    f->width = light->cone_slope * sqrtf(Dot(water_delta, water_delta));
    f->footprint_length = f->width / fmaxf(.15f, fabsf(Dot(f->axis, normal)));
  }
  for (int i = 0; i < count; i++) {
    const float seed = Random(i + 11), sideways = Random(i + 113), depth = Random(i + 311);
    const float life = water ? 1.4f + seed * 2.4f : 4 + seed * 5;
    const float age = (float)fmod((double)p->time + seed * 31, life) / life;
    const float envelope = sinf(age * kPi);
    const bool spray = water && i >= kGlints;
    const int light_id =
        water ? (i < 24 || spray ? 0 : 1 + (i - 24) / 6) : (i < 12 ? 0 : 1 + (i - 12) / 3);
    const CompletionVistaLight *light = &p->lights[light_id];
    const struct ParticleLight *f = &frames[light_id];
    float x, y, size, alpha;
    if (spray) {
      const bool platform = (p->detail_flags & kDeathHeimCompletion_Platform) && i < kGlints + 16;
      const float angle = sideways * 2 * kPi;
      x = platform
              ? p->waterline.x + cosf(angle) * (.115f + depth * .025f) * viewport.h / viewport.w
              : .05f + .9f * sideways + age * .035f;
      y = platform ? p->waterline.y + sinf(angle) * .015f - age * .035f
                   : .72f + depth * .25f - age * .045f;
      size = (2 + depth * 4) * scale;
      alpha = envelope * envelope * (platform ? .18f : .08f) * p->brightness;
    } else if (water) {
      const float angle = sideways * 2 * kPi, radius = sqrtf(depth) * .85f;
      VistaVector point =
          Add(f->footprint, Add(Scale(f->side, cosf(angle) * radius * f->width),
                                Scale(f->along, sinf(angle) * radius * f->footprint_length)));
      point = Scale(Unit(point), p->radius * .9975f);
      const ArRenderPointF uv = Project(p, point);
      x = uv.x;
      y = uv.y;
      size = (.65f + seed * 1.5f) * scale;
      alpha = f->valid ? envelope * envelope * (.22f + .45f * seed) * p->brightness *
                             light->strength * f->water_fade
                       : 0;
    } else {
      const float axial = (.12f + (1 - age) * 1.08f) * f->length;
      const float radius = sqrtf(depth) * axial * light->cone_slope * .7f,
                  angle = sideways * 2 * kPi;
      const VistaVector point =
          Add(f->source, Add(Scale(f->axis, axial), Add(Scale(f->mote_side, cosf(angle) * radius),
                                                        Scale(f->mote_up, sinf(angle) * radius))));
      const ArRenderPointF uv = Project(p, point);
      x = uv.x;
      y = uv.y;
      size = (1.4f + depth * 2.2f) * scale;
      alpha = sqrtf(Dot(point, point)) > p->radius * .9975f
                  ? envelope * envelope * .16f * p->brightness * light->strength
                  : 0;
    }
    const float cx = viewport.x + x * viewport.w, cy = viewport.y + y * viewport.h;
    const float u0 = water && !spray ? 0 : .5f, u1 = water && !spray ? .5f : 1;
    const ArRenderColorF color =
        spray ? (ArRenderColorF){.64f, .83f, .87f, alpha} : (ArRenderColorF){.97f, .93f, 1, alpha};
    const ArRenderVertex2D quad[4] = {
        {{cx - size, cy - size}, color, {u0, 0}},
        {{cx + size, cy - size}, color, {u1, 0}},
        {{cx + size, cy + size}, color, {u1, 1}},
        {{cx - size, cy + size}, color, {u0, 1}},
    };
    memcpy(vertices + i * 4, quad, sizeof(quad));
    const int32_t faces[6] = {i * 4, i * 4 + 1, i * 4 + 2, i * 4, i * 4 + 2, i * 4 + 3};
    memcpy(indices + i * 6, faces, sizeof(faces));
  }
  const ArRenderDrawState draw = {.flags = kArRenderDrawState_Blend,
                                  .blend = kArRenderBlendMode_Add};
  const bool drawn = ArRenderDevice_DrawGeometryWithState(device, s_sparkles, vertices, count * 4,
                                                          indices, count * 6, &draw);
  if (drawn) Sim3DPerformance_AddDraw(count * 4, count * 6);
  return drawn;
}

PresentationOutcome DrawCompletionVista(const FrameSlot *slot, ArRenderRectI viewport,
                                        const WorldNavigationProjection *projection,
                                        CompletionVistaStage stage, ArRenderPointF player,
                                        DeathHeimCompletionArt art, uint64_t elapsed_ms) {
  if (!slot->sim.death_heim_completion_world ||
      !CompletionVistaBackend_IsAvailable(&g_render_device))
    return kPresentationOutcome_OptionalOmitted;
  CompletionVistaParams p = {.radius = projection->globe_radius_world,
                             .time = (float)((double)elapsed_ms * .001),
                             .stage = stage,
                             .horizon = PresentWorldNavSky_Horizon(
                                 (ArRenderRectI){0, 0, viewport.w, viewport.h}, projection),
                             .player = player,
                             .brightness = slot->sim.world_navigation_brightness / 15.0f,
                             .width = viewport.w,
                             .height = viewport.h};
  p.detail_flags = slot->sim.death_heim_completion_flags;
  p.native_art = art.texture;
  p.waterline = ArRenderTexture_IsValid(art.texture) ? art.waterline
                                                     : (ArRenderPointF){player.x, player.y + .24f};
  if ((stage == kCompletionVistaStage_Reflection || stage == kCompletionVistaStage_Rim) &&
      !ArRenderTexture_IsValid(p.native_art))
    return kPresentationOutcome_Complete;
  /* One world-space source/cone drives scattering, water and particles. */
  p.sun = (ArRenderPointF){.5f, .035f};
  p.waves = (p.detail_flags & kDeathHeimCompletion_Waves) != 0;
  p.pixel_water = (p.detail_flags & kDeathHeimCompletion_PixelWater) != 0;
  if (stage == kCompletionVistaStage_Ocean && p.pixel_water && EnsureWaterPattern(&g_render_device))
    p.water_pattern = s_water_pattern;
  float right = 0, up = 0;
  for (int c = 0; c < 3; c++) {
    p.eye[c] = projection->camera_world[c];
    p.right[c] = projection->matrix[c * 4];
    p.up[c] = projection->matrix[c * 4 + 1];
    p.forward[c] = projection->matrix[c * 4 + 3];
    right += p.right[c] * p.right[c];
    up += p.up[c] * p.up[c];
  }
  if (right <= 0 || up <= 0) return kPresentationOutcome_CoreFailure;
  right = sqrtf(right);
  up = sqrtf(up);
  p.right_scale = 1 / right;
  p.up_scale = 1 / up;
  for (int c = 0; c < 3; c++) {
    p.right[c] /= right;
    p.up[c] /= up;
  }
  if (!PrepareLights(&p)) return kPresentationOutcome_CoreFailure;
  PresentationOutcome outcome = CompletionVistaBackend_Draw(&g_render_device, viewport, &p);
  if (outcome == kPresentationOutcome_Complete) Sim3DPerformance_AddDraw(4, 6);
  if (outcome == kPresentationOutcome_Complete && stage == kCompletionVistaStage_Sky &&
      (p.detail_flags & kDeathHeimCompletion_Cherubs))
    DrawCherubs(&g_render_device, viewport, p.time);
  if (outcome == kPresentationOutcome_Complete &&
      (stage == kCompletionVistaStage_Ocean || stage == kCompletionVistaStage_Shafts) &&
      !DrawParticles(&g_render_device, viewport, &p))
    outcome = kPresentationOutcome_OptionalOmitted;
  if (outcome == kPresentationOutcome_Complete && stage == kCompletionVistaStage_Ocean &&
      !DrawSunGlints(&g_render_device, viewport, &p))
    outcome = kPresentationOutcome_OptionalOmitted;
  if (outcome == kPresentationOutcome_Complete && stage == kCompletionVistaStage_Shafts &&
      !DrawFeathers(&g_render_device, viewport, &p))
    outcome = kPresentationOutcome_OptionalOmitted;
  return outcome;
}

void ResetCompletionVista(void) {
  CompletionVistaBackend_Reset(&g_render_device);
  ArRenderDevice_DestroyTexture(&g_render_device, s_sparkles);
  s_sparkles = ArRenderTexture_Invalid();
  ArRenderDevice_DestroyTexture(&g_render_device, s_cherubs);
  s_cherubs = ArRenderTexture_Invalid();
  s_cherub_revision = 0;
  ArRenderDevice_DestroyTexture(&g_render_device, s_water_pattern);
  s_water_pattern = ArRenderTexture_Invalid();
  s_water_pattern_revision = 0;
  ArRenderDevice_DestroyTexture(&g_render_device, s_feathers);
  s_feathers = ArRenderTexture_Invalid();
}
