#ifndef AR_DIORAMA_GPU_POLICY_H
#define AR_DIORAMA_GPU_POLICY_H

#include <stdbool.h>
#include "actraiser_game.h"
#include <stdlib.h>
#include <string.h>
#include "snesrecomp/runner/ppu_bg_packet.h"

/* One policy shared by producer and presenter. All action rooms use the same
 * pipeline. Configuration is read-only; per-stage overrides are diagnostics.
 * Backend capability and exceptional recovery remain presenter decisions. */
typedef enum DioramaGpuCaptureMode {
  kDioramaGpuCapture_Off, kDioramaGpuCapture_Pixels, kDioramaGpuCapture_Validate,
  kDioramaGpuCapture_Owned, kDioramaGpuCapture_TilesValidate
} DioramaGpuCaptureMode;
typedef enum DioramaGpuMotionMode {
  kDioramaGpuMotion_Off, kDioramaGpuMotion_Compute,
  kDioramaGpuMotion_Validate, kDioramaGpuMotion_Owned
} DioramaGpuMotionMode;
typedef struct DioramaGpuPolicy {
  DioramaGpuCaptureMode capture;
  DioramaGpuMotionMode motion;
  bool pack, unpack, resident;
} DioramaGpuPolicy;

static inline bool DioramaGpuPolicy_Is(const char *value, const char *expected) {
  return value && !strcmp(value, expected);
}

/* Full-add ownership uses the existing decoder. The PPU still proves each
 * row eligible; unsupported policies retain CPU pixels. Explicit 0 (or an
 * unknown diagnostic value) disables this extension without disabling the
 * rest of the GPU pipeline. */
static inline uint32_t DioramaGpuPolicy_CaptureFlags(DioramaGpuCaptureMode mode,
                                                    const char *full_add) {
  if (mode != kDioramaGpuCapture_Owned && mode != kDioramaGpuCapture_TilesValidate)
    return 0;
  uint32_t flags = SR_PPU_BG_PACKET_TILES;
  if (mode == kDioramaGpuCapture_TilesValidate) flags |= SR_PPU_BG_PACKET_VALIDATE_TILES;
  if (!full_add || !*full_add || DioramaGpuPolicy_Is(full_add, "auto") ||
      DioramaGpuPolicy_Is(full_add, "1")) flags |= SR_PPU_BG_PACKET_FULL_ADD;
  return flags;
}

static inline DioramaGpuPolicy DioramaGpuPolicy_ResolveAutomatic(bool automatic,
    const char *pipeline, const char *capture, const char *motion,
    const char *handoff, const char *projection) {
  DioramaGpuPolicy p = {0};
  if (pipeline && *pipeline && !DioramaGpuPolicy_Is(pipeline, "auto")) return p;
  p.capture = capture ?
      DioramaGpuPolicy_Is(capture, "owned") ? kDioramaGpuCapture_Owned :
      DioramaGpuPolicy_Is(capture, "tiles-validate") ? kDioramaGpuCapture_TilesValidate :
      DioramaGpuPolicy_Is(capture, "validate") ? kDioramaGpuCapture_Validate :
      DioramaGpuPolicy_Is(capture, "1") ? kDioramaGpuCapture_Pixels : kDioramaGpuCapture_Off :
      automatic ? kDioramaGpuCapture_Owned : kDioramaGpuCapture_Off;
  p.motion = motion ?
      DioramaGpuPolicy_Is(motion, "owned") ? kDioramaGpuMotion_Owned :
      DioramaGpuPolicy_Is(motion, "validate") ? kDioramaGpuMotion_Validate :
      (DioramaGpuPolicy_Is(motion, "1") || DioramaGpuPolicy_Is(motion, "compute"))
          ? kDioramaGpuMotion_Compute : kDioramaGpuMotion_Off :
      automatic ? kDioramaGpuMotion_Owned : kDioramaGpuMotion_Off;
  p.pack = handoff ? DioramaGpuPolicy_Is(handoff, "all") : automatic;
  p.unpack = p.pack || DioramaGpuPolicy_Is(handoff, "unpack");
  p.resident = (projection ? DioramaGpuPolicy_Is(projection, "resident") : automatic) &&
      p.pack && p.motion == kDioramaGpuMotion_Owned;
  return p;
}

static inline DioramaGpuPolicy DioramaGpuPolicy_Resolve(unsigned group, unsigned room,
    const char *pipeline, const char *capture, const char *motion,
    const char *handoff, const char *projection) {
  const bool automatic = group <= 255 && room <= 255 && ActRaiser_IsActionMap(group, room);
  return DioramaGpuPolicy_ResolveAutomatic(automatic, pipeline, capture, motion, handoff,
                                           projection);
}

static inline DioramaGpuPolicy DioramaGpuPolicy_ForRoom(unsigned group, unsigned room) {
  return DioramaGpuPolicy_Resolve(group, room, getenv("AR_GPU_PIPELINE"),
      getenv("AR_GPU_BG_CAPTURE"), getenv("AR_GPU_BG_MOTION"),
      getenv("AR_GPU_FRAME_HANDOFF"), getenv("AR_GPU_EFFECT_PROJECTION"));
}

/* What every action room resolves to, for preparing its pipelines before the
 * first stage. Honors the same AR_GPU_* overrides. */
static inline DioramaGpuPolicy DioramaGpuPolicy_ForActionRooms(void) {
  return DioramaGpuPolicy_ResolveAutomatic(true, getenv("AR_GPU_PIPELINE"),
      getenv("AR_GPU_BG_CAPTURE"), getenv("AR_GPU_BG_MOTION"),
      getenv("AR_GPU_FRAME_HANDOFF"), getenv("AR_GPU_EFFECT_PROJECTION"));
}

#endif
