/* Runs the actual compositor against a validating command recorder. The same
 * fixture is linked natively and as WASM; shader math/pixels are a separate gate. */
#include "diorama/diorama.h"
#include "diorama/diorama_effect_backend.h"
#include "diorama/diorama_performance.h"
#include "support/test_assert.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define TRACE_CAPACITY 524288
static float s_trace[TRACE_CAPACITY];
static unsigned s_count;
static DioramaLayerOrderTable s_layers;

typedef struct Recorder {
  ArRenderTargetState state;
  ArRenderTextureDesc textures[128];
  unsigned next, live, draws, creates, draws_at_failed_restore;
  int width, height;
  bool shaders, fail_draw, fail_restore;
  int bound_effect;
  const DioramaCapture *capture;
  const DioramaProjection *projection;
  unsigned checked_object_meshes;
  uint32_t skybox_inputs;
} Recorder;

static void Record(float value) {
  assert(isfinite(value) && s_count < TRACE_CAPACITY);
  s_trace[s_count++] = value;
}

static void Rectangle(const ArRenderRectF *r) {
  Record(r != NULL);
  if (!r) return;
  Record(r->x);
  Record(r->y);
  Record(r->w);
  Record(r->h);
}

static void IntRectangle(const ArRenderRectI *r) {
  const ArRenderRectF f = r ? (ArRenderRectF){r->x,r->y,r->w,r->h} : (ArRenderRectF){0};
  Rectangle(r ? &f : NULL);
}

static void Color(ArRenderColorF c) {
  Record(c.r);
  Record(c.g);
  Record(c.b);
  Record(c.a);
}

static void DrawState(const ArRenderDrawState *s) {
  Record(s != NULL);
  if (!s) return;
  Record(s->flags);
  Record(s->blend);
  Record(s->address_u);
  Record(s->address_v);
  Color(s->tint);
}

static bool Create(void *ctx, const ArRenderTextureDesc *desc, ArRenderTexture *texture) {
  Recorder *r = ctx;
  assert(++r->next < 128);
  r->textures[r->next] = *desc;
  r->live++;
  r->creates++;
  *texture = (ArRenderTexture){r->next};
  Record(1);
  Record(r->next);
  Record(desc->width);
  Record(desc->height);
  Record(desc->format);
  Record(desc->usage);
  Record(desc->filter);
  Record(desc->blend);
  return true;
}

static void Destroy(void *ctx, ArRenderTexture texture) {
  Recorder *r = ctx;
  assert(texture.value < 128 && r->textures[texture.value].width > 0);
  r->textures[texture.value].width = 0;
  r->live--;
  Record(2);
  Record(texture.value);
}

static bool Update(void *ctx, ArRenderTexture t, const ArRenderRectI *rect,
                   const void *pixels, int pitch) {
  (void)ctx;
  (void)t;
  (void)rect;
  (void)pixels;
  (void)pitch;
  assert(!"the compositor must not upload captured pixels");
  return false;
}

static bool Target(void *ctx, ArRenderTexture texture) {
  Recorder *r = ctx;
  r->state.target = texture;
  Record(3);
  Record(texture.value);
  return true;
}

static bool Coordinates(void *ctx) { (void)ctx; Record(4); return true; }
static bool Size(void *ctx, int *w, int *h) {
  Recorder *r = ctx;
  unsigned t = (unsigned)r->state.target.value;
  *w = t ? r->textures[t].width : r->width;
  *h = t ? r->textures[t].height : r->height;
  assert(*w > 0 && *h > 0);
  return true;
}

static bool Viewport(void *ctx, const ArRenderRectI *rect) {
  Recorder *r = ctx;
  r->state.viewport_set = rect != NULL;
  if (rect) r->state.viewport = *rect;
  Record(5);
  IntRectangle(rect);
  return true;
}

static bool Clip(void *ctx, const ArRenderRectI *rect) {
  Recorder *r = ctx;
  r->state.clip_enabled = rect != NULL;
  if (rect) r->state.clip = *rect;
  Record(6);
  IntRectangle(rect);
  return true;
}

static bool Capture(void *ctx, ArRenderTargetState *state) {
  *state = ((Recorder *)ctx)->state;
  state->valid = true;
  return true;
}

