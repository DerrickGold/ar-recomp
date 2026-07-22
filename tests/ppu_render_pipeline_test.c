/* ppu_render_pipeline_test.c — end-to-end SDL3 present validation with the REAL PPU.
 *
 * Stronger companion to render_pipeline_test.c: instead of a synthetic pixel
 * buffer, this instantiates the actual SNES PPU, points it at a framebuffer via
 * the same PpuBeginDrawing/ppu_runLine path main.c uses, sets a known backdrop
 * color, renders real scanlines, then pushes that framebuffer through the exact
 * SDL3 present + read-back pipeline. It proves the pixels the emulator actually
 * produces (RGB with alpha byte = 0) are VISIBLE after presenting — i.e. the
 * texture blend mode is correct and the screen is not black.
 *
 * Runs headless under the dummy video driver with the software renderer.
 */
#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "snes/ppu.h"
#include "actraiser_game.h"
#include "sim3d.h"
#include "sim_render_atlas.h"
#include "sim_render_metadata.h"

/* main.c owns this global; the PPU line renderer reads it to pick new/old path. */
bool g_new_ppu = false;

/* Trace/debug hooks the PPU references but that only fire under instrumentation
 * (SNESRECOMP_TRACE / debug server). Stub them so this standalone harness links
 * without pulling in the whole runtime. None run in a normal render. */
int ar_trace_active(void) { return 0; }
void ar_trace_ppumem(uint16_t a, uint8_t v) { (void)a; (void)v; }
void ar_trace_reg(uint16_t a, uint8_t v) { (void)a; (void)v; }
void ar_trace_vmadd(uint16_t a) { (void)a; }
void ar_trace_vram(uint16_t a, uint16_t v) { (void)a; (void)v; }
void ar_vramraw(uint16_t a, uint8_t v, int p) { (void)a; (void)v; (void)p; }
int ar_vramwatch(uint16_t a, uint8_t v) { (void)a; (void)v; return 0; }
void CpuDispatchLogWriteFile(const char *path) { (void)path; }
unsigned g_ar_blk_idx;
uint32_t g_ar_blk_ring[256];
const char *g_last_recomp_func;
uint8_t g_ram[0x20000];
int snes_frame_counter;

static int s_failures;
#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

enum { kW = 256, kH = 224 };

/* Build a BGR555 SNES color word (what CGRAM stores): 5 bits each, R low. */
static uint16_t bgr555(int r5, int g5, int b5) {
  return (uint16_t)((r5 & 0x1f) | ((g5 & 0x1f) << 5) | ((b5 & 0x1f) << 10));
}

/* Expand a 5-bit channel to 8-bit the way the PPU does ((v<<3)|(v>>2)). */
static int expand5(int v5) { return ((v5 & 0x1f) << 3) | ((v5 & 0x1f) >> 2); }

static void set_solid_4bpp_tile(Ppu *ppu, int tile, int color) {
  for (int row = 0; row < 8; row++) {
    uint16_t lo = 0, hi = 0;
    if (color & 1) lo |= 0x00ff;
    if (color & 2) lo |= 0xff00;
    if (color & 4) hi |= 0x00ff;
    if (color & 8) hi |= 0xff00;
    ppu->vram[tile * 16 + row] = lo;
    ppu->vram[tile * 16 + row + 8] = hi;
  }
}

