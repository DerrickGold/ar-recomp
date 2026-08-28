/* Unit tests for the HD replacement manifest parser and gate evaluator.
 * Links hd_replacements.c against a fake public runner ABI so no concrete
 * PPU, renderer, or SDL dependency can leak back into the application. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hd_replacements.h"
#include "settings.h"

static int g_failures;
#define CHECK(cond) do { \
  if (!(cond)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
    g_failures++; \
  } \
} while (0)

/* ---- stubs -------------------------------------------------------------- */

uint8 g_ram[0x20000];
Settings g_settings;
static uint8_t g_runner_storage;
static SrPpuStateSnapshot g_ppu_state;
static int g_ppu_query_calls;
static bool g_overlay_busy[SR_PPU_OVERLAY_SOURCE_COUNT];
static bool g_mode7_busy;

static struct {
  int calls;
  int source, x, y, width, height;
  uint8_t flags;
} g_capture_log;

static struct {
  int calls;
  int width, height, x0, y0, x1, y1;
} g_m7_log;

static SrResult QueryPpuState(
    SrRunnerHandle *runner, SrPpuStateSnapshot *out_state) {
  if (runner != (SrRunnerHandle *)&g_runner_storage || !out_state ||
      out_state->struct_size < SR_PPU_STATE_SNAPSHOT_V2_SIZE)
    return SR_RESULT_INVALID_ARGUMENT;
  g_ppu_query_calls++;
  *out_state = g_ppu_state;
  out_state->struct_size = sizeof(*out_state);
  return SR_RESULT_OK;
}

static SrResult ClaimOverlayCapture(
    SrRunnerHandle *runner, const SrPpuOverlayCaptureRequest *request) {
  if (runner != (SrRunnerHandle *)&g_runner_storage || !request ||
      request->source >= SR_PPU_OVERLAY_SOURCE_COUNT)
    return SR_RESULT_INVALID_ARGUMENT;
  if (g_overlay_busy[request->source]) return SR_RESULT_BUSY;
  g_overlay_busy[request->source] = true;
  g_capture_log.calls++;
  g_capture_log.source = (int)request->source;
  g_capture_log.x = request->x;
  g_capture_log.y = request->y;
  g_capture_log.width = request->width;
  g_capture_log.height = request->height;
  g_capture_log.flags = (uint8_t)request->flags;
  return SR_RESULT_OK;
}

static SrResult ClaimMode7Override(
    SrRunnerHandle *runner, const SrPpuMode7OverrideRequest *request) {
  if (runner != (SrRunnerHandle *)&g_runner_storage || !request)
    return SR_RESULT_INVALID_ARGUMENT;
  if (g_mode7_busy) return SR_RESULT_BUSY;
  g_mode7_busy = true;
  g_m7_log.calls++;
  g_m7_log.width = (int)request->width_pixels;
  g_m7_log.height = (int)request->height_pixels;
  g_m7_log.x0 = request->canvas_x0;
  g_m7_log.y0 = request->canvas_y0;
  g_m7_log.x1 = request->canvas_x1;
  g_m7_log.y1 = request->canvas_y1;
  return SR_RESULT_OK;
}

static const SnesRunnerApi kRunnerApi = {
    .abi_version = SR_RUNNER_ABI_VERSION,
    .struct_size = sizeof(SnesRunnerApi),
    .capabilities = SR_RUNNER_CAP_PPU_STATE |
                    SR_RUNNER_CAP_PPU_CAPTURE_CONTROL,
    .query_ppu_state = QueryPpuState,
    .claim_ppu_overlay_capture = ClaimOverlayCapture,
    .claim_ppu_mode7_override = ClaimMode7Override,
};

const SnesRunnerApi *sr_runner_get_api(uint32_t requested_abi_version) {
  return requested_abi_version == SR_RUNNER_ABI_VERSION ? &kRunnerApi : NULL;
}

/* ---- helpers ------------------------------------------------------------ */

static const char *WriteManifest(const char *body) {
  static char path[512];
  const char *dir = getenv("TMPDIR");
  snprintf(path, sizeof(path), "%s/hd_manifest_test.ini", dir ? dir : "/tmp");
  FILE *f = fopen(path, "w");
  if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(1); }
  fputs(body, f);
  fclose(f);
  return path;
}