static bool Restore(void *ctx, const ArRenderTargetState *state) {
  Recorder *r = ctx;
  Record(7);
  Record(state->target.value);
  if (r->fail_restore) {
    r->draws_at_failed_restore = r->draws;
    return false;
  }
  r->state = *state;
  return true;
}

static bool Clear(void *ctx, ArRenderColorF c) {
  (void)ctx;
  Record(8);
  Color(c);
  return true;
}

static bool Texture(void *ctx, ArRenderTexture t, const ArRenderRectF *src,
                    const ArRenderRectF *dst, const ArRenderDrawState *state) {
  Recorder *r = ctx;
  assert(t.value < 128 && r->textures[t.value].width > 0);
  Record(9);
  Record(t.value);
  Rectangle(src);
  Rectangle(dst);
  DrawState(state);
  r->draws++;
  return !r->fail_draw;
}

static bool Geometry(void *ctx, ArRenderTexture t, const ArRenderVertex2D *v,
                     int nv, const int32_t *indices, int ni, const ArRenderDrawState *state) {
  Recorder *r = ctx;
  assert(!t.value || (t.value < 128 && r->textures[t.value].width > 0));
  if (r->capture && nv == 4 && ni == 6)
    for (int plane = 0; plane < kDioramaPlane_Count; plane++)
      if (t.value == r->capture->textures[plane].value)
        r->skybox_inputs |= 1u << plane;
  if (r->capture && r->projection && r->bound_effect == kDioramaEffect_RimLight) {
    for (unsigned priority=0;priority<4;++priority) {
      const int plane = DioramaPlaneForObjectPriority(priority);
      if (t.value != r->capture->textures[plane].value) continue;
      float min_u=INFINITY, max_u=-INFINITY;
      for (int i=0;i<nv;++i) {
        min_u=fminf(min_u,v[i].tex_coord.x);
        max_u=fmaxf(max_u,v[i].tex_coord.x);
        /* Enlarging the drawable mesh must not shift/stretch the sprites or
         * disagree with the published transform used by attached effects. */
        const DioramaPlaneProjection *p=&r->projection->object_planes[priority];
        const float x=v[i].tex_coord.x*SR_PPU_SURFACE_MAX_WIDTH-
            r->capture->obj_apron-p->capture_offset.x;
        const float y=v[i].tex_coord.y*SR_PPU_SURFACE_MAX_HEIGHT-p->capture_offset.y;
        ArRenderPointF expected;
        assert(Diorama_ProjectCapturedPoint(r->projection,x,y,priority,&expected,NULL,NULL));
        assert(fabsf(v[i].position.x+r->projection->output_x-expected.x)<.002f);
        assert(fabsf(v[i].position.y+r->projection->output_y-expected.y)<.002f);
      }
      assert(min_u == 0);
      assert(fabsf(max_u-(float)(r->capture->width+2*r->capture->obj_apron)/
          SR_PPU_SURFACE_MAX_WIDTH)<.00001f);
      ++r->checked_object_meshes;
    }
  }
  Record(10);
  Record(t.value);
  Record(nv);
  Record(ni);
  DrawState(state);
  for (int i = 0; i < nv; ++i) {
    Record(v[i].position.x);
    Record(v[i].position.y);
    Color(v[i].color);
    Record(v[i].tex_coord.x);
    Record(v[i].tex_coord.y);
  }
  for (int i = 0; i < ni; ++i) {
    assert(indices[i] >= 0 && indices[i] < nv);
    Record(indices[i]);
  }
  r->draws++;
  return !r->fail_draw;
}

static bool Present(void *ctx) { (void)ctx; assert(!"caller owns present"); return false; }
static const char *Error(void *ctx) { (void)ctx; return "injected recorder failure"; }
static const ArRenderBackendOps kOps = {
  sizeof(ArRenderBackendOps), Create, Destroy, Update, Target, Coordinates, Size,
  Viewport, Clip, Capture, Restore, Clear, Texture, Geometry, Present, Error,
};

