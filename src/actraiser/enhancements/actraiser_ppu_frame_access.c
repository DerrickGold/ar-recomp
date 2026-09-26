/* ActRaiser PPU frame access: the capture, overlay and OBJ-rasterize helpers
 * the enhancement passes use during a frame transaction, over a
 * callback-scoped view of the PPU.
 * Phase: game (frame transaction). */
#include "actraiser/enhancements/actraiser_enhancements_internal.h"

/* ActRaiser is a singleton linked game and frame transactions are synchronous.
 * This callback-scoped adapter keeps the many legacy enhancement helpers on a
 * compact call surface while preventing any concrete PPU pointer from leaking
 * back into them. It is cleared before the callback returns. */
static ActRaiserPpuFrameAccess *s_ppu_frame_access;

void ActRaiser_BeginPpuFrameAccess(ActRaiserPpuFrameAccess *access) {
  s_ppu_frame_access = access;
}

void ActRaiser_EndPpuFrameAccess(void) {
  s_ppu_frame_access = NULL;
}

bool ActRaiser_ClaimOverlayCapture(
    uint32_t source, int x, int y, int width, int height, uint32_t flags) {
  SrPpuStateSnapshot ppu;
  if (!ActRaiser_QueryPpuState(&ppu) || !ActRaiser_RunnerApi() ||
      ActRaiser_RunnerApi()->struct_size < SNES_RUNNER_API_PPU_CAPTURE_CONTROL_SIZE ||
      !ActRaiser_RunnerApi()->claim_ppu_overlay_capture)
    return false;
  const SrPpuOverlayCaptureRequest request = {
    .struct_size = sizeof(request),
    .flags = flags,
    .lifetime_generation = ppu.lifetime_generation,
    .source = source,
    .x = x,
    .y = y,
    .width = width,
    .height = height,
  };
  return ActRaiser_RunnerApi()->claim_ppu_overlay_capture(
             ActRaiser_Runner(), &request) == SR_RESULT_OK;
}

SrPpuOverlayCaptureState ActRaiser_OverlayCaptureState(
    const SrPpuOverlayState *overlay) {
  if (!overlay) return (SrPpuOverlayCaptureState){0};
  return (SrPpuOverlayCaptureState) {
    .x0 = overlay->x0,
    .x1 = overlay->x1,
    .y0 = overlay->y0,
    .y1 = overlay->y1,
    .flags = overlay->flags,
    .transparent_fill_configured = overlay->transparent_fill_configured,
    .transparent_fill_mode = overlay->transparent_fill_mode,
    .transparent_fill_cgram = overlay->transparent_fill_cgram,
    .oam_first = overlay->oam_first,
    .oam_count = overlay->oam_count,
  };
}

bool ActRaiser_ExchangeOverlayCapture(
    uint32_t source, uint64_t generation,
    const SrPpuOverlayCaptureState *expected,
    const SrPpuOverlayCaptureState *replacement) {
  if (source >= SR_PPU_OVERLAY_SOURCE_COUNT || !expected || !replacement ||
      !ActRaiser_Runner() || !ActRaiser_RunnerApi() ||
      ActRaiser_RunnerApi()->struct_size <
          SNES_RUNNER_API_PPU_FRAME_TRANSACTION_SIZE ||
      !ActRaiser_RunnerApi()->compare_exchange_ppu_overlay_captures)
    return false;
  SrPpuOverlayCaptureExchangeRequest request = {
    .struct_size = sizeof(request),
    .lifetime_generation = generation,
    .source_mask = UINT32_C(1) << source,
  };
  request.expected[source] = *expected;
  request.replacement[source] = *replacement;
  return ActRaiser_RunnerApi()->compare_exchange_ppu_overlay_captures(
             ActRaiser_Runner(), &request) == SR_RESULT_OK;
}

const SrPpuFrameTransactionContext *ActRaiser_PpuFrame(void) {
  return s_ppu_frame_access ? s_ppu_frame_access->context : NULL;
}

