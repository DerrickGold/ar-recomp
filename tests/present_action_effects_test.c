#include "action/present_action_effects.h"
#include "render/scenery_dimming.h"
#include "action/action_effect_projection.h"
#include "action/action_effect_render.h"
#include "action/action_effect_source.h"
#include "actraiser_game.h"
#include "app/session_fatal.h"
#include "diorama/diorama.h"
#include "present/present.h"
#include "render/effect_batch.h"

#include "support/test_assert.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Exercise the real presenter, geometry builders, upload mirror and device
 * scopes. Only backend operations and fatal reporting are replaced. */
typedef struct Backend {
  ArRenderTargetState state;
  ArRenderTextureDesc textures[64];
  int created, destroyed, updated, geometries, resolves, restores;
  bool fail_create, fail_geometry, fail_restore, fail_bind, fail_update;
  bool fail_viewport, fail_surface_light, check_mask_uv;
  ArRenderRectI update;
  ArRenderRectF mask_source, mask_destination;
  char draws[64];
  int draw_count;
  ArRenderBlendMode geometry_blends[64];
  ArRenderColorF geometry_colors[64];
  float geometry_peak_rgb[64];
  uint32_t geometry_uv_hash[64];
  ArRenderBlendMode composite_blend;
  uint32_t first_uploaded_pixel;
} Backend;

static int fatal_count;
void SessionFatal_Request(const char *format, ...) {
  assert(format && format[0]);
  fatal_count++;
}

static bool Create(void *ctx, const ArRenderTextureDesc *desc, ArRenderTexture *out) {
  Backend *b = ctx;
  if (b->fail_create) return false;
  assert(b->created + 1 < 64);
  b->textures[++b->created] = *desc;
  *out = (ArRenderTexture){(uintptr_t)b->created};
  return true;
}
static void Destroy(void *ctx, ArRenderTexture texture) {
  Backend *b = ctx;
  assert(texture.value && texture.value <= (uintptr_t)b->created);
  b->destroyed++;
}
static bool Update(void *ctx, ArRenderTexture texture, const ArRenderRectI *rect,
                   const void *pixels, int pitch) {
  Backend *b = ctx;
  assert(texture.value && pixels && rect && pitch > 0);
  memcpy(&b->first_uploaded_pixel, pixels, sizeof(uint32_t));
  b->updated++;
  b->update = *rect;
  return !b->fail_update;
}
static bool Target(void *ctx, ArRenderTexture target) {
  Backend *b = ctx;
  if (b->fail_bind) return false;
  b->state.target = target;
  return true;
}
static bool Coordinates(void *ctx) { (void)ctx; return true; }
static bool OutputSize(void *ctx, int *w, int *h) {
  Backend *b = ctx;
  *w = b->state.target.value ? b->textures[b->state.target.value].width : 800;
  *h = b->state.target.value ? b->textures[b->state.target.value].height : 600;
  return true;
}
static bool Viewport(void *ctx, const ArRenderRectI *rect) {
  Backend *b = ctx;
  if (b->fail_viewport) return false;
  b->state.viewport_set = rect != NULL;
  if (rect) b->state.viewport = *rect;
  return true;
}
static bool Clip(void *ctx, const ArRenderRectI *rect) {
  Backend *b = ctx;
  b->state.clip_enabled = rect != NULL;
  if (rect) b->state.clip = *rect;
  return true;
}
static bool Capture(void *ctx, ArRenderTargetState *state) {
  *state = ((Backend *)ctx)->state;
  state->valid = true;
  return true;
}
static bool Restore(void *ctx, const ArRenderTargetState *state) {
  Backend *b = ctx;
  b->restores++;
  if (b->fail_restore) return false;
  b->state = *state;
  return true;
}
static bool Clear(void *ctx, ArRenderColorF color) {
  (void)ctx;
  (void)color;
  return true;
}
static void Record(Backend *b, char draw) {
  assert(b->draw_count + 1 < (int)sizeof(b->draws));
  b->draws[b->draw_count++] = draw;
  b->draws[b->draw_count] = '\0';
}
static bool Texture(void *ctx, ArRenderTexture texture,
                    const ArRenderRectF *src, const ArRenderRectF *dst,
                    const ArRenderDrawState *state) {
  Backend *b = ctx;
  assert(texture.value && dst);
  if (state && (state->blend == kArRenderBlendMode_Multiply ||
                state->blend == kArRenderBlendMode_Modulate)) {
    assert(src);
    b->mask_source = *src;
    b->mask_destination = *dst;
    Record(b, 'M');
  } else if (state && state->blend == kArRenderBlendMode_DestinationAlphaMask) {
    Record(b, 'A');
  } else {
    if (state) b->composite_blend = state->blend;
    b->resolves++;
    Record(b, 'T');
  }
  return true;
}
static bool Geometry(void *ctx, ArRenderTexture texture,
                     const ArRenderVertex2D *vertices, int vertex_count,
                     const int32_t *indices, int index_count,
                     const ArRenderDrawState *state) {
  Backend *b = ctx;
  assert(vertices && vertex_count > 0 && indices && index_count > 0);
  if (b->check_mask_uv) {
    assert(texture.value && b->textures[texture.value].usage == kArRenderTextureUsage_Streaming);
    for (int i = 0; i < vertex_count; i++) {
      /* The visible 224x224 crop starts at native X=16, at output (20,30). */
      assert(fabsf(vertices[i].tex_coord.x * kFrameSlotLayerTextureWidth -
          (16 + (vertices[i].position.x - 20) * .35f)) < .001f);
      assert(fabsf(vertices[i].tex_coord.y * kFrameSlotLayerTextureHeight -
          (vertices[i].position.y - 30) / 2) < .001f);
    }
  }
  assert(b->geometries < 64);
  b->geometry_blends[b->geometries] = state ? state->blend : kArRenderBlendMode_Opaque;
  b->geometry_colors[b->geometries] = vertices[0].color;
  float peak = 0;
  for (int i = 0; i < vertex_count; i++) {
    peak = fmaxf(peak, vertices[i].color.r);
    peak = fmaxf(peak, vertices[i].color.g);
    peak = fmaxf(peak, vertices[i].color.b);
  }
  b->geometry_peak_rgb[b->geometries] = peak;
  uint32_t uv_hash = 2166136261u;
  for (int i = 0; i < vertex_count; ++i) {
    uint32_t words[2];
    memcpy(words, &vertices[i].tex_coord, sizeof(words));
    uv_hash = ((uv_hash ^ words[0]) * 16777619u ^ words[1]) * 16777619u;
  }
  b->geometry_uv_hash[b->geometries] = uv_hash;
  b->geometries++;
  Record(b, texture.value ? 'H' : 'G');
  return !b->fail_geometry && !(b->fail_surface_light && state &&
      state->blend == kArRenderBlendMode_Light);
}
static bool Present(void *ctx) { (void)ctx; return true; }
static const char *Error(void *ctx) { (void)ctx; return "injected backend failure"; }
static const ArRenderBackendOps kOps = {
  .struct_size = sizeof(ArRenderBackendOps),
  .create_texture = Create, .destroy_texture = Destroy, .update_texture = Update,
  .set_render_target = Target, .use_output_coordinates = Coordinates,
  .get_output_size = OutputSize, .set_viewport = Viewport, .set_clip_rect = Clip,
  .capture_render_target_state = Capture, .restore_render_target_state = Restore,
  .clear = Clear, .draw_texture = Texture, .draw_geometry = Geometry,
  .present = Present, .last_error = Error,
};

static void Init(Backend *b, ArRenderDevice *device) {
  *b = (Backend){0};
  b->state.valid = true;
  b->state.viewport_set = b->state.clip_enabled = true;
  b->state.viewport = (ArRenderRectI){3, 4, 700, 500};
  b->state.clip = (ArRenderRectI){7, 8, 50, 60};
  assert(ArRenderDevice_Init(device, &kOps, b, (ArRenderCapabilities){
    .flags = kArRenderCapability_StreamingTextures |
             kArRenderCapability_RenderTargets |
             kArRenderCapability_ScopedRenderTargets |
             kArRenderCapability_Geometry,
  }));
  EffectRenderer_Reset();
  fatal_count = 0;
}

static FrameSlot frame;
static uint32_t pixels[256 * 224];
static const ArRenderRectI viewport = {20, 30, 640, 448};

static void LavaFrame(void) {
  frame = (FrameSlot){0};
  frame.inidisp = 15;
  frame.snes_width = frame.visible_width = 256;
  frame.snes_height = 224;
  frame.diorama_map_group = kActRaiserMapGroup_Aitos;
  frame.diorama_map_number = 4;
  frame.action_effect_lighting = frame.action_effect_particles = true;
  frame.action_environmental_effects = true;
  frame.action_scene_effects.decoration_count = 1;
  frame.action_scene_effects.decoration_visible_count = 1;
  frame.action_scene_effects.game_frame = 400;
  frame.action_scene_effects.decorations[0] = (ActionEffectInstance){
    .world_x = 128, .world_y = 180,
    .kind = kActionEffect_AitosLavaReservoir,
    .phase = kActionEffectPhase_AitosLavaReservoir,
    .flags = kActionEffectFlag_Visible,
    .render_layer = kActionEffectRenderLayer_Bg1HighPlane,
    .projection_plane = kActionEffectProjectionPlane_Bg1High,
    .geometry = {.kind = kActionEffectGeometry_Rect,
                 .data.rect = {-64, -4, 64, 4}},
  };
}

