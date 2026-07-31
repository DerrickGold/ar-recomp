/* Standalone proof-of-concept for the in-game manual reader.
 *
 *     manual_poc <manual.pdf>
 *     manual_poc --synth <snes|gbc|gba|WxH>[xN]
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
 * A MANUAL IS NOT NECESSARILY PORTRAIT. The one scan in this repo is 739x1080,
 * and building against it alone put its proportions into things that looked like
 * constants: the mesh density, the camera's lens, the zoom ceiling, the shadow
 * offset, the window size. `--synth` generates a booklet of any geometry with
 * legible text on it -- square like a Game Boy Color insert, wide and short like
 * a Game Boy Advance one -- so those can be judged without owning such a manual.
 * It runs the whole real path; only the page bytes are stand-ins.
 *
 * CONTROLS
 *   left / right, PgUp / PgDn   turn a page          space  turn forward
 *   wheel, +/-                  zoom                 0      reset zoom
 *   drag, arrows (when zoomed)   pan                 f      toggle 3D tilt
 *   home / end                  first / last page    t      cycle turn speed
 *   s                           spreads / single page
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
  /* The mesh is SOLVED per layout, not fixed -- see ManualTurn_SolveMesh. These
   * only size the buffers it can ask for.
   *
   * It used to be a fixed 16x6, chosen against the 739x1080 page in one window.
   * That is 4x over its own error budget on a square or wide page (which fill
   * the width instead of the height) and 6x over on a 1440p display, and with
   * the 3D tilt on, 6 rows is 7.5x over. A constant here is a constant tuned to
   * whichever manual the author happened to have. */
  kPageMaxVerts = (kManualMeshMaxColumns + 1) * (kManualMeshMaxRows + 1),
  kPageMaxIndices = kManualMeshMaxColumns * kManualMeshMaxRows * 6,
  /* Decoded pages held at once. Only three are ever visible (the settled page,
   * the leaf, and the page it lands on), so this is that plus one of slack.
   * A slot COUNT rather than a byte budget is only defensible because
   * LooksLikeAlbum guarantees one geometry throughout, so the slots cannot
   * differ wildly in size; the real manual's 739x1080 RGBA is 3.19 MB a slot. */
  kPageCacheSlots = 4,
};

/* One frame's worth of geometry. Held here rather than on the stack because the
 * solved mesh can reach 861 vertices and 4800 indices, and two ~47 KB frames
 * live at once during a turn -- fine on a desktop stack, but the overlay shim
 * this becomes will not want that per draw. */
typedef struct MeshBuffer {
  SDL_Vertex verts[kPageMaxVerts];
  int indices[kPageMaxIndices];
  int vert_count;
  int index_count;
} MeshBuffer;

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
  bool spread_mode;      /* two-up openings, as a real booklet reads */
  bool show_debug;
  float turn_seconds;
  bool dragging;

  /* Scratch for the page and leaf meshes, one at a time. */
  MeshBuffer mesh;
  ManualMesh density;    /* solved per frame from the sheet and camera */

  /* SYNTHETIC PAGES. A manual reader has to work for manuals that are not the
   * one manual in this repo -- square (Game Boy Color) and wide-and-short (Game
   * Boy Advance) are the shapes that break assumptions built around a portrait
   * scan. Owning such a manual is not a prerequisite for checking that, so the
   * demo can generate a booklet of any geometry with legible text on it. */
  bool synthetic;
  int synth_w, synth_h;

  /* True when --force admitted a file that is NOT a page album. What is on
   * screen is then not necessarily a manual, and the reader says so rather than
   * letting it be mistaken for one. */
  bool forced;

  /* --shot: render one deterministic frame, write it out, exit. The reader's
   * whole purpose is how it LOOKS, and "the process started" is not evidence of
   * that -- this makes a frame reviewable without a display, and lets a change
   * to the geometry be compared against the frame it produced before. */
  const char *shot_path;
  float shot_turn;

  bool vsync;
  /* Presented-frame rate, smoothed. The turn's DURATION is clock-driven and so
   * is honest at any cadence, but its smoothness is not -- and this demo used to
   * run unsynced at ~1000 fps, which is not what the game will present at. */
  float fps;

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

/* ── Synthetic pages ────────────────────────────────────────────────────────
 *
 * Draws a legible stand-in page at any geometry, so the reader can be judged on
 * shapes no manual in this repo has. Rendered into a target texture with the
 * renderer's own debug font -- no asset, no encoder, no file.
 *
 * WHAT IS ON THE PAGE IS CHOSEN TO EXPOSE THE THINGS THAT GO WRONG:
 *   - body text at reading size, because the whole point of a manual is text,
 *     and mesh warping shows up in strokes long before it shows in artwork;
 *   - a rule grid, whose straight lines make a bowed or under-tessellated sheet
 *     obvious in a way photographic content hides;
 *   - a gutter-side marker, so a mirrored leaf or a page on the wrong half of a
 *     spread is visible rather than merely plausible. */