const SrPpuOverlayCaptureState *ActRaiser_PpuCapture(
    uint32_t source) {
  return s_ppu_frame_access && source < SR_PPU_OVERLAY_SOURCE_COUNT
      ? &s_ppu_frame_access->captures[source]
      : NULL;
}

static bool ActRaiser_SetPpuCaptureState(
    uint32_t source, const SrPpuOverlayCaptureState *replacement) {
  if (!s_ppu_frame_access || source >= SR_PPU_OVERLAY_SOURCE_COUNT ||
      !replacement)
    return false;
  SrPpuOverlayCaptureState *current =
      &s_ppu_frame_access->captures[source];
  if (!ActRaiser_ExchangeOverlayCapture(
          source, s_ppu_frame_access->context->lifetime_generation,
          current, replacement))
    return false;
  *current = *replacement;
  return true;
}

bool ActRaiser_SetPpuOverlayCapture(
    uint32_t source, int x, int y, int width, int height, uint32_t flags) {
  const SrPpuOverlayCaptureState *current = ActRaiser_PpuCapture(source);
  if (!current || width <= 0 || height <= 0) return false;
  SrPpuOverlayCaptureState replacement = *current;
  replacement.x0 = (int16_t)x;
  replacement.x1 = (int16_t)(x + width);
  replacement.y0 = (int16_t)y;
  replacement.y1 = (int16_t)(y + height);
  replacement.flags = flags;
  replacement.oam_first = 0u;
  replacement.oam_count = 0u;
  return ActRaiser_SetPpuCaptureState(source, &replacement);
}

bool ActRaiser_SetPpuOverlayFill(
    uint32_t source, SrPpuTransparentFillMode mode, uint8_t cgram_index) {
  const SrPpuOverlayCaptureState *current = ActRaiser_PpuCapture(source);
  if (!current) return false;
  SrPpuOverlayCaptureState replacement = *current;
  replacement.transparent_fill_configured = 1u;
  replacement.transparent_fill_mode = (uint8_t)mode;
  replacement.transparent_fill_cgram = cgram_index;
  return ActRaiser_SetPpuCaptureState(source, &replacement);
}

bool ActRaiser_SetPpuOverlayOamRange(
    uint8_t first, uint8_t count) {
  const SrPpuOverlayCaptureState *current =
      ActRaiser_PpuCapture(SR_PPU_OVERLAY_OBJ);
  if (!current || count == 0u || first >= 128u || count > 128u - first)
    return false;
  SrPpuOverlayCaptureState replacement = *current;
  replacement.oam_first = first;
  replacement.oam_count = count;
  return ActRaiser_SetPpuCaptureState(
      SR_PPU_OVERLAY_OBJ, &replacement);
}

bool ActRaiser_BindPpuOutput(
    SrPpuOutputKind kind, uint32_t source, uint32_t band,
    uint8_t *pixels, size_t pitch, uint32_t height) {
  if (!ActRaiser_Runner() || !ActRaiser_RunnerApi() ||
      ActRaiser_RunnerApi()->struct_size < SNES_RUNNER_API_PPU_OUTPUT_CONTROL_SIZE ||
      !ActRaiser_RunnerApi()->bind_ppu_output_surface || pitch > UINT64_MAX / height)
    return false;
  const SrPpuOutputBindingRequest request = {
    .struct_size = sizeof(request),
    .lifetime_generation = s_ppu_frame_access
        ? s_ppu_frame_access->context->lifetime_generation : 0u,
    .kind = kind,
    .source = source,
    .band = band,
    .pixels = pixels,
    .pixel_byte_size = (uint64_t)pitch * height,
    .pitch_bytes = pitch,
    .height_pixels = height,
  };
  return ActRaiser_RunnerApi()->bind_ppu_output_surface(
             ActRaiser_Runner(), &request) == SR_RESULT_OK;
}