/* Record semantic shader parameters; this backend does not claim pixel parity. */
bool DioramaEffectBackend_IsAvailable(ArRenderDevice *device, DioramaEffectKind effect) {
  (void)effect;
  return ((Recorder *)device->context)->shaders;
}
static bool Bind(ArRenderDevice *device, int effect) {
  Recorder *r = device->context;
  assert(r->shaders && r->bound_effect == -1);
  r->bound_effect = effect;
  Record(11);
  Record(effect);
  return true;
}
bool DioramaEffectBackend_BindBlur(ArRenderDevice *d, const DioramaBlurEffectParams *p) {
  Bind(d, kDioramaEffect_Blur);
  Record(p->texel_width);
  Record(p->texel_height);
  Record(p->radius);
  return true;
}
bool DioramaEffectBackend_BindRimLight(ArRenderDevice *d, const DioramaRimLightEffectParams *p) {
  Bind(d, kDioramaEffect_RimLight);
  Record(p->texel_width);
  Record(p->texel_height);
  Record(p->strength);
  return true;
}
bool DioramaEffectBackend_BindDofEdge(ArRenderDevice *d, const DioramaDofEdgeEffectParams *p) {
  Bind(d, kDioramaEffect_DofEdge);
  Record(p->texel_width);
  Record(p->texel_height);
  Record(p->blur_radius);
  Record(p->u_min);
  Record(p->u_max);
  Record(p->v_min);
  Record(p->v_max);
  Record(p->edge_feather);
  Record(p->lower_content_v_max);
  return true;
}
bool DioramaEffectBackend_BindPrioritySurface(
    ArRenderDevice *d, const DioramaPrioritySurfaceEffectParams *p) {
  Bind(d, kDioramaEffect_PrioritySurface);
  Record(p->width);
  Record(p->height);
  Record(p->high_band);
  Record(p->additive);
  return true;
}
bool DioramaEffectBackend_Unbind(ArRenderDevice *d) {
  ((Recorder *)d->context)->bound_effect = -1;
  Record(12);
  return true;
}
void DioramaEffectBackend_Reset(ArRenderDevice *d) {
  assert(((Recorder *)d->context)->bound_effect == -1);
}

/* Timings are deliberately excluded from deterministic command comparison. */
DioramaPerformanceScope DioramaPerformance_Begin(DioramaPerformanceStage stage) {
  return (DioramaPerformanceScope){.stage = stage};
}
void DioramaPerformance_End(DioramaPerformanceScope scope) { (void)scope; }
void DioramaPerformance_SetViewport(int w, int h) { (void)w; (void)h; }
void DioramaPerformance_SetRasterViewport(int w, int h) { (void)w; (void)h; }
void DioramaPerformance_SetPlane(int plane) { (void)plane; }
void DioramaPerformance_AddDraw(bool ok, const ArRenderVertex2D *v, int nv,
                               const int32_t *i, int ni, ArRenderBlendMode blend) {
  (void)ok;
  (void)v;
  (void)nv;
  (void)i;
  (void)ni;
  (void)blend;
}

static void PlaneEffect(void *data, int plane, const DioramaProjection *projection) {
  Recorder *r = data;
  assert(r->bound_effect == -1 && projection->valid);
  assert(!r->state.target.value && !r->state.viewport_set);
  Record(13);
  Record(plane);
}

static ArRenderTexture Skybox(void *data, ArRenderDevice *d, int source,
                             bool fill, uint32_t color, bool *failed) {
  (void)d;
  *failed = false;
  Record(14);
  Record(source);
  Record(fill);
  Record(color);
  return *(ArRenderTexture *)data;
}