static void ResetRuntime(void) {
  memset(g_ram, 0, sizeof(g_ram));
  memset(&g_ppu_state, 0, sizeof(g_ppu_state));
  g_ppu_state.struct_size = sizeof(g_ppu_state);
  g_ppu_state.lifetime_generation = 7u;
  g_ppu_query_calls = 0;
  memset(g_overlay_busy, 0, sizeof(g_overlay_busy));
  g_mode7_busy = false;
  memset(&g_capture_log, 0, sizeof(g_capture_log));
  memset(&g_m7_log, 0, sizeof(g_m7_log));
  memset(&g_settings, 0, sizeof(g_settings));
  g_settings.hd_replacements = true;
  HdReplacements_BindRunner((SrRunnerHandle *)&g_runner_storage);
}

static void MakeTitleState(void) {
  g_ram[0x18] = 0x00;
  g_ram[0x19] = 0x00;
  g_ppu_state.bg_mode = 7;
  g_ppu_state.mode7_matrix[0] = 0x0100;
  g_ppu_state.mode7_matrix[1] = 0;
  g_ppu_state.mode7_matrix[2] = 0;
  g_ppu_state.mode7_matrix[3] = 0x0100;
}

static const char kTitleManifest[] =
    "# comment\n"
    "[replace:title-logo]\n"
    "plane = screen\n"
    "layer = bg1\n"
    "rect = 11,27,248,122\n"
    "image = title-logo.png\n"
    "when = wram[0018]==0x00, wram[0019]==0x00, mode==7, m7==identity\n";

/* ---- tests -------------------------------------------------------------- */

static void TestParseTitleEntry(void) {
  const char *path = WriteManifest(kTitleManifest);
  CHECK(HdReplacements_Load(path) == 1);
  const HdReplacement *e = &g_hd_replacements[0];
  CHECK(!strcmp(e->name, "title-logo"));
  CHECK(e->plane == kHdPlane_Screen);
  CHECK(e->source == SR_PPU_OVERLAY_BG1);
  CHECK(e->x0 == 11 && e->y0 == 27 && e->x1 == 248 && e->y1 == 122);
  CHECK(strstr(e->image, "title-logo.png") != NULL);
  /* image resolves relative to the manifest directory */
  CHECK(strncmp(e->image, path, strlen(path) - strlen("hd_manifest_test.ini"))
        == 0);
  CHECK(e->brightness_mod);
  CHECK(e->condition_count == 4);
  CHECK(e->conditions[0].kind == kHdCond_WramByte);
  CHECK(e->conditions[0].address == 0x18);
  CHECK(e->conditions[0].value == 0);
  CHECK(e->conditions[2].kind == kHdCond_BgMode);
  CHECK(e->conditions[2].value == 7);
  CHECK(e->conditions[3].kind == kHdCond_M7Identity);
}

static void TestParseRejections(void) {
  /* Missing rect on a screen entry drops it; the next entry still parses. */
  CHECK(HdReplacements_Load(WriteManifest(
      "[replace:broken]\n"
      "layer = bg2\n"
      "image = x.png\n"
      "when = mode==1\n"
      "[replace:ok]\n"
      "layer = bg3\n"
      "rect = 0,0,256,40\n"
      "image = y.png\n"
      "when = wram[0018]!=0x01, m7b==0x0000\n")) == 1);
  CHECK(!strcmp(g_hd_replacements[0].name, "ok"));
  CHECK(g_hd_replacements[0].conditions[0].negate == 1);
  CHECK(g_hd_replacements[0].conditions[1].kind == kHdCond_M7Element);
  CHECK(g_hd_replacements[0].conditions[1].address == 1);

  /* Bad condition syntax drops the entry. */
  CHECK(HdReplacements_Load(WriteManifest(
      "[replace:bad-cond]\n"
      "layer = bg1\n"
      "rect = 0,0,8,8\n"
      "image = x.png\n"
      "when = m7==rotated\n")) == 0);

  /* Unknown plane value drops the entry. */
  CHECK(HdReplacements_Load(WriteManifest(
      "[replace:bad-plane]\n"
      "plane = hologram\n"
      "layer = bg1\n"
      "rect = 0,0,8,8\n"
      "image = x.png\n"
      "when = mode==7\n")) == 0);

  /* Missing manifest file is silent and empty. */
  CHECK(HdReplacements_Load("/nonexistent/manifest.ini") == 0);
}

static void TestReservedPlanesParseButStayInert(void) {
  CHECK(HdReplacements_Load(WriteManifest(
      "[replace:everything]\n"
      "plane = tiles\n"
      "image = pack.png\n"
      "when = wram[0018]==0x00\n")) == 1);
  ResetRuntime();
  MakeTitleState();
  g_hd_replacements[0].texture = (ArRenderTexture){1}; /* even with art bound */
  g_hd_replacements[0].pixels = (void *)0x1;
  HdReplacements_EvaluateFrame();
  CHECK(g_capture_log.calls == 0 && g_m7_log.calls == 0);
  CHECK(!g_hd_replacements[0].active);
}

