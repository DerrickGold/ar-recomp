/* Standalone proof-of-concept for the in-game manual reader.
 *
 *     manual_poc <manual.pdf | album.jpgpack>
 *
 * WHY THIS EXISTS. The reader's feel -- turn timing, easing, zoom rate, how the
 * leaf catches light -- can only be judged by looking at it, and it has nothing
 * to do with the emulator. Building it here means the experience can be iterated
 * with NO ROM, no recompilation, and no game boot: this links exactly three of
 * the game's own sources and SDL.
 *
 * WHAT IS REAL HERE, AND SO NOT THROWAWAY:
 *   - src/manual_pages.c   the page index and every kinematic decision
 *   - src/scene3d_math.c   the SAME projection the sim and diorama use
 *   - third_party/stb      the SAME decoder, with JPEG enabled
 * The draw path below is what the game's overlay shim will do, in the same
 * order, so what you tune here transfers. What is NOT real: the window and event
 * loop (the game has its own), and the settings-overlay entry point.
 *
 * THE CONSTRAINT THIS DEMONSTRATES. SDL_RenderGeometry has no depth test and no
 * backface culling, so the four draws below happen in ONE fixed order for every
 * frame of a turn -- backdrop, destination page, shadow, leaf. That is only
 * correct because the leaf never dips behind a settled page, which
 * manual_pages.c guarantees and its test asserts across the whole turn domain.
 *
 * CONTROLS
 *   left / right, PgUp / PgDn   turn a page          space  turn forward
 *   wheel, +/-                  zoom                 0      reset zoom
 *   drag, arrows (when zoomed)   pan                 f      toggle 3D tilt
 *   home / end                  first / last page    t      cycle turn speed
 *   d                           overlay the diagnostics
 *   esc / q                     quit
 */

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "manual_pages.h"
#include "scene3d_math.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG            /* the pages are baseline JPEG */
#define STBI_ONLY_PNG             /* and a converted pack may be PNG */
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

enum {
  /* A page of dense TEXT in strong perspective needs finer subdivision than the
   * diorama's scenery: measured affine-UV error at 1080-tall output is ~5 px at
   * 8 columns versus ~1.3 px at 16, and the scan's strokes are 1-2 px. These are
   * the reader's OWN constants -- widening the diorama's would inflate its
   * per-frame stack arrays for no reason. */
  kPageSubdivU = 16,
  kPageSubdivV = 6,
  kPageVerts = (kPageSubdivU + 1) * (kPageSubdivV + 1),
  kPageIndices = kPageSubdivU * kPageSubdivV * 6,
  /* Decoded pages held at once. Only three are ever visible (the settled page,
   * the leaf, and the page it lands on), so this is that plus one of slack.
   * 739x1080 RGBA is 3.19 MB, so four is ~12.8 MB -- against 128 MB if all 40
   * were resident, which would buy nothing. */
  kPageCacheSlots = 4,
};

typedef struct PageTexture {
  SDL_Texture *texture;
  int page;
  uint64_t used_at;      /* for LRU eviction */
  int width, height;
} PageTexture;

enum { kMaxRememberedFailures = 64 };

typedef struct Poc {
  SDL_Window *window;
  SDL_Renderer *renderer;

  uint8_t *file;
  size_t file_size;
  ManualPageIndex index;

  PageTexture cache[kPageCacheSlots];
  uint64_t clock;

  ManualView view;
  bool tilt_3d;
  bool show_debug;
  float turn_seconds;
  bool dragging;

  /* Decode is deliberately NOT on the draw path: a 739x1080 baseline JPEG costs
   * ~4.7 ms scalar (2.9 ms with NEON), against a vsync budget of refresh/2 --
   * 8.3 ms at 60 Hz but only 5.6 ms at the Steam Deck's 90. So a page is decoded
   * at most once per frame, before drawing, and until it is ready the previous
   * page keeps being shown. A turn then costs one frame of LATENCY rather than a
   * dropped frame. */
  int pending_decode;

  /* Pages whose decode failed. Remembered because LoadPage is called from the
   * DRAW path: without this a single undecodable page retries every frame, which
   * spams the log at refresh rate and pays the decode cost forever. */
  int failed[kMaxRememberedFailures];
  int failed_count;
} Poc;

