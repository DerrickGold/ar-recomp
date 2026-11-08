/* The WebGL sink receives the same clipped meshes and native actor priority
 * policy as the production GPU sink. Immediate submission preserves ordering
 * with ordinary effect draws; depth belongs to each currently bound target. */
static bool DepthSubmit(void *userdata, ArRenderDevice *device,
                         const DioramaDepthSubmission *draw) {
  (void)userdata;
  if (!s_depth_active || s_depth_failed || device != &s_device || !draw ||
      !draw->vertices || !draw->depths || !draw->indices || draw->vertex_count <= 0 ||
      draw->index_count <= 0 || draw->index_count % 3 ||
      draw->vertex_count > kDioramaDepthMaximumVertices ||
      draw->index_count > kDioramaDepthMaximumIndices ||
      draw->texture.value >= sizeof(s_textures) / sizeof(*s_textures) ||
      (draw->texture.value && s_textures[draw->texture.value].width <= 0) ||
      (draw->native_actor && draw->clip_w)) {
    s_depth_failed = true;
    return false;
  }
  for (int i = 0; i < draw->vertex_count; ++i) {
    if (!isfinite(draw->depths[i]) ||
        (draw->clip_w && (!isfinite(draw->clip_w[i]) || draw->clip_w[i] <= 0))) {
      s_depth_failed = true;
      return false;
    }
  }
  const DioramaSceneryLightEffectParams *lighting = draw->lighting;
  const bool lit = lighting && ArRenderTexture_IsValid(lighting->illumination);
  bool material = false, bound = true;
  if (lit) {
    material = true;
    bound = DioramaEffectBackend_BindSceneryLight(device, lighting);
  } else if (lighting && ArRenderTexture_IsValid(draw->texture)) {
    /* Depth submissions suppress the compositor's ordinary material binds;
     * the sink realizes the same priority/rim/DOF parameters instead. A draw
     * without its own material continues to borrow its caller's binding. */
    if (lighting->mode == 2) {
      material = true;
      bound = DioramaEffectBackend_BindPrioritySurface(device, &lighting->priority);
    } else if (draw->native_actor && lighting->rim_strength > 0) {
      const DioramaRimLightEffectParams rim = {
          .texel_width=1.f/SR_PPU_SURFACE_MAX_WIDTH,
          .texel_height=1.f/SR_PPU_SURFACE_MAX_HEIGHT,
          .strength=lighting->rim_strength};
      material = true;
      bound = DioramaEffectBackend_BindRimLight(device, &rim);
    } else if (lighting->mode == 1) {
      const DioramaDofEdgeEffectParams dof = {
          .texel_width=lighting->texel_width, .texel_height=lighting->texel_height,
          .blur_radius=lighting->blur_radius, .u_max=1, .v_max=1,
          .lower_content_v_max=lighting->lower_content_v_max};
      /* UV feather is disabled in production; geometry owns edge coverage. */
      material = true;
      bound = DioramaEffectBackend_BindDofEdge(device, &dof);
    }
  }
  if (!bound) {
    (void)DioramaEffectBackend_Unbind(device);
    s_depth_failed = true;
    return false;
  }
  const DioramaDepthMode mode = draw->native_actor ? kDioramaDepth_NativeActor :
      draw->blend == kArRenderBlendMode_Add ? kDioramaDepth_Read : kDioramaDepth_Write;
  const bool drawn = WebDepthGeometry((int)draw->texture.value, draw->vertices,
      draw->vertex_count, draw->indices, draw->index_count, draw->blend,
      0, 0, -1, draw->depths, draw->clip_w, mode) != 0;
  const bool restored = !material || DioramaEffectBackend_Unbind(device);
  s_depth_failed = !drawn || !restored;
  return !s_depth_failed;
}
static bool DepthClear(void *userdata, ArRenderDevice *device) {
  (void)userdata;
  if (!s_depth_active || s_depth_failed || device != &s_device) return false;
  s_depth_failed = !WebDepthClear();
  return !s_depth_failed;
}
static bool DepthFinish(void *userdata, ArRenderDevice *device, bool success) {
  (void)userdata;
  if (!s_depth_active || device != &s_device) return false;
  const bool complete = success && !s_depth_failed;
  const bool finished = WebDepthFinish(complete) != 0;
  s_depth_active = s_depth_failed = false;
  return finished && (!success || complete);
}
const DioramaDepthSink *DioramaPreview_DepthBegin(void) {
  static const DioramaDepthSink sink = {
      .submit=DepthSubmit, .clear=DepthClear, .finish=DepthFinish};
  if (s_depth_active || !WebDepthBegin()) return NULL;
  s_depth_active = true;
  s_depth_failed = false;
  return &sink;
}