static void HeatLifecycle(void) {
  Backend b;
  ArRenderDevice device;
  Init(&b, &device);
  LavaFrame();
  frame.action_effect_lighting = frame.action_effect_particles = false;
  const ArRenderTargetState before = b.state;
  frame.action_environmental_effects = false;
  assert(!PresentActionHeat_Begin(&device, &frame, viewport));
  frame.action_environmental_effects = true;
  frame.diorama_active = true;
  assert(!PresentActionHeat_Begin(&device, &frame, viewport));
  frame.diorama_active = false;
  frame.action_scene_effects.decorations[0].world_x = 2000;
  assert(!PresentActionHeat_Begin(&device, &frame, viewport));
  frame.action_scene_effects.decorations[0].world_x = 128;
  assert(b.created == 0);
  assert(PresentActionHeat_Begin(&device, &frame, viewport));
  assert(!PresentActionHeat_Begin(&device, &frame, viewport));
  const ArRenderRectI local = PresentActionHeat_SceneViewport(viewport);
  assert(local.x == 0 && local.y == 0 && local.w == viewport.w && local.h == viewport.h);
  PresentActionHeat_Cancel(&device);
  assert(!memcmp(&b.state, &before, sizeof(before)));
  PresentActionHeat_Cancel(&device);
  assert(b.restores == 1);
  assert(PresentActionHeat_Begin(&device, &frame, viewport));
  PresentActionHeat_End(&device, &frame, viewport);
  assert(b.created == 1 && b.geometries == 1 && !b.state.target.value);
  assert(PresentActionHeat_Begin(&device, &frame, viewport));
  PresentActionHeat_End(&device, &frame, viewport);
  assert(b.created == 1 && b.geometries == 2); /* Retained draw reuses the target. */
  ArRenderRectI resized = viewport;
  resized.w -= 10;
  assert(PresentActionHeat_Begin(&device, &frame, resized));
  PresentActionHeat_Cancel(&device);
  assert(b.created == 2 && b.destroyed == 1);
  PresentActionEffects_Reset(&device);
  assert(b.destroyed == 2);
  assert(PresentActionHeat_Begin(&device, &frame, resized));
  PresentActionHeat_Cancel(&device);
  PresentActionEffects_Reset(&device);
  assert(b.created == 3 && b.destroyed == 3 && !fatal_count);
}

static void HeatEditsInvalidateRetainedMesh(void) {
  Backend b;
  ArRenderDevice device;
  Init(&b, &device);
  LavaFrame();
  frame.action_scene_effects.surface_fields_valid = 2;
  ActionSurfaceField *field = &frame.action_scene_effects.surface_fields[1];
  *field = *ActionSurfaceField_Bundled(1);
  field->Components[0] = 0; /* Refraction is independent of glow/sparks. */
  ActionSurfaceField_Prepare(field);
  assert(PresentActionHeat_Begin(&device, &frame, viewport));
  PresentActionHeat_End(&device, &frame, viewport);
  const uint32_t original = b.geometry_uv_hash[0];
  memset(field->HeatAmplitude, 0, sizeof(field->HeatAmplitude));
  ActionSurfaceField_Prepare(field);
  assert(PresentActionHeat_Begin(&device, &frame, viewport));
  PresentActionHeat_End(&device, &frame, viewport);
  assert(b.geometry_uv_hash[1] != original && b.created == 1);
  assert(PresentActionHeat_Begin(&device, &frame, viewport));
  PresentActionHeat_End(&device, &frame, viewport);
  assert(b.geometry_uv_hash[2] == b.geometry_uv_hash[1]);
  field->Heat[0] = 0;
  ActionSurfaceField_Prepare(field);
  assert(!PresentActionHeat_Begin(&device, &frame, viewport));
  PresentActionEffects_Reset(&device);
  assert(b.created == b.destroyed);
}

static void HeatFailures(void) {
  Backend b;
  ArRenderDevice device;
  Init(&b, &device);
  LavaFrame();
  b.fail_create = true;
  assert(!PresentActionHeat_Begin(&device, &frame, viewport));
  b.fail_create = false;
  assert(!PresentActionHeat_Begin(&device, &frame, viewport)); /* Disabled until reset. */
  PresentActionEffects_Reset(&device);
  b.fail_bind = true;
  assert(!PresentActionHeat_Begin(&device, &frame, viewport));
  assert(!b.state.target.value && !fatal_count);
  b.fail_bind = false;
  PresentActionEffects_Reset(&device);
  assert(PresentActionHeat_Begin(&device, &frame, viewport));
  b.fail_geometry = true;
  PresentActionHeat_End(&device, &frame, viewport);
  assert(b.resolves == 1 && !b.state.target.value && !fatal_count);
  assert(!strcmp(b.draws, "HT")); /* Preserve the world when refraction fails. */
  assert(!PresentActionHeat_Begin(&device, &frame, viewport));
  PresentActionEffects_Reset(&device);
  b.fail_geometry = false;
  assert(PresentActionHeat_Begin(&device, &frame, viewport));
  b.fail_restore = true;
  PresentActionHeat_Cancel(&device);
  assert(fatal_count == 1); /* Lost target state must reach session-fatal reporting. */
  const int restores = b.restores;
  PresentActionHeat_Cancel(&device);
  assert(b.restores == restores);
  b.fail_restore = false;
  b.state.target = ArRenderTexture_Invalid();
  PresentActionEffects_Reset(&device);

  /* Entry can bind the new target but fail to restore after a viewport error. */
  b.fail_viewport = b.fail_restore = true;
  assert(!PresentActionHeat_Begin(&device, &frame, viewport));
  assert(fatal_count == 2 && b.state.target.value);
  b.fail_viewport = b.fail_restore = false;
  b.state.target = ArRenderTexture_Invalid();
  PresentActionEffects_Reset(&device);
}

static void MasksAndPlaneComposition(void) {
  Backend b;
  ArRenderDevice device;
  Init(&b, &device);
  LavaFrame();
  memset(pixels, 0xff, sizeof(pixels));
  assert(PresentActionEffects_UploadMask(&device, SR_PPU_OVERLAY_BG1, &frame,
      (const uint8_t *)pixels, 256 * 4) == sizeof(pixels));
  assert(PresentActionEffects_UploadMask(&device, SR_PPU_OVERLAY_BG1, &frame,
      (const uint8_t *)pixels, 256 * 4) == 0);
  pixels[256 * 3 + 7] = 0;
  assert(PresentActionEffects_UploadMask(&device, SR_PPU_OVERLAY_BG1, &frame,
      (const uint8_t *)pixels, 256 * 4) == 4);
  assert(b.update.x == 7 && b.update.y == 3 && b.update.w == 1 && b.update.h == 1);
  b.fail_update = true;
  pixels[0] = 0;
  assert(PresentActionEffects_UploadMask(&device, SR_PPU_OVERLAY_BG1, &frame,
      (const uint8_t *)pixels, 256 * 4) == 0);
  b.fail_update = false;
  assert(PresentActionEffects_UploadMask(&device, SR_PPU_OVERLAY_BG1, &frame,
      (const uint8_t *)pixels, 256 * 4) == sizeof(pixels));
  frame.action_bg1_mask_valid = true;
  frame.visible_x0 = 8;
  frame.visible_width = 240;
  const ArRenderTargetState before = b.state;
  frame.action_environmental_effects = false;
  assert(PresentActionEffects_DrawFlatPlanes(&device, &frame, viewport));
  assert(b.geometries == 0 && b.created == 1);
  frame.action_environmental_effects = true;
  frame.action_effect_lighting = frame.action_effect_particles = false;
  assert(PresentActionEffects_DrawFlatPlanes(&device, &frame, viewport));
  assert(!strcmp(b.draws, "GMT")); /* Build geometry, mask it, then resolve. */
  assert(b.mask_source.x == 8 && b.mask_source.w == 240 && b.mask_source.h == 224);
  assert(!memcmp(&b.state, &before, sizeof(before)));
  assert(b.created == 2);
  assert(PresentActionEffects_DrawFlatPlanes(&device, &frame, viewport));
  assert(b.created == 2); /* Same-sized retained plane pass reuses its target. */
  PresentActionEffects_Reset(&device);
  assert(b.destroyed == 2);
  assert(PresentActionEffects_UploadMask(&device, SR_PPU_OVERLAY_BG1, &frame,
      (const uint8_t *)pixels, 256 * 4) == sizeof(pixels));
  PresentActionEffects_Reset(&device);
  assert(b.created == b.destroyed && !fatal_count);
}

static void PlaneTargetFailures(void) {
  for (int failure = 0; failure < 3; failure++) {
    Backend b;
    ArRenderDevice device;
    Init(&b, &device);
    LavaFrame();
    assert(PresentActionEffects_UploadMask(&device, SR_PPU_OVERLAY_BG1, &frame,
        (const uint8_t *)pixels, 256 * 4) == sizeof(pixels));
    frame.action_bg1_mask_valid = true;
    const ArRenderTargetState before = b.state;
    b.fail_bind = failure == 0;
    b.fail_restore = failure != 0;
    b.fail_viewport = failure == 1;
    const bool usable = PresentActionEffects_DrawFlatPlanes(&device, &frame, viewport);
    if (failure == 0) {
      /* Rejected entry leaves the caller intact; omit only the enhancement. */
      assert(usable && fatal_count == 0);
      assert(!memcmp(&b.state, &before, sizeof(before)));
    } else {
      /* Failed entry rollback and failed final restore both lose ownership. */
      assert(!usable && fatal_count == 1 && b.state.target.value);
    }
    assert(b.resolves == 0);
    assert(b.geometries == (failure == 2 ? 1 : 0));
    b.fail_restore = b.fail_viewport = false;
    b.state = before;
    PresentActionEffects_Reset(&device);
    assert(b.created == b.destroyed);
  }
}

