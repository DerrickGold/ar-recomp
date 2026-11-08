/* Real compositor submissions, including compact source copies. The oracle
 * recovers source coordinates from camera rays rather than the new helper. */
typedef struct DeathHeimWaterProbe {
  const DioramaCapture *capture;
  const DioramaProjection *projection;
  ArRenderTexture source;
  bool expected;
  unsigned draws, extensions, max_quads, edge_samples;
  unsigned scenario, frame;
} DeathHeimWaterProbe;
static DeathHeimWaterProbe *s_death_heim_water;

static ArRenderPointF DeathHeimWaterSourcePoint(
    const DeathHeimWaterProbe *probe, ArRenderPointF point) {
  const DioramaProjection *p = probe->projection;
  const DioramaPlaneProjection *plane = &p->bg2_plane;
  const float *m = p->matrix;
  const double nx = 2.0 * point.x / p->output_width - 1;
  const double ny = 1 - 2.0 * point.y / p->output_height;
  const double z = plane->z_world;
  const double a = m[0] - nx*m[3], b = m[4] - nx*m[7];
  const double c = m[1] - ny*m[3], d = m[5] - ny*m[7];
  const double u = (nx*m[11]-m[8])*z + nx*m[15]-m[12];
  const double v = (ny*m[11]-m[9])*z + ny*m[15]-m[13];
  const double det = a*d-b*c;
  assert(fabs(det) > .000001);
  const double wx = (u*d-b*v)/det;
  const double wy = (a*v-u*c)/det - plane->world_y_offset;
  return (ArRenderPointF){
      (plane->u0 + (wx/p->aspect_x+.5)*(plane->u1-plane->u0))*p->texture_width,
      (plane->v0 + (.5-wy/p->height_scale)*(plane->v1-plane->v0))*p->texture_height};
}

static bool DeathHeimWaterTexture(void *ctx, ArRenderTexture texture,
    const ArRenderRectF *source, const ArRenderRectF *destination,
    const ArRenderDrawState *state) {
  Recorder *r = ctx;
  if (s_death_heim_water && r->state.target.value)
    r->copied_sources[r->state.target.value] =
        r->copied_sources[texture.value].value ? r->copied_sources[texture.value] : texture;
  return Texture(ctx, texture, source, destination, state);
}

static bool DeathHeimWaterGeometry(void *ctx, ArRenderTexture texture,
    const ArRenderVertex2D *vertices, int count, const int32_t *indices,
    int index_count, const ArRenderDrawState *state) {
  DeathHeimWaterProbe *probe = s_death_heim_water;
  Recorder *r = ctx;
  const ArRenderTexture source = r->copied_sources[texture.value].value
      ? r->copied_sources[texture.value] : texture;
  const int host = 9*(kDioramaPlaneSubdivY+1);
  if (probe && ArRenderTexture_Equals(source, probe->source) && count >= host) {
    ++probe->draws;
    const DioramaCapture *capture = probe->capture;
    const int extra = count-host;
    assert(extra >= 0 && extra%4 == 0);
    if (!probe->expected) assert(extra == 0);
    if (extra) ++probe->extensions;
    if ((unsigned)extra/4 > probe->max_quads) probe->max_quads = extra/4;
    assert(extra <= 72); /* Existing attachment storage, no dynamic geometry. */
    assert(index_count == 8*kDioramaPlaneSubdivY*6 + extra/4*6);
    const float anchor = capture->obj_apron + (capture->width-256)*.5f;
    const float host_left = capture->obj_apron, host_right = host_left+capture->width;
    float covered_left = host_left, covered_right = host_right;
    for (int i = 0; i < count; ++i) {
      ArRenderPointF uv = vertices[i].tex_coord;
      if (!ArRenderTexture_Equals(texture, source)) {
        uv.x = (uv.x*capture->width+capture->obj_apron)/SR_PPU_SURFACE_MAX_WIDTH;
        uv.y *= capture->height/(float)SR_PPU_SURFACE_MAX_HEIGHT;
      }
      const float sx = uv.x*SR_PPU_SURFACE_MAX_WIDTH;
      const float sy = uv.y*SR_PPU_SURFACE_MAX_HEIGHT;
      const ArRenderPointF native = DeathHeimWaterSourcePoint(probe, vertices[i].position);
      if (i < extra) {
        assert(sx >= anchor-.001f && sx <= anchor+256.001f);
        assert(fabsf(sy-(capture->authentic_y0+(i%4 >= 2 ? 224 : 144))) < .001f);
        assert(fabsf(native.y-sy) < .015f);
        assert(native.x <= host_left+.015f || native.x >= host_right-.015f);
        const float periods = (native.x-sx)/256;
        assert(fabsf(periods-roundf(periods)) < .00008f);
        covered_left = fminf(covered_left, native.x);
        covered_right = fmaxf(covered_right, native.x);
        if (i%4 == 0 && i && native.x < host_left-.015f) {
          const ArRenderPointF previous = DeathHeimWaterSourcePoint(probe, vertices[i-3].position);
          assert(fabsf(previous.x-native.x) < .02f);
        }
      } else {
        /* The original host geometry and its source phase are untouched. */
        assert(fabsf(native.x-sx) < .02f && fabsf(native.y-sy) < .02f);
        const int j = i-extra;
        assert(fabsf(sx-(host_left+(j%9)*capture->width/8.f)) < .001f);
        assert(fabsf(sy-(j/9)*capture->height/(float)kDioramaPlaneSubdivY) < .001f);
      }
    }
    if (probe->expected) {
      /* Include the filter guard at both output edges. A ray that intersects
       * native water must be covered by the host or its continuation. */
      for (unsigned side = 0; side < 2; ++side)
        for (unsigned row = 0; row <= 64; ++row) {
          const ArRenderPointF native = DeathHeimWaterSourcePoint(probe,
              (ArRenderPointF){side ? r->width+2.f : -2.f, row*r->height/64.f});
          if (native.y < capture->authentic_y0+144 || native.y > capture->authentic_y0+224) continue;
          ++probe->edge_samples;
          if (native.x < covered_left-.025f || native.x > covered_right+.025f)
            fprintf(stderr,"DH water coverage: scenario%u frame%u extra%d source(%g,%g) covered[%g,%g]\n",
                probe->scenario,probe->frame,extra,native.x,native.y,covered_left,covered_right);
          assert(native.x >= covered_left-.025f && native.x <= covered_right+.025f);
        }
    }
  }
  return Geometry(ctx, texture, vertices, count, indices, index_count, state);
}

