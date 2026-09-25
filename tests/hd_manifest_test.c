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
  AssetConditions_BindRunner((SrRunnerHandle *)&g_runner_storage);
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
  CHECK(e->conditions[0].kind == kAssetCondition_WramByte);
  CHECK(e->conditions[0].address == 0x18);
  CHECK(e->conditions[0].value == 0);
  CHECK(e->conditions[2].kind == kAssetCondition_BgMode);
  CHECK(e->conditions[2].value == 7);
  CHECK(e->conditions[3].kind == kAssetCondition_M7Identity);
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
  CHECK(g_hd_replacements[0].conditions[1].kind == kAssetCondition_M7Element);
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

static void TestRegionalTitleCoverage(void) {
  const int width = 2212, height = 760, padding = 28;
  const size_t pitch = (size_t)width * 4u;
  uint8_t *rgba = malloc(pitch * height);
  CHECK(rgba != NULL);
  if (!rgba) return;
  for (size_t i = 0; i < pitch * height; ++i) rgba[i] = (uint8_t)(i * 13u);
  for (int mode7 = 0; mode7 <= 1; ++mode7) {
    CHECK(HdReplacements_Load(WriteManifest(kTitleManifest)) == 1);
    HdReplacement *entry = &g_hd_replacements[0];
    if (mode7) {
      strcpy(entry->name, "title-swirl");
      entry->plane = kHdPlane_Mode7;
      entry->canvas_x0 = 139; entry->canvas_y0 = 156;
      entry->canvas_x1 = 376; entry->canvas_y1 = 251;
    }
    uint8_t *padded = NULL;
    int expanded_width = 0;
    CHECK(HdReplacements_PrepareTitleCoverage(entry, rgba, width, height,
                                            &padded, &expanded_width));
    CHECK(mode7 ? padded && expanded_width == 2240
                : !padded && expanded_width == width && entry->image_inset_left == 3);
    if (mode7 && !padded) continue;
    const size_t expanded_pitch = (size_t)expanded_width * 4u;
    const uint8_t clear[28 * 4] = {0};
    for (int y = 0; mode7 && y < height; ++y) {
      CHECK(!memcmp(padded + (size_t)y * expanded_pitch, clear, sizeof(clear)));
      CHECK(!memcmp(padded + (size_t)y * expanded_pitch + padding * 4u,
                    rgba + (size_t)y * pitch, pitch));
    }
    /* Exact mapping: same texel scale and original image origin, not a
     * widened/stretched logo. Canvas and screen differ only by scroll. */
    const int old_left = mode7 ? 139 : 11;
    const int left = mode7 ? entry->canvas_x0 : entry->x0;
    const int right = mode7 ? entry->canvas_x1 : entry->x1;
    CHECK(left == old_left - 3 && right - left == 240);
    if (mode7) {
      CHECK(expanded_width * 237 == width * 240);
      CHECK((old_left - left) * expanded_width == padding * 240);
    } else {
      CHECK(left + entry->image_inset_left == old_left);
      CHECK(right - left - entry->image_inset_left == 237);
    }
    ResetRuntime(); MakeTitleState();
    if (mode7) {
      entry->pixels = padded;
      entry->pixels_width = expanded_width; entry->pixels_height = height;
      g_ppu_state.mode7_matrix[1] = 0x123;
      entry->conditions[3].negate = 1; // non-identity title swirl
    } else entry->texture = (ArRenderTexture){1};
    HdReplacements_EvaluateFrame();
    if (mode7) CHECK(g_m7_log.calls == 1 && g_m7_log.x0 == 136 &&
                     g_m7_log.x1 == 376 && g_m7_log.width == expanded_width);
    else CHECK(g_capture_log.calls == 1 && g_capture_log.x == 8 &&
               g_capture_log.width == 240);
    // Repeat preparation does not add a second gutter.
    uint8_t *again = NULL; int again_width = 0;
    CHECK(HdReplacements_PrepareTitleCoverage(entry, mode7 ? padded : rgba,
        expanded_width, height, &again, &again_width));
    CHECK(!again && again_width == expanded_width);
    g_settings.hd_replacements = false;
    HdReplacements_EvaluateFrame();
    CHECK(!entry->active);
    free(padded);
  }
  for (int custom = 0; custom < 4; ++custom) {
    CHECK(HdReplacements_Load(WriteManifest(kTitleManifest)) == 1);
    HdReplacement *entry = &g_hd_replacements[0];
    int image_width = width;
    if (custom == 0) strcpy(entry->name, "custom-title");
    if (custom == 1) entry->x0 = 7;
    if (custom == 2) entry->source = SR_PPU_OVERLAY_BG2;
    if (custom == 3) { // No fractional-gutter rounding for custom affine art.
      image_width = 2087;
      entry->plane = kHdPlane_Mode7;
      strcpy(entry->name, "title-swirl");
      entry->canvas_x0 = 139; entry->canvas_y0 = 156;
      entry->canvas_x1 = 376; entry->canvas_y1 = 251;
    }
    const HdReplacement before = *entry;
    uint8_t *padded = NULL; int expanded_width = 0;
    CHECK(HdReplacements_PrepareTitleCoverage(entry, rgba, image_width, height,
                                            &padded, &expanded_width));
    CHECK(!padded && expanded_width == image_width);
    CHECK(!memcmp(entry, &before, sizeof(before)));
  }
  free(rgba);
}

int main(void) {
  TestParseTitleEntry();
  TestParseRejections();
  TestReservedPlanesParseButStayInert();
  TestMode7Entries();
  TestEvaluateGates();
  TestRegionalTitleCoverage();
  if (g_failures) {
    fprintf(stderr, "hd manifest tests: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("hd manifest tests: all passed\n");
  return 0;
}