static void EnvironmentalEffectsIndependence(void) {
  Backend b;
  ArRenderDevice device;
  Init(&b, &device);
  LavaFrame();
  frame.action_effect_lighting = frame.action_effect_particles = false;
  DioramaProjection projection = {
    .valid = true, .matrix = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},
    .aspect_x = 256.0f / 224.0f, .height_scale = 1,
    .texture_width = 256, .texture_height = 224,
    .output_width = 640, .output_height = 448,
    .bg1_high_plane = {.valid = true, .u1 = 1, .v1 = 1},
  };
  PresentActionPlaneEffectContext context = {&device, &frame, viewport};
  PresentActionEffects_DrawDioramaPlane(&context, kDioramaPlane_Bg1Hi, &projection);
  assert(b.geometries == 1);
  frame.action_environmental_effects = false;
  PresentActionEffects_DrawDioramaPlane(&context, kDioramaPlane_Bg1Hi, &projection);
  assert(b.geometries == 1);

  /* A world-overlay decoration also honors the environment switch, whereas
   * a captured enemy projectile continues to use the action-effect controls. */
  frame.action_scene_effects.decorations[0].render_layer = kActionEffectRenderLayer_WorldOverlay;
  frame.action_environmental_effects = true;
  PresentActionEffects_Draw(&device, &frame, viewport, NULL);
  assert(b.geometries == 2);
  frame.action_environmental_effects = false;
  frame.action_effect_lighting = true;
  PresentActionEffects_Draw(&device, &frame, viewport, NULL);
  assert(b.geometries == 2);
  frame.action_scene_effects.effect_count = frame.action_scene_effects.visible_count = 1;
  frame.action_scene_effects.effects[0] = (ActionEffectInstance){
    .world_x = 128, .world_y = 100,
    .kind = kActionEffect_EnemyFireball, .phase = kActionEffectPhase_EnemyFireballFlight,
    .flags = kActionEffectFlag_Visible, .render_layer = kActionEffectRenderLayer_WorldOverlay,
    .projection_plane = kActionEffectProjectionPlane_Obj,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-4, -4, 4, 4}},
  };
  PresentActionEffects_Draw(&device, &frame, viewport, NULL);
  assert(b.geometries == 3);
  PresentActionEffects_Reset(&device);
}

static void ForestFoliageComposition(void) {
  Backend b;
  ArRenderDevice device;
  Init(&b, &device);
  LavaFrame();
  frame.action_scene_effects.decoration_count = 2;
  frame.action_scene_effects.decoration_visible_count = 2;
  frame.visible_x0 = 16;
  frame.visible_width = 224;
  frame.bg1_camera_x = frame.bg2_camera_x = 800;
  frame.bg1_camera_y = frame.bg2_camera_y = 240;
  frame.ws_extra_top = 0; /* Flat native frame has no captured top rows. */
  frame.action_scene_effects.decorations[0] = (ActionEffectInstance){
    .world_x = 928, .world_y = 80,
    .generation = 0x46000000u, .pulse_generation = 0x66000000u, .phase_ticks = 512,
    .kind = kActionEffect_ForestCanopyLight, .phase = kActionEffectPhase_ForestCanopyLight,
    .flags = kActionEffectFlag_Visible,
    .render_layer = kActionEffectRenderLayer_Bg2Plane,
    .projection_plane = kActionEffectProjectionPlane_BetweenBackgrounds,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,544}},
  };
  frame.action_scene_effects.decorations[1] = frame.action_scene_effects.decorations[0];
  frame.action_scene_effects.decorations[1].kind = kActionEffect_ForestLeaves;
  frame.action_scene_effects.decorations[1].render_layer = kActionEffectRenderLayer_Bg2Alpha;
  memset(pixels, 0xff, sizeof(pixels));
  pixels[0] = 0xff000000u; /* Native non-winning pixel has opaque alpha. */
  assert(PresentActionEffects_UploadMask(&device, SR_PPU_OVERLAY_BG2, &frame,
      (const uint8_t *)pixels, 256 * 4) == sizeof(pixels));
  assert(b.first_uploaded_pixel == 0); /* Must not darken foreground terrain. */
  frame.action_bg2_mask_valid = true;
  const ArRenderTargetState before = b.state;
  device.capabilities.flags &= ~(kArRenderCapability_RenderTargets |
                                kArRenderCapability_ScopedRenderTargets);
  b.check_mask_uv = true;
  assert(PresentActionEffects_DrawFlatPlanes(&device, &frame, viewport));
  b.check_mask_uv = false;
  assert(!strcmp(b.draws, "HH"));
  assert(b.geometry_blends[0] == kArRenderBlendMode_Add);
  assert(b.geometry_blends[1] == kArRenderBlendMode_Alpha);
  assert(b.created == 1 && b.resolves == 0 && b.restores == 0); /* Mask only; no target. */
  assert(!memcmp(&b.state, &before, sizeof(before)));
  pixels[0] = 0xffffffffu;
  b.fail_update = true;
  assert(!PresentActionEffects_UploadMask(&device, SR_PPU_OVERLAY_BG2, &frame,
      (const uint8_t *)pixels, 256 * 4));
  assert(PresentActionEffects_DrawFlatPlanes(&device, &frame, viewport));
  assert(b.geometries == 2); /* Old foreground occlusion must not leak through. */
  b.fail_update = false;
  assert(PresentActionEffects_UploadMask(&device, SR_PPU_OVERLAY_BG2, &frame,
      (const uint8_t *)pixels, 256 * 4));
  assert(!PresentActionEffects_UploadMask(&device, SR_PPU_OVERLAY_BG2, &frame,
      (const uint8_t *)pixels, 256 * 4)); /* Successful unchanged upload. */
  assert(PresentActionEffects_DrawFlatPlanes(&device, &frame, viewport));
  assert(b.geometries == 4); /* Recovery and retained-mask readiness. */
  frame.ws_extra_top = 160; /* Separate Diorama projection fixture. */
  DioramaProjection projection = {
    .valid = true, .matrix = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},
    .aspect_x = 1, .height_scale = 1, .texture_x_origin = 384,
    .texture_width = 1024, .texture_height = 768,
    .output_width = 640, .output_height = 448,
    .bg2_plane = {.valid = true, .u1 = 1, .v1 = 1},
  };
  const PresentActionPlaneEffectContext context = {&device, &frame, viewport};
  PresentActionEffects_DrawDioramaPlane((void *)&context, SR_PPU_OVERLAY_BG2, &projection);
  assert(b.geometries == 6);
  assert(b.geometry_blends[4] == kArRenderBlendMode_Add);
  assert(b.geometry_blends[5] == kArRenderBlendMode_Alpha);
  DioramaProjection sky = projection;
  sky.bg2_plane.valid = false;
  sky.bg2_skybox = (DioramaSkyboxProjection){.count = 1, .active_band = 0,
      .bands = {{0,0,256,544,0,1}}};
  PresentActionEffects_DrawDioramaPlane((void *)&context, SR_PPU_OVERLAY_BG2, &sky);
  assert(b.geometries == 8);
  assert(b.geometry_blends[6] == kArRenderBlendMode_Add &&
         b.geometry_blends[7] == kArRenderBlendMode_Alpha);
  assert(b.created == 1 && !b.resolves && !b.restores);
  b.geometries = 6;
  frame.action_scene_effects.decorations[2] = frame.action_scene_effects.decorations[0];
  frame.action_scene_effects.decorations[2].kind = kActionEffect_ForestForwardLight;
  frame.action_scene_effects.decorations[2].render_layer = kActionEffectRenderLayer_ForegroundLight;
  frame.action_scene_effects.decoration_count = 3;
  frame.action_scene_effects.decoration_visible_count = 3;
  PresentActionEffects_Draw(&device, &frame, viewport, &projection);
  assert(b.geometries == 7 && b.geometry_blends[6] == kArRenderBlendMode_Light);
  b.fail_surface_light = true;
  PresentActionEffects_Draw(&device, &frame, viewport, &projection);
  assert(b.geometries == 8 && EffectRenderer_Available());
  PresentActionEffects_Draw(&device, &frame, viewport, &projection);
  assert(b.geometries == 8); /* Unsupported custom blend is tried only once. */
  PresentActionEffects_DrawDioramaPlane((void *)&context, SR_PPU_OVERLAY_BG2, &projection);
  assert(b.geometries == 10); /* Ordinary alpha/additive effects still work. */
  PresentActionEffects_Reset(&device);
  b.fail_surface_light = false;
  PresentActionEffects_Draw(&device, &frame, viewport, &projection);
  assert(b.geometries == 11); /* A new device may support the blend. */
  frame.action_environmental_effects = false;
  PresentActionEffects_Draw(&device, &frame, viewport, &projection);
  PresentActionEffects_DrawDioramaPlane((void *)&context, SR_PPU_OVERLAY_BG2, &projection);
  assert(PresentActionEffects_DrawFlatPlanes(&device, &frame, viewport));
  assert(b.geometries == 11);
  PresentActionEffects_Reset(&device);
  assert(b.created == b.destroyed && !fatal_count);
}

