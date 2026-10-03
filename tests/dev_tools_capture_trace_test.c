/* Exercise the actual PPM capture function. Only its producer/presenter/
 * readback dependencies are stubs; no GPU claim is made. */
#include "dev/dev_tools.h"
#include "host/host_display.h"
#include "present/presentation_frame_generation.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <string.h>

static int captured, uploaded, presented, readbacks, released;
static bool fatal, fatal_upload, fatal_present;
static float expected_phase;
static const FrameSlot *drawn_slot;
static const ArRenderRectI actual_viewport = {1, 0, 2, 2};
static uint8_t rgb[24];

void Diorama_CaptureCameraPresentationState(DioramaCameraPresentationState *state) {
  *state = (DioramaCameraPresentationState){.zoom_offset = 1.25f};
}

void FrameSlot_Capture(FrameSlot *slot, const SimFrameData *sim) {
  assert(!sim);
  ++captured;
  *slot = (FrameSlot){.snes_width = 256, .snes_height = 224, .visible_width = 256};
}
void PresentUpload(const FrameSlot *slot) {
  assert(slot->diorama_camera.controls.zoom_offset == 1.25f);
  ++uploaded;
  drawn_slot = slot;
  if (fatal_upload) fatal = true;
}
ArRenderRectI PresentFrame(const FrameSlot *slot, float alpha, double fps) {
  assert(slot == drawn_slot && alpha == expected_phase && fps == 60);
  ++presented;
  if (fatal_present) fatal = true; /* Renderer latches fatal, still returns a rect. */
  return actual_viewport;
}
double HostDisplay_FramesPerSecond(void) { return 60; }
int Settings_VisibleWidth(void) { return 2; }
int Settings_VisibleX0(void) { return 0; }
bool SessionFatal_Requested(void) { return fatal; }

static void Release(void *owner) {
  assert(owner == rgb);
  ++released;
}
static bool Readback(void *context, DevToolsRgb24Capture *out) {
  assert(context == rgb && presented == 1);
  ++readbacks;
  *out = (DevToolsRgb24Capture){
    .pixels = rgb, .width = 4, .height = 2, .pitch_bytes = 12,
    .owner = rgb, .release = Release,
  };
  return true;
}

static void Reset(void) {
  captured = uploaded = presented = readbacks = released = 0;
  fatal = fatal_upload = fatal_present = false;
  expected_phase = kPresentationFrameGenerationPhaseNone;
}

int main(void) {
  static uint8_t native[16];
  DevToolsContext context = {
    .readback = {.capture_rgb24 = Readback, .context = rgb}, .hud_bg_texture = {1},
    .framebuffer_pixels = native, .framebuffer_pitch = 8, .snes_height = 2,
  };
  memset(rgb, 0x73, sizeof(rgb));

  /* An ordinary composite is read back once and written as a PPM. */
  Reset();
  FILE *file = tmpfile();
  assert(file);
  DevToolsCaptureResult result = DevTools_WriteFramebufferPpm(file, &context, true);
  assert(result.kind == kDevToolsCapture_Composite);
  assert(result.width == 4 && result.height == 2);
  assert(captured == 1 && uploaded == 1 && presented == 1);
  assert(readbacks == 1 && released == 1);
  rewind(file);
  char ppm[64] = {0};
  const size_t bytes = fread(ppm, 1, sizeof(ppm), file);
  assert(bytes == 35 && !memcmp(ppm, "P6\n4 2\n255\n", 11));
  assert(!memcmp(ppm + 11, rgb, 24));
  assert(fclose(file) == 0);

  /* Fatal render with a nonempty viewport: failed, no readback, no native
   * fallback (even when allowed), and no PPM bytes written. */
  for (int allow_native = 0; allow_native < 2; ++allow_native) {
    Reset();
    fatal_present = true;
    file = tmpfile();
    assert(file);
    result = DevTools_WriteFramebufferPpm(file, &context, !allow_native);
    assert(result.kind == kDevToolsCapture_Failed && !result.width && !result.height);
    assert(captured == 1 && uploaded == 1 && presented == 1 && fatal);
    assert(!readbacks && !released);
    assert(fflush(file) == 0 && ftell(file) == 0);
    assert(fclose(file) == 0);
  }
  /* A resident/interpolated capture borrows an already uploaded frame. It
   * must obey the same failure contract without capturing or uploading again. */
  const FrameSlot retained = {.snes_width = 256, .snes_height = 224};
  for (int fail = 0; fail < 2; ++fail) {
    Reset();
    drawn_slot = &retained;
    expected_phase = .5f;
    fatal_present = fail;
    file = tmpfile();
    assert(file);
    result = DevTools_WriteFramebufferPpmAtPhase(file, &context, false, .5f, &retained);
    assert(!captured && !uploaded && presented == 1);
    assert(result.kind == (fail ? kDevToolsCapture_Failed : kDevToolsCapture_Composite));
    assert(readbacks == !fail && released == !fail);
    if (fail) assert(fflush(file) == 0 && ftell(file) == 0);
    assert(fclose(file) == 0);
  }
  /* Do not submit a failed upload or start a capture after session failure. */
  for (int already_fatal = 0; already_fatal < 2; ++already_fatal) {
    Reset();
    fatal = already_fatal;
    fatal_upload = !already_fatal;
    file = tmpfile();
    assert(file);
    result = DevTools_WriteFramebufferPpm(file, &context, false);
    assert(result.kind == kDevToolsCapture_Failed && !result.width && !result.height);
    assert(captured == !already_fatal && uploaded == !already_fatal);
    assert(!presented && !readbacks && !released);
    assert(fflush(file) == 0 && ftell(file) == 0);
    assert(fclose(file) == 0);
  }
  puts("dev_tools_capture_trace_test: PASS");
  return 0;
}
