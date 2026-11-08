/* This target uses only portable guard layout validation; geometry splitting
 * belongs to the separate native/source fixtures. */
void ActionTrunk_Split(const ActionTrunkAssets *assets, int ox, int oy, int width,
    int height, int pitch, const uint32_t *const input[3], const size_t input_pitch[3],
    uint32_t *const output[3], unsigned brightness) {
  (void)assets; (void)ox; (void)oy; (void)width; (void)height; (void)pitch;
  (void)input; (void)input_pitch; (void)output; (void)brightness;
  fputs("unexpected native split in frame-generation ownership fixture\n", stderr); abort();
}

/* The guard and primary map deliberately keep the current native animation
 * pose. Only authoritative camera translation is interpolated; actor motion
 * and other backgrounds retain their existing endpoint/flow paths. */
static void TestTrunkGuardPlacement(ArRenderDevice *device, SDL_Renderer *renderer,
    SDL_Texture *current) {
  const int bands[] = {SR_PPU_OVERLAY_BG1, kDioramaPlane_Bg1Hi, kDioramaPlane_Bg1Far};
  const uint32_t mask = (1u << bands[0]) | (1u << bands[1]) | (1u << bands[2]);
  static ActionTrunkAssets assets;
  static FrameSlot slot;
  static SrPpuBgPacket packet;
  memset(&assets, 0, sizeof(assets)); assets.revision = 1; assets.height = 2048;
  memset(&packet, 0, sizeof(packet)); packet.owned_sources = packet.words[2] = 1;
  slot = (FrameSlot){.diorama_active = true, .interp_setting_enabled = true,
    .diorama_voxel_mode = true, .diorama_trunk = &assets, .diorama_trunk_settings = {.enabled = true},
    .snes_width = kDisplayWidth, .snes_height = kHeight, .obj_apron = kApron,
    .capture_ticks = 1, .bg_mode = 1, .diorama_map_group = 6, .diorama_map_number = 6,
    .diorama_plane_request_mask = mask, .diorama_plane_content_mask = mask,
    .background_packet = &packet};
  slot.action_bg_plan.valid = true;
  slot.action_bg_plan.layer[0] = (ActionBgLayerPlan){.valid = true,
    .role = kActionBgLayerRole_Playfield, .source = kActionBgSource_WorldMap,
    .default_edge = kActionBgEdge_LiveWorld, .world_width = 1024, .world_height = 2048};
  ArRenderTexture raw[kDioramaPlane_Count] = {0}, resolved[kDioramaPlane_Count] = {0};
  const uint8_t *pixels[kDioramaPlane_Count] = {0}; size_t pitches[kDioramaPlane_Count] = {0};
  uint32_t native[kSurfaceWidth * kHeight];
  for (unsigned i = 0; i < 3; ++i) {
    raw[bands[i]] = ArSdlRenderBackend_BorrowTexture(current);
    pixels[bands[i]] = (const uint8_t *)native; pitches[bands[i]] = kSurfaceWidth * 4;
  }
  DioramaFrameGeneration_InvalidateHistory();
  int x = 100, y = 200, previous_x = x, previous_y = y;
  const float phases[] = {0, .1f, .49f, .51f, .9f, 1};
  for (unsigned frame = 0; frame < 15; ++frame) {
    previous_x = x; previous_y = y;
    if (frame != 2) { x += (frame & 1) ? 2 : -2; y += (frame & 1) ? -1 : 1; }
    if (frame == 3) ++assets.revision;
    if (frame == 7) slot.action_bg_plan.layer[0].world_height += 16;
    slot.action_bg_plan.layer[0].band_count = frame == 12 ? 1 : 0;
    slot.bg1_camera_x = x; slot.bg1_camera_y = y;
    slot.timestamp_ns = UINT64_C(5000000000) + frame * UINT64_C(16666667);
    CHECK(DioramaTrunkGuard_Resolve(x - kApron, y + (frame >= 5 ? 2 : 1),
        kSurfaceWidth, kHeight, (DioramaTrunkGuardBounds){0, 0, 1024,
        frame >= 7 ? 2064 : 2048}, &slot.diorama_trunk_guard.layout));
    slot.diorama_trunk_guard.capture_generation = frame == 9 ? 0 : frame + 1;
    /* A retained FrameSlot no longer has its borrowed source pointer. */
    slot.diorama_trunk_guard.pixels = NULL;
    for (int py = 0; py < kHeight; ++py) for (int px = 0; px < kSurfaceWidth; ++px)
      native[py * kSurfaceWidth + px] = MapPixel(px + x, py + y, frame & 3, false);
    DioramaFrameGeneration_FinishCapture();
    const SDL_Rect rectangle = {0, 0, kSurfaceWidth, kHeight};
    CHECK(SDL_UpdateTexture(current, &rectangle, native, kSurfaceWidth * 4));
    DioramaFrameGeneration_Capture(device, &slot, raw, pixels, pitches, mask);
    const bool paired = frame == 1 || frame == 2 || frame == 4 || frame == 6 ||
        frame == 8 || frame == 11 || frame == 14;
    for (unsigned phase = 0; phase < SDL_arraysize(phases); ++phase) {
      const uint32_t generated = DioramaFrameGeneration_Prepare(device, &slot, phases[phase], raw, mask, resolved);
      if (frame == 9) continue; /* No guard: established unguarded policy owns this frame. */
      CHECK(!(generated & mask));
      CHECK(!(DioramaFrameGeneration_GpuPlaneMask() & mask));
      CHECK(DioramaFrameGeneration_VoxelMapPlaneMask() == (paired ? mask : 0));
      for (unsigned i = 0; i < 3; ++i) {
        CHECK(ArRenderTexture_Equals(raw[bands[i]], resolved[bands[i]]));
        const ArRenderPointF offset = DioramaFrameGeneration_PlaneOffset(bands[i]);
        CHECK(fabsf(offset.x - (paired ? (x - previous_x) * (1 - phases[phase]) : 0)) < .0001f);
        CHECK(fabsf(offset.y - (paired ? (y - previous_y) * (1 - phases[phase]) : 0)) < .0001f);
      }
    }
    if (frame == 1) {
      ArRenderTexture missing[kDioramaPlane_Count]; memcpy(missing, raw, sizeof(missing));
      missing[bands[1]] = ArRenderTexture_Invalid();
      CHECK(!(DioramaFrameGeneration_Prepare(device, &slot, .25f, missing, mask, resolved) & mask));
      CHECK(!DioramaFrameGeneration_VoxelMapPlaneMask());
      for (unsigned i = 0; i < 3; ++i) {
        const ArRenderPointF offset = DioramaFrameGeneration_PlaneOffset(bands[i]);
        CHECK(offset.x == 0 && offset.y == 0);
      }
    }
  }
  /* Ordinary Diorama never inherits the guard's exact-map camera contract. */
  slot.diorama_voxel_mode = false; slot.timestamp_ns += UINT64_C(16666667);
  DioramaFrameGeneration_Capture(device, &slot, raw, pixels, pitches, mask);
  (void)DioramaFrameGeneration_Prepare(device, &slot, .25f, raw, mask, resolved);
  CHECK(!DioramaFrameGeneration_VoxelMapPlaneMask());
  DioramaFrameGeneration_InvalidateHistory();
  CHECK(!DioramaFrameGeneration_VoxelMapPlaneMask());
  puts("guarded tree map: current native CHR source, camera translation, revision/bounds/margin/availability/raster reset; CPU/GPU contract pass");
}