static void CaveWaterComposition(void) {
  Backend b;
  ArRenderDevice device;
  Init(&b, &device);
  LavaFrame();
  frame.action_effect_lighting = frame.action_effect_particles = false;
  frame.bg1_camera_x = frame.bg2_camera_x = 608;
  frame.bg1_camera_y = frame.bg2_camera_y = 752;
  frame.action_scene_effects.decorations[0] = (ActionEffectInstance){
    .world_x = 736, .world_y = 592, .phase_ticks = 32,
    .kind = kActionEffect_CaveWater, .phase = kActionEffectPhase_CaveEnvironment,
    .visual = 2, .flags = kActionEffectFlag_Visible,
    .render_layer = kActionEffectRenderLayer_Bg2HighPlane,
    .projection_plane = kActionEffectProjectionPlane_Bg2High,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,544}},
  };
  memset(pixels, 0xff, sizeof(pixels));
  pixels[0] = 0xff000000u;
  assert(PresentActionEffects_UploadMask(&device, SR_PPU_OVERLAY_BG2, &frame,
      (const uint8_t *)pixels, 256 * 4) == sizeof(pixels));
  assert(b.first_uploaded_pixel == 0);
  frame.action_bg2_mask_valid = true;
  device.capabilities.flags &= ~(kArRenderCapability_RenderTargets |
                                kArRenderCapability_ScopedRenderTargets);
  assert(PresentActionEffects_DrawFlatPlanes(&device, &frame, viewport));
  assert(b.geometries == 1 && b.geometry_blends[0] == kArRenderBlendMode_Add);
  assert(b.created == 1 && b.resolves == 0 && b.restores == 0);
  DioramaProjection projection = {
    .valid = true, .matrix = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},
    .aspect_x = 1, .height_scale = 1, .texture_x_origin = 384,
    .texture_width = 1024, .texture_height = 768,
    .output_width = 640, .output_height = 448,
    .bg2_plane = {.valid = true, .u1 = 1, .v1 = 1},
    .bg2_high_plane = {.valid = true, .u1 = 1, .v1 = 1, .z_world = .2f},
  };
  PresentActionPlaneEffectContext context = {&device, &frame, viewport};
  PresentActionEffects_DrawDioramaPlane(&context, SR_PPU_OVERLAY_BG2, &projection);
  assert(b.geometries == 1); /* The low plane must not consume priority-1 water. */
  PresentActionEffects_DrawDioramaPlane(&context, kDioramaPlane_Bg2Hi, &projection);
  assert(b.geometries == 2 && b.geometry_blends[1] == kArRenderBlendMode_Add);
  assert(ActionEffectProjection_RequiredBgPlaneMask(NULL, &frame.action_scene_effects) ==
         (1u << kDioramaPlane_Bg2Hi));
  frame.action_environmental_effects = false;
  assert(PresentActionEffects_DrawFlatPlanes(&device, &frame, viewport));
  PresentActionEffects_DrawDioramaPlane(&context, kDioramaPlane_Bg2Hi, &projection);
  assert(b.geometries == 2);
  PresentActionEffects_Reset(&device);
  assert(b.created == b.destroyed && !fatal_count);
}

static void LandingDustComposition(void) {
  Backend b;
  ArRenderDevice device;
  Init(&b, &device);
  LavaFrame();
  frame.action_effect_lighting = frame.action_effect_particles = false;
  frame.action_scene_effects.decorations[0] = (ActionEffectInstance){
    .world_x = 128, .world_y = 192, .phase_ticks = 12,
    .kind = kActionEffect_LandingDust, .phase = kActionEffectPhase_CaveEnvironment,
    .visual = 2, .flags = kActionEffectFlag_Visible,
    .render_layer = kActionEffectRenderLayer_WorldDust,
    .projection_plane = kActionEffectProjectionPlane_Bg1,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-64,-40,64,2}},
  };
  device.capabilities.flags &= ~(kArRenderCapability_RenderTargets |
                                kArRenderCapability_ScopedRenderTargets);
  PresentActionEffects_Draw(&device, &frame, viewport, NULL);
  assert(b.geometries == 1 && b.geometry_blends[0] == kArRenderBlendMode_Alpha);
  assert(!b.created && !b.resolves);
  frame.action_environmental_effects = false;
  PresentActionEffects_Draw(&device, &frame, viewport, NULL);
  assert(b.geometries == 1);
  PresentActionEffects_Reset(&device);
}

static void SharedEffectSupport(void) {
  Backend b;
  ArRenderDevice device;
  Init(&b, &device);
  ArRenderVertex2D vertices[3] = {0};
  int32_t indices[] = {0, 1, 2};
  EffectBatch batch = {.vertices = vertices, .indices = indices,
                       .vertex_count = 3, .index_count = 3};
  assert(EffectRenderer_Submit(&device, &batch, kArRenderBlendMode_Add));
  b.fail_geometry = true;
  assert(!EffectRenderer_Submit(&device, &batch, kArRenderBlendMode_Add));
  assert(!EffectRenderer_Available());
  EffectRenderer_Reset();
  assert(EffectRenderer_Available());
  EffectRenderer_DisableBlend(&device, "test blend rejection");
  assert(!EffectRenderer_Available());
  EffectRenderer_Reset();
  batch.overflow = true;
  assert(!EffectRenderer_Submit(&device, &batch, kArRenderBlendMode_Add));
  assert(EffectRenderer_Available()); /* Local capacity failure isn't a device failure. */
}

static void CaveSheenComposition(void) {
  Backend b;
  ArRenderDevice device;
  Init(&b, &device);
  LavaFrame();
  frame.action_effect_lighting = frame.action_effect_particles = false;
  frame.bg1_camera_x = 128;
  frame.bg1_camera_y = 320;
  frame.action_bg1_mask_valid = true;
  frame.action_scene_effects.decorations[0] = (ActionEffectInstance){
    .world_x = 256, .world_y = 160, .phase_ticks = 80,
    .kind = kActionEffect_CaveSheen, .phase = kActionEffectPhase_CaveEnvironment,
    .source_mask = 0xFF,
    .visual = 2, .flags = kActionEffectFlag_Visible,
    .render_layer = kActionEffectRenderLayer_Bg1Plane,
    .projection_plane = kActionEffectProjectionPlane_Bg1,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,544}},
  };
  memset(pixels,0xff,sizeof(pixels));
  pixels[0] = 0xff000000u;
  assert(PresentActionEffects_UploadMask(&device,SR_PPU_OVERLAY_BG1,&frame,
      (const uint8_t *)pixels,256*4) == sizeof(pixels));
  assert(b.first_uploaded_pixel == 0);
  device.capabilities.flags &= ~(kArRenderCapability_RenderTargets |
                                kArRenderCapability_ScopedRenderTargets);
  assert(PresentActionEffects_DrawFlatPlanes(&device,&frame,viewport));
  assert(b.geometries == 1 && b.geometry_blends[0] == kArRenderBlendMode_Add);
  assert(b.created == 1 && !b.resolves && !b.restores);
  b.fail_update = true;
  pixels[1] = 0xff000000u;
  assert(!PresentActionEffects_UploadMask(&device,SR_PPU_OVERLAY_BG1,&frame,
      (const uint8_t *)pixels,256*4));
  assert(PresentActionEffects_DrawFlatPlanes(&device,&frame,viewport));
  assert(b.geometries == 1); /* Failed upload cannot reuse the previous occlusion. */
  PresentActionEffects_Reset(&device);
  assert(b.created == b.destroyed);
}

static void TempleSceneryDimming(void) {
  Backend b;
  ArRenderDevice device;
  Init(&b,&device);
  LavaFrame();
  frame.diorama_map_group = kActRaiserMapGroup_Fillmore;
  frame.diorama_map_number = 3;
  frame.action_effect_lighting = frame.action_effect_particles = false;
  frame.visible_x0 = 16;
  frame.visible_width = 224;
  frame.bg1_camera_y = 400; /* Above the floor mist, but still inside the temple. */
  frame.action_bg1_mask_valid = true;
  ActionEffectInstance *e = &frame.action_scene_effects.decorations[0];
  *e = (ActionEffectInstance){
    .kind = kActionEffect_CaveAmbientLight, .visual = 3,
    .phase = kActionEffectPhase_CaveEnvironment, .flags = kActionEffectFlag_Visible,
    .render_layer = kActionEffectRenderLayer_ForegroundLight,
    .projection_plane = kActionEffectProjectionPlane_Bg1,
  };
  assert(PresentActionEffects_Bg1Dimming(&frame) > .4f);
  frame.action_environmental_effects = false;
  assert(PresentActionEffects_Bg1Dimming(&frame) == 0);
  frame.action_environmental_effects = true;
  frame.diorama_map_number = 2;
  assert(PresentActionEffects_Bg1Dimming(&frame) == 0);
  e->visual = 2;
  const float cave_dim = PresentActionEffects_Bg1Dimming(&frame);
  const ArRenderRectF ramp = PresentActionEffects_Bg1DimmingRamp(&frame);
  assert(cave_dim == .45f);
  assert(SceneryDimming_Amount(cave_dim,ramp,800,1200) == 0); /* Rock half. */
  assert(SceneryDimming_Amount(cave_dim,ramp,1500,400) == 0); /* Upper openings. */
  assert(SceneryDimming_Amount(cave_dim,ramp,1500,1200) == cave_dim);
  assert(SceneryDimming_Amount(cave_dim,ramp,1500,864) == cave_dim*.5f);
  frame.diorama_map_number = 3;
  e->visual = 2; /* An inherited previous-room field must not dim the new scene. */
  assert(PresentActionEffects_Bg1Dimming(&frame) == 0);
  e->visual = 3;
  frame.action_scene_effects.decoration_overflow = true;
  assert(PresentActionEffects_Bg1Dimming(&frame) == 0);
  frame.action_scene_effects.decoration_overflow = false;
  memset(pixels,0xff,sizeof(pixels));
  pixels[0] = 0xff000000u; /* Non-BG1 winners, including actors, become transparent. */
  assert(PresentActionEffects_UploadMask(&device,SR_PPU_OVERLAY_BG1,&frame,
      (const uint8_t *)pixels,256*4) == sizeof(pixels));
  assert(b.first_uploaded_pixel == 0);
  device.capabilities.flags &= ~(kArRenderCapability_RenderTargets |
                                kArRenderCapability_ScopedRenderTargets);
  b.check_mask_uv = true;
  assert(PresentActionEffects_DrawFlatPlanes(&device,&frame,viewport));
  b.check_mask_uv = false;
  assert(b.geometries == 1 && b.geometry_blends[0] == kArRenderBlendMode_Alpha);
  const ArRenderColorF color = b.geometry_colors[0];
  assert(color.r == 0 && color.g == 0 && color.b == 0 && color.a > .4f && color.a < .5f);
  assert(b.created == 1 && !b.resolves && !b.restores);
  b.fail_update = true;
  pixels[1] = 0xff000000u;
  assert(!PresentActionEffects_UploadMask(&device,SR_PPU_OVERLAY_BG1,&frame,
      (const uint8_t *)pixels,256*4));
  assert(PresentActionEffects_DrawFlatPlanes(&device,&frame,viewport));
  assert(b.geometries == 1); /* A failed mask upload cannot darken actors with old pixels. */
  b.fail_update = false;
  assert(PresentActionEffects_UploadMask(&device,SR_PPU_OVERLAY_BG1,&frame,
      (const uint8_t *)pixels,256*4));
  frame.action_environmental_effects = false;
  assert(PresentActionEffects_DrawFlatPlanes(&device,&frame,viewport));
  assert(b.geometries == 1);
  PresentActionEffects_Reset(&device);
  assert(b.created == b.destroyed && !fatal_count);
}

