#ifdef NDEBUG
#undef NDEBUG
#endif
#include "present_sim3d_canvas.h"
#include "sim/sim_town_canvas.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t source[kSimTownCanvasPixels * kSimTownCanvasPixels];
static uint32_t uploaded[kSimTownCanvasPixels * kSimTownCanvasPixels];
static uint32_t serial = 1;
static ArRenderRectI dirty[4], last_upload;
static unsigned dirty_count, dirty_at, creates, updates, destroys, fail_at;

uint32_t SimTownCanvas_Serial(void) { return serial; }
const uint32_t *SimTownCanvas_Pixels(void) { return source; }
bool SimTownCanvas_TakeDirtyRect(int *x, int *y, int *w, int *h) {
  if (dirty_at == dirty_count) return false;
  const ArRenderRectI rect = dirty[dirty_at++];
  *x = rect.x; *y = rect.y; *w = rect.w; *h = rect.h;
  return true;
}
void Sim3DPerformance_AddUpload(uint64_t bytes) { assert(bytes); }

static bool Create(void *context, const ArRenderTextureDesc *desc, ArRenderTexture *out) {
  (void)context;
  assert(desc->width == kSimTownCanvasPixels && desc->height == kSimTownCanvasPixels);
  *out = (ArRenderTexture){++creates};
  memset(uploaded, 0xa5, sizeof(uploaded));
  return true;
}
static void Destroy(void *context, ArRenderTexture texture) {
  (void)context;
  if (ArRenderTexture_IsValid(texture)) ++destroys;
}
static const char *Error(void *context) { (void)context; return "injected failure"; }
static bool Update(void *context, ArRenderTexture texture, const ArRenderRectI *rect,
    const void *pixels, int pitch) {
  (void)context; (void)texture;
  assert(rect && pixels && pitch == kSimTownCanvasPixels * 4);
  last_upload = *rect;
  if (++updates == fail_at) return false;
  for (int y = 0; y < rect->h; ++y)
    memcpy(uploaded + (rect->y + y) * kSimTownCanvasPixels + rect->x,
        (const uint8_t *)pixels + y * pitch, rect->w * sizeof(uint32_t));
  return true;
}

static void Change(int x, int y) {
  source[y * kSimTownCanvasPixels + x] = ++serial;
  dirty_count = 1; dirty_at = 0;
  dirty[0] = (ArRenderRectI){x, y, 1, 1};
}
static void CheckFull(void) {
  assert(last_upload.x == 0 && last_upload.y == 0);
  assert(last_upload.w == kSimTownCanvasPixels && last_upload.h == kSimTownCanvasPixels);
  assert(memcmp(source, uploaded, sizeof(source)) == 0);
  assert(ArRenderTexture_IsValid(PresentSim3DCanvas_Texture(serial)));
  assert(dirty_at == dirty_count);
}

int main(void) {
  const ArRenderBackendOps ops = {.create_texture = Create, .destroy_texture = Destroy,
    .update_texture = Update, .last_error = Error};
  ArRenderDevice device = {.ops = &ops, .context = &creates};
  PresentSim3DCanvas_Upload(&device, false);
  assert(!creates && !updates);
  PresentSim3DCanvas_Upload(&device, true);
  assert(creates == 1 && updates == 1);
  CheckFull();
  PresentSim3DCanvas_Upload(&device, true);
  assert(updates == 1);

  Change(5, 7);
  PresentSim3DCanvas_Upload(&device, true);
  assert(updates == 2 && last_upload.w == 1 && last_upload.h == 1);
  assert(!memcmp(source, uploaded, sizeof(source)));

  PresentSim3DCanvas_Upload(&device, false);
  assert(!ArRenderTexture_IsValid(PresentSim3DCanvas_Texture(serial)));
  Change(200, 100);
  PresentSim3DCanvas_Upload(&device, false);
  Change(400, 400); /* The next frame replaces the previous dirty rectangle. */
  PresentSim3DCanvas_Upload(&device, false);
  assert(updates == 2);
  PresentSim3DCanvas_Upload(&device, true);
  assert(updates == 3 && creates == 1);
  CheckFull();

  Change(0, 0);
  source[kSimTownCanvasPixels * kSimTownCanvasPixels - 1] = serial;
  dirty[1] = (ArRenderRectI){kSimTownCanvasPixels - 1, kSimTownCanvasPixels - 1, 1, 1};
  dirty_count = 2;
  fail_at = updates + 2;
  PresentSim3DCanvas_Upload(&device, true);
  assert(!ArRenderTexture_IsValid(PresentSim3DCanvas_Texture(serial)));
  fail_at = 0;
  PresentSim3DCanvas_Upload(&device, true);
  CheckFull();

  Change(10, 10);
  fail_at = updates + 1;
  PresentSim3DCanvas_Upload(&device, true);
  fail_at = updates + 1;
  PresentSim3DCanvas_Upload(&device, true);
  assert(destroys == 1 && !ArRenderTexture_IsValid(PresentSim3DCanvas_Texture(serial)));
  const unsigned stopped = updates;
  PresentSim3DCanvas_Upload(&device, false);
  PresentSim3DCanvas_Upload(&device, true);
  assert(updates == stopped);
  PresentSim3DCanvas_Reset(&device);
  fail_at = 0;
  PresentSim3DCanvas_Upload(&device, true);
  assert(creates == 2);
  CheckFull();
  PresentSim3DCanvas_Reset(&device);
  puts("present_sim3d_canvas_test: PASS");
  return 0;
}