static bool AlreadyFailed(const Poc *poc, int page) {
  for (int i = 0; i < poc->failed_count; i++)
    if (poc->failed[i] == page) return true;
  return false;
}

static void RememberFailure(Poc *poc, int page) {
  if (AlreadyFailed(poc, page)) return;
  if (poc->failed_count < kMaxRememberedFailures)
    poc->failed[poc->failed_count++] = page;
}

/* ── Page cache ─────────────────────────────────────────────────────────────── */

static PageTexture *FindCached(Poc *poc, int page) {
  for (int i = 0; i < kPageCacheSlots; i++)
    if (poc->cache[i].texture && poc->cache[i].page == page) {
      poc->cache[i].used_at = ++poc->clock;
      return &poc->cache[i];
    }
  return NULL;
}

/* Decode one page into the cache, evicting the least recently used slot.
 * Returns NULL on a decode or texture failure, which the caller must treat as
 * "keep showing what is already up" rather than as a fatal error. */
static PageTexture *LoadPage(Poc *poc, int page) {
  if (page < 0 || page >= poc->index.count) return NULL;
  PageTexture *hit = FindCached(poc, page);
  if (hit) return hit;
  if (AlreadyFailed(poc, page)) return NULL;   /* reported once, never retried */

  PageTexture *slot = &poc->cache[0];
  for (int i = 1; i < kPageCacheSlots; i++) {
    if (!poc->cache[i].texture) { slot = &poc->cache[i]; break; }
    if (poc->cache[i].used_at < slot->used_at) slot = &poc->cache[i];
  }

  const ManualPageEntry *entry = &poc->index.pages[page];
  int w = 0, h = 0, channels = 0;
  stbi_uc *pixels = stbi_load_from_memory(poc->file + entry->offset,
                                          (int)entry->length, &w, &h,
                                          &channels, 4);
  if (!pixels) {
    SDL_Log("page %d: decode failed (%s)", page + 1, stbi_failure_reason());
    RememberFailure(poc, page);
    return NULL;
  }

  /* No max-texture-size pre-check on purpose. The software renderer does not
   * publish SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER at all, so a guard against
   * it refuses 739x1080 textures that in fact create fine -- and would disable
   * the reader precisely on the headless path. SDL_CreateTexture is the oracle. */
  SDL_Texture *texture = SDL_CreateTexture(poc->renderer,
                                           SDL_PIXELFORMAT_ABGR8888,
                                           SDL_TEXTUREACCESS_STATIC, w, h);
  if (!texture) {
    SDL_Log("page %d: texture failed (%s)", page + 1, SDL_GetError());
    RememberFailure(poc, page);
    stbi_image_free(pixels);
    return NULL;
  }
  SDL_UpdateTexture(texture, NULL, pixels, w * 4);
  SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
  stbi_image_free(pixels);

  if (slot->texture) SDL_DestroyTexture(slot->texture);
  slot->texture = texture;
  slot->page = page;
  slot->width = w;
  slot->height = h;
  slot->used_at = ++poc->clock;
  return slot;
}

/* ── Mesh building ──────────────────────────────────────────────────────────── */

/* A flat page as a subdivided quad in screen space. Subdivided even when flat so
 * the vertex path is identical to the leaf's -- one code path to trust. */