static void TempleMistComposition(bool authored) {
  Backend b;
  ArRenderDevice device;
  Init(&b, &device);
  LavaFrame();
  frame.action_effect_lighting = frame.action_effect_particles = false;
  frame.bg1_camera_x = 500;
  frame.bg1_camera_y = 1500;
  frame.visible_x0 = 16;
  frame.visible_width = 224;
  frame.ws_extra_top = 0; /* Native flat capture, not a Diorama apron. */
  frame.action_bg1_mask_valid = true;
  frame.action_scene_effects.decorations[0] = (ActionEffectInstance){
    .world_x = 592, .world_y = 1680, .phase_ticks = 80,
    .kind = kActionEffect_TempleGroundMist, .phase = kActionEffectPhase_CaveEnvironment,
    .visual = 3, .flags = kActionEffectFlag_Visible,
    .render_layer = kActionEffectRenderLayer_Bg1Mist,
    .projection_plane = kActionEffectProjectionPlane_Bg1,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {0,-26,64,0}},
  };
  if (authored) {
    ActionEffectInstance e=frame.action_scene_effects.decorations[0];
    e.kind=kActionEffect_AuthoredFloorMist;e.particle_lifetime=240;
    e.tuning=(ActionEffectTuning){.active=1,.intensity=1,.color=0x91bedf};
    frame.action_scene_effects=(ActionSceneEffectFrame){.authored_count=1,.authored={e},
      .authored_floor={{.count=1,.spans={{592,656,1680,26}}}}};
  }
  memset(pixels,0xff,sizeof(pixels));
  pixels[0] = 0xff000000u; /* A winning actor excludes the BG1 mist. */
  assert(PresentActionEffects_UploadMask(&device,SR_PPU_OVERLAY_BG1,&frame,
      (const uint8_t *)pixels,256*4) == sizeof(pixels));
  assert(b.first_uploaded_pixel == 0);
  device.capabilities.flags &= ~(kArRenderCapability_RenderTargets |
                                kArRenderCapability_ScopedRenderTargets);
  b.check_mask_uv = true;
  assert(PresentActionEffects_DrawFlatPlanes(&device,&frame,viewport));
  b.check_mask_uv = false;
  assert(b.geometries == 1 && b.geometry_blends[0] == kArRenderBlendMode_Alpha);
  assert(!strcmp(b.draws,"H") && b.created == 1 && !b.resolves && !b.restores);
  PresentActionEffects_Draw(&device,&frame,viewport,NULL);
  assert(b.geometries == 1); /* No late world overlay over the actors. */
  b.fail_update = true;
  pixels[1] = 0xff000000u;
  assert(!PresentActionEffects_UploadMask(&device,SR_PPU_OVERLAY_BG1,&frame,
      (const uint8_t *)pixels,256*4));
  assert(PresentActionEffects_DrawFlatPlanes(&device,&frame,viewport));
  assert(b.geometries == 1); /* Stale occlusion must never be reused. */
  const DioramaProjection projection = {
    .valid = true, .matrix = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},
    .aspect_x = 1, .height_scale = 1, .texture_x_origin = 384,
    .texture_width = 1024, .texture_height = 768,
    .output_width = 640, .output_height = 448,
    .bg1_plane = {.valid = true, .u1 = 1, .v1 = 1},
    .bg2_plane = {.valid = true, .u1 = 1, .v1 = 1},
  };
  PresentActionPlaneEffectContext context = {&device,&frame,viewport};
  PresentActionEffects_DrawDioramaPlane(&context,SR_PPU_OVERLAY_BG2,&projection);
  assert(b.geometries == 1);
  PresentActionEffects_DrawDioramaPlane(&context,SR_PPU_OVERLAY_BG1,&projection);
  assert(b.geometries == 2 && b.geometry_blends[1] == kArRenderBlendMode_Alpha);
  frame.action_environmental_effects = false;
  PresentActionEffects_DrawDioramaPlane(&context,SR_PPU_OVERLAY_BG1,&projection);
  assert(b.geometries == 2);
  PresentActionEffects_Reset(&device);
  assert(b.created == b.destroyed);
}

static void CaveMaskUploadBudget(void) {
  enum { width = kFrameSlotLayerTextureWidth, height = kFrameSlotAuthenticHeight,
         stride = width*4 + 13 };
  static uint8_t source[stride*height+2], before[sizeof(source)];
  Backend b;
  ArRenderDevice device;
  Init(&b,&device);
  memset(&frame,0,sizeof(frame));
  frame.inidisp = 15;
  frame.action_environmental_effects = true;
  frame.snes_width = width;
  frame.action_scene_effects.decoration_count = 2;
  frame.action_scene_effects.decorations[0].kind = kActionEffect_CaveWater;
  frame.action_scene_effects.decorations[1].kind = kActionEffect_CaveSheen;
  const int planes[] = {SR_PPU_OVERLAY_BG1,SR_PPU_OVERLAY_BG2};
  const int heights[] = {height,height,height,height,height-64,height};
  for (unsigned plane = 0; plane < 2; plane++) {
    /* Exercise byte-aligned source pixels and a non-word-aligned row pitch. */
    memset(source,0xFF,sizeof(source));
    for (unsigned pass = 0; pass < 6; pass++) {
      if (pass == 2) {
        /* Scattered changes must not become many expensive SDL updates. */
        const uint32_t non_winner = 0xFF000000u;
        for (int y = 0; y < height; y += 5)
          for (int x = 0; x < width; x += 17)
            memcpy(source+1+y*stride+x*4,&non_winner,sizeof(non_winner));
      }
      memcpy(before,source,sizeof(source));
      frame.snes_height = heights[pass];
      const int updates_before = b.updated;
      const uint64_t uploaded = PresentActionEffects_UploadMask(
          &device,planes[plane],&frame,source+1,stride);
      const bool changed = pass != 1 && pass != 3;
      assert(b.updated-updates_before == (int)changed);
      assert((uploaded != 0) == changed);
      assert(!memcmp(source,before,sizeof(source))); /* Includes row padding/guards. */
      assert(b.created == (int)plane+1); /* Resizing reuses the existing texture. */
    }
  }
  assert(!b.resolves && !b.restores);
  PresentActionEffects_Reset(&device);
  assert(b.created == b.destroyed);
}

