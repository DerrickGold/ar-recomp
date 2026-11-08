/* Actual room/camera fixtures. Raw guards are compared against an independent
 * ordinary primary scanout of the same world rows, not a second guard decode. */
static unsigned EditorVoxelGuardCompare(EditorRoomScene *room,
    const ActionSceneSnapshot *assets, const char *ini, int x, int y, unsigned clock) {
  const DioramaVoxelGuardSource *guard = EditorRoomScene_VoxelGuard(room);
  assert(guard && DioramaVoxelGuard_Valid(guard));
  const DioramaCapture *capture = EditorRoomScene_Capture(room);
  const SrSceneSurfaces *surface = EditorRoomScene_Surfaces(room);
  const DioramaTrunkGuardStrip *strip = &guard->layout.lower;
  assert(strip->world_y == capture->voxel_frame->layer_camera_y[0] + 1 -
      capture->authentic_y0 + surface->height);
  assert(strip->world_x == capture->voxel_frame->layer_camera_x[0] -
      (surface->pitch_pixels - 256) / 2);
  assert(strip->atlas_y == 0 && strip->width == surface->pitch_pixels);
  assert(!EditorRoomScene_TrunkGuard(room));
  EditorRoomScene *other = EditorRoomScene_Create(assets); assert(other);
  assert(EditorRoomScene_Configure(other, ini));
  const int reference_y = strip->world_y - 64 > y + 64 ? strip->world_y - 64 : y + 64;
  assert(EditorRoomScene_Render(other, x, reference_y, clock, 52, 64, NULL));
  const DioramaCapture *reference_capture = EditorRoomScene_Capture(other);
  const SrSceneSurfaces *reference_surface = EditorRoomScene_Surfaces(other);
  const int ox = reference_capture->voxel_frame->layer_camera_x[0] -
      (reference_surface->pitch_pixels - 256) / 2;
  const int oy = reference_capture->voxel_frame->layer_camera_y[0] + 1 - reference_capture->authentic_y0;
  unsigned pixels = 0, ink = 0;
  for (int py = 0; py < 64; ++py) for (int px = 0; px < strip->width; ++px) {
    const bool valid = px >= strip->valid.x && py >= strip->valid.y &&
        px < strip->valid.x + strip->valid.width && py < strip->valid.y + strip->valid.height;
    for (unsigned band = 0; band < 3; ++band) {
      const uint32_t actual = guard->pixels[band][(size_t)py * guard->pitch_pixels + px];
      if (!valid) { assert(!actual); continue; }
      const int sx = strip->world_x + px - ox, sy = strip->world_y + py - oy;
      assert(sx >= 0 && sx < reference_surface->pitch_pixels && sy >= 0 && sy < reference_surface->height);
      const uint32_t expected = reference_surface->bands[0][band][(size_t)sy * reference_surface->pitch_pixels + sx];
      if (actual != expected)
        fprintf(stderr, "guard/primary room=%02x/%02x terrain=%u world=%d,%d band=%u %08x != %08x\n",
            assets->scene.group, assets->scene.map, assets->terrain_profile,
            strip->world_x + px, strip->world_y + py, band, actual, expected);
      assert(actual == expected);
      ++pixels; ink += !!(actual >> 24);
    }
  }
  assert(pixels && ink);
  EditorRoomScene_Destroy(other);
  return pixels;
}