static void BuildFlatPage(SDL_Vertex *verts, int *indices,
                          float cx, float cy, float w, float h, float shade) {
  const float half_w = w * 0.5f, half_h = h * 0.5f;
  int v = 0;
  for (int iv = 0; iv <= kPageSubdivV; iv++) {
    for (int iu = 0; iu <= kPageSubdivU; iu++) {
      const float u = (float)iu / (float)kPageSubdivU;
      const float t = (float)iv / (float)kPageSubdivV;
      verts[v].position.x = cx - half_w + w * u;
      verts[v].position.y = cy - half_h + h * t;
      verts[v].tex_coord.x = u;
      verts[v].tex_coord.y = t;
      verts[v].color = (SDL_FColor){ shade, shade, shade, 1.0f };
      v++;
    }
  }
  int n = 0;
  for (int iv = 0; iv < kPageSubdivV; iv++) {
    for (int iu = 0; iu < kPageSubdivU; iu++) {
      const int base = iv * (kPageSubdivU + 1) + iu;
      indices[n++] = base;
      indices[n++] = base + 1;
      indices[n++] = base + kPageSubdivU + 1;
      indices[n++] = base + 1;
      indices[n++] = base + kPageSubdivU + 2;
      indices[n++] = base + kPageSubdivU + 1;
    }
  }
}

/* The turning leaf, projected through the game's own scene3d_math.
 *
 * Returns false if ANY vertex fails to project -- Scene3D_ProjectWorldPoint
 * refuses points at or behind the camera plane, and drawing a primitive with a
 * partially-projected vertex set turns it inside out. Rejecting the whole leaf
 * for one bad vertex is the documented contract. */
static bool BuildLeaf(SDL_Vertex *verts, int *indices, float turn,
                      int view_w, int view_h, float page_w, float page_h,
                      bool tilt_3d, float alpha) {
  Scene3DCamera camera = {
    .tilt_x = tilt_3d ? -0.35f : 0.0f,
    .tilt_y = tilt_3d ? 0.22f : 0.0f,
    .distance = 2.6f,
    .fov_y = 0.9f,
  };
  float matrix[16];
  Scene3D_BuildViewProjection(&camera, view_w, view_h, matrix);

  /* World units per fit-relative unit, so the leaf lands exactly on the flat
   * page it replaces. */
  const float sx = page_w / (float)view_w * 2.0f;
  const float sy = page_h / (float)view_h * 2.0f;

  int v = 0;
  for (int iv = 0; iv <= kPageSubdivV; iv++) {
    for (int iu = 0; iu <= kPageSubdivU; iu++) {
      const float u = (float)iu / (float)kPageSubdivU;
      const float t = (float)iv / (float)kPageSubdivV;
      float lx = 0.0f, ly = 0.0f, lz = 0.0f;
      ManualTurn_LeafPoint(turn, u, t, &lx, &ly, &lz);
      Scene3DPoint screen;
      if (!Scene3D_ProjectWorldPoint(matrix, lx * sx, (ly - 0.5f) * sy, lz * sy,
                                     view_w, view_h, &screen))
        return false;
      const float shade = ManualTurn_LeafShade(turn, u);
      verts[v].position.x = screen.x;
      verts[v].position.y = screen.y;
      /* Past halfway the sheet's reverse faces us, so the texture must be
       * mirrored in u or the page reads backwards. */
      verts[v].tex_coord.x = ManualTurn_FrontFaceVisible(turn) ? u : 1.0f - u;
      verts[v].tex_coord.y = t;
      verts[v].color = (SDL_FColor){ shade, shade, shade, alpha };
      v++;
    }
  }
  int n = 0;
  for (int iv = 0; iv < kPageSubdivV; iv++) {
    for (int iu = 0; iu < kPageSubdivU; iu++) {
      const int base = iv * (kPageSubdivU + 1) + iu;
      indices[n++] = base;
      indices[n++] = base + 1;
      indices[n++] = base + kPageSubdivU + 1;
      indices[n++] = base + 1;
      indices[n++] = base + kPageSubdivU + 2;
      indices[n++] = base + kPageSubdivU + 1;
    }
  }
  return true;
}