static SDL_Texture *RenderSyntheticPage(Poc *poc, int page, int w, int h) {
  SDL_Texture *texture = SDL_CreateTexture(poc->renderer,
                                           SDL_PIXELFORMAT_RGBA8888,
                                           SDL_TEXTUREACCESS_TARGET, w, h);
  if (!texture) return NULL;
  SDL_Texture *previous = SDL_GetRenderTarget(poc->renderer);
  if (!SDL_SetRenderTarget(poc->renderer, texture)) {
    SDL_DestroyTexture(texture);
    return NULL;
  }

  const bool recto = (page % 2) == 0;   /* which side of the gutter it lands on */
  SDL_SetRenderDrawColor(poc->renderer, 246, 242, 232, 255);
  SDL_RenderClear(poc->renderer);

  /* A rule grid at a tenth of the page, so a cell is a fixed fraction of the
   * sheet whatever its shape. */
  SDL_SetRenderDrawColor(poc->renderer, 226, 220, 205, 255);
  for (int i = 1; i < 10; i++) {
    const float x = (float)w * (float)i / 10.0f;
    const float y = (float)h * (float)i / 10.0f;
    SDL_RenderLine(poc->renderer, x, 0.0f, x, (float)h);
    SDL_RenderLine(poc->renderer, 0.0f, y, (float)w, y);
  }

  /* The gutter edge, so the binding side is never ambiguous. */
  SDL_SetRenderDrawColor(poc->renderer, 120, 104, 84, 255);
  SDL_FRect gutter = { recto ? 0.0f : (float)w - 12.0f, 0.0f, 12.0f, (float)h };
  SDL_RenderFillRect(poc->renderer, &gutter);

  SDL_SetRenderDrawColor(poc->renderer, 96, 84, 70, 255);
  SDL_FRect border = { 20.0f, 20.0f, (float)w - 40.0f, (float)h - 40.0f };
  SDL_RenderRect(poc->renderer, &border);

  /* The page number, big. Scale is relative to the page's SHORT side so it is
   * the same visual weight on a wide page as on a tall one. */
  const float unit = (float)(w < h ? w : h) / 100.0f;
  char label[32];
  snprintf(label, sizeof label, "PAGE %d", page + 1);
  const float title_scale = unit * 0.55f;
  SDL_SetRenderScale(poc->renderer, title_scale, title_scale);
  SDL_SetRenderDrawColor(poc->renderer, 40, 36, 32, 255);
  SDL_RenderDebugText(poc->renderer, 46.0f / title_scale, 52.0f / title_scale,
                      label);

  /* Body text at reading size -- this is what tells you whether the turn, the
   * tilt and the mesh density are acceptable for something you have to READ. */
  const float body_scale = unit * 0.17f;
  SDL_SetRenderScale(poc->renderer, body_scale, body_scale);
  SDL_SetRenderDrawColor(poc->renderer, 58, 52, 46, 255);
  static const char *const kLines[] = {
    "The reader must carry a manual of any proportion: a tall",
    "SNES booklet, a square Game Boy Color insert, or a wide",
    "and short Game Boy Advance leaflet. None of the geometry",
    "below may assume the shape of the one scan it was built",
    "around -- not the fitted size, not the camera's lens, not",
    "the density of the mesh the turning sheet is drawn with.",
    "",
    "If these strokes smear or swim as the page turns, the",
    "mesh is too coarse for this sheet at this window size.",
    "If the page changes size when a turn begins, the leaf and",
    "the settled page are not sharing one projection.",
  };
  const float line_h = 11.0f;
  const float top = (float)h * 0.30f / body_scale;
  for (size_t i = 0; i < sizeof kLines / sizeof kLines[0]; i++)
    SDL_RenderDebugText(poc->renderer, 46.0f / body_scale,
                        top + (float)i * line_h, kLines[i]);

  snprintf(label, sizeof label, "%dx%d", w, h);
  SDL_RenderDebugText(poc->renderer, 46.0f / body_scale,
                      ((float)h - 44.0f) / body_scale, label);

  SDL_SetRenderScale(poc->renderer, 1.0f, 1.0f);
  SDL_SetRenderTarget(poc->renderer, previous);
  SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
  return texture;
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
  int w = 0, h = 0;

  if (poc->synthetic) {
    SDL_Texture *drawn = RenderSyntheticPage(poc, page, poc->synth_w,
                                             poc->synth_h);
    if (!drawn) {
      SDL_Log("page %d: synthetic render failed (%s)", page + 1, SDL_GetError());
      RememberFailure(poc, page);
      return NULL;
    }
    if (slot->texture) SDL_DestroyTexture(slot->texture);
    slot->texture = drawn;
    slot->page = page;
    slot->width = poc->synth_w;
    slot->height = poc->synth_h;
    slot->used_at = ++poc->clock;
    return slot;
  }

  int channels = 0;
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

/* THE camera. Both the settled page and the turning leaf are projected through
 * this one matrix, which is the whole fix for the page changing size the moment a
 * turn started: previously the flat page was laid out in pixel space while the
 * leaf went through a perspective projection with hand-picked scale factors, so
 * the two never agreed. */
static void ReaderCamera(bool tilt_3d, int view_w, int view_h, float fov_y,
                         float out[16]) {
  Scene3DCamera camera = {
    .tilt_x = tilt_3d ? -0.35f : 0.0f,
    .tilt_y = tilt_3d ? 0.22f : 0.0f,
    .distance = 2.6f,
    /* NOT a constant. A sheet lifts by its own width, so a wide sheet swings
     * deep toward the camera -- at a fixed 0.9 a wide manual read one page at a
     * time puts the leaf THROUGH the camera plane, and the leaf then fails to
     * project and is dropped entirely. ManualSheet_CameraFov narrows the lens
     * only when that would happen, so every shape that already framed well
     * keeps exactly the camera it had. */
    .fov_y = fov_y,
  };
  Scene3D_BuildViewProjection(&camera, view_w, view_h, out);
}

/* Emit the shared triangle list for a (kPageSubdivU x kPageSubdivV) grid.
 *
 * COLUMN-MAJOR (u outer, v inner), and that ordering is load-bearing. With no
 * depth test, the bowed sheet's self-overlap is resolved only by the later
 * triangle being the nearer one, which holds because depth rises monotonically
 * with u. Row-major emission resets u to 0 on every new row, so depth jumps
 * BACKWARD at each row boundary and a far triangle paints over a near one --
 * measured 6 out-of-order steps per frame, worst near a fully landed turn.
 * The sheet is constant in v, so ordering columns costs nothing. */
static void BuildGridIndices(MeshBuffer *mesh, const ManualMesh *density) {
  const int columns = density->columns, rows = density->rows;
  const int stride = columns + 1;
  int n = 0;
  for (int iu = 0; iu < columns; iu++) {
    for (int iv = 0; iv < rows; iv++) {
      const int base = iv * stride + iu;
      mesh->indices[n++] = base;
      mesh->indices[n++] = base + 1;
      mesh->indices[n++] = base + stride;
      mesh->indices[n++] = base + 1;
      mesh->indices[n++] = base + stride + 1;
      mesh->indices[n++] = base + stride;
    }
  }
  mesh->index_count = n;
  mesh->vert_count = stride * (rows + 1);
}

/* A settled page: the same unit sheet as the leaf, at rest, through the same
 * projection -- NOT a pixel-space rectangle. Offsets by pan in world units so a
 * zoomed page still pans. Returns false if any vertex fails to project. */
static bool BuildFlatPage(MeshBuffer *mesh, const ManualMesh *density,
                          const float matrix[16], int view_w, int view_h,
                          float x0, float x1, float half_y,
                          float pan_world_x, float pan_world_y, float shade) {
  SDL_Vertex *verts = mesh->verts;
  int v = 0;
  for (int iv = 0; iv <= density->rows; iv++) {
    for (int iu = 0; iu <= density->columns; iu++) {
      const float u = (float)iu / (float)density->columns;
      const float t = (float)iv / (float)density->rows;
      /* x0..x1 is where this SHEET sits: a spread's left half runs gutter-ward
       * from its outer edge, the right half the other way, and a single page
       * (either cover) spans the middle. Vertical extent is always the full
       * sheet, so a page is never short. */
      const float wx = (x0 + (x1 - x0) * u) - pan_world_x;
      const float wy = (0.5f - t) * 2.0f * half_y + pan_world_y;
      Scene3DPoint screen;
      if (!Scene3D_ProjectWorldPoint(matrix, wx, wy, 0.0f,
                                     view_w, view_h, &screen))
        return false;
      verts[v].position.x = screen.x;
      verts[v].position.y = screen.y;
      verts[v].tex_coord.x = u;
      verts[v].tex_coord.y = t;
      verts[v].color = (SDL_FColor){ shade, shade, shade, 1.0f };
      v++;
    }
  }
  BuildGridIndices(mesh, density);
  return true;
}

/* The turning leaf, projected through the game's own scene3d_math.
 *
 * Returns false if ANY vertex fails to project -- Scene3D_ProjectWorldPoint
 * refuses points at or behind the camera plane, and drawing a primitive with a
 * partially-projected vertex set turns it inside out. Rejecting the whole leaf
 * for one bad vertex is the documented contract. */
static bool BuildLeaf(MeshBuffer *mesh, const ManualMesh *density, float turn,
                      const float matrix[16], int view_w, int view_h,
                      const ManualSheet *sheet,
                      float pan_world_x, float pan_world_y,
                      bool mirrored, float alpha) {
  SDL_Vertex *verts = mesh->verts;
  int v = 0;
  for (int iv = 0; iv <= density->rows; iv++) {
    for (int iu = 0; iu <= density->columns; iu++) {
      const float u = (float)iu / (float)density->columns;
      const float t = (float)iv / (float)density->rows;
      float lx = 0.0f, ly = 0.0f, lz = 0.0f;
      ManualTurn_LeafPoint(turn, u, t, &lx, &ly, &lz);
      /* PLACED BY THE MODULE, not by an expression here. The hinge and the
       * sheet's width differ between layouts -- gutter and one half in a spread,
       * the page's own outer edge and the WHOLE width on a single page -- and
       * this used to scale by half_x in both, which drew a half-width leaf
       * hinged down the middle of the text whenever spreads were off.
       *
       * Lift shares the width scale, since the rotation is in the x/z plane. */
      Scene3DPoint screen;
      if (!Scene3D_ProjectWorldPoint(matrix,
                                     ManualTurn_LeafWorldX(sheet, turn, lx) -
                                         pan_world_x,
                                     -ly * 2.0f * sheet->half_y + pan_world_y,
                                     lz * 2.0f * sheet->width,
                                     view_w, view_h, &screen))
        return false;
      const float shade = ManualTurn_LeafShade(turn, u);
      verts[v].position.x = screen.x;
      verts[v].position.y = screen.y;
      /* Whether u is flipped is decided by ManualTurn_ResolveFrame, not here: it
       * depends on the turn DIRECTION as well as the visible face, and deciding
       * it from the face alone reversed every backward turn. */
      verts[v].tex_coord.x = mirrored ? 1.0f - u : u;
      verts[v].tex_coord.y = t;
      verts[v].color = (SDL_FColor){ shade, shade, shade, alpha };
      v++;
    }
  }
  BuildGridIndices(mesh, density);
  return true;
}

/* ── Frame ──────────────────────────────────────────────────────────────────── */

/* Draw one settled sheet: look up its texture, place it, render it. Returns
 * false only if there was nothing to draw. */
static bool DrawSheet(Poc *poc, int page, const float matrix[16],
                      int view_w, int view_h, float x0, float x1, float half_y,
                      float pan_x, float pan_y) {
  if (page < 0) return false;
  PageTexture *texture = LoadPage(poc, page);
  if (!texture) return false;
  if (!BuildFlatPage(&poc->mesh, &poc->density, matrix, view_w, view_h, x0, x1,
                     half_y, pan_x, pan_y, 1.0f))
    return false;
  SDL_RenderGeometry(poc->renderer, texture->texture, poc->mesh.verts,
                     poc->mesh.vert_count, poc->mesh.indices,
                     poc->mesh.index_count);
  return true;
}

static void DrawFrame(Poc *poc) {
  int view_w = 0, view_h = 0;
  SDL_GetRenderOutputSize(poc->renderer, &view_w, &view_h);

  SDL_SetRenderDrawColor(poc->renderer, 24, 22, 30, 255);
  SDL_RenderClear(poc->renderer);

  const int items = poc->spread_mode
      ? ManualPages_SpreadCount(poc->index.count) : poc->index.count;
  if (items <= 0) { SDL_RenderPresent(poc->renderer); return; }

  /* EVERY page-identity decision comes from here. Doing it inline is what
   * produced both of the reported faults: the underlying pages jumped to the
   * destination on frame one, and a backward turn showed its leaf mirrored. */
  ManualTurnFrame frame;
  if (!ManualTurn_ResolveFrame(&poc->view, poc->index.count,
                               poc->spread_mode, &frame)) {
    SDL_RenderPresent(poc->renderer);
    return;
  }

  /* THE BOOK'S GEOMETRY, not a page's. Taken from the index, which knows every
   * page's size before anything is decoded -- so the layout does not depend on
   * which texture happens to be resident, and cannot change on the frame a
   * decode lands. It also means an admitted mixed album lays out to one stable
   * size instead of rescaling the view as the reader pages through it. */
  int nominal_w = 0, nominal_h = 0;
  if (!ManualPages_NominalGeometry(&poc->index, &nominal_w, &nominal_h)) {
    SDL_RenderPresent(poc->renderer);
    return;
  }

  /* THE LAYOUT WIDTH IS CONSTANT IN SPREAD MODE: always two pages, whatever the
   * current opening holds. Sizing a cover to one page and an interior opening to
   * two rescaled the entire view by 2x the moment a cover was turned, so every
   * page visibly jumped mid-animation. A lone page instead occupies ONE HALF of
   * the same area -- which is also what the paper does, since a front cover is
   * one leaf of the opening it swings away from. */
  const int fit_w = nominal_w * ManualPages_LayoutPageWidths(poc->spread_mode);
  float page_w = 0.0f, page_h = 0.0f;
  ManualView_FittedSize(fit_w, nominal_h, view_w, view_h,
                        poc->view.zoom, &page_w, &page_h);

  /* The lens is solved BEFORE the projection, from how much of the view the
   * sheet occupies -- a wide sheet lifts deep enough to reach the camera at the
   * preferred fov, and the leaf is then dropped for failing to project. */
  float matrix[16];
  ReaderCamera(poc->tilt_3d, view_w, view_h,
               ManualSheet_CameraFov(
                   ManualSheet_PixelWidth(page_w, poc->spread_mode),
                   view_h, 0.9f),
               matrix);

  ManualSheet sheet;
  if (!ManualSheet_Solve(matrix, view_w, view_h, page_w, page_h,
                         poc->spread_mode, &sheet)) {
    SDL_RenderPresent(poc->renderer);
    return;
  }
  const float half_x = sheet.half_x, half_y = sheet.half_y;

  /* Mesh density follows the sheet and the camera. Solved per frame because the
   * window is resizable and the tilt is a keypress -- both change the answer. */
  ManualTurn_SolveMesh(matrix, &sheet,
                       (float)kManualMeshBudgetCentipixels / 100.0f,
                       &poc->density);

  const float pan_world_x = page_w > 0.0f
      ? poc->view.pan_x * (2.0f * half_x / page_w) : 0.0f;
  const float pan_world_y = page_h > 0.0f
      ? poc->view.pan_y * (2.0f * half_y / page_h) : 0.0f;

  /* ORDER IS THE WHOLE CORRECTNESS ARGUMENT, and it never changes mid-turn.
   * 1. the settled pages, beneath everything. Each occupies its own half of the
   *    constant-width area; a missing side simply draws nothing, which is what
   *    keeps a cover on its correct half without resizing anything. */
  if (poc->spread_mode) {
    DrawSheet(poc, frame.left_page, matrix, view_w, view_h,
              -half_x, 0.0f, half_y, pan_world_x, pan_world_y);
    DrawSheet(poc, frame.right_page, matrix, view_w, view_h,
              0.0f, half_x, half_y, pan_world_x, pan_world_y);
  } else {
    const int page = frame.right_page >= 0 ? frame.right_page : frame.left_page;
    DrawSheet(poc, page, matrix, view_w, view_h, -half_x, half_x, half_y,
              pan_world_x, pan_world_y);
  }

  if (poc->view.turn != 0.0f && frame.leaf_page >= 0) {
    PageTexture *leaf = LoadPage(poc, frame.leaf_page);
    if (leaf) {
      /* 2. the shadow, offset toward the side the sheet is falling AWAY from, so
       *    it reads as cast by a lifted page rather than pasted under it.
       *
       *    THE OFFSET IS A FRACTION OF THE SHEET, not a pixel count. It was 10
       *    and 14 px, which is a sensible drop for a 739-wide page and a heavy
       *    one for a short GBA page a third the height -- the shadow has to
       *    scale with what is casting it. These ratios reproduce the original
       *    offsets on the page they were chosen for. */
      const float drop = sheet.pixels_w;
      if (BuildLeaf(&poc->mesh, &poc->density, poc->view.turn, matrix, view_w,
                    view_h, &sheet, pan_world_x, pan_world_y,
                    frame.leaf_mirrored, 0.30f)) {
        const float shadow_dx = (frame.leaf_on_right ? 0.0135f : -0.0135f) * drop;
        const float shadow_dy = 0.0189f * drop;
        for (int i = 0; i < poc->mesh.vert_count; i++) {
          poc->mesh.verts[i].position.x += shadow_dx;
          poc->mesh.verts[i].position.y += shadow_dy;
          poc->mesh.verts[i].color = (SDL_FColor){ 0.0f, 0.0f, 0.0f, 0.30f };
        }
        SDL_RenderGeometry(poc->renderer, NULL, poc->mesh.verts,
                           poc->mesh.vert_count, poc->mesh.indices,
                           poc->mesh.index_count);
      }
      /* 3. the leaf itself. */
      if (BuildLeaf(&poc->mesh, &poc->density, poc->view.turn, matrix, view_w,
                    view_h, &sheet, pan_world_x, pan_world_y,
                    frame.leaf_mirrored, 1.0f))
        SDL_RenderGeometry(poc->renderer, leaf->texture, poc->mesh.verts,
                           poc->mesh.vert_count, poc->mesh.indices,
                           poc->mesh.index_count);
    }
  }

  if (poc->show_debug) {
    float zoom_min = 0.0f, zoom_max = 0.0f;
    ManualView_ZoomLimit(fit_w, nominal_h, view_w, view_h, &zoom_min, &zoom_max);
    char line[320];
    snprintf(line, sizeof line,
             "%s %d/%d  L%d R%d  leaf %d%s  %dx%d page  %s  zoom %.2f/%.2fx  "
             "mesh %dx%d  turn %+.2f %.2fs  %s  %.0f fps%s",
             poc->spread_mode ? "opening" : "page",
             poc->view.item + 1, items,
             frame.left_page + 1, frame.right_page + 1,
             frame.leaf_page + 1, frame.leaf_mirrored ? "m" : "",
             nominal_w, nominal_h,
             poc->spread_mode ? "spread" : "single",
             (double)poc->view.zoom, (double)zoom_max,
             poc->density.columns, poc->density.rows,
             (double)poc->view.turn, (double)poc->turn_seconds,
             poc->tilt_3d ? "3D" : "flat",
             (double)poc->fps, poc->vsync ? "" : " UNSYNCED");
    SDL_SetRenderDrawColor(poc->renderer, 0, 0, 0, 190);
    SDL_SetRenderDrawBlendMode(poc->renderer, SDL_BLENDMODE_BLEND);
    SDL_FRect bar = { 0.0f, 0.0f, (float)view_w, 22.0f };
    SDL_RenderFillRect(poc->renderer, &bar);
    SDL_SetRenderDrawColor(poc->renderer, 235, 230, 220, 255);
    SDL_RenderDebugText(poc->renderer, 8.0f, 7.0f, line);
  }

  /* --force admitted a file that FAILED the album test, so what is on screen is
   * probably figures or letterheads rather than a manual. Said on screen, not
   * only in the terminal the demo was launched from: the whole risk of --force
   * is that the result looks plausible. */
  if (poc->forced) {
    SDL_SetRenderDrawColor(poc->renderer, 120, 30, 20, 220);
    SDL_SetRenderDrawBlendMode(poc->renderer, SDL_BLENDMODE_BLEND);
    SDL_FRect bar = { 0.0f, (float)view_h - 22.0f, (float)view_w, 22.0f };
    SDL_RenderFillRect(poc->renderer, &bar);
    SDL_SetRenderDrawColor(poc->renderer, 255, 235, 225, 255);
    SDL_RenderDebugText(poc->renderer, 8.0f, (float)view_h - 15.0f,
                        "--force: this file is NOT a page album. These may be "
                        "figures or letterheads, not manual pages.");
  }

  SDL_RenderPresent(poc->renderer);
}

/* ── Input ──────────────────────────────────────────────────────────────────── */

static void PageDimensions(Poc *poc, int *w, int *h) {
  /* The book's geometry, by the same call the draw path uses -- so the zoom and
   * pan clamps cannot disagree with what is on screen. No 739x1080 fallback:
   * that was the one manual in this repo standing in for every manual, and with
   * no pages there is nothing to clamp against anyway. */
  if (!ManualPages_NominalGeometry(&poc->index, w, h)) { *w = 0; *h = 0; return; }
  /* Same constant width the draw path uses, from the same function, so the
   * clamps cannot disagree with what is on screen. */
  *w *= ManualPages_LayoutPageWidths(poc->spread_mode);
}

/* Items in the current layout: openings when two-up, pages otherwise. */
static int ItemCount(const Poc *poc) {
  return poc->spread_mode ? ManualPages_SpreadCount(poc->index.count)
                          : poc->index.count;
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
            ManualView_BeginTurn(&poc->view, +1, ItemCount(poc));
          break;
        case SDLK_LEFT: case SDLK_PAGEUP:
          if (poc->view.zoom > 1.001f)
            ManualView_Pan(&poc->view, -60.0f, 0.0f, pw, ph, view_w, view_h);
          else
            ManualView_BeginTurn(&poc->view, -1, ItemCount(poc));
          break;
        case SDLK_UP:
          ManualView_Pan(&poc->view, 0.0f, -60.0f, pw, ph, view_w, view_h);
          break;
        case SDLK_DOWN:
          ManualView_Pan(&poc->view, 0.0f, 60.0f, pw, ph, view_w, view_h);
          break;
        case SDLK_HOME: ManualView_GoTo(&poc->view, 0, ItemCount(poc)); break;
        case SDLK_END:
          ManualView_GoTo(&poc->view, ItemCount(poc) - 1, ItemCount(poc));
          break;
        case SDLK_EQUALS: case SDLK_KP_PLUS:
          ManualView_Zoom(&poc->view, 1.25f, pw, ph, view_w, view_h); break;
        case SDLK_MINUS: case SDLK_KP_MINUS:
          ManualView_Zoom(&poc->view, 0.8f, pw, ph, view_w, view_h); break;
        case SDLK_0:
          ManualView_Zoom(&poc->view, 0.0001f, pw, ph, view_w, view_h); break;
        case SDLK_F: poc->tilt_3d = !poc->tilt_3d; break;
        case SDLK_S: {
          /* Toggle layout, keeping the CURRENT PAGE in view rather than the item
           * index -- opening 5 and page 5 are different places, so reusing the
           * index would jump the reader somewhere arbitrary. */
          ManualSpread spread = { -1, poc->view.item };
          if (poc->spread_mode)
            ManualPages_SpreadAt(poc->index.count, poc->view.item, &spread);
          const int page = spread.right >= 0 ? spread.right : spread.left;
          poc->spread_mode = !poc->spread_mode;
          const int item = poc->spread_mode
              ? ManualPages_SpreadForPage(poc->index.count, page) : page;
          ManualView_GoTo(&poc->view, item < 0 ? 0 : item, ItemCount(poc));
          break;
        }
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

/* --synth <preset|WxH>[xN]. The presets are the shapes that break assumptions
 * built around a portrait scan, so they are the ones worth having by name. */
static bool ParseSynth(const char *spec, int *out_w, int *out_h, int *out_pages) {
  static const struct { const char *name; int w, h; } kPresets[] = {
    { "snes", 739, 1080 },   /* the real manual's proportions */
    { "gbc",  900,  900 },   /* square */
    { "gba", 1000,  620 },   /* wide and short */
  };
  int pages = 24;
  for (size_t i = 0; i < sizeof kPresets / sizeof kPresets[0]; i++) {
    size_t n = strlen(kPresets[i].name);
    if (strncmp(spec, kPresets[i].name, n) != 0) continue;
    if (spec[n] == 'x' && atoi(spec + n + 1) > 0) pages = atoi(spec + n + 1);
    else if (spec[n] != '\0') continue;
    *out_w = kPresets[i].w;
    *out_h = kPresets[i].h;
    *out_pages = pages;
    return true;
  }
  int w = 0, h = 0, n = 0;
  const int fields = sscanf(spec, "%dx%dx%d", &w, &h, &n);
  if (fields < 2 || w <= 0 || h <= 0) return false;
  if (fields == 3 && n > 0) pages = n;
  *out_w = w;
  *out_h = h;
  *out_pages = pages;
  return true;
}

static void Usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s [--force] <manual.pdf>\n"
          "       %s --synth <preset|WxH>[xN]\n\n"
          "Reads page images out of a scan-album PDF and opens the reader.\n"
          "A PDF whose pages are vector text yields no images -- that is the\n"
          "correct answer, and the case the builder must rasterise upstream.\n\n"
          "--synth generates a booklet of any page geometry instead, so the\n"
          "reader can be judged on shapes no manual here has:\n"
          "    snes   739x1080   the real manual's proportions\n"
          "    gbc    900x900    square\n"
          "    gba   1000x620    wide and short\n"
          "  e.g. --synth gba        --synth gbcx40       --synth 1400x500x12\n\n"
          "--single opens in single-page layout, --tilt with the 3D tilt on.\n"
          "--shot FILE renders ONE deterministic frame to a BMP and exits, so a\n"
          "  frame can be reviewed or diffed with no display; --turn T picks the\n"
          "  phase (0 settled, 0.45 mid-sweep, negative for a backward turn).\n",
          argv0, argv0);
}