static void BloodpoolComposition(void) {
  Backend b;
  ArRenderDevice device;
  Init(&b,&device);
  memset(&frame,0,sizeof(frame));
  frame.inidisp = 15;
  frame.snes_width = frame.snes_height = frame.visible_width = 224;
  frame.action_environmental_effects = true;
  frame.action_bg1_mask_valid = frame.action_bg2_mask_valid = true;
  frame.bg1_camera_x = 384;
  frame.bg1_camera_y = 287;
  frame.action_scene_effects.decoration_count = 4;
  frame.action_scene_effects.decoration_visible_count = 4;
  for (unsigned i = 0; i < 2; i++)
    frame.action_scene_effects.decorations[i] = (ActionEffectInstance){
      .kind = i ? kActionEffect_BloodpoolMist : kActionEffect_BloodpoolWater,
      .phase = kActionEffectPhase_BloodpoolEnvironment, .visual = 1, .source_mask = 0xFF,
      .world_x = 512, .world_y = 480, .phase_ticks = 149, .flags = kActionEffectFlag_Visible,
      .render_layer = i ? kActionEffectRenderLayer_Bg2HighAlpha :
                               kActionEffectRenderLayer_Bg1HighPlane,
      .projection_plane = i ? kActionEffectProjectionPlane_Bg1 :
                                   kActionEffectProjectionPlane_Bg1High,
      .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,-48,384,32}},
    };
  frame.action_scene_effects.moonlight.valid = true;
  for (unsigned i = 0; i < 2; i++)
    frame.action_scene_effects.decorations[2+i] = (ActionEffectInstance){
      .kind = i ? kActionEffect_BloodpoolMoonReflection : kActionEffect_BloodpoolMoonlight,
      .phase = kActionEffectPhase_BloodpoolEnvironment, .visual = 1,
      .world_x = 112, .world_y = 62, .flags = kActionEffectFlag_Visible,
      .render_layer = kActionEffectRenderLayer_Bg2Plane,
      .projection_plane = kActionEffectProjectionPlane_Bg2,
      .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,194}},
    };
  memset(pixels,0xFF,sizeof(pixels));
  pixels[0] = 0xFF000000u;
  for (int plane = SR_PPU_OVERLAY_BG1; plane <= SR_PPU_OVERLAY_BG2; plane++) {
    assert(PresentActionEffects_UploadMask(&device,plane,&frame,(const uint8_t *)pixels,256*4));
    assert(b.first_uploaded_pixel == 0);
    assert(!PresentActionEffects_UploadMask(&device,plane,&frame,(const uint8_t *)pixels,256*4));
  }
  device.capabilities.flags &= ~(kArRenderCapability_RenderTargets |
                                kArRenderCapability_ScopedRenderTargets);
  assert(PresentActionEffects_DrawFlatPlanes(&device,&frame,viewport));
  assert(b.geometries == 3 && b.geometry_blends[0] == kArRenderBlendMode_Add &&
      b.geometry_blends[2] == kArRenderBlendMode_Alpha);
  assert(b.created == 2 && b.updated == 2 && !b.resolves && !b.restores);
  const DioramaProjection projection = {
    .valid = true, .matrix = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},
    .aspect_x = 1, .height_scale = 1, .texture_x_origin = 384,
    .texture_width = 1024, .texture_height = 768, .output_width = 640, .output_height = 448,
    .bg1_plane = {.valid = true, .u1 = 1, .v1 = 1},
    .bg1_high_plane = {.valid = true, .u1 = 1, .v1 = 1},
    .bg2_plane = {.valid = true, .u1 = 1, .v1 = 1},
    .bg2_high_plane = {.valid = true, .u1 = 1, .v1 = 1},
  };
  PresentActionPlaneEffectContext context = {&device,&frame,viewport};
  PresentActionEffects_DrawDioramaPlane(&context,SR_PPU_OVERLAY_BG2,&projection);
  assert(b.geometries == 4); /* Rays/reflection share BG2-low; subsequent timber occludes both. */
  PresentActionEffects_DrawDioramaPlane(&context,kDioramaPlane_Bg2Hi,&projection);
  assert(b.geometries == 5 && b.geometry_blends[4] == kArRenderBlendMode_Alpha);
  PresentActionEffects_DrawDioramaPlane(&context,kDioramaPlane_Bg1Hi,&projection);
  assert(b.geometries == 6 && b.geometry_blends[5] == kArRenderBlendMode_Add);
  DioramaProjection sky = projection;
  sky.bg2_plane.valid = false;
  sky.bg2_skybox = (DioramaSkyboxProjection){
    .count = 1, .active_band = 0, .bands = {{2,2,222,222,0,1}},
  };
  const int old_geometries = b.geometries;
  PresentActionEffects_DrawDioramaPlane(&context,SR_PPU_OVERLAY_BG2,&sky);
  assert(b.geometries == old_geometries+1);
  /* Moon-dependent foreground receivers retain the sky source mapping. */
  sky.bg2_skybox.active_band = -1;
  PresentActionEffects_DrawDioramaPlane(&context,kDioramaPlane_Bg1Hi,&sky);
  assert(b.geometries == old_geometries+2);
  PresentActionEffects_DrawDioramaPlane(&context,kDioramaPlane_Bg2Hi,&sky);
  assert(b.geometries == old_geometries+3 &&
         b.geometry_blends[old_geometries+2] == kArRenderBlendMode_Alpha);
  b.geometries = old_geometries;
  frame.action_scene_effects.decoration_count = 7;
  frame.action_scene_effects.bloodpool = (ActionBloodpoolDetails){
    .valid = true, .timber_count = 1,
    .timber = {{.x0=520,.x1=536,.y=416,.drip_x=528,.drip_y=424,.landing_y=488,
                .water_landing=1}},
  };
  for (unsigned i = 0; i < 3; i++) {
    ActionEffectInstance *e = &frame.action_scene_effects.decorations[4+i];
    *e = frame.action_scene_effects.decorations[2];
    e->kind = (uint8_t)(kActionEffect_BloodpoolTimber+i);
    e->render_layer = i == 0 ? kActionEffectRenderLayer_Bg1Plane :
        i == 1 ? kActionEffectRenderLayer_Bg2HighAlpha : kActionEffectRenderLayer_Bg2Alpha;
    if (i == 2) e->geometry.data.rect = (ActionEffectLocalRect){-100,-40,100,44};
    else {
      e->world_x = 512;
      e->world_y = 0;
      e->source_mask = 0xFF;
      e->projection_plane = kActionEffectProjectionPlane_Bg1;
      e->geometry.data.rect = (ActionEffectLocalRect){-384,0,384,512};
    }
  }
  sky.bg2_skybox.active_band = 0;
  PresentActionEffects_DrawDioramaPlane(&context,SR_PPU_OVERLAY_BG2,&sky);
  assert(b.geometries == old_geometries+2);
  assert(b.geometry_blends[old_geometries] == kArRenderBlendMode_Add);
  assert(b.geometry_blends[old_geometries+1] == kArRenderBlendMode_Alpha);
  assert(b.created == 2 && b.updated == 2 && !b.resolves && !b.restores);
  b.geometries = old_geometries;
  assert(PresentActionEffects_DrawFlatPlanes(&device,&frame,viewport));
  assert(b.geometries == 11); /* Five submissions; details share existing water/mist batches. */
  assert(b.created == 2 && b.updated == 2 && !b.resolves && !b.restores);
  frame.action_environmental_effects = false;
  assert(PresentActionEffects_DrawFlatPlanes(&device,&frame,viewport));
  PresentActionEffects_DrawDioramaPlane(&context,SR_PPU_OVERLAY_BG2,&projection);
  assert(b.geometries == 11);
  PresentActionEffects_Reset(&device);
  assert(b.created == b.destroyed);
}

static void CastleComposition(void) {
  Backend b;
  ArRenderDevice device;
  Init(&b,&device);
  memset(&frame,0,sizeof(frame));
  frame.inidisp = 15;
  frame.snes_width = frame.snes_height = frame.visible_width = 224;
  frame.action_environmental_effects = true;
  frame.action_bg1_mask_valid = frame.action_bg2_mask_valid = true;
  frame.diorama_map_group = kActRaiserMapGroup_Bloodpool;
  frame.diorama_map_number = 8;
  frame.action_scene_effects.decoration_count = 3;
  frame.action_scene_effects.decoration_visible_count = 3;
  for (unsigned i = 0; i < 3; i++) {
    ActionEffectInstance *e = &frame.action_scene_effects.decorations[i];
    *e = (ActionEffectInstance){
      .kind = (uint8_t)(kActionEffect_CastleLight+i),
      .phase = kActionEffectPhase_CastleEnvironment, .visual = 8, .source_mask = 1,
      .phase_ticks = 499, .flags = kActionEffectFlag_Visible,
      .render_layer = i == 0 ? kActionEffectRenderLayer_Bg1Plane :
          i == 1 ? kActionEffectRenderLayer_Bg2Plane : kActionEffectRenderLayer_Bg1Mist,
      .projection_plane = i == 1 ? kActionEffectProjectionPlane_Bg2 :
          kActionEffectProjectionPlane_Bg1,
      .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {0,0,256,256}},
    };
    if (i == 1) {
      e->world_x = 128;
      e->world_y = 48;
      e->geometry.data.rect = (ActionEffectLocalRect){-128,-48,128,208};
    } else if (i == 2) {
      e->world_x = 32;
      e->world_y = 224;
      e->geometry.data.rect = (ActionEffectLocalRect){0,-18,192,0};
    }
  }
  memset(pixels,0xFF,sizeof(pixels));
  pixels[0] = 0xFF000000u; /* Actor/other-layer winners must remain excluded. */
  for (int plane = SR_PPU_OVERLAY_BG1; plane <= SR_PPU_OVERLAY_BG2; plane++) {
    assert(PresentActionEffects_UploadMask(&device,plane,&frame,(const uint8_t *)pixels,256*4));
    assert(b.first_uploaded_pixel == 0);
    assert(!PresentActionEffects_UploadMask(&device,plane,&frame,(const uint8_t *)pixels,256*4));
  }
  device.capabilities.flags &= ~(kArRenderCapability_RenderTargets |
                                kArRenderCapability_ScopedRenderTargets);
  assert(PresentActionEffects_DrawFlatPlanes(&device,&frame,viewport));
  assert(b.geometries == 4 && b.created == 2 && b.updated == 2);
  assert(!b.resolves && !b.restores);
  frame.action_environmental_effects = false;
  assert(PresentActionEffects_DrawFlatPlanes(&device,&frame,viewport));
  assert(b.geometries == 4);
  /* The blue moat shares BG1's direct winner-masked path, including devices
   * without render targets. Camera scroll must keep its surface in view. */
  frame.action_environmental_effects = true;
  frame.diorama_map_number = 5;
  frame.bg1_camera_x = 592;
  frame.bg1_camera_y = 832;
  frame.action_scene_effects.decoration_count = 1;
  frame.action_scene_effects.decoration_visible_count = 1;
  frame.action_scene_effects.decorations[0] = (ActionEffectInstance){
    .kind = kActionEffect_CastleWater, .phase = kActionEffectPhase_CastleEnvironment,
    .visual = 5, .source_mask = 1, .world_x = 592, .world_y = 944, .phase_ticks = 321,
    .flags = kActionEffectFlag_Visible, .render_layer = kActionEffectRenderLayer_Bg1Plane,
    .projection_plane = kActionEffectProjectionPlane_Bg1,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {0,0,144,16}},
  };
  assert(PresentActionEffects_DrawFlatPlanes(&device,&frame,viewport));
  assert(b.geometries == 5 && b.created == 2 && !b.resolves && !b.restores);
  PresentActionEffects_Reset(&device);
  assert(b.created == b.destroyed);
}