static void EditorVoxelGuardFixtures(EditorRoomScene *room,
    const ActionSceneSnapshot *assets, const char *ini) {
  if (assets->scene.group != 4 || assets->scene.map < 2 || assets->scene.map > 3) return;
  const int x = assets->scene.map == 2 ? 712 : 72, y = assets->scene.map == 2 ? 400 : 16;
  const bool extended = assets->scene.map == 2 || assets->terrain_profile != 2;
  unsigned compared = 0;
  assert(EditorRoomScene_Configure(room, ini));
  EditorRoomScene_Set3DMode(room, kAction3D_Voxel);
  for (unsigned clock = 0; clock <= 17; clock += 17) {
    assert(EditorRoomScene_Render(room, x, y, clock, 52, 64, NULL));
    const DioramaVoxelGuardSource *guard = EditorRoomScene_VoxelGuard(room);
    assert(!!guard == extended);
    assert(!EditorRoomScene_TrunkGuard(room));
    if (guard) {
      assert(guard->layout.source.x < 0 && guard->layout.source.y < 0);
      assert(guard->layout.source.y + guard->layout.source.height == (assets->scene.map == 2 ? 832 : 352));
      compared += EditorVoxelGuardCompare(room, assets, ini, x, y, clock);
      const uint64_t generation = guard->capture_generation;
      const unsigned rasters = EditorRoomScene_RasterCount(room);
      assert(EditorRoomScene_Render(room, x, y, clock, 52, 64, NULL));
      assert(EditorRoomScene_RasterCount(room) == rasters);
      assert(EditorRoomScene_VoxelGuard(room)->capture_generation == generation);
    }
  }
  for (unsigned mode = kAction3D_Off; mode <= kAction3D_Diorama; ++mode) {
    EditorRoomScene_Set3DMode(room, (Action3DMode)mode);
    assert(!EditorRoomScene_VoxelGuard(room));
    assert(EditorRoomScene_Render(room, x, y, 0, 52, 64, NULL));
    assert(!EditorRoomScene_VoxelGuard(room));
  }
  EditorRoomScene_Set3DMode(room, kAction3D_Voxel);
  assert(EditorRoomScene_Render(room, x, y, 0, 52, 64, NULL));
  EditorRoomCamera flat;
  assert(EditorRoomScene_RenderFlat(room, x, y, 0, 52, &flat));
  assert(!EditorRoomScene_VoxelGuard(room) && !EditorRoomScene_TrunkGuard(room));

  /* Empty workspace is not source. Then place a real high-priority native
   * stamp in negative X below the native end, with intentionally legacy-Far
   * stamp metadata. Only that occupied source may extend the guard. */
  if (assets->scene.map == 3) {
    const char workspace[] = "[layers:04:03]\nbg1-map:us+jp+eu = bounds:-12,-5,44,30\n";
    assert(EditorRoomScene_Configure(room, workspace));
    assert(!EditorRoomScene_VoxelGuard(room));
    assert(EditorRoomScene_Render(room, 0, 16, 0, 52, 64, NULL));
    assert(!EditorRoomScene_VoxelGuard(room));
    /* Obtain genuine source artwork from a visible native BG1 character. */
    const ActionEnvironmentScene *environment = EditorRoomScene_Environment(room);
    ActionEnvironmentTileEdit source = {0}; bool found = false;
    const SrSceneSurfaces *native = EditorRoomScene_Surfaces(room);
    const DioramaCapture *native_capture = EditorRoomScene_Capture(room);
    for (int row = 0; row < native->height && !found; ++row)
      for (int col = 116; col < 372 && !found; ++col)
        if (native->bands[0][0][(size_t)row * native->pitch_pixels + col] >> 24) {
          const int wx = col - 116, wy = 17 - native_capture->authentic_y0 + row;
          if (wy >= 0) found = environment->tile_edit(environment->tile_edit_context, 0, wx / 8, wy / 8, &source);
        }
    assert(found);
    const unsigned word = source.entry | 0x2000;
    char edited[1024];
    snprintf(edited, sizeof(edited), "%sbg1-stamp:us+jp+eu = cell:-3,25 metatile:00 words:%04X,%04X,%04X,%04X bands:0,0,0,0\n"
        "bg1-pixels:us+jp+eu = cell:-3,25 black:FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\n",
        workspace, word, word, word, word);
    assert(EditorRoomScene_Configure(room, edited));
    assert(EditorRoomScene_Render(room, 0, 16, 0, 52, 64, NULL));
    const DioramaVoxelGuardSource *guard = EditorRoomScene_VoxelGuard(room);
    assert(guard && guard->layout.source.x == -48 && guard->layout.source.y == 0);
    assert(guard->layout.source.y + guard->layout.source.height == 416);
    compared += EditorVoxelGuardCompare(room, assets, edited, 0, 16, 0);
    const int sx = -48 - guard->layout.lower.world_x, sy = 400 - guard->layout.lower.world_y;
    assert(sx >= 0 && sy >= 0 && sx + 16 <= guard->layout.lower.width && sy + 16 <= 64);
    for (int row = 0; row < 16; ++row) for (int col = 0; col < 16; ++col) {
      const size_t at = (size_t)(sy + row) * guard->pitch_pixels + sx + col;
      assert(!guard->pixels[0][at] && !guard->pixels[2][at]);
      assert(guard->pixels[1][at] >> 24);
    }
    const uint64_t old_generation = guard->capture_generation;
    char *black = strstr(edited, "black:"); assert(black);
    memmove(black + 12, black + 6, strlen(black + 6) + 1);
    memcpy(black, "transparent:", 12);
    assert(EditorRoomScene_Configure(room, edited));
    assert(!EditorRoomScene_VoxelGuard(room));
    assert(EditorRoomScene_Render(room, 0, 16, 0, 52, 64, NULL));
    guard = EditorRoomScene_VoxelGuard(room);
    assert(guard && guard->capture_generation != old_generation);
    for (unsigned band = 0; band < 3; ++band)
      for (int row = 0; row < 16; ++row) for (int col = 0; col < 16; ++col)
        assert(!guard->pixels[band][(size_t)(sy + row) * guard->pitch_pixels + sx + col]);
    assert(EditorRoomScene_Configure(room, workspace));
    assert(EditorRoomScene_Render(room, 0, 16, 0, 52, 64, NULL));
    assert(!EditorRoomScene_VoxelGuard(room));
  }
  assert(EditorRoomScene_Configure(room, ini));
  printf("voxel lower guard %02x/%02x terrain=%u compared=%u native lane pixels; modes, finite bounds and edits PASS\n",
      assets->scene.group, assets->scene.map, assets->terrain_profile, compared);
}
