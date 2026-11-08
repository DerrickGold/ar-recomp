/* Exercise the actual semantic waterfall hook, its submitted native source
 * window and the projection used by attached atmosphere. No GPU or ROM. */
typedef struct WaterfallContinuationProbe {
  const DioramaCapture *capture;
  const DioramaProjection *projection;
  ArRenderTexture textures[3];
  unsigned meshes[3];
  bool voxel;
} WaterfallContinuationProbe;
static WaterfallContinuationProbe *s_waterfall_continuation;

static bool WaterfallContinuationTexture(void *ctx, ArRenderTexture texture,
    const ArRenderRectF *source, const ArRenderRectF *destination,
    const ArRenderDrawState *state) {
  Recorder *recorder = ctx;
  if (s_waterfall_continuation && recorder->state.target.value)
    recorder->copied_sources[recorder->state.target.value] =
        recorder->copied_sources[texture.value].value
            ? recorder->copied_sources[texture.value] : texture;
  return Texture(ctx, texture, source, destination, state);
}

static bool WaterfallContinuationGeometry(void *ctx, ArRenderTexture texture,
    const ArRenderVertex2D *vertices, int count, const int32_t *indices,
    int index_count, const ArRenderDrawState *state) {
  WaterfallContinuationProbe *probe = s_waterfall_continuation;
  const Recorder *recorder = ctx;
  const ArRenderTexture source = recorder->copied_sources[texture.value].value
      ? recorder->copied_sources[texture.value] : texture;
  const int overflow_vertices = 9 * (kDioramaPlaneSubdivY + 2);
  const int host_vertices = 9 * (kDioramaPlaneSubdivY + 1);
  for (unsigned band = 0; probe && band < 3; ++band) {
    if (!ArRenderTexture_Equals(source, probe->textures[band]) ||
        count != overflow_vertices + host_vertices) continue;
    ++probe->meshes[band];
    const DioramaCapture *capture = probe->capture;
    DioramaProjection expected = *probe->projection;
    DioramaPlaneProjection *plane = &expected.bg2_plane;
    const float depths[] = {.20f - .5f, .21f - .5f, .05f - .5f};
    plane->z_world = depths[band];
    plane->rake = plane->bow = 0;
    const float source_y = capture->authentic_y0;
    const float fold_y = source_y + 224;
    const float overlap = (capture->height - fold_y + 16) / 224;
    const float fold_t = (fold_y / expected.texture_height - plane->v0) /
        (plane->v1 - plane->v0);
    /* The Voxel oracle is ordinary flat extrapolation, independent of the
     * overflow policy. Ordinary Diorama retains its established fold. */
    plane->overflow_valid = !probe->voxel;
    plane->overflow_fold_t = fold_t;
    plane->overflow_height = 1;
    plane->overflow_overlap_t = overlap;
    plane->overflow_handoff_z = depths[band];
    plane->overflow_front_z = .45f;
    plane->overflow_front_drop = .18f;
    for (int i = 0; i < overflow_vertices; ++i) {
      float u = vertices[i].tex_coord.x;
      float v = vertices[i].tex_coord.y;
      if (!ArRenderTexture_Equals(texture, source)) {
        u = (u * capture->width + capture->obj_apron) / SR_PPU_SURFACE_MAX_WIDTH;
        v *= capture->height / (float)SR_PPU_SURFACE_MAX_HEIGHT;
      }
      const float t = (v * SR_PPU_SURFACE_MAX_HEIGHT - source_y) / 224;
      assert(t >= -.00001f && t <= 1.00001f);
      const float x = u * expected.texture_width - expected.texture_x_origin;
      ArRenderPointF point;
      assert(Diorama_ProjectCapturedBg2Point(&expected, x, fold_y + t * 224,
          &point, NULL, NULL));
      assert(fabsf(point.x - vertices[i].position.x) < .003f);
      assert(fabsf(point.y - vertices[i].position.y) < .003f);
    }
    /* The continuation samples the same authentic224 rows in every mode,
     * including its exact endpoints; it never invents a floor texture. */
    const float texture_height = ArRenderTexture_Equals(texture, source)
        ? SR_PPU_SURFACE_MAX_HEIGHT : capture->height;
    assert(fabsf(vertices[0].tex_coord.y * texture_height - source_y) < .0001f);
    assert(fabsf(vertices[overflow_vertices - 1].tex_coord.y * texture_height - fold_y) < .0001f);
    assert(index_count == 8 * (2 * kDioramaPlaneSubdivY + 1) * 6);
  }
  return Geometry(ctx, texture, vertices, count, indices, index_count, state);
}