/* ── Frame ──────────────────────────────────────────────────────────────────── */

static void DrawFrame(Poc *poc) {
  int view_w = 0, view_h = 0;
  SDL_GetRenderOutputSize(poc->renderer, &view_w, &view_h);

  SDL_SetRenderDrawColor(poc->renderer, 24, 22, 30, 255);
  SDL_RenderClear(poc->renderer);

  const bool turning = poc->view.turn != 0.0f;
  const int settled = poc->view.page;
  const int destination = turning ? poc->view.turn_target : settled;

  PageTexture *under = LoadPage(poc, destination);
  PageTexture *leaf = turning ? LoadPage(poc, settled) : NULL;
  if (!under) under = leaf;
  if (!under) return;

  float page_w = 0.0f, page_h = 0.0f;
  ManualView_FittedSize(under->width, under->height, view_w, view_h,
                        poc->view.zoom, &page_w, &page_h);
  const float cx = (float)view_w * 0.5f - poc->view.pan_x;
  const float cy = (float)view_h * 0.5f - poc->view.pan_y;

  SDL_Vertex verts[kPageVerts];
  int indices[kPageIndices];

  /* ORDER IS THE WHOLE CORRECTNESS ARGUMENT, and it never changes mid-turn.
   * 1. the page being turned ONTO */
  BuildFlatPage(verts, indices, cx, cy, page_w, page_h, 1.0f);
  SDL_RenderGeometry(poc->renderer, under->texture, verts, kPageVerts,
                     indices, kPageIndices);

  if (turning && leaf) {
    /* 2. the leaf's shadow, as the same mesh darkened and offset. Cheap, and it
     *    is what makes the sheet read as lifted rather than sliding. */
    if (BuildLeaf(verts, indices, poc->view.turn, view_w, view_h,
                  page_w, page_h, poc->tilt_3d, 0.30f)) {
      for (int i = 0; i < kPageVerts; i++) {
        verts[i].position.x += 10.0f;
        verts[i].position.y += 14.0f;
        verts[i].color = (SDL_FColor){ 0.0f, 0.0f, 0.0f, 0.30f };
      }
      SDL_RenderGeometry(poc->renderer, NULL, verts, kPageVerts,
                         indices, kPageIndices);
    }
    /* 3. the leaf itself */
    if (BuildLeaf(verts, indices, poc->view.turn, view_w, view_h,
                  page_w, page_h, poc->tilt_3d, 1.0f)) {
      SDL_Texture *face = ManualTurn_FrontFaceVisible(poc->view.turn)
                              ? leaf->texture : under->texture;
      SDL_RenderGeometry(poc->renderer, face, verts, kPageVerts,
                         indices, kPageIndices);
    }
  }

  if (poc->show_debug) {
    char line[256];
    float lx = 0.0f, ly = 0.0f;
    ManualView_PanLimit(&poc->view, under->width, under->height,
                        view_w, view_h, &lx, &ly);
    snprintf(line, sizeof line,
             "page %d/%d  zoom %.2fx  pan %.0f,%.0f (limit %.0f,%.0f)  "
             "turn %+.2f  tilt %s  turn_time %.2fs",
             poc->view.page + 1, poc->index.count, (double)poc->view.zoom,
             (double)poc->view.pan_x, (double)poc->view.pan_y,
             (double)lx, (double)ly, (double)poc->view.turn,
             poc->tilt_3d ? "3D" : "flat", (double)poc->turn_seconds);
    SDL_SetRenderDrawColor(poc->renderer, 0, 0, 0, 190);
    SDL_SetRenderDrawBlendMode(poc->renderer, SDL_BLENDMODE_BLEND);
    SDL_FRect bar = { 0.0f, 0.0f, (float)view_w, 22.0f };
    SDL_RenderFillRect(poc->renderer, &bar);
    SDL_SetRenderDrawColor(poc->renderer, 235, 230, 220, 255);
    SDL_RenderDebugText(poc->renderer, 8.0f, 7.0f, line);
  }

  SDL_RenderPresent(poc->renderer);
}