static void TestObjRangeRaster(void) {
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;
  ppu_reset(ppu);
  ppu->inidisp = 0x0f;
  ppu->obsel = 0;  /* 8x8 small objects; OBJ tiles start at VRAM word 0. */
  ppu->cgram[0x81] = bgr555(31, 0, 0);
  ppu->cgram[0x82] = bgr555(0, 0, 31);
  set_solid_4bpp_tile(ppu, 0, 1);
  set_solid_4bpp_tile(ppu, 1, 2);

  /* Two fully overlapping priority-2 entries. Normal OAM order makes slot 0
   * own the range; priority rotation beginning at slot 1 reverses ownership. */
  ppu->oam[0] = 10 | (20 << 8);
  ppu->oam[1] = 0 | (2 << 12);
  ppu->oam[2] = 10 | (20 << 8);
  ppu->oam[3] = 1 | (2 << 12);
  PpuObjRangeBounds bounds;
  CHECK(PpuGetObjRangeBounds(ppu, 0, 2, 2, &bounds));
  CHECK(bounds.x0 == 10 && bounds.y0 == 20 &&
        bounds.x1 == 18 && bounds.y1 == 28);

  uint32_t pixels[8 * 8];
  CHECK(PpuRasterizeObjRange(ppu, 0, 2, 2, &bounds, pixels, 8, 8,
                             8 * sizeof(uint32_t)));
  CHECK(pixels[0] == 0xffff0000u);
  CHECK(pixels[63] == 0xffff0000u);

  ppu->oamaddh = 0x80;
  ppu->oamaddl = 2;  /* byte index 2 = OAM slot 1 */
  CHECK(PpuRasterizeObjRange(ppu, 0, 2, 2, &bounds, pixels, 8, 8,
                             8 * sizeof(uint32_t)));
  CHECK(pixels[0] == 0xff0000ffu);

  /* High-OAM x/size bits and vertical OAM wrap are interpreted by the same
   * bounds path used by scanout: x=$1fc -> -4, y=250 -> -6, large=16. */
  ppu->oam[4] = 0xfc | (250 << 8);
  ppu->oam[5] = 0 | (1 << 12);
  ppu->highOam[0] = (1 << 4) | (1 << 5);
  CHECK(PpuGetObjRangeBounds(ppu, 2, 1, 1, &bounds));
  CHECK(bounds.x0 == -4 && bounds.y0 == -6 &&
        bounds.x1 == 12 && bounds.y1 == 10);
  CHECK(!PpuGetObjRangeBounds(ppu, 2, 1, 0, &bounds));

  /* An asymmetric source pixel proves horizontal and vertical flip handling
   * is shared with scanout rather than approximated by the atlas caller. */
  memset(&ppu->vram[2 * 16], 0, 16 * sizeof(ppu->vram[0]));
  ppu->vram[2 * 16] = 1u << 6;  /* source (x=1,y=0), color index 1 */
  ppu->oamaddh = 0;
  ppu->oam[6] = 30 | (40 << 8);
  ppu->oam[7] = 2;
  CHECK(PpuGetObjRangeBounds(ppu, 3, 1, 0, &bounds));
  CHECK(PpuRasterizeObjRange(ppu, 3, 1, 0, &bounds, pixels, 8, 8,
                             8 * sizeof(uint32_t)));
  CHECK(pixels[1] == 0xffff0000u && pixels[0] == 0);
  ppu->oam[7] |= 0xc000;  /* H+V flip -> destination (x=6,y=7). */
  CHECK(PpuRasterizeObjRange(ppu, 3, 1, 0, &bounds, pixels, 8, 8,
                             8 * sizeof(uint32_t)));
  CHECK(pixels[7 * 8 + 6] == 0xffff0000u && pixels[1] == 0);

  ppu_free(ppu);
}

static void BeginSimRecord(uint16_t record, bool world, uint16_t cursor,
                           uint16_t world_x, uint16_t world_y) {
  SimRenderMetadata_BeginRecord(
      record, world, false, 0xe000, world_x, world_y, 1, 0, 0, cursor);
}