bool ActRaiser_ResolvePpuObjRange(
    uint8_t first, uint8_t count, uint8_t priority,
    SrPpuObjResolveResult *result) {
  SrPpuObjPart parts[128];
  const SrPpuFrameTransactionContext *frame = ActRaiser_PpuFrame();
  if (!frame || !result || count == 0u || !ActRaiser_RunnerApi() ||
      ActRaiser_RunnerApi()->struct_size < SNES_RUNNER_API_PPU_OBJ_RESOLVE_SIZE ||
      !ActRaiser_RunnerApi()->resolve_ppu_obj_range)
    return false;
  const SrPpuObjResolveRequest request = {
    .struct_size = sizeof(request),
    .lifetime_generation = frame->lifetime_generation,
    .first_sprite = first,
    .sprite_count = count,
    .priority = priority,
    .part_capacity = 128u,
    .parts = parts,
  };
  *result = (SrPpuObjResolveResult){
    .struct_size = sizeof(*result),
  };
  return ActRaiser_RunnerApi()->resolve_ppu_obj_range(
             ActRaiser_Runner(), &request, result) == SR_RESULT_OK;
}

bool ActRaiser_RasterizePpuObjRange(
    uint8_t first, uint8_t count, uint8_t priority,
    uint32_t *pixels, size_t pitch, size_t byte_size,
    SrPpuObjRasterResult *result) {
  const SrPpuFrameTransactionContext *frame = ActRaiser_PpuFrame();
  if (!frame || !pixels || !result || !ActRaiser_RunnerApi() ||
      ActRaiser_RunnerApi()->struct_size < SNES_RUNNER_API_PPU_OBJ_RASTER_SIZE ||
      !ActRaiser_RunnerApi()->rasterize_ppu_obj_range)
    return false;
  const SrPpuObjRasterRequest request = {
    .struct_size = sizeof(request),
    .lifetime_generation = frame->lifetime_generation,
    .first_sprite = first,
    .sprite_count = count,
    .priority = priority,
    .pixel_format = SR_PPU_OBJ_PIXEL_FORMAT_ARGB8888_U32,
    .pixels = pixels,
    .pixel_byte_size = byte_size,
    .pitch_bytes = pitch,
  };
  *result = (SrPpuObjRasterResult){
    .struct_size = sizeof(*result),
  };
  return ActRaiser_RunnerApi()->rasterize_ppu_obj_range(
             ActRaiser_Runner(), &request, result) == SR_RESULT_OK;
}

bool ActRaiser_RasterizePpuObjParts(
    const SrPpuObjPart *parts, size_t part_count,
    int x0, int y0, int x1, int y1,
    uint32_t *pixels, size_t pitch, size_t byte_size) {
  const SrPpuFrameTransactionContext *frame = ActRaiser_PpuFrame();
  SrPpuObjRasterResult result = {.struct_size = sizeof(result)};
  if (!frame || !parts || !part_count || !pixels || !ActRaiser_RunnerApi() ||
      ActRaiser_RunnerApi()->struct_size < SNES_RUNNER_API_PPU_OBJ_PARTS_SIZE ||
      !ActRaiser_RunnerApi()->rasterize_ppu_obj_parts)
    return false;
  const SrPpuObjPartsRasterRequest request = {
    .struct_size = sizeof(request),
    .lifetime_generation = frame->lifetime_generation,
    .parts = parts,
    .part_count = part_count,
    .x0 = x0,
    .y0 = y0,
    .x1 = x1,
    .y1 = y1,
    .pixel_format = SR_PPU_OBJ_PIXEL_FORMAT_ARGB8888_U32,
    .pixels = pixels,
    .pixel_byte_size = byte_size,
    .pitch_bytes = pitch,
  };
  return ActRaiser_RunnerApi()->rasterize_ppu_obj_parts(
             ActRaiser_Runner(), &request, &result) == SR_RESULT_OK;
}

bool ActRaiser_ConfigurePpuObjCapture(
    const SrPpuObjCaptureRequest *request) {
  return request && ActRaiser_Runner() && ActRaiser_RunnerApi() &&
      ActRaiser_RunnerApi()->struct_size >= SNES_RUNNER_API_PPU_OBJ_CAPTURE_SIZE &&
      ActRaiser_RunnerApi()->configure_ppu_obj_capture &&
      ActRaiser_RunnerApi()->configure_ppu_obj_capture(
          ActRaiser_Runner(), request) == SR_RESULT_OK;
}