/* ── Input ──────────────────────────────────────────────────────────────────── */

static void PageDimensions(Poc *poc, int *w, int *h) {
  *w = poc->index.count ? poc->index.pages[0].width : 739;
  *h = poc->index.count ? poc->index.pages[0].height : 1080;
}

static bool HandleEvent(Poc *poc, const SDL_Event *event) {
  int view_w = 0, view_h = 0, pw = 0, ph = 0;
  SDL_GetRenderOutputSize(poc->renderer, &view_w, &view_h);
  PageDimensions(poc, &pw, &ph);

  switch (event->type) {
    case SDL_EVENT_QUIT:
      return false;
    case SDL_EVENT_KEY_DOWN:
      switch (event->key.key) {
        case SDLK_ESCAPE: case SDLK_Q: return false;
        case SDLK_RIGHT: case SDLK_PAGEDOWN: case SDLK_SPACE:
          if (poc->view.zoom > 1.001f)   /* zoomed in: arrows pan instead */
            ManualView_Pan(&poc->view, 60.0f, 0.0f, pw, ph, view_w, view_h);
          else
            ManualView_BeginTurn(&poc->view, +1, poc->index.count);
          break;
        case SDLK_LEFT: case SDLK_PAGEUP:
          if (poc->view.zoom > 1.001f)
            ManualView_Pan(&poc->view, -60.0f, 0.0f, pw, ph, view_w, view_h);
          else
            ManualView_BeginTurn(&poc->view, -1, poc->index.count);
          break;
        case SDLK_UP:
          ManualView_Pan(&poc->view, 0.0f, -60.0f, pw, ph, view_w, view_h);
          break;
        case SDLK_DOWN:
          ManualView_Pan(&poc->view, 0.0f, 60.0f, pw, ph, view_w, view_h);
          break;
        case SDLK_HOME: ManualView_GoTo(&poc->view, 0, poc->index.count); break;
        case SDLK_END:
          ManualView_GoTo(&poc->view, poc->index.count - 1, poc->index.count);
          break;
        case SDLK_EQUALS: case SDLK_KP_PLUS:
          ManualView_Zoom(&poc->view, 1.25f, pw, ph, view_w, view_h); break;
        case SDLK_MINUS: case SDLK_KP_MINUS:
          ManualView_Zoom(&poc->view, 0.8f, pw, ph, view_w, view_h); break;
        case SDLK_0:
          ManualView_Zoom(&poc->view, 0.0001f, pw, ph, view_w, view_h); break;
        case SDLK_F: poc->tilt_3d = !poc->tilt_3d; break;
        case SDLK_D: poc->show_debug = !poc->show_debug; break;
        case SDLK_T:
          /* Turn duration is the single most subjective number here, so make it
           * adjustable while looking at it rather than a rebuild away. */
          poc->turn_seconds = poc->turn_seconds > 0.7f ? 0.18f
                            : poc->turn_seconds + 0.16f;
          break;
        default: break;
      }
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      ManualView_Zoom(&poc->view, event->wheel.y > 0 ? 1.12f : 0.89f,
                      pw, ph, view_w, view_h);
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: poc->dragging = true; break;
    case SDL_EVENT_MOUSE_BUTTON_UP: poc->dragging = false; break;
    case SDL_EVENT_MOUSE_MOTION:
      if (poc->dragging)
        ManualView_Pan(&poc->view, -event->motion.xrel, -event->motion.yrel,
                       pw, ph, view_w, view_h);
      break;
    default: break;
  }
  return true;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr,
            "usage: %s [--force] <manual.pdf>\n\n"
            "Reads page images out of a scan-album PDF and opens the reader.\n"
            "A PDF whose pages are vector text yields no images -- that is the\n"
            "correct answer, and the case the builder must rasterise upstream.\n",
            argv[0]);
    return 2;
  }

  bool force = false;
  const char *path = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--force") == 0) force = true;
    else path = argv[i];
  }
  if (!path) { fprintf(stderr, "%s: no input file\n", argv[0]); return 2; }

  Poc poc;
  memset(&poc, 0, sizeof poc);
  poc.turn_seconds = 0.34f;
  poc.show_debug = true;
  ManualView_Init(&poc.view);

  FILE *file = fopen(path, "rb");
  if (!file) { perror(path); return 1; }
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  rewind(file);
  if (size <= 0) { fprintf(stderr, "%s: empty\n", path); fclose(file); return 1; }
  poc.file = (uint8_t *)malloc((size_t)size);
  if (!poc.file) { fclose(file); return 1; }
  poc.file_size = fread(poc.file, 1, (size_t)size, file);
  fclose(file);

  const int found = ManualPages_CarveAlbum(poc.file, poc.file_size, &poc.index);
  printf("%s: %d page image(s) in %zu bytes\n", path, found, poc.file_size);
  if (found == 0) {
    fprintf(stderr,
            "\nNo page images found. This file's pages are almost certainly\n"
            "vector text, which has no image to extract -- it must be\n"
            "rasterised to page images first. Try:\n"
            "    pdftoppm -jpeg -r 150 in.pdf page   (poppler)\n"
            "    mutool draw -o page%%03d.jpg in.pdf  (mupdf)\n");
    free(poc.file);
    return 1;
  }
  if (!ManualPages_LooksLikeAlbum(&poc.index, poc.file_size)) {
    /* REFUSE, rather than open a window onto whatever was found. "I extracted an
     * image" is not "I extracted a manual": a vector document with a logo on each
     * page satisfies the first and would present the user a booklet of
     * letterheads. Opening anyway would also be dishonest about what the shipped
     * builder must do here, which is rasterise or refuse. Override with
     * --force to inspect what was actually carved. */
    fprintf(stderr,
            "\n%s: found %d image(s), but this is NOT a page album --\n"
            "the images are a small fraction of the file, or they differ in size,\n"
            "so they are figures or letterheads rather than pages.\n\n"
            "Its pages are almost certainly vector text and must be rasterised:\n"
            "    pdftoppm -jpeg -r 150 in.pdf page   (poppler)\n"
            "    mutool draw -o page%%03d.jpg in.pdf  (mupdf)\n\n"
            "Pass --force to open it anyway and see what was carved.\n",
            path, found);
    if (!force) { free(poc.file); return 1; }
  }

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    free(poc.file);
    return 1;
  }
  if (!SDL_CreateWindowAndRenderer("Manual reader (proof of concept)",
                                   960, 1040,
                                   SDL_WINDOW_RESIZABLE |
                                       SDL_WINDOW_HIGH_PIXEL_DENSITY,
                                   &poc.window, &poc.renderer)) {
    fprintf(stderr, "SDL_CreateWindowAndRenderer: %s\n", SDL_GetError());
    SDL_Quit();
    free(poc.file);
    return 1;
  }

  uint64_t previous = SDL_GetTicksNS();
  bool running = true;
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event))
      if (!HandleEvent(&poc, &event)) { running = false; break; }

    const uint64_t now = SDL_GetTicksNS();
    const float elapsed = (float)((double)(now - previous) / 1e9);
    previous = now;
    ManualView_AdvanceTurn(&poc.view, elapsed, poc.turn_seconds);

    DrawFrame(&poc);
    SDL_Delay(1);
  }

  for (int i = 0; i < kPageCacheSlots; i++)
    if (poc.cache[i].texture) SDL_DestroyTexture(poc.cache[i].texture);
  SDL_DestroyRenderer(poc.renderer);
  SDL_DestroyWindow(poc.window);
  SDL_Quit();
  free(poc.file);
  return 0;
}