int main(int argc, char **argv) {
  if (argc < 2) { Usage(argv[0]); return 2; }

  bool force = false, single = false, tilt = false;
  const char *path = NULL;
  const char *synth = NULL;
  const char *shot = NULL;
  float shot_turn = 0.45f;   /* mid-sweep, where the leaf is most of the frame */
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--force") == 0) force = true;
    else if (strcmp(argv[i], "--synth") == 0 && i + 1 < argc) synth = argv[++i];
    else if (strncmp(argv[i], "--synth=", 8) == 0) synth = argv[i] + 8;
    else if (strcmp(argv[i], "--single") == 0) single = true;
    else if (strcmp(argv[i], "--tilt") == 0) tilt = true;
    else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot = argv[++i];
    else if (strcmp(argv[i], "--turn") == 0 && i + 1 < argc)
      shot_turn = (float)atof(argv[++i]);
    else if (strncmp(argv[i], "--", 2) == 0) { Usage(argv[0]); return 2; }
    else path = argv[i];
  }
  if (!path && !synth) { fprintf(stderr, "%s: no input file\n", argv[0]); return 2; }

  Poc poc;
  memset(&poc, 0, sizeof poc);
  poc.turn_seconds = 0.34f;
  poc.show_debug = true;
  /* Spreads on by default: artwork (maps especially) is drawn across the gutter,
   * so single-page is the mode that CUTS pictures in half, not the safe default. */
  poc.spread_mode = !single;
  poc.tilt_3d = tilt;
  poc.shot_path = shot;
  poc.shot_turn = shot_turn;
  ManualView_Init(&poc.view);

  if (synth) {
    int pages = 0;
    if (!ParseSynth(synth, &poc.synth_w, &poc.synth_h, &pages)) {
      fprintf(stderr, "%s: cannot read --synth '%s'\n\n", argv[0], synth);
      Usage(argv[0]);
      return 2;
    }
    if (pages > kManualMaxPages) pages = kManualMaxPages;
    poc.synthetic = true;
    /* A synthetic index carries geometry and no bytes: the page cache renders
     * these rather than decoding them, and everything downstream -- layout,
     * spreads, the camera, the mesh -- runs exactly as it does for a real
     * album. That is the point; a separate path would prove nothing. */
    for (int i = 0; i < pages; i++) {
      poc.index.pages[i].width = (uint16_t)poc.synth_w;
      poc.index.pages[i].height = (uint16_t)poc.synth_h;
    }
    poc.index.count = pages;
    printf("synthetic manual: %d pages at %dx%d\n", pages, poc.synth_w,
           poc.synth_h);
  } else {

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
    poc.forced = true;
  }

  }   /* end of the real-file path */

  /* THE WINDOW IS SIZED FROM THE BOOK. A fixed 960x1040 is a portrait window,
   * which frames the ActRaiser manual and letterboxes a wide one into a strip.
   * Fit the opening's own proportions into a sane box instead, so the reader
   * opens showing the manual rather than showing the margins. */
  int window_w = 960, window_h = 1040;
  {
    int nominal_w = 0, nominal_h = 0;
    if (ManualPages_NominalGeometry(&poc.index, &nominal_w, &nominal_h)) {
      const float aspect =
          (float)(nominal_w * ManualPages_LayoutPageWidths(poc.spread_mode)) /
          (float)nominal_h;
      /* Aim for a constant AREA rather than a constant height, so a wide book
       * and a tall one open at comparable size instead of one of them filling
       * the display. */
      const float area = 960.0f * 1040.0f;
      float h = sqrtf(area / aspect);
      float w = h * aspect;
      const float cap_w = 1680.0f, cap_h = 1200.0f;
      const float shrink = SDL_min(1.0f, SDL_min(cap_w / w, cap_h / h));
      window_w = (int)(w * shrink);
      window_h = (int)(h * shrink);
      if (window_w < 480) window_w = 480;
      if (window_h < 360) window_h = 360;
    }
  }

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    free(poc.file);
    return 1;
  }
  if (!SDL_CreateWindowAndRenderer("Manual reader (proof of concept)",
                                   window_w, window_h,
                                   SDL_WINDOW_RESIZABLE |
                                       SDL_WINDOW_HIGH_PIXEL_DENSITY,
                                   &poc.window, &poc.renderer)) {
    fprintf(stderr, "SDL_CreateWindowAndRenderer: %s\n", SDL_GetError());
    SDL_Quit();
    free(poc.file);
    return 1;
  }

  /* VSYNC ON. Unsynced, this spun at ~1000 fps -- and every judgement about
   * whether the turn feels right was being made at a cadence no player will
   * ever see. The DURATION was always honest (the animation is clock-driven),
   * but smoothness is not a property of duration. If the display cannot sync,
   * the diagnostics bar says UNSYNCED rather than letting the reading stand. */
  poc.vsync = SDL_SetRenderVSync(poc.renderer, 1);
  if (!poc.vsync)
    SDL_Log("vsync unavailable (%s) -- pacing is NOT representative",
            SDL_GetError());

  if (poc.shot_path) {
    /* One frame, at a fixed page and a fixed phase, so two runs of the same
     * build produce the same image and two builds can be compared. */
    const int items = ItemCount(&poc);
    ManualView_GoTo(&poc.view, items > 4 ? 3 : 0, items);
    if (poc.shot_turn != 0.0f) {
      ManualView_BeginTurn(&poc.view, poc.shot_turn > 0.0f ? +1 : -1, items);
      poc.view.turn = poc.shot_turn;
    }
    DrawFrame(&poc);
    SDL_Surface *shot_surface = SDL_RenderReadPixels(poc.renderer, NULL);
    int ok = 0;
    if (shot_surface) {
      ok = SDL_SaveBMP(shot_surface, poc.shot_path);
      SDL_DestroySurface(shot_surface);
    }
    if (!ok) fprintf(stderr, "shot failed: %s\n", SDL_GetError());
    else printf("wrote %s\n", poc.shot_path);
    for (int i = 0; i < kPageCacheSlots; i++)
      if (poc.cache[i].texture) SDL_DestroyTexture(poc.cache[i].texture);
    SDL_DestroyRenderer(poc.renderer);
    SDL_DestroyWindow(poc.window);
    SDL_Quit();
    free(poc.file);
    return ok ? 0 : 1;
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
    if (elapsed > 0.0f) {
      const float instant = 1.0f / elapsed;
      poc.fps = poc.fps > 0.0f ? poc.fps * 0.9f + instant * 0.1f : instant;
    }

    DrawFrame(&poc);
    /* Only sleep when nothing is pacing us. With vsync the present blocks, and
     * an extra delay on top of it just drops frames. */
    if (!poc.vsync) SDL_Delay(1);
  }

  for (int i = 0; i < kPageCacheSlots; i++)
    if (poc.cache[i].texture) SDL_DestroyTexture(poc.cache[i].texture);
  SDL_DestroyRenderer(poc.renderer);
  SDL_DestroyWindow(poc.window);
  SDL_Quit();
  free(poc.file);
  return 0;
}