static void TestSemanticAtlasPacking(void) {
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;
  ppu_reset(ppu);
  ppu->inidisp = 0x0f;
  ppu->cgram[0x81] = bgr555(31, 31, 31);
  set_solid_4bpp_tile(ppu, 0, 1);

  SimRenderMetadata_Reset();
  BeginSimRecord(kActRaiserWram_SimWorldRecords, true, 0, 110, 70);
  SimRenderMetadata_RecordPart(0, 1u << 12);
  SimRenderMetadata_RecordPart(4, 2u << 12);
  SimRenderMetadata_EndRecord(8);
  ppu->oam[0] = 5 | (7 << 8);
  ppu->oam[1] = 1u << 12;
  ppu->oam[2] = 13 | (7 << 8);
  ppu->oam[3] = 2u << 12;

  CHECK(SimRenderAtlas_Build(ppu, 100, 50));
  SimAtlasBuildInput atlas;
  CHECK(SimRenderMetadata_CopyAtlasInput(&atlas));
  CHECK(atlas.object_count == 2);
  CHECK(atlas.objects[0].atlas_valid && atlas.objects[1].atlas_valid);
  CHECK(atlas.objects[0].atlas_x == 1 && atlas.objects[0].atlas_y == 1);
  CHECK(atlas.objects[0].atlas_w == 8 && atlas.objects[0].atlas_h == 8);
  CHECK(atlas.objects[1].atlas_x == 10 && atlas.objects[1].atlas_y == 1);
  CHECK(atlas.objects[0].local_x0 == -5);
  CHECK(atlas.objects[0].local_y0 == -13);
  CHECK(atlas.objects[0].foot_x == 113 && atlas.objects[0].foot_y == 65);
  CHECK(atlas.objects[1].foot_x == 113 && atlas.objects[1].foot_y == 65);
  CHECK(g_sim_obj_atlas_pixels[1 * kSimObjAtlasWidth + 1] ==
        0xffffffffu);

  /* Fifty independent 64x64 fragments cannot all fit with the mandatory
   * gutter in a 512x512 atlas.
   *
   * This once required the builder to invalidate every descriptor and publish
   * AtlasOverflow -- "never the first 49 as a partial success" -- on the
   * principle that a partial atlas is untrustworthy. Reversed once the SIM 3D
   * view shipped, because an invalid atlas invalidated the frame's metadata,
   * which dropped the whole view to the flat composite: a full-screen
   * perspective flash standing in for one sprite that would not pack. The ROM
   * makes actors vanish at the screen edge anyway, so purging the fragment is
   * both proportionate and authentic. The condition is still reported --
   * purged objects keep atlas_valid clear and the D1 census counts them, so a
   * checkpoint still fails on it -- it just no longer costs the frame. */
  ppu_reset(ppu);
  ppu->inidisp = 0x0f;
  ppu->obsel = 2 << 5;  /* size pair 8/64 */
  SimRenderMetadata_Reset();
  for (int slot = 0; slot < 50; slot++) {
    bool world = slot >= kActRaiserSimFixedRecordCount;
    int record_index = world ? slot - kActRaiserSimFixedRecordCount : slot;
    uint16_t record = (uint16_t)(
        (world ? kActRaiserWram_SimWorldRecords
               : kActRaiserWram_SimFixedRecords) +
        record_index * (world ? kActRaiserSimWorldRecordStride
                              : kActRaiserSimFixedRecordStride));
    BeginSimRecord(record, world, (uint16_t)(slot * 4), 0, 0);
    SimRenderMetadata_RecordPart((uint16_t)(slot * 4), 1u << 12);
    SimRenderMetadata_EndRecord((uint16_t)((slot + 1) * 4));
    uint8_t index = (uint8_t)(slot * 2);
    ppu->oam[index] = 0;
    ppu->oam[index + 1] = 1u << 12;
    ppu->highOam[index >> 3] |= (uint8_t)(1u << ((index & 7) + 1));
  }
  CHECK(SimRenderAtlas_Build(ppu, 0, 0));

  uint8_t wram[kActRaiserWramSize] = {0};
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  wram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Fillmore;
  SimFrameData frame;
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, 0, 0, 0);
  /* The frame survives, which is the entire point of the reversal. */
  CHECK(frame.metadata_valid);
  CHECK(frame.atlas_valid);
  CHECK(!(frame.integrity_flags & kSimMetadataIntegrity_AtlasOverflow));
  int packed = 0, purged = 0;
  for (int i = 0; i < frame.object_count; i++) {
    if (frame.objects[i].atlas_valid) packed++;
    else purged++;
  }
  /* 512 / (64 + 1) = 7 columns of 7 rows, so 49 of the 50 pack. */
  CHECK(packed == 49);
  CHECK(purged == 1);

  ppu_free(ppu);
}

static void TestSim3DFlatComposition(void) {
  enum { width = 3, height = 2 };
  uint32_t storage[kSim3DPlane_Count][width * height];
  uint8_t *planes[kSim3DPlane_Count];
  memset(storage, 0, sizeof(storage));
  for (int plane = 0; plane < kSim3DPlane_Count; plane++)
    planes[plane] = (uint8_t *)storage[plane];

  storage[kSim3DPlane_Bg3Low][1] = 0xffff0000u;
  storage[kSim3DPlane_Obj0][1] = 0xff00ff00u;
  storage[kSim3DPlane_Obj1][1] = 0x000000ffu; /* transparent despite RGB */
  storage[kSim3DPlane_Bg1High][1] = 0xff0000ffu;
  storage[kSim3DPlane_Obj3][1] = 0xffffff00u;
  storage[kSim3DPlane_Bg3High][1] = 0xffff00ffu;

  uint32_t output[(width + 1) * height];
  memset(output, 0xcc, sizeof(output));
  Sim3D_ComposeFlatPixels(
      output, width, height, (width + 1) * (int)sizeof(uint32_t),
      0xff112233u, 1, 3, planes, 0, 0);
  CHECK(output[0] == 0xff000000u);
  CHECK(output[1] == 0xffff00ffu); /* last hardware-rank plane wins */
  CHECK(output[2] == 0xff112233u);
  CHECK(output[width + 1] == 0xff000000u);
  CHECK(output[width + 2] == 0xff112233u);
  CHECK(output[width + 3] == 0xff112233u);

  Sim3D_ComposeFlatPixels(
      output, width, 1, (width + 1) * (int)sizeof(uint32_t),
      0xff112233u, 1, 3, planes, 1u << kSim3DPlane_Obj0, 0);
  CHECK(output[1] == 0xff00ff00u);

  Sim3D_ComposeFlatPixels(
      output, width, 1, (width + 1) * (int)sizeof(uint32_t),
      0xff112233u, 1, 3, planes, 1u << kSim3DPlane_Obj1, 0);
  CHECK(output[1] == 0xff112233u); /* alpha-zero capture cannot cover */
}

