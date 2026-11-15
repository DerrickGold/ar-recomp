/* One staging map and one ordered command buffer for the entire transfer. */
enum { kMaxTextureRegions = 32 };

typedef struct NativeTextureRegion {
  SDL_GPUTexture *texture;
  SDL_PixelFormat format;
} NativeTextureRegion;

static NativeTextureRegion NativeRegion(ArRenderTexture texture, ArRenderRectI rect) {
  if (!ArRenderTexture_IsValid(texture)) return (NativeTextureRegion){0};
  const SDL_PropertiesID props = SDL_GetTextureProperties(ArSdlRenderBackend_UnwrapTexture(texture));
  const Sint64 width = SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_WIDTH_NUMBER, 0);
  const Sint64 height = SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_HEIGHT_NUMBER, 0);
  const SDL_PixelFormat format = (SDL_PixelFormat)SDL_GetNumberProperty(
      props, SDL_PROP_TEXTURE_FORMAT_NUMBER, SDL_PIXELFORMAT_UNKNOWN);
  if ((format != SDL_PIXELFORMAT_RGBA32 && format != SDL_PIXELFORMAT_BGRA32) ||
      rect.x < 0 || rect.y < 0 || rect.w <= 0 || rect.h <= 0 ||
      rect.x > width - rect.w || rect.y > height - rect.h)
    return (NativeTextureRegion){0};
  return (NativeTextureRegion){SDL_GetPointerProperty(props, SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, NULL), format};
}

static bool StageTextureRegions(ArSdlRenderBackend *backend, const ArRenderTextureRegionUpload *uploads,
    const NativeTextureRegion *regions, const ArSdlTextureUploadLayout *layouts, unsigned count, Uint32 size) {
  SDL_GPUDevice *gpu = backend->gpu_device;
  if (size > backend->region_staging_capacity) {
    const SDL_GPUTransferBufferCreateInfo info = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = size};
    SDL_GPUTransferBuffer *buffer = SDL_CreateGPUTransferBuffer(gpu, &info);
    if (!buffer) return false;
    ReleaseRegionStaging(backend);
    backend->region_staging = buffer;
    backend->region_staging_capacity = size;
  }
  uint8_t *mapped = SDL_MapGPUTransferBuffer(gpu, backend->region_staging, true);
  if (!mapped) return false;
  bool ok = true;
  for (unsigned i = 0; ok && i < count; ++i) {
    const ArRenderTextureRegionUpload *u = &uploads[i];
    ok = SDL_ConvertPixels(u->rect.w, u->rect.h, SDL_PIXELFORMAT_ARGB8888,
        u->pixels, u->pitch_bytes, regions[i].format,
        mapped + layouts[i].offset, (int)layouts[i].row_pitch);
  }
  SDL_UnmapGPUTransferBuffer(gpu, backend->region_staging);
  return ok;
}

static bool TransferTextureRegions(void *context, const ArRenderTextureRegionCopy *copies,
    unsigned copy_count, const ArRenderTextureRegionUpload *uploads, unsigned upload_count) {
  ArSdlRenderBackend *backend = context;
  if (!backend->output_window || copy_count > kMaxTextureRegions || upload_count > kMaxTextureRegions ||
      !UploadBoundary(backend)) return false;
  SDL_GPUDevice *gpu = backend->gpu_device;
  SDL_GPUTextureLocation from[kMaxTextureRegions], to[kMaxTextureRegions];
  for (unsigned i = 0; i < copy_count; ++i) {
    const ArRenderTextureRegionCopy *c = &copies[i];
    const ArRenderRectI r = c->source_rect;
    const NativeTextureRegion src = NativeRegion(c->source, r);
    const NativeTextureRegion dst = NativeRegion(c->destination,
        (ArRenderRectI){c->destination_x, c->destination_y, r.w, r.h});
    if (!src.texture || !dst.texture || src.texture == dst.texture || src.format != dst.format)
      return false;
    from[i] = (SDL_GPUTextureLocation){.texture = src.texture, .x = r.x, .y = r.y};
    to[i] = (SDL_GPUTextureLocation){.texture = dst.texture, .x = c->destination_x, .y = c->destination_y};
  }
  NativeTextureRegion regions[kMaxTextureRegions];
  ArSdlTextureUploadLayout layouts[kMaxTextureRegions];
  const bool aligned = !strcmp(SDL_GetGPUDeviceDriver(gpu), "direct3d12");
  Uint32 size = 0;
  uint64_t bytes = 0;
  for (unsigned i = 0; i < upload_count; ++i) {
    const ArRenderTextureRegionUpload *u = &uploads[i];
    regions[i] = NativeRegion(u->texture, u->rect);
    if (!regions[i].texture || !u->pixels || u->rect.w > INT_MAX / 4 || u->pitch_bytes < u->rect.w * 4 ||
        !ArSdlTextureUploadLayout_Append(u->rect.w, u->rect.h, aligned, &size, &layouts[i])) return false;
    bytes += (uint64_t)u->rect.w * u->rect.h * 4;
  }
  if ((upload_count && !StageTextureRegions(backend, uploads, regions, layouts, upload_count, size)) ||
      !SubmitPendingSdl(backend)) return false;
  SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(gpu);
  if (!commands) return false;
  if (copy_count) {
    SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(commands);
    if (!pass) { SDL_CancelGPUCommandBuffer(commands); return false; }
    for (unsigned i = 0; i < copy_count; ++i)
      SDL_CopyGPUTextureToTexture(pass, &from[i], &to[i], copies[i].source_rect.w, copies[i].source_rect.h, 1, false);
    SDL_EndGPUCopyPass(pass);
  }
  if (upload_count) {
    SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(commands);
    if (!pass) { SDL_CancelGPUCommandBuffer(commands); return false; }
    for (unsigned i = 0; i < upload_count; ++i) {
      const ArRenderRectI r = uploads[i].rect;
      const SDL_GPUTextureTransferInfo source = {.transfer_buffer = backend->region_staging,
          .offset = layouts[i].offset, .pixels_per_row = layouts[i].row_pitch / 4, .rows_per_layer = r.h};
      const SDL_GPUTextureRegion destination = {.texture = regions[i].texture, .x = r.x, .y = r.y,
          .w = r.w, .h = r.h, .d = 1};
      SDL_UploadToGPUTexture(pass, &source, &destination, false);
    }
    SDL_EndGPUCopyPass(pass);
  }
  const bool ok = SDL_SubmitGPUCommandBuffer(commands);
  if (ok) PerformanceMetrics_AddTextureUpload(upload_count, bytes);
  return ok;
}