static unsigned DeathHeimWaterFixture(unsigned scenario) {
  s_count = 0;
  assert(scenario < 8);
  Recorder r = {.width=1280, .height=800, .bound_effect=-1, .shaders=(scenario&2)!=0};
  ArRenderBackendOps ops = kOps;
  ops.draw_texture = DeathHeimWaterTexture;
  ops.draw_geometry = DeathHeimWaterGeometry;
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device, &ops, &r, (ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];
  const uint8_t *pixels[kDioramaPlane_Count];
  const uint8_t present=255;
  for (unsigned i=0; i<kDioramaPlane_Count; ++i) {
    assert(Create(&r, &(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,
        .height=SR_PPU_SURFACE_MAX_HEIGHT, .format=kArRenderPixelFormat_Argb8888,
        .usage=kArRenderTextureUsage_Streaming}, &textures[i]));
    pixels[i]=&present;
  }
  ActionVoxelGeometry *voxel=ActionVoxel_Create(); assert(voxel);
  voxel->active_group=7; voxel->active_room=1; voxel->profile=kActionVoxelProfile_DeathHeim;
  voxel->settings=ActionVoxel_SettingsForRoom(7,1);
  voxel->width=voxel->height=512; voxel->ready=true;
  DioramaCapture capture={.width=scenario==2?256:624, .height=352, .authentic_y0=64,
      .obj_apron=scenario==2?64:8, .textures=textures, .pixels=pixels};
  DioramaView view={.viewport={0,0,1280,800}, .visible_width=capture.width,
      .distance_scale=1, .camera={.tilt_x=.25f, .tilt_y=scenario&1?.3f:-.3f}};
  DioramaRenderOptions options={.visible_planes=1u<<SR_PPU_OVERLAY_BG2,
      .skybox=kDioramaSky_Only, .margin_fix=true, .depth_of_field=(scenario&2)!=0};
  DioramaScene scene={.render=&options, .map_group=7, .map_number=1,
      .layer_section=kDioramaLayerSection_Room, .voxel_geometry=voxel};
  if (scenario==4) scene.layer_section=kDioramaLayerSection_DeathHeimCompletion;
  if (scenario==5) scene.voxel_geometry=NULL;
  if (scenario==6) options.skybox=kDioramaSky_Off;
  DioramaProjection projection;
  DeathHeimWaterProbe probe={.capture=&capture, .projection=&projection,
      .source=textures[SR_PPU_OVERLAY_BG2], .expected=scenario<4||scenario==7, .scenario=scenario};
  s_death_heim_water=&probe;
  r.capture=&capture; r.projection=&projection;
  const bool camera_grid=scenario==2||scenario==7;
  const unsigned frames=camera_grid?27:2;
  unsigned retained_resources=0;
  for (unsigned frame=0; frame<frames; ++frame) {
    probe.frame=frame;
    if (camera_grid) {
      view.camera.tilt_x=((int)(frame%3)-1)*.7f;
      view.camera.tilt_y=((int)((frame/3)%3)-1)*.7f;
      view.camera.distance=(float[]){2,8,20}[frame/9];
    } else if (frame) view.camera.tilt_y += .001f;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    if (frame) assert(r.creates==retained_resources);
    retained_resources=r.creates;
  }
  const unsigned expected_draws=scenario==4?0:frames;
  assert(probe.draws==expected_draws);
  if (probe.expected) assert(probe.extensions>0 && probe.edge_samples>0);
  else assert(!probe.extensions);
  /* Absent native water cannot create an attachment. Active Voxel deliberately
   * enables native scenery slots, so a plane mask is not a visibility oracle. */
  pixels[SR_PPU_OVERLAY_BG2]=NULL;
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
  assert(probe.draws==expected_draws);
  s_death_heim_water=NULL;
  ActionVoxel_Release(voxel);
  Diorama_ResetCompositorResources(&device);
  for (unsigned i=0; i<kDioramaPlane_Count; ++i) Destroy(&r,textures[i]);
  assert(!r.live);
  printf("Death Heim water case %u: %u draws, %u extensions, %u maximum quads, %u covered edge rays\n",
      scenario,probe.draws,probe.extensions,probe.max_quads,probe.edge_samples);
  return s_count;
}