static unsigned WaterfallContinuationFixture(unsigned scenario) {
  s_count = 0;
  assert(scenario < 8);
  const bool voxel_enabled = (scenario & 4) != 0;
  const unsigned room = 2 + ((scenario >> 1) & 1);
  Recorder recorder = {.width = 1280, .height = 800, .bound_effect = -1};
  ArRenderBackendOps ops = kOps;
  ops.draw_texture = WaterfallContinuationTexture;
  ops.draw_geometry = WaterfallContinuationGeometry;
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device, &ops, &recorder,
      (ArRenderCapabilities){.flags = UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];
  const uint8_t *pixels[kDioramaPlane_Count];
  const uint8_t present = 255;
  for (unsigned i = 0; i < kDioramaPlane_Count; ++i) {
    assert(Create(&recorder, &(ArRenderTextureDesc){
        .width = SR_PPU_SURFACE_MAX_WIDTH, .height = SR_PPU_SURFACE_MAX_HEIGHT,
        .format = kArRenderPixelFormat_Argb8888, .usage = kArRenderTextureUsage_Streaming}, &textures[i]));
    pixels[i] = &present;
  }
  ActionVoxelGeometry *voxel = ActionVoxel_Create(); assert(voxel);
  voxel->active_group = 4; voxel->active_room = room;
  voxel->profile = kActionVoxelProfile_AitosMountains;
  voxel->settings = ActionVoxel_SettingsForRoom(4, room);
  voxel->width = voxel->height = 1024; voxel->ready = true;
  const int bands[] = {SR_PPU_OVERLAY_BG2, kDioramaPlane_Bg2Hi, kDioramaPlane_Bg2Far};
  const DioramaCapture capture = {.width = 624, .height = room == 2 ? 288 : 352,
      .authentic_y0 = room == 2 ? 32 : 64, .textures = textures, .pixels = pixels};
  const DioramaView view = {.viewport = {0, 0, 1280, 800}, .visible_width = 624,
      .distance_scale = 1, .camera = {.tilt_x = .25f, .tilt_y = scenario & 1 ? .3f : -.3f}};
  const DioramaRenderOptions options = {.visible_planes = (1u << bands[0]) |
      (1u << bands[1]) | (1u << bands[2]), .margin_fix = true};
  DioramaScene scene = {.render = &options, .map_group = 4, .map_number = room,
      .layer_section = kDioramaLayerSection_AitosWaterfall,
      .voxel_geometry = voxel_enabled ? voxel : NULL};
  DioramaProjection projection;
  WaterfallContinuationProbe probe = {.capture = &capture, .projection = &projection,
      .voxel = voxel_enabled};
  for (unsigned i = 0; i < 3; ++i) probe.textures[i] = textures[bands[i]];
  s_waterfall_continuation = &probe;
  recorder.capture = &capture; recorder.projection = &projection;
  for (unsigned frame = 0; frame < 2; ++frame) {
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device, &capture, &view, &scene, &projection)));
    const DioramaPlaneProjection *plane = &projection.bg2_plane;
    assert(plane->valid && plane->overflow_valid);
    assert(plane->overflow_height == 1);
    if (voxel_enabled) {
      assert(plane->overflow_front_z == plane->z_world);
      assert(fabsf(plane->overflow_front_drop - (1 - plane->overflow_overlap_t)) < .000001f);
    } else {
      assert(plane->overflow_front_z == .45f && plane->overflow_front_drop == .18f);
    }
    for (unsigned i = 0; i < 3; ++i) assert(probe.meshes[i] == frame + 1);
  }
  /* A semantic section mismatch cannot activate either continuation. */
  scene.layer_section = kDioramaLayerSection_Room;
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device, &capture, &view, &scene, &projection)));
  assert(!projection.bg2_plane.overflow_valid);
  for (unsigned i = 0; i < 3; ++i) assert(probe.meshes[i] == 2);
  s_waterfall_continuation = NULL;
  ActionVoxel_Release(voxel);
  Diorama_ResetCompositorResources(&device);
  for (unsigned i = 0; i < kDioramaPlane_Count; ++i) Destroy(&recorder, textures[i]);
  assert(!recorder.live);
  return s_count;
}
