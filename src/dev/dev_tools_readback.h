#ifndef AR_DEV_TOOLS_READBACK_H
#define AR_DEV_TOOLS_READBACK_H

#include <stdbool.h>
#include <stdint.h>

typedef enum DevToolsCaptureKind {
  kDevToolsCapture_Failed = 0,
  kDevToolsCapture_Composite,
  kDevToolsCapture_NativeFramebuffer,
} DevToolsCaptureKind;

typedef struct DevToolsCaptureResult {
  int width, height;
  DevToolsCaptureKind kind;
} DevToolsCaptureResult;

static inline const char *DevToolsCaptureKind_Name(DevToolsCaptureKind kind) {
  switch (kind) {
    case kDevToolsCapture_Composite: return "final-composite";
    case kDevToolsCapture_NativeFramebuffer: return "native-framebuffer";
    default: return "failed";
  }
}

typedef struct DevToolsRgb24Capture {
  /* Rows contain width*3 RGB bytes followed by any provider-owned padding. */
  const uint8_t *pixels;
  int width;
  int height;
  int pitch_bytes;
  void *owner;
  void (*release)(void *owner);
} DevToolsRgb24Capture;

typedef bool (*DevToolsCaptureRgb24Fn)(
    void *context, DevToolsRgb24Capture *capture);

typedef struct DevToolsReadbackProvider {
  DevToolsCaptureRgb24Fn capture_rgb24;
  void *context;
} DevToolsReadbackProvider;

#endif /* AR_DEV_TOOLS_READBACK_H */