static void TestMode7Entries(void) {
  /* mode7 requires canvas_rect: entry without one is dropped. */
  CHECK(HdReplacements_Load(WriteManifest(
      "[replace:no-canvas]\n"
      "plane = mode7\n"
      "image = map.png\n"
      "when = mode==7\n")) == 0);

  CHECK(HdReplacements_Load(WriteManifest(
      "[replace:title-swirl]\n"
      "plane = mode7\n"
      "canvas_rect = 139,156,376,251\n"
      "image = logo.png\n"
      "when = wram[0018]==0x00, mode==7, m7!=identity\n")) == 1);
  ResetRuntime();
  MakeTitleState();

  /* Settled identity matrix: m7!=identity fails, no override. */
  g_hd_replacements[0].pixels = (void *)0x1;
  g_hd_replacements[0].pixels_width = 2048;
  g_hd_replacements[0].pixels_height = 820;
  HdReplacements_EvaluateFrame();
  CHECK(g_m7_log.calls == 0 && !g_hd_replacements[0].active);

  /* Mid-swirl matrix: override requested with the canvas rect. */
  g_ppu_state.mode7_matrix[1] = 0x0123;
  HdReplacements_EvaluateFrame();
  CHECK(g_m7_log.calls == 1);
  CHECK(g_m7_log.x0 == 139 && g_m7_log.y0 == 156 &&
        g_m7_log.x1 == 376 && g_m7_log.y1 == 251);
  CHECK(g_m7_log.width == 2048 && g_m7_log.height == 820);
  CHECK(g_hd_replacements[0].active);

  /* No art: never requests. */
  memset(&g_m7_log, 0, sizeof(g_m7_log));
  g_mode7_busy = false;
  g_hd_replacements[0].pixels = NULL;
  g_ppu_query_calls = 0;
  HdReplacements_EvaluateFrame();
  CHECK(g_ppu_query_calls == 0 && g_m7_log.calls == 0 &&
        !g_hd_replacements[0].active);
}

static void TestEvaluateGates(void) {
  CHECK(HdReplacements_Load(WriteManifest(kTitleManifest)) == 1);
  ResetRuntime();
  MakeTitleState();

  /* No texture (headless / missing art): never captures. */
  HdReplacements_EvaluateFrame();
  CHECK(g_capture_log.calls == 0 && !g_hd_replacements[0].active);

  g_hd_replacements[0].texture = (ArRenderTexture){1};

  /* All gates pass: capture requested with the entry rect + removal flag. */
  HdReplacements_EvaluateFrame();
  CHECK(g_capture_log.calls == 1);
  CHECK(g_capture_log.source == SR_PPU_OVERLAY_BG1);
  CHECK(g_capture_log.x == 11 && g_capture_log.y == 27);
  CHECK(g_capture_log.width == 237 && g_capture_log.height == 95);
  CHECK(g_capture_log.flags == SR_PPU_OVERLAY_REMOVE_FROM_GAME);
  CHECK(g_hd_replacements[0].active);

  /* Swirl (non-identity matrix): gate fails. */
  g_ppu_state.mode7_matrix[1] = 0x0123;
  memset(&g_capture_log, 0, sizeof(g_capture_log));
  HdReplacements_EvaluateFrame();
  CHECK(g_capture_log.calls == 0 && !g_hd_replacements[0].active);
  g_ppu_state.mode7_matrix[1] = 0;

  /* Wrong map byte: gate fails. */
  g_ram[0x18] = 0x01;
  HdReplacements_EvaluateFrame();
  CHECK(g_capture_log.calls == 0 && !g_hd_replacements[0].active);
  g_ram[0x18] = 0x00;

  /* Master toggle off: gate fails. */
  g_settings.hd_replacements = false;
  HdReplacements_EvaluateFrame();
  CHECK(g_capture_log.calls == 0 && !g_hd_replacements[0].active);
  g_settings.hd_replacements = true;

  /* Source already claimed this frame (e.g. HUD split): entry skipped. */
  g_overlay_busy[SR_PPU_OVERLAY_BG1] = true;
  HdReplacements_EvaluateFrame();
  CHECK(g_capture_log.calls == 0 && !g_hd_replacements[0].active);
}

int main(void) {
  TestParseTitleEntry();
  TestParseRejections();
  TestReservedPlanesParseButStayInert();
  TestMode7Entries();
  TestEvaluateGates();
  if (g_failures) {
    fprintf(stderr, "hd manifest tests: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("hd manifest tests: all passed\n");
  return 0;
}
