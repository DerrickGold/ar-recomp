/* Hub water is a native repeating band below unique boss faces. Extend only
 * that band outside its existing captured rectangle. Source phase stays tied
 * to the authentic capture; rotating the camera changes coverage, not UVs. */
static bool AppendDeathHeimWaterSide(const DioramaCapture *capture,
    const DioramaViewGeometry *geometry, const DioramaLayerDraw *layer,
    float x0, float x1, DioramaAttachedMesh *attached) {
  const float period = kActRaiserAuthenticWidth;
  const float anchor = capture->obj_apron +
      .5f * (capture->width - kActRaiserAuthenticWidth);
  const float source_x0 = geometry->u0 * SR_PPU_SURFACE_MAX_WIDTH;
  const float source_width = (geometry->u1 - geometry->u0) * SR_PPU_SURFACE_MAX_WIDTH;
  const float source_y0 = geometry->v0 * SR_PPU_SURFACE_MAX_HEIGHT;
  const float source_height = (geometry->v1 - geometry->v0) * SR_PPU_SURFACE_MAX_HEIGHT;
  const float top = capture->authentic_y0 + kDeathHeimFaceRows;
  const float bottom = capture->authentic_y0 + kActRaiserAuthenticHeight;
  const float z = layer->authored->z - .5f;
  while (x0 < x1) {
    if (attached->vertex_count > DIORAMA_OVERFLOW_VERTS - 4 ||
        attached->index_count > DIORAMA_OVERFLOW_INDICES - 6) return false;
    const float cell = floorf((x0 - anchor) / period);
    const float cell_x = anchor + cell * period;
    const float end = fminf(x1, cell_x + period);
    if (!isfinite(end) || end <= x0) return false;
    const float x[4] = {x0, end, end, x0};
    const float y[4] = {top, top, bottom, bottom};
    const int first = attached->vertex_count;
    for (unsigned corner = 0; corner < 4; ++corner) {
      ArRenderVertex2D *vertex = &attached->vertices[first + corner];
      if (!ProjectWorldPoint(geometry->matrix,
          ((x[corner] - source_x0) / source_width - .5f) * geometry->aspect_x,
          (.5f - (y[corner] - source_y0) / source_height) * geometry->height_scale,
          z, geometry->width, geometry->height, &vertex->position)) return false;
      vertex->tex_coord = (ArRenderPointF){
        (anchor + x[corner] - cell_x) /
            SR_PPU_SURFACE_MAX_WIDTH,
        y[corner] / SR_PPU_SURFACE_MAX_HEIGHT};
      vertex->color = layer->shade;
    }
    const int corners[] = {0, 1, 2, 0, 2, 3};
    for (unsigned i = 0; i < 6; ++i)
      attached->indices[attached->index_count++] = first + corners[i];
    attached->vertex_count += 4;
    x0 = end;
  }
  return true;
}

static void PrepareDeathHeimWaterExtension(const DioramaCapture *capture,
    const DioramaScene *scene, const DioramaViewGeometry *geometry,
    const DioramaLayerDraw *layer, DioramaAttachedMesh *attached) {
  if (!DeathHeimFaceScene(scene) || scene->render->skybox != kDioramaSky_Only ||
      !ActionVoxel_Active(scene->voxel_geometry, scene->map_group, scene->map_number) ||
      layer->description->plane != SR_PPU_OVERLAY_BG2 ||
      layer->authored->rake || layer->authored->bow ||
      capture->width < kActRaiserAuthenticWidth ||
      capture->authentic_y0 + kActRaiserAuthenticHeight > capture->height)
    return;
  /* This is the layer matrix after PlaceDeathHeimWater's vertical placement.
   * Invert the actual water plane, including a two-output-pixel filter guard;
   * BG1's aperture does not describe this recessed and lowered surface. */
  const float source_x0 = geometry->u0 * SR_PPU_SURFACE_MAX_WIDTH;
  const float source_x1 = geometry->u1 * SR_PPU_SURFACE_MAX_WIDTH;
  const float source_width = source_x1 - source_x0;
  if (!(source_width > 0) || !(geometry->v1 > geometry->v0)) return;
  ArRenderPointF polygon[8], scratch[8];
  for (unsigned corner = 0; corner < 4; ++corner) {
    const float sx = corner == 1 || corner == 2
        ? 1 + 2.f / geometry->width : -2.f / geometry->width;
    const float sy = corner >= 2 ? 1 + 2.f / geometry->height : -2.f / geometry->height;
    float x, y;
    if (!DioramaSkyboxWorldPoint(geometry->matrix, layer->authored->z - .5f,
        geometry->aspect_x, geometry->height_scale, sx, sy, &x, &y)) return;
    polygon[corner] = (ArRenderPointF){source_x0 + x * source_width,
        (geometry->v0 + y * (geometry->v1 - geometry->v0)) * SR_PPU_SURFACE_MAX_HEIGHT};
  }
  /* Only rays intersecting the water band need continuation. Including the
   * entire sky aperture at maximum zoom wastes bounded geometry offscreen. */
  unsigned count = 4;
  for (unsigned side = 0; side < 2 && count; ++side) {
    const float edge = capture->authentic_y0 + (side ? kActRaiserAuthenticHeight : kDeathHeimFaceRows);
    unsigned next = 0;
    for (unsigned i = 0; i < count; ++i) {
      const ArRenderPointF a = polygon[i], b = polygon[(i + 1) % count];
      const bool in_a = side ? a.y <= edge : a.y >= edge;
      const bool in_b = side ? b.y <= edge : b.y >= edge;
      if (in_a) scratch[next++] = a;
      if (in_a != in_b) {
        const float t = (edge - a.y) / (b.y - a.y);
        scratch[next++] = (ArRenderPointF){a.x + t * (b.x - a.x), edge};
      }
    }
    count = next;
    memcpy(polygon, scratch, count * sizeof(*polygon));
  }
  float left = source_x0, right = source_x1;
  for (unsigned i = 0; i < count; ++i) {
    left = fminf(left, floorf(polygon[i].x));
    right = fmaxf(right, ceilf(polygon[i].x));
  }
  /* Unsupported near-singular poses retain the ordinary finite source; no
   * unbounded tessellation, fresh texture, or invented background is allowed. */
  if (!AppendDeathHeimWaterSide(capture, geometry, layer, left, source_x0, attached) ||
      !AppendDeathHeimWaterSide(capture, geometry, layer, source_x1, right, attached))
    attached->vertex_count = attached->index_count = 0;
}