static void TestCastleDimming(void) {
  memset(&frame,0,sizeof(frame));
  frame.inidisp = 15;
  frame.action_environmental_effects = true;
  frame.diorama_map_group = kActRaiserMapGroup_Bloodpool;
  frame.action_scene_effects.decoration_count = 1;
  ActionEffectInstance *e = &frame.action_scene_effects.decorations[0];
  *e = (ActionEffectInstance){.kind = kActionEffect_CastleLight,
    .phase = kActionEffectPhase_CastleEnvironment, .source_mask = 1,
    .render_layer = kActionEffectRenderLayer_Bg1Plane,
    .projection_plane = kActionEffectProjectionPlane_Bg1,
    .flags = kActionEffectFlag_Visible};
  for (unsigned room = 2; room <= 8; room++) {
    frame.diorama_map_number = (uint8_t)room;
    e->visual = (uint16_t)room;
    const float dim = PresentActionEffects_Bg1Dimming(&frame);
    assert((room == 2 || room == 6) ? dim == 0 : dim >= .30f && dim <= .42f);
  }
  frame.action_environmental_effects = false;
  assert(PresentActionEffects_Bg1Dimming(&frame) == 0);
  frame.action_environmental_effects = true;
  e->source_mask = 0;
  assert(PresentActionEffects_Bg1Dimming(&frame) == 0);
  e->source_mask = 1;
  e->visual = 3;
  assert(PresentActionEffects_Bg1Dimming(&frame) == 0);
}

static void TestEffectMasterBrightness(void) {
  Backend b;
  ArRenderDevice device;
  Init(&b, &device);
  LavaFrame();
  frame.action_bg1_mask_valid = true;
  memset(pixels, 0xff, sizeof(pixels));
  assert(PresentActionEffects_UploadMask(&device, SR_PPU_OVERLAY_BG1, &frame,
      (const uint8_t *)pixels, 256 * 4));
  const DioramaProjection projection = {
    .valid = true, .matrix = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},
    .aspect_x = 256.0f / 224.0f, .height_scale = 1,
    .texture_width = 256, .texture_height = 224,
    .output_width = 640, .output_height = 448,
    .bg1_high_plane = {.valid = true, .u1 = 1, .v1 = 1},
  };
  PresentActionPlaneEffectContext context = {&device, &frame, viewport};
  const uint8_t controls[] = {15, 7, 15, 0, 0x80, 0x8f};
  for (int path = 0; path < 5; path++) {
    LavaFrame();
    frame.action_bg1_mask_valid = true;
    float full_peak = 0;
    ArRenderColorF full_color = {0};
    frame.action_scene_effects.decorations[0].render_layer = path >= 2
        ? kActionEffectRenderLayer_WorldOverlay : kActionEffectRenderLayer_Bg1HighPlane;
    if (path == 3) {
      frame.action_scene_effects.decoration_count = 0;
      frame.action_scene_effects.decoration_visible_count = 0;
      frame.action_scene_effects.effect_count = frame.action_scene_effects.visible_count = 1;
      frame.action_scene_effects.effects[0] = (ActionEffectInstance){
        .world_x = 128, .world_y = 100,
        .kind = kActionEffect_EnemyFireball, .phase = kActionEffectPhase_EnemyFireballFlight,
        .flags = kActionEffectFlag_Visible, .render_layer = kActionEffectRenderLayer_WorldOverlay,
        .projection_plane = kActionEffectProjectionPlane_Obj,
        .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-4,-4,4,4}},
      };
    } else if (path == 4) {
      frame.action_scene_effects.decorations[0] = (ActionEffectInstance){
        .world_x = 128, .world_y = 192, .phase_ticks = 12,
        .kind = kActionEffect_LandingDust, .phase = kActionEffectPhase_CaveEnvironment,
        .visual = 2, .flags = kActionEffectFlag_Visible,
        .render_layer = kActionEffectRenderLayer_WorldDust,
        .projection_plane = kActionEffectProjectionPlane_Bg1,
        .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-64,-40,64,2}},
      };
    }
    for (unsigned i = 0; i < sizeof(controls); i++) {
      frame.inidisp = controls[i];
      const int before = b.geometries;
      if (path == 0)
        PresentActionEffects_DrawDioramaPlane(&context, kDioramaPlane_Bg1Hi, &projection);
      else if (path == 1)
        assert(PresentActionEffects_DrawFlatPlanes(&device, &frame, viewport));
      else
        PresentActionEffects_Draw(&device, &frame, viewport, NULL);
      if (i >= 3) {
        assert(b.geometries == before);
        assert(!PresentActionHeat_Begin(&device, &frame, viewport));
        continue;
      }
      assert(b.geometries == before + 1);
      if (!i) {
        full_peak = b.geometry_peak_rgb[before];
        full_color = b.geometry_colors[before];
        assert(full_peak > 0);
      }
      const float brightness = controls[i] / 15.0f;
      assert(fabsf(b.geometry_peak_rgb[before] - full_peak * brightness) < .00001f);
      assert(b.geometry_colors[before].a == full_color.a);
    }
  }
  PresentActionEffects_Reset(&device);
  assert(b.created == b.destroyed);
}

static void SkyboxWaterfallComposition(void) {
  Backend b;
  ArRenderDevice device;
  Init(&b, &device);
  memset(&frame, 0, sizeof(frame));
  frame.inidisp = 15;
  frame.action_environmental_effects = true;
  frame.ws_extra = 128;
  frame.ws_extra_top = 64;
  frame.action_scene_effects.decoration_count = 2;
  frame.action_scene_effects.decoration_visible_count = 2;
  frame.action_scene_effects.decorations[0] = (ActionEffectInstance){
    .world_x = 128, .world_y = 112,
    .kind = kActionEffect_AitosWaterfall,
    .phase = kActionEffectPhase_AitosWaterfallFlow,
    .phase_ticks = 96, .flags = kActionEffectFlag_Visible,
    .render_layer = kActionEffectRenderLayer_Bg2Plane,
    .projection_plane = kActionEffectProjectionPlane_Bg2,
    .geometry = {.kind = kActionEffectGeometry_Rect,
                 .data.rect = {-256,-176,256,312}},
  };
  frame.action_scene_effects.decorations[1] = frame.action_scene_effects.decorations[0];
  frame.action_scene_effects.decorations[1].kind = kActionEffect_AitosWaterfallMist;
  frame.action_scene_effects.decorations[1].render_layer = kActionEffectRenderLayer_Atmosphere;
  const DioramaProjection projection = {
    .valid = true, .output_width = 640, .output_height = 448,
    .bg2_skybox = {.count = 1, .active_band = 0,
                  .bands = {{0,0,512,352,0,1}}},
  };
  PresentActionPlaneEffectContext context = {&device, &frame, viewport};
  PresentActionEffects_DrawDioramaPlane(&context, SR_PPU_OVERLAY_BG2, &projection);
  assert(b.geometries == 1 && b.geometry_blends[0] == kArRenderBlendMode_Add);
  /* No finite-plane seam to hide, and no new textures or intermediate target. */
  assert(!b.created && !b.resolves && !b.restores);
  PresentActionEffects_Reset(&device);
}

static void AuthoredOnlyComposition(void) {
  Backend b;
  ArRenderDevice device;
  Init(&b, &device);
  LavaFrame();
  frame.action_effect_lighting = frame.action_effect_particles = false;
  frame.action_scene_effects = (ActionSceneEffectFrame){.authored_count = 1};
  ActionEffectInstance *e = &frame.action_scene_effects.authored[0];
  *e = (ActionEffectInstance){
    .world_x = 128, .world_y = 112, .kind = kActionEffect_AuthoredLight,
    .flags = kActionEffectFlag_Visible,
    .render_layer = kActionEffectRenderLayer_WorldOverlay,
    .projection_plane = kActionEffectProjectionPlane_Bg1,
    .tuning = {.intensity = 1, .color = 0xdde6ff, .active = 1},
    .particle_lifetime = 240,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-48,-32,48,32}},
  };
  PresentActionEffects_Draw(&device,&frame,viewport,NULL);
  assert(b.geometries == 1 && b.geometry_blends[0] == kArRenderBlendMode_Add);
  e->kind = kActionEffect_AuthoredMist;
  e->render_layer = kActionEffectRenderLayer_WorldDust;
  PresentActionEffects_Draw(&device,&frame,viewport,NULL);
  assert(b.geometries == 2 && b.geometry_blends[1] == kArRenderBlendMode_Alpha);
  /* An authored-only BG2 cloud must submit in plane and skybox-only modes,
   * with alpha-correct flat masking and no intermediate render target. */
  e->projection_plane=kActionEffectProjectionPlane_Bg2;
  e->render_layer=kActionEffectRenderLayer_Bg2Alpha;
  e->flags|=kActionEffectFlag_StaticAnchor;
  memset(pixels,0xff,sizeof(pixels));pixels[0]=0xff000000u;
  assert(PresentActionEffects_UploadMask(&device,SR_PPU_OVERLAY_BG2,&frame,
      (const uint8_t *)pixels,256*4)==sizeof(pixels));
  assert(b.first_uploaded_pixel==0);
  const unsigned resources=b.created;
  DioramaProjection projection={.valid=true,.output_width=800,.output_height=600,
    .matrix={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},.aspect_x=2,.height_scale=1,
    .texture_width=256,.texture_height=224,.bg2_plane={.valid=true,.u1=1,.v1=1}};
  PresentActionPlaneEffectContext context={&device,&frame,viewport};
  PresentActionEffects_DrawDioramaPlane(&context,SR_PPU_OVERLAY_BG2,&projection);
  assert(b.geometries==3&&b.geometry_blends[2]==kArRenderBlendMode_Alpha);
  projection.bg2_plane.valid=false;
  projection.bg2_skybox=(DioramaSkyboxProjection){.count=1,.active_band=0,.bands={{0,0,256,224,0,1}}};
  PresentActionEffects_DrawDioramaPlane(&context,SR_PPU_OVERLAY_BG2,&projection);
  assert(b.geometries==4&&b.geometry_blends[3]==kArRenderBlendMode_Alpha);
  assert(b.created==resources&&!b.resolves&&!b.restores);
  frame.action_environmental_effects = false;
  PresentActionEffects_Draw(&device,&frame,viewport,NULL);
  assert(b.geometries == 4 && b.created==resources);
  PresentActionEffects_Reset(&device);
}

