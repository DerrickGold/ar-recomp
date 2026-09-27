#include "action/present_action_effects.h"
#include "action/action_effect_render.h"
#include "actraiser_game.h"
#include "app/session_fatal.h"
#include "present/present.h"
#include "render/effect_batch.h"

#include "support/test_assert.h"
#include <stdio.h>
#include <string.h>

/* Exercise the real presenter, geometry builders, upload mirror and device
 * scopes. Only backend operations and fatal reporting are replaced. */
typedef struct Backend {
  ArRenderTargetState state;
  ArRenderTextureDesc textures[64];
  int created, destroyed, updated, geometries, resolves, restores;
  bool fail_create, fail_geometry, fail_restore, fail_bind, fail_update;
  bool fail_viewport;
  ArRenderRectI update;
  ArRenderRectF mask_source;
  char draws[64];
  int draw_count;
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
  if (state && state->blend == kArRenderBlendMode_Multiply) {
    assert(src);
    b->mask_source = *src;
    Record(b, 'M');
  } else {
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
  (void)state;
  b->geometries++;
  Record(b, texture.value ? 'H' : 'G');
  return !b->fail_geometry;
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
  frame.snes_width = frame.visible_width = 256;
  frame.snes_height = 224;
  frame.diorama_map_group = kActRaiserMapGroup_Aitos;
  frame.diorama_map_number = 4;
  frame.action_effect_lighting = frame.action_effect_particles = true;
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
  const ArRenderTargetState before = b.state;
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

int main(void) {
  HeatLifecycle();
  HeatFailures();
  MasksAndPlaneComposition();
  PlaneTargetFailures();
  SharedEffectSupport();
  puts("action presentation: resource lifetime, masks, fallback and target restoration passed");
  return 0;
}