static void TestSim3DWidescreenHudCaptureHandoff(void) {
  CHECK(Sim3D_ObjPlaneForPriority(0) == kSim3DPlane_Obj0);
  CHECK(Sim3D_ObjPlaneForPriority(1) == kSim3DPlane_Obj1);
  CHECK(Sim3D_ObjPlaneForPriority(2) == kSim3DPlane_Obj2);
  CHECK(Sim3D_ObjPlaneForPriority(3) == kSim3DPlane_Obj3);
  CHECK(Sim3D_ObjPlaneForPriority(-1) == -1);
  CHECK(Sim3D_ObjPlaneForPriority(4) == -1);

  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;
  ppu_reset(ppu);
  ppu->inidisp = 0x0f;
  ppu->bgmode = 9;
  ppu->screenEnabled[0] = 0x17;
  ppu->screenEnabled[1] = 0;
  PpuSetWidescreenHudSplit(
      ppu, kActRaiserSimulationHudHeight,
      kActRaiserSimulationHudSplit, kActRaiserSimulationHudSplit,
      kActRaiserSimulationHudHeight, kActRaiserSimulationHudHeight);
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Bg3, 0, 0,
      kActRaiserAuthenticWidth, kActRaiserSimulationHudHeight,
      kPpuOverlayFlag_RemoveFromGame));
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Obj, 0, 0,
      kActRaiserAuthenticWidth, kActRaiserSimulationHudHeight,
      kPpuOverlayFlag_RemoveFromGame));
  CHECK(PpuSetOverlayOamRange(
      ppu, kActRaiserHudObjOamFirst, kActRaiserHudObjOamCount));

  const int extra = 43;
  Sim3DCaptureRequest request = {
    .town = true,
    .master_enabled = true,
    .renderer_ready = true,
    .requested_features = kSimFeature_SeparatedComposite,
    .width = kActRaiserAuthenticWidth + 2 * extra,
    .height = kActRaiserAuthenticHeight,
  };
  Sim3D_BeginFrame();
  CHECK(Sim3D_PrepareCapture(ppu, &request));
  const PpuOverlayCapture *bg3 =
      &ppu->overlayCaptures[kPpuOverlaySource_Bg3];
  const PpuOverlayCapture *obj =
      &ppu->overlayCaptures[kPpuOverlaySource_Obj];
  CHECK(bg3->x0 == -extra && bg3->x1 == kActRaiserAuthenticWidth + extra);
  CHECK(bg3->y0 == 0 && bg3->y1 == kActRaiserAuthenticHeight);
  CHECK(bg3->flags == 0);
  CHECK(obj->x0 == -extra && obj->x1 == kActRaiserAuthenticWidth + extra);
  CHECK(obj->oamFirst == 0 && obj->oamCount == 128);
  CHECK(Sim3D_BeginFrame());

  /* An unrelated layer capture still owns its source and must fail closed. */
  PpuClearOverlayCaptures(ppu);
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Bg1, 0, 0, 16, 16,
      kPpuOverlayFlag_RemoveFromGame));
  CHECK(!Sim3D_PrepareCapture(ppu, &request));
  CHECK(!Sim3D_BeginFrame());
  ppu_free(ppu);
}