static uint32_t source_hash;
static unsigned source_count, source_submissions;
static uint64_t source_revision;
static const DioramaProjection *source_projection;
static float source_brightness;

static bool InspectSourcePacket(ArRenderDevice *device, const ActionEffectSourceBatch *batch,
    const DioramaProjection *projection, const ActionMoonlightOcclusion *scenery,
    ArRenderBlendMode blend, float brightness) {
  (void)device; (void)scenery; (void)blend;
  assert(!batch->failed && batch->context.diorama_projection == projection);
  if (!batch->count) return true;
  source_projection = projection;
  source_brightness = brightness;
  source_count = batch->count;
  source_revision = batch->revision;
  source_hash = 2166136261u;
  const uint8_t *bytes = (const uint8_t *)batch->primitives;
  for (size_t i = 0; i < batch->count * sizeof(*batch->primitives); ++i)
    source_hash = (source_hash ^ bytes[i]) * 16777619u;
  ++source_submissions;
  return true;
}

static void SourcePacketCaptureLifetime(void) {
  Backend b;
  ArRenderDevice device;
  Init(&b, &device);
  LavaFrame();
  frame.action_effects.visible_count = 0;
  frame.action_scene_effects = (ActionSceneEffectFrame){.authored_count = 1};
  ActionEffectInstance *e = &frame.action_scene_effects.authored[0];
  *e = (ActionEffectInstance){
    .world_x = 128, .world_y = 112, .kind = kActionEffect_AuthoredLight,
    .flags = kActionEffectFlag_Visible,
    .render_layer = kActionEffectRenderLayer_WorldOverlay,
    .projection_plane = kActionEffectProjectionPlane_Bg1,
    .tuning = {.intensity = 1, .color = 0xdde6ff, .active = 1},
    .particle_lifetime = 240,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-48,-32,48,32}},
  };
  DioramaProjection a = {.valid = true}, next_band = {.valid = true};
  source_submissions = 0;
  PresentActionEffects_DrawWithSource(&device, &frame, viewport, &a, InspectSourcePacket);
  assert(source_submissions == 1 && source_count && source_projection == &a);
  const uint32_t original_hash = source_hash;
  /* Model an in-place editor edit: packets stay immutable until explicitly
   * invalidated, but the current band/projection and fade must never be cached. */
  e->world_x += 17;
  frame.inidisp = 7;
  PresentActionEffects_DrawWithSource(&device, &frame, viewport, &next_band, InspectSourcePacket);
  assert(source_submissions == 2 && source_hash == original_hash);
  assert(source_projection == &next_band && source_brightness == 7.0f / 15.0f);
  PresentActionEffects_InvalidateSourcePackets();
  PresentActionEffects_DrawWithSource(&device, &frame, viewport, &a, InspectSourcePacket);
  assert(source_submissions == 3 && source_hash != original_hash);
  e->world_x -= 17;
  ++frame.timestamp_ns;
  PresentActionEffects_DrawWithSource(&device, &frame, viewport, &a, InspectSourcePacket);
  assert(source_submissions == 4 && source_hash == original_hash);
  PresentActionEffects_Reset(&device);
  e->world_x += 23;
  PresentActionEffects_DrawWithSource(&device, &frame, viewport, &a, InspectSourcePacket);
  assert(source_submissions == 5 && source_hash != original_hash);
  PresentActionEffects_Reset(&device);
}

static void SourceParticleViewLifetime(void) {
  Backend b;
  ArRenderDevice device;
  Init(&b, &device);
  LavaFrame();
  frame.action_effects = (ActionEffectFrame){0};
  frame.action_scene_effects = (ActionSceneEffectFrame){.authored_count=1};
  frame.bg2_camera_x = frame.bg2_camera_y = 7000;
  frame.action_scene_effects.authored[0] = (ActionEffectInstance){
    .world_x=8000,.world_y=8000,.kind=kActionEffect_AuthoredParticleArea,
    .flags=kActionEffectFlag_Visible|kActionEffectFlag_StaticAnchor,
    .render_layer=kActionEffectRenderLayer_WorldOverlay,
    .projection_plane=kActionEffectProjectionPlane_Bg2,
    .tuning={.intensity=1,.color=0xdde6ff,.active=1},
    .particle_count=8,.particle_lifetime=240,.pulse_ticks=37,
    .particle_style={.active=1,.size_min=.35f,.size_max=.75f,.travel_y=32,.spread=1},
    .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={-8000,-8000,8000,8000}},
  };
  DioramaProjection projection={.valid=true,.bg2_skybox={.count=2,.active_band=0,
      .bands={{0,0,512,224,0,.5f},{0,224,512,448,.5f,1}}}};
  source_submissions=0;
  PresentActionEffects_InvalidateSourcePackets();
  PresentActionEffects_DrawWithSource(&device,&frame,viewport,&projection,InspectSourcePacket);
  assert(source_submissions==1 && source_count && source_count<512);
  uint64_t revision=source_revision;
  const uint32_t hash=source_hash;
  projection.bg2_skybox.active_band=1;
  PresentActionEffects_DrawWithSource(&device,&frame,viewport,&projection,InspectSourcePacket);
  assert(source_submissions==2 && source_revision==revision && source_hash==hash);
  /* Reprojection alone does not change which world cells are needed. */
  projection.output_width=1280;
  projection.output_height=800;
  projection.matrix[0]=.85f;
  projection.matrix[8]=.15f;
  PresentActionEffects_DrawWithSource(&device,&frame,viewport,&projection,InspectSourcePacket);
  assert(source_revision==revision && source_hash==hash);
  /* Resize/zoom while the capture is paused: new source cells must be emitted.
   * The ordinary light packet test above still reuses geometry across views. */
  projection.bg2_skybox.bands[0].x1=1024;
  projection.bg2_skybox.bands[1].x1=1024;
  PresentActionEffects_DrawWithSource(&device,&frame,viewport,&projection,InspectSourcePacket);
  assert(source_submissions==4 && source_revision!=revision && source_hash!=hash);
  revision=source_revision;
  PresentActionEffects_DrawWithSource(&device,&frame,viewport,&projection,InspectSourcePacket);
  assert(source_submissions==5 && source_revision==revision);
  PresentActionEffects_Reset(&device);
}

static void AutoExpandedMask(void) {
  Backend b;
  ArRenderDevice device;
  Init(&b,&device);
  LavaFrame();
  frame.visible_x0=8;
  frame.visible_width=240;
  frame.visible_height=256;
  frame.visible_top=16;
  frame.ws_extra_top=0; /* At the top of a finite room. */
  frame.ws_extra_bottom=16;
  frame.action_bg1_mask_valid=true;
  static uint32_t expanded[256*240];
  memset(expanded,0xff,sizeof(expanded));
  assert(PresentActionEffects_UploadMask(&device,SR_PPU_OVERLAY_BG1,&frame,
      (const uint8_t *)expanded,256*4)==sizeof(expanded));
  assert(b.update.h==240);
  assert(PresentActionEffects_DrawFlatPlanes(&device,&frame,viewport));
  assert(b.mask_source.h==240);
  assert(b.mask_destination.y==28 && b.mask_destination.h==420);
  /* Retained uploads must keep their immutable dimensions even after the
   * next host canvas was resized. Only a newly captured slot changes them. */
  assert(PresentActionEffects_UploadMask(&device,SR_PPU_OVERLAY_BG1,&frame,
      (const uint8_t *)expanded,256*4)==0);
  PresentActionEffects_Reset(&device);
}

static void DetachedFireballSmokeComposition(void) {
  Backend b;
  ArRenderDevice device;
  Init(&b, &device);
  LavaFrame();
  frame.action_effects = (ActionEffectFrame){0};
  frame.action_scene_effects = (ActionSceneEffectFrame){0};
  frame.action_environmental_effects = frame.action_effect_lighting = false;
  frame.action_effect_particles = true;
  frame.action_scene_effects.fireball_smoke.count = 1;
  frame.action_scene_effects.fireball_smoke.puffs[0] = (ActionFireballSmokePuff){
    .seed = 42, .x = 128, .y = 120, .age = 40, .priority = 2,
  };
  PresentActionEffects_Draw(&device, &frame, viewport, NULL);
  assert(b.geometries == 1 && b.geometry_blends[0] == kArRenderBlendMode_Alpha);
  frame.action_effect_particles = false;
  PresentActionEffects_Draw(&device, &frame, viewport, NULL);
  assert(b.geometries == 1);
  frame.action_effect_particles = true;
  const unsigned before = source_submissions;
  PresentActionEffects_InvalidateSourcePackets();
  PresentActionEffects_DrawWithSource(&device, &frame, viewport, NULL, InspectSourcePacket);
  assert(source_submissions == before + 1 && source_count > 0);
  PresentActionEffects_Reset(&device);
}

int main(void) {
  DetachedFireballSmokeComposition();
  AutoExpandedMask();
  SourcePacketCaptureLifetime();
  SourceParticleViewLifetime();
  AuthoredOnlyComposition();
  TestEffectMasterBrightness();
  SkyboxWaterfallComposition();
  TestCastleDimming();
  CastleComposition();
  BloodpoolComposition();
  TempleSceneryDimming();
  CaveMaskUploadBudget();
  CaveSheenComposition();
  TempleMistComposition(false);
  TempleMistComposition(true);
  HeatLifecycle();
  HeatEditsInvalidateRetainedMesh();
  HeatFailures();
  MasksAndPlaneComposition();
  PlaneTargetFailures();
  EnvironmentalEffectsIndependence();
  ForestFoliageComposition();
  CaveWaterComposition();
  LandingDustComposition();
  SharedEffectSupport();
  puts("action presentation: resource lifetime, masks, fallback and target restoration passed");
  return 0;
}