/* The generic lower guard has three independent native lanes, but the retained
 * frame intentionally owns only their uploaded generation/layout metadata.
 * Exercise the same current-source placement contract without borrowing a
 * tree's masks or accidentally pairing the two representations. */
static void TestVoxelGuardPlacement(ArRenderDevice *device, SDL_Renderer *renderer,
    SDL_Texture *current) {
  (void)renderer;
  const int bands[] = {SR_PPU_OVERLAY_BG1, kDioramaPlane_Bg1Hi, kDioramaPlane_Bg1Far};
  const uint32_t mask = (1u << bands[0]) | (1u << bands[1]) | (1u << bands[2]);
  ActionVoxelGeometry *voxel = ActionVoxel_Create(); CHECK(voxel != NULL); if (!voxel) return;
  voxel->ready = true; voxel->active_group = 4; voxel->active_room = 2;
  voxel->profile = kActionVoxelProfile_AitosMountains;
  voxel->settings = ActionVoxel_SettingsForRoom(4, 2);
  CHECK(ActionVoxel_Active(voxel, 4, 2));
  static ActionTrunkAssets tree;
  static FrameSlot slot;
  static SrPpuBgPacket packet;
  memset(&tree, 0, sizeof(tree)); tree.height = 2048;
  memset(&packet, 0, sizeof(packet)); packet.owned_sources = packet.words[2] = 1;
  slot = (FrameSlot){.diorama_active = true, .interp_setting_enabled = true,
    .diorama_voxel_mode = true, .diorama_voxel = voxel,
    .snes_width = kDisplayWidth, .snes_height = kHeight, .obj_apron = kApron,
    .capture_ticks = 1, .bg_mode = 1, .diorama_map_group = 4, .diorama_map_number = 2,
    .diorama_plane_request_mask = mask, .diorama_plane_content_mask = mask,
    .background_packet = &packet};
  slot.action_bg_plan.valid = true;
  slot.action_bg_plan.layer[0] = (ActionBgLayerPlan){.valid = true,
    .role = kActionBgLayerRole_Playfield, .source = kActionBgSource_WorldMap,
    .default_edge = kActionBgEdge_LiveWorld, .world_width = 1024, .world_height = 2048};
  ArRenderTexture raw[kDioramaPlane_Count] = {0}, resolved[kDioramaPlane_Count] = {0};
  const uint8_t *pixels[kDioramaPlane_Count] = {0}; size_t pitches[kDioramaPlane_Count] = {0};
  uint32_t native[kSurfaceWidth * kHeight];
  for (unsigned i = 0; i < 3; ++i) {
    raw[bands[i]] = ArSdlRenderBackend_BorrowTexture(current);
    pixels[bands[i]] = (const uint8_t *)native; pitches[bands[i]] = kSurfaceWidth * 4;
  }
  DioramaFrameGeneration_InvalidateHistory();
  int x = 100, y = 200;
  const float phases[] = {0, .1f, .49f, .51f, .9f, 1};
  for (unsigned frame = 0; frame < 24; ++frame) {
    const int previous_x = x, previous_y = y;
    if (frame != 2) { x += (frame & 1) ? 2 : -2; y += (frame & 1) ? -1 : 1; }
    if (frame == 3) ++voxel->revision;
    tree.revision = voxel->revision;
    slot.action_bg_plan.layer[0].world_height = frame >= 7 ? 2064 : 2048;
    slot.action_bg_plan.layer[0].band_count = frame == 12 ? 1 : 0;
    slot.diorama_voxel_mode = frame != 21;
    /* Keep room, voxel revision, source bounds and camera identical at both
     * kind switches. Only the representation discriminator may reject these. */
    slot.diorama_trunk = frame == 17 || frame == 18 ? &tree : NULL;
    slot.diorama_trunk_settings.enabled = slot.diorama_trunk != NULL;
    slot.bg1_camera_x = x; slot.bg1_camera_y = y;
    slot.timestamp_ns = UINT64_C(6000000000) + frame * UINT64_C(16666667);
    const int origin_y = y + (frame >= 5 ? 2 : 1);
    const int width = kSurfaceWidth - (frame >= 15 ? 2 : 0);
    const DioramaTrunkGuardBounds bounds = {0, 0, 1024, frame >= 7 ? 2064 : 2048};
    CHECK(DioramaVoxelGuard_Resolve(x - kApron, origin_y, width, kHeight,
        bounds, &slot.diorama_voxel_guard.layout));
    slot.diorama_voxel_guard.capture_generation = frame == 9 ? 0 : frame + 1;
    for (unsigned i = 0; i < 3; ++i) CHECK(slot.diorama_voxel_guard.pixels[i] == NULL);
    CHECK(DioramaTrunkGuard_Resolve(x - kApron, origin_y, width, kHeight,
        bounds, &slot.diorama_trunk_guard.layout));
    slot.diorama_trunk_guard.capture_generation = frame + 1;
    CHECK(slot.diorama_trunk_guard.pixels == NULL);
    for (int py = 0; py < kHeight; ++py) for (int px = 0; px < kSurfaceWidth; ++px)
      native[py * kSurfaceWidth + px] = MapPixel(px + x, py + y, frame & 3, false);
    DioramaFrameGeneration_FinishCapture();
    const SDL_Rect rectangle = {0, 0, kSurfaceWidth, kHeight};
    CHECK(SDL_UpdateTexture(current, &rectangle, native, kSurfaceWidth * 4));
    DioramaFrameGeneration_Capture(device, &slot, raw, pixels, pitches, mask);
    const bool paired = frame == 1 || frame == 2 || frame == 4 || frame == 6 ||
        frame == 8 || frame == 11 || frame == 14 || frame == 16 || frame == 18 ||
        frame == 20 || frame == 23;
    for (unsigned phase = 0; phase < SDL_arraysize(phases); ++phase) {
      const uint32_t generated = DioramaFrameGeneration_Prepare(device, &slot, phases[phase], raw, mask, resolved);
      CHECK(DioramaFrameGeneration_VoxelMapPlaneMask() == (paired ? mask : 0));
      if (frame == 9 || frame == 21) continue; /* Normal unguarded/Diorama interpolation owns these. */
      CHECK(!(generated & mask)); CHECK(!(DioramaFrameGeneration_GpuPlaneMask() & mask));
      for (unsigned i = 0; i < 3; ++i) {
        CHECK(ArRenderTexture_Equals(raw[bands[i]], resolved[bands[i]]));
        const ArRenderPointF offset = DioramaFrameGeneration_PlaneOffset(bands[i]);
        CHECK(fabsf(offset.x - (paired ? (x - previous_x) * (1 - phases[phase]) : 0)) < .0001f);
        CHECK(fabsf(offset.y - (paired ? (y - previous_y) * (1 - phases[phase]) : 0)) < .0001f);
      }
    }
    if (frame == 1) for (unsigned absent = 0; absent < 3; ++absent) {
      ArRenderTexture missing[kDioramaPlane_Count]; memcpy(missing, raw, sizeof(missing));
      missing[bands[absent]] = ArRenderTexture_Invalid();
      CHECK(!(DioramaFrameGeneration_Prepare(device, &slot, .25f, missing, mask, resolved) & mask));
      CHECK(!DioramaFrameGeneration_VoxelMapPlaneMask());
      for (unsigned i = 0; i < 3; ++i) {
        const ArRenderPointF offset = DioramaFrameGeneration_PlaneOffset(bands[i]);
        CHECK(offset.x == 0 && offset.y == 0);
      }
    }
  }
  DioramaFrameGeneration_InvalidateHistory();
  CHECK(!DioramaFrameGeneration_VoxelMapPlaneMask());
  slot.diorama_voxel = NULL; ActionVoxel_Release(voxel);
  puts("generic guard map: 144 phases, current source, revision/bounds/margin/width/availability/raster/mode resets, no cross-kind pairing; pass");
}