int main(void) {
  TestObjRangeRaster();
  TestSemanticAtlasPacking();
  TestSim3DFlatComposition();
  TestSim3DWidescreenHudCaptureHandoff();
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  CHECK(SDL_Init(SDL_INIT_VIDEO));

  /* ---- Real PPU: draw a solid backdrop, full brightness, force-blank off ---- */
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) { SDL_Quit(); return 1; }
  ppu_reset(ppu);

  /* Framebuffer the PPU renders into (ARGB8888, 4 bytes/px), same shape as
   * main.c's g_pixels. The PPU writes RGB and leaves the alpha byte 0. */
  static uint8_t fb[kW * kH * 4];
  memset(fb, 0, sizeof(fb));
  PpuBeginDrawing(ppu, fb, kW * 4, 0);

  /* Backdrop = CGRAM entry 0. Pick a distinctive color: R=25,G=10,B=3 (5-bit).
   * With no BG/OBJ layers enabled, every pixel resolves to this backdrop. */
  const int R5 = 25, G5 = 10, B5 = 3;
  ppu->cgram[0] = bgr555(R5, G5, B5);
  ppu->inidisp = 0x0f;   /* brightness 15, force-blank (bit 7) OFF */
  ppu->bgmode = 0;       /* mode 0; no layers enabled -> pure backdrop */

  for (int line = 0; line <= kH; line++) ppu_runLine(ppu, line);

  /* Sanity: the PPU wrote RGB with alpha byte 0 (BGRA byte order in memory:
   * [0]=B [1]=G [2]=R [3]=A). Confirm a mid-screen pixel matches the backdrop
   * and its alpha byte is indeed 0 (the condition that black-screens under a
   * BLEND texture). */
  const uint8_t *mid = fb + ((size_t)(kH / 2) * kW + (kW / 2)) * 4;
  int exp_r = expand5(R5), exp_g = expand5(G5), exp_b = expand5(B5);
  fprintf(stderr, "[ppu-test] backdrop pixel BGRA = %d,%d,%d,%d (expected RGB %d,%d,%d, A 0)\n",
          mid[0], mid[1], mid[2], mid[3], exp_r, exp_g, exp_b);
  CHECK(mid[2] == exp_r && mid[1] == exp_g && mid[0] == exp_b);
  CHECK(mid[3] == 0);   /* the alpha-0 framebuffer that triggered the bug */

  /* ---- Present through the exact SDL3 path main.c uses ---- */
  SDL_Window *window =
      SDL_CreateWindow("ppu-pipeline-test", kW * 3, kH * 3, SDL_WINDOW_HIDDEN);
  CHECK(window != NULL);
  SDL_Renderer *renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
  CHECK(renderer != NULL);

  if (renderer) {
    SDL_Texture *texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kW, kH);
    CHECK(texture != NULL);
    /* The fix under test: force NONE so the alpha-0 framebuffer blits opaque.
     * (SDL3 defaults to BLEND, which would present this as a black screen.) */
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);

    CHECK(SDL_UpdateTexture(texture, NULL, fb, kW * 4));
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    CHECK(SDL_RenderClear(renderer));
    CHECK(SDL_RenderTexture(renderer, texture, NULL, NULL));
    CHECK(SDL_RenderPresent(renderer));

    SDL_Surface *raw = SDL_RenderReadPixels(renderer, NULL);
    SDL_Surface *argb =
        raw ? SDL_ConvertSurface(raw, SDL_PIXELFORMAT_ARGB8888) : NULL;
    if (raw) SDL_DestroySurface(raw);
    CHECK(argb != NULL);

    if (argb) {
      /* Center of the presented output must be the backdrop RGB, not black. */
      const uint8_t *row = (const uint8_t *)argb->pixels +
                           (size_t)(argb->h / 2) * argb->pitch;
      const uint8_t *cp = row + (size_t)(argb->w / 2) * 4;  /* B,G,R,A */
      int got_b = cp[0], got_g = cp[1], got_r = cp[2];
      long nonblack = 0;
      for (int y = 0; y < argb->h; y++) {
        const uint8_t *r = (const uint8_t *)argb->pixels + (size_t)y * argb->pitch;
        for (int x = 0; x < argb->w; x++)
          if (r[x * 4] || r[x * 4 + 1] || r[x * 4 + 2]) nonblack++;
      }
      fprintf(stderr, "[ppu-test] presented center RGB = %d,%d,%d; nonblack=%ld/%ld\n",
              got_r, got_g, got_b, nonblack, (long)argb->w * argb->h);
      CHECK(got_r == exp_r && got_g == exp_g && got_b == exp_b);
      CHECK(nonblack == (long)argb->w * argb->h);  /* whole frame visible */
      SDL_DestroySurface(argb);
    }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
  }

  SDL_DestroyWindow(window);
  ppu_free(ppu);
  SDL_Quit();

  if (s_failures) {
    fprintf(stderr, "ppu render pipeline tests: %d failure(s)\n", s_failures);
    return 1;
  }
  fprintf(stderr, "ppu render pipeline tests: pass\n");
  return 0;
}