unsigned DioramaFixture_Count(void) { return 120; }
const float *DioramaFixture_Trace(void) { return s_trace; }
unsigned DioramaFixture_Run(unsigned scenario) {
  assert(scenario < DioramaFixture_Count());
  s_count = 0;
  memset(&s_layers, 0, sizeof(s_layers));
  const int widths[] = {640,960,960}, heights[] = {480,540,600};
  const int extension[] = {0,32,64,128};
  const unsigned aspect = scenario % 3, ext = (scenario / 3) % 4;
  const bool death_heim = scenario >= 108;
  const bool completion = death_heim && ((scenario - 108) & 2u);
  const unsigned zoom = (scenario / 12) % 3;
  const unsigned sky = death_heim ? (scenario - 108) / 4 : scenario / 36;
  Recorder r = {.width = widths[aspect]+16, .height = heights[aspect]+20,
    .shaders = scenario % 2 != 0, .bound_effect = -1};
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device, &kOps, &r,
      (ArRenderCapabilities){.flags = UINT64_MAX,
        .maximum_texture_width = 8192, .maximum_texture_height = 8192,
        .maximum_render_target_width = 8192, .maximum_render_target_height = 8192}));
  ArRenderTexture textures[kDioramaPlane_Count];
  const uint8_t *pixels[kDioramaPlane_Count];
  DioramaCoverageMask masks[kDioramaPlane_Count];
  ArRenderPointF offsets[kDioramaPlane_Count];
  const uint8_t pixel = 255;
  for (int i = 0; i < kDioramaPlane_Count; ++i) {
    assert(Create(&r, &(ArRenderTextureDesc){.width = SR_PPU_SURFACE_MAX_WIDTH,
        .height = SR_PPU_SURFACE_MAX_HEIGHT, .format = kArRenderPixelFormat_Argb8888,
        .usage = kArRenderTextureUsage_Streaming}, &textures[i]));
    pixels[i] = &pixel; /* Presence only; captured uploads belong to the caller. */
    masks[i] = DioramaCoverage_Dilate(UINT64_C(1) << ((i * 3) % 48));
    offsets[i] = (ArRenderPointF){0.25f,-0.5f};
  }
  DioramaRenderOptions options = {
    .visible_planes = (1u << kDioramaPlane_Count)-1,
    .skybox = sky, .depth_shade = 0.65f, .hud_flat = scenario % 5 == 0,
    .shoebox = scenario % 7 == 0, .margin_fix = true,
    .shadow_blur = true, .rim_light = true, .depth_of_field = true, .edge_aa = true,
    .stack_grouping = true, .sparse_coverage = true,
    .skybox_prefilter = true, .priority_surface = true,
    .layers = &s_layers, .resolve_skybox = Skybox,
    .skybox_userdata = &textures[SR_PPU_OVERLAY_BG2],
  };
  if (scenario % 4 == 0) options.visible_planes &= ~(1u << kDioramaPlane_Bg1Far);
  s_layers.count = 1;
  DioramaRoomOverride *room = &s_layers.rooms[0];
  room->map_group = death_heim ? 7 : 1;
  room->map_number = death_heim ? 1 : 2;
  room->section = completion ? kDioramaLayerSection_DeathHeimCompletion :
      kDioramaLayerSection_Room;
  DioramaPlaneOverride *bg1 = &room->planes[SR_PPU_OVERLAY_BG1];
  bg1->set_rake = bg1->set_bow = true;
  bg1->rake = scenario % 2 ? 0 : 0.17f;
  bg1->bow = scenario % 2 ? 0 : -0.09f;
  room->planes[kDioramaPlane_Bg1Hi] = *bg1;
  room->planes[kDioramaPlane_Bg1Hi].set_z = true;
  room->planes[kDioramaPlane_Bg1Hi].z = 0.5f;
  DioramaPlaneOverride *bg2 = &room->planes[SR_PPU_OVERLAY_BG2];
  bg2->set_stack = bg2->set_stack_copies = true;
  bg2->stack = 0.12f;
  bg2->stack_copies = 3;
  if (!death_heim && scenario % 9 == 0) {
    room->planes[kDioramaPlane_Backdrop].set_source = true;
    room->planes[kDioramaPlane_Backdrop].source = kDioramaLayerSource_AitosSky;
  }
  DioramaCapture capture = {
    .width = ext == 3 ? 576 : (ext ? 384 : 256),
    .height = 224 + (ext == 3 ? extension[ext] : 2 * extension[ext]),
    .authentic_y0 = extension[ext], .obj_apron = ext == 3 ? 32 : 64,
    .textures = textures, .pixels = pixels, .coverage_masks = masks,
    .plane_capture_offsets = offsets, .camera_y = 320,
    .bg2_camera_y = 160, .bg2_world_height = 1024, .bg2_vertical_ratio = 0x12,
    .bg2_scroll_valid = true, .bg2_revision = 1,
    .vertical_bounds = {.valid = true, .plane = SR_PPU_OVERLAY_BG1,
      .bottom_reached = scenario % 6 == 0},
    .horizontal_bounds = {.valid = true, .plane = SR_PPU_OVERLAY_BG1,
      .left = 0, .right = 1, .left_reached = scenario % 5 == 0},
  };
  DioramaView view = {
    .camera = {.tilt_x = 0.1f, .tilt_y = -0.15f,
      .distance = scenario % 4 == 3 ? 4.5f : 0},
    .distance_scale = 0.8f + zoom * 0.2f,
    .camera_framing_weight = scenario % 3 * 0.5f,
    .pixel_aspect = scenario % 2, .ignore_aspect_ratio = scenario % 7 == 0,
    .visible_width = capture.width,
    .viewport = {8,10,widths[aspect],heights[aspect]},
  };
  const DioramaBgValidSpanPlan spans = {.count = 2, .spans = {
    {0, capture.height / 2, capture.obj_apron + 16, capture.obj_apron + 256},
    {capture.height / 2, capture.height, capture.obj_apron, capture.obj_apron + capture.width},
  }};
  const DioramaSkyboxView skybox = {
    .texture = textures[SR_PPU_OVERLAY_BG2], .revision = 1, .width = capture.width,
    .periodic = scenario % 8 == 0, .capture_offset = {0.25f,-0.5f},
  };
  if (scenario % 2) capture.bg2_valid_spans = &spans;
  if (!death_heim && scenario % 4 == 0) capture.skybox = &skybox;
  if (scenario % 5 == 0) capture.bg_apron_mask = 3;
  if (scenario % 6 == 0) capture.vertical_bounds.top_reached = true;
  DioramaScene scene = {.render = &options,
    .map_group = room->map_group, .map_number = room->map_number,
    .layer_section = room->section,
    .bg1_dimming = 0.23f, .bg1_dimming_ramp = {0.1f,0.2f,0.2f,0.1f},
    .effect_obj_priority_mask = 15, .effect_bg_plane_mask = 3,
    .plane_effect = PlaneEffect, .plane_effect_userdata = &r};
  DioramaProjection projection;
  r.capture = &capture;
  r.projection = &projection;
  const unsigned base_resources = r.live;
  for (int frame = 0; frame < 2; ++frame) {
    unsigned creates = r.creates;
    const PresentationOutcome outcome = Diorama_Composite(
        &device, &capture, &view, &scene, &projection);
    assert(PresentationOutcome_IsUsable(outcome));
    assert(projection.valid && r.draws && r.bound_effect == -1);
    if (death_heim) {
      assert(projection.bg2_plane.valid == (sky != kDioramaSky_Only || !completion));
      if (sky != kDioramaSky_Off && !completion) {
        assert(r.skybox_inputs & (1u << kDioramaPlane_Bg2Far));
        assert(projection.bg2_skybox.count == 1);
        assert(projection.bg2_skybox.motion_source == 4);
        /* The enlarged face image must end above the moving cloud plane.
         * Check the published transforms used by effect/light projection too. */
        DioramaProjection water = projection;
        water.bg2_skybox = (DioramaSkyboxProjection){0};
        ArRenderPointF cloud, face;
        const float bottom = capture.authentic_y0 + 9 * 16;
        assert(Diorama_ProjectCapturedBg2Point(&water, capture.width * 0.5f,
            bottom, &cloud, NULL, NULL));
        assert(Diorama_ProjectSkyboxAnchorPoint(&projection, bottom,
            capture.width * 0.5f, bottom, &face));
        const float clearance = projection.output_height * 8.0f / 224;
        if (cloud.y >= projection.output_y + clearance)
          assert(face.y <= cloud.y - clearance + 0.01f);
        if (sky == kDioramaSky_Only) {
          /* Lower the existing water, including its published effect mapping,
           * until its bottom covers the viewport at both projected sides. */
          for (int side = 0; side < 2; side++) {
            ArRenderPointF edge;
            assert(Diorama_ProjectCapturedBg2Point(&water,
                side * capture.width, capture.authentic_y0 + 224 - 4,
                &edge, NULL, NULL));
            assert(edge.y >= projection.output_y + projection.output_height - 0.01f);
          }
          assert(!water.bg2_plane.overflow_valid);
        }
      } else if (sky == kDioramaSky_Only) {
        assert(r.skybox_inputs & (1u << SR_PPU_OVERLAY_BG2));
        assert(projection.bg2_skybox.motion_source == 3);
      }
    }
    if (r.shaders) assert(r.checked_object_meshes >= (unsigned)(frame+1)*4);
    assert(!r.state.target.value && !r.state.viewport_set && !r.state.clip_enabled);
    assert(r.live <= base_resources + 7); /* Two SS, two DOF, priority, stack, skybox. */
    if (frame) assert(r.creates == creates); /* Retained intermediates, no churn. */
    Record(15);
    Record(outcome);
    for (int i = 0; i < 16; ++i) Record(projection.matrix[i]);
    Record(projection.aspect_x);
    Record(projection.height_scale);
  }
  if (scenario == 0) {
    const DioramaCapture good_capture = capture;
    const DioramaView good_view = view;
    const unsigned before = s_count;
    for (int bad = 0; bad < 5; ++bad) {
      capture = good_capture;
      view = good_view;
      if (bad == 0) capture.width = 0;
      if (bad == 1) capture.obj_apron = INT32_MAX;
      if (bad == 2) capture.authentic_y0 = INT32_MAX;
      if (bad == 3) view.camera.tilt_y = NAN;
      if (bad == 4) view.distance_scale = 0;
      assert(Diorama_Composite(&device, &capture, &view, &scene, &projection) ==
          kPresentationOutcome_CoreFailure);
      assert(!projection.valid && s_count == before);
    }
    capture = good_capture;
    view = good_view;
    options.visible_planes &= ~(1u << SR_PPU_OVERLAY_BG1);
    assert(PresentationOutcome_IsUsable(
        Diorama_Composite(&device, &capture, &view, &scene, &projection)));
    assert(!projection.bg1_plane.valid);
    options.visible_planes |= 1u << SR_PPU_OVERLAY_BG1;
    r.fail_draw = true;
  } else if (scenario == 1) {
    r.fail_restore = true;
  } else if (scenario == 2) {
    /* Changing room/capture size evicts bounded scratch entries without
     * retaining a texture for every view ever visited by the editor. */
    for (int width = 272; width <= 368; width += 16) {
      capture.width = width;
      assert(PresentationOutcome_IsUsable(
          Diorama_Composite(&device, &capture, &view, &scene, &projection)));
      assert(r.live <= base_resources + 7);
    }
  } else if (scenario == 3) {
    device.capabilities.flags &= ~kArRenderCapability_ScopedRenderTargets;
    assert(Diorama_Composite(&device, &capture, &view, &scene, &projection) ==
        kPresentationOutcome_OptionalOmitted);
    assert(projection.valid && r.bound_effect == -1);
  }
  if (r.fail_draw || r.fail_restore) {
    assert(Diorama_Composite(&device, &capture, &view, &scene, &projection) ==
        kPresentationOutcome_CoreFailure);
    assert(!projection.valid && r.bound_effect == -1);
    if (r.fail_restore) {
      /* A failed backend restore loses ownership. The compositor must stop;
       * the host then abandons the frame before resetting device resources. */
      assert(r.draws == r.draws_at_failed_restore);
      Target(&r, ArRenderTexture_Invalid());
    } else {
      assert(!r.state.target.value && !r.state.viewport_set && !r.state.clip_enabled);
    }
  }
  Diorama_ResetCompositorResources(&device);
  Diorama_ResetCompositorResources(&device);
  assert(r.live == base_resources);
  for (int i = 0; i < kDioramaPlane_Count; ++i) Destroy(&r, textures[i]);
  assert(r.live == 0);
  return s_count;
}

#ifndef AR_COMPOSITOR_WASM
int main(int argc, char **argv) {
  FILE *stream = argc == 2 ? fopen(argv[1], "wb") : NULL;
  assert(argc == 1 || stream);
  for (unsigned test = 0; test < DioramaFixture_Count(); ++test) {
    unsigned count = DioramaFixture_Run(test);
    if (!stream) continue;
    for (unsigned i = 0; i <= count; ++i) {
      uint32_t word = count;
      if (i) memcpy(&word, &s_trace[i-1], sizeof(word));
      const uint8_t bytes[] = {word, word>>8, word>>16, word>>24};
      assert(fwrite(bytes, 1, 4, stream) == 4);
    }
  }
  if (stream) assert(fclose(stream) == 0);
  else puts("Diorama compositor: 120 scenarios passed, including Death Heim A/B skies and water");
  return 0;
}
#endif
