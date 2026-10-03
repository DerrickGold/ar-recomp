#ifndef AR_DIORAMA_GPU_POLICY_H
#define AR_DIORAMA_GPU_POLICY_H

#include <stdbool.h>
#include "actraiser_game.h"
#include <stdlib.h>
#include <string.h>

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

static inline DioramaGpuPolicy DioramaGpuPolicy_Resolve(unsigned group, unsigned room,
    const char *pipeline, const char *capture, const char *motion,
    const char *handoff, const char *projection) {
  const bool automatic = group <= 255 && room <= 255 && ActRaiser_IsActionMap(group, room);
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

static inline DioramaGpuPolicy DioramaGpuPolicy_ForRoom(unsigned group, unsigned room) {
  return DioramaGpuPolicy_Resolve(group, room, getenv("AR_GPU_PIPELINE"),
      getenv("AR_GPU_BG_CAPTURE"), getenv("AR_GPU_BG_MOTION"),
      getenv("AR_GPU_FRAME_HANDOFF"), getenv("AR_GPU_EFFECT_PROJECTION"));
}

#endif
