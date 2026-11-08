/* Selection observes the submitted grid, including its triangle diagonal.
 * This is editor-only work: production leaves the callback NULL. */
static void ObserveVoxelLayerMesh(const DioramaCapture *capture,
    const DioramaViewGeometry *g, const DioramaLayerDraw *layer,
    const DioramaLayerMesh *mesh) {
  if (!g->options->voxel_observe_face || layer->voxel_depth <= 0) return;
  const int plane = layer->description->plane;
  const bool actor = DioramaPlaneIsObjectPriority(plane);
  if (plane != SR_PPU_OVERLAY_BG1 && plane != kDioramaPlane_Bg1Hi && !actor) return;
  const float ox = capture->bg1_camera_x - (capture->width - 256) * .5f - capture->obj_apron;
  const float oy = capture->camera_y - capture->authentic_y0 + (actor ? 0 : 1);
  const float du = g->u1 - g->u0, dv = g->v1 - g->v0;
  if (du == 0 || dv == 0) return;
  for (int i = 0; i + 2 < mesh->index_count; i += 3) {
    DioramaVoxelPickFace face = {.kind = actor ? kDioramaVoxelFace_Object : plane == SR_PPU_OVERLAY_BG1 ?
        kDioramaVoxelFace_Backing : kDioramaVoxelFace_Front,
      .priority = actor ? DioramaPlaneObjectPriority(plane) : plane == kDioramaPlane_Bg1Hi, .plane = plane,
      .cell_x = -1, .cell_y = -1};
    bool valid = true;
    for (int j = 0; j < 4; ++j) {
      const ArRenderVertex2D *v = &mesh->vertices[mesh->indices[i + (j < 3 ? j : 2)]];
      const float s = (v->tex_coord.x - g->u0) / du;
      const float t = (v->tex_coord.y - g->v0) / dv;
      const float z = DioramaTiltedRowDepth(layer->authored->z - .5f,
          layer->authored->rake, layer->authored->bow, t);
      Scene3DClipPoint clip;
      if (!Scene3D_TransformToClip(g->matrix, (s - .5f) * g->aspect_x,
          (.5f - t) * g->height_scale, z, &clip) ||
          clip.w <= kScene3DMinimumProjectionDepth) { valid = false; break; }
      face.screen_xy[j * 2] = v->position.x;
      face.screen_xy[j * 2 + 1] = v->position.y;
      face.world_xy[j * 2] = ox + v->tex_coord.x * SR_PPU_SURFACE_MAX_WIDTH;
      face.world_xy[j * 2 + 1] = oy + v->tex_coord.y * SR_PPU_SURFACE_MAX_HEIGHT;
      face.depth[j] = clip.z / clip.w * .5f + .5f;
      face.clip_w[j] = clip.w;
    }
    if (valid) g->options->voxel_observe_face(g->options->voxel_observer_userdata, &face);
  }
}
