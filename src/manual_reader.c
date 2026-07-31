#include "manual_reader.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_display.h"
#include "input_map.h"
#include "manual_input.h"
#include "manual_pages.h"
#include "scene3d_math.h"
#include "settings.h"
/* For the game's own menu font: the overlay owns the atlases. */
#include "settings_overlay.h"

/* Declarations only. src/hd_replacement_host.c owns STB_IMAGE_IMPLEMENTATION
 * for the whole binary; this file must not define it again. JPEG support is one
 * more STBI_ONLY_* beside the PNG one there -- the macros are a positive
 * allowlist, so the manual costs a #define rather than a dependency. */
#include "stb_image.h"

extern SDL_Renderer *g_renderer;

enum {
  /* Only three pages are ever on screen at once -- the settled page, the leaf,
   * and the page it lands on -- so this is that plus one slot of slack, which
   * keeps a turn from evicting a page it is about to want back. */
  kCacheSlots = 4,
  /* Decodes allowed per presented frame. ONE. See ManualReader_NextDecode. */
  kDecodesPerFrame = 1,
  kMaxFailures = 64,
  kPageMaxVerts = (kManualMeshMaxColumns + 1) * (kManualMeshMaxRows + 1),
  kPageMaxIndices = kManualMeshMaxColumns * kManualMeshMaxRows * 6,
};

/* Where a player's manual goes. Relative on purpose: a shipped bundle chdirs
 * beside its executable at startup precisely so game-assets/ resolves, and an
 * in-tree dev build keeps the working directory authoritative. Same rule the HD
 * art and music manifests already follow. */
static const char kManualPath[] = "game-assets/manual.pdf";

typedef struct PageTexture {
  SDL_Texture *texture;
  int page;
  uint64_t used_at;
} PageTexture;

static struct {
  bool load_attempted;
  uint8_t *file;
  size_t file_size;
  ManualPageIndex index;
  char status[192];

  bool open;
  ManualView view;

  /* Present-thread only, all of it. */
  PageTexture cache[kCacheSlots];
  uint64_t clock;
  uint64_t last_tick_ns;
  int failed[kMaxFailures];
  int failed_count;
  SDL_Vertex verts[kPageMaxVerts];
  int indices[kPageMaxIndices];
} s_reader;

/* Re-shows the hint line. Defined with the drawing it belongs to; declared here
 * because opening the reader and every input handler bump it. */
static void BumpHint(void);

/* WHICH DEVICE THE HINT SHOULD DESCRIBE. Tracked rather than queried, because
 * the question is "what is the player holding", and InputMap_GamepadIsActive
 * answers the narrower "is a pad button down THIS INSTANT" -- true only while
 * something is pressed, so a hint keyed off it would flicker between devices.
 * Every input handler sets this, so it follows whatever was last touched. */
static ManualHintDevice s_hint_device;

/* Latest left-stick position, -1..1, deadzone already removed. Held rather than
 * consumed as it arrives: a stick parked at full deflection emits no further
 * events, so panning has to be applied every frame from the last known position
 * rather than driven by the events themselves. Written on the main thread, read
 * by the draw -- two independent float stores, and a torn read costs one frame
 * of slightly stale aim. */
static float s_stick_x, s_stick_y;

/* Mouse drag state is session state, not page state. It is reset on both sides
 * of an open/close transition because the button-up that ended a drag can arrive
 * after the reader has closed and main.c will then correctly route it elsewhere. */
static bool s_dragging;
static int s_drag_x, s_drag_y;

/* ── Loading ───────────────────────────────────────────────────────────────── */

static void SetStatus(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(s_reader.status, sizeof s_reader.status, fmt, args);
  va_end(args);
}

bool ManualReader_Load(void) {
  if (s_reader.load_attempted) return s_reader.index.count > 0;
  s_reader.load_attempted = true;

  FILE *file = fopen(kManualPath, "rb");
  if (!file) {
    SetStatus("No manual: %s not found.", kManualPath);
    return false;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    SetStatus("No manual: %s could not be read.", kManualPath);
    return false;
  }
  const long size = ftell(file);
  rewind(file);
  if (size <= 0) {
    fclose(file);
    SetStatus("No manual: %s is empty.", kManualPath);
    return false;
  }
  s_reader.file = (uint8_t *)malloc((size_t)size);
  if (!s_reader.file) {
    fclose(file);
    SetStatus("No manual: out of memory reading %s.", kManualPath);
    return false;
  }
  s_reader.file_size = fread(s_reader.file, 1, (size_t)size, file);
  fclose(file);

  const int found = ManualPages_CarveAlbum(s_reader.file, s_reader.file_size,
                                           &s_reader.index);
  /* REFUSED RATHER THAN SHOWN when it is not an album. "I found a JPEG" is a
   * harmful success signal -- a document with a logo on every page satisfies it
   * and would present the player a booklet of letterheads. The predicate tests
   * completeness, and a manual that fails it is one the builder has to convert
   * rather than one the reader should improvise over. */
  if (found == 0 || !ManualPages_LooksLikeAlbum(&s_reader.index,
                                                s_reader.file_size)) {
    SetStatus(found == 0
                  ? "No manual: %s holds no page images (needs converting)."
                  : "No manual: %s is not a page album (needs converting).",
              kManualPath);
    free(s_reader.file);
    s_reader.file = NULL;
    s_reader.file_size = 0;
    memset(&s_reader.index, 0, sizeof s_reader.index);
    return false;
  }

  int w = 0, h = 0;
  ManualPages_NominalGeometry(&s_reader.index, &w, &h);
  SetStatus("%d pages, %dx%d.", s_reader.index.count, w, h);
  return true;
}

bool ManualReader_Available(void) {
  /* The overlay asks before it can decide whether the section exists. Keep the
   * file work lazy until that first menu visit, then Load's one-shot gate makes
   * every subsequent navigation/render query a cheap state read. */
  return ManualReader_Load();
}

const char *ManualReader_Status(void) {
  return s_reader.status[0] ? s_reader.status : "Manual not loaded yet.";
}

int ManualReader_PageCount(void) { return s_reader.index.count; }

/* ── Open / close ──────────────────────────────────────────────────────────── */

static bool SpreadMode(void) { return g_settings.manual_spreads; }

static int ItemCount(void) {
  return SpreadMode() ? ManualPages_SpreadCount(s_reader.index.count)
                      : s_reader.index.count;
}

bool ManualReader_IsOpen(void) { return s_reader.open; }

bool ManualReader_Open(void) {
  if (!ManualReader_Load()) return false;
  if (s_reader.open) return true;
  ManualView_Init(&s_reader.view);
  s_reader.open = true;
  /* Zeroed so the first rendered frame measures its own elapsed time from that
   * frame rather than from whenever the reader was last closed -- otherwise the
   * first turn after a long pause advances by the whole gap at once. */
  s_reader.last_tick_ns = 0;
  /* Seed from the configured/active device because the reader opens with no
   * input event of its own. A connected pad is not enough: Keyboard mode disables
   * it, and in Auto the control that invoked the action is the useful answer. On
   * a handheld Gamepad mode still gives the only-present controls immediately. */
  const bool gamepad_connected = InputMap_GamepadCount() > 0;
  const bool gamepad_preferred =
      gamepad_connected &&
      (g_settings.input_device == kInputDevice_Gamepad ||
       (g_settings.input_device == kInputDevice_Auto &&
        InputMap_GamepadIsActive()));
  s_hint_device = gamepad_preferred ? kManualHintDevice_Gamepad
                                    : kManualHintDevice_Keyboard;
  /* Cleared, or a stick held as the reader opened would pan on the first frame
   * from a position nobody has touched since. */
  s_stick_x = s_stick_y = 0.0f;
  s_dragging = false;
  BumpHint();
  fprintf(stderr, "[manual] opened (%s)\n", ManualReader_Status());
  return true;
}

void ManualReader_Close(void) {
  if (!s_reader.open) return;
  s_reader.open = false;
  s_dragging = false;
  fprintf(stderr, "[manual] closed\n");
}

void ManualReader_DestroyTextures(void) {
  for (int i = 0; i < kCacheSlots; i++) {
    if (s_reader.cache[i].texture) SDL_DestroyTexture(s_reader.cache[i].texture);
    s_reader.cache[i].texture = NULL;
    s_reader.cache[i].page = -1;
    s_reader.cache[i].used_at = 0;
  }
  s_reader.clock = 0;
  /* Texture creation/upload failures can be caused by the renderer being lost.
   * Give those pages one fresh attempt after its replacement. Corrupt source
   * pages may be decoded once more too, then return to the normal failure gate. */
  s_reader.failed_count = 0;
}

/* ── Applying intent ───────────────────────────────────────────────────────── */

/* Page dimensions as the clamps need them: the layout width, in source pixels. */
static void LayoutDimensions(int *out_w, int *out_h) {
  int w = 0, h = 0;
  if (!ManualPages_NominalGeometry(&s_reader.index, &w, &h)) { w = 0; h = 0; }
  *out_w = w * ManualPages_LayoutPageWidths(SpreadMode());
  *out_h = h;
}

static void ApplyIntent(ManualIntent intent, SDL_Rect viewport) {
  /* ANY input brings the hint back, whatever it was -- someone reaching for a
   * control is exactly who needs to be told what the controls are. One place,
   * so keyboard, pad and mouse cannot drift apart. */
  BumpHint();
  int pw = 0, ph = 0;
  LayoutDimensions(&pw, &ph);
  const int vw = viewport.w > 0 ? viewport.w : 1;
  const int vh = viewport.h > 0 ? viewport.h : 1;
  /* A pan step in fit-relative units. A fraction of the view rather than a pixel
   * count, so one press moves the same proportion of the page on a handheld and
   * on a desktop monitor. */
  const float pan_step = (float)vh * 0.12f;

  switch (intent) {
    case kManualIntent_PageForward:
      ManualView_BeginTurn(&s_reader.view, +1, ItemCount());
      break;
    case kManualIntent_PageBack:
      ManualView_BeginTurn(&s_reader.view, -1, ItemCount());
      break;
    case kManualIntent_First: ManualView_GoTo(&s_reader.view, 0, ItemCount()); break;
    case kManualIntent_Last:
      ManualView_GoTo(&s_reader.view, ItemCount() - 1, ItemCount());
      break;
    case kManualIntent_ZoomIn:
      ManualView_Zoom(&s_reader.view, 1.25f, pw, ph, vw, vh);
      break;
    case kManualIntent_ZoomOut:
      ManualView_Zoom(&s_reader.view, 0.8f, pw, ph, vw, vh);
      break;
    case kManualIntent_ZoomReset:
      /* A factor small enough to hit the floor from any zoom, which is what
       * "reset" means when the floor is fit. */
      ManualView_Zoom(&s_reader.view, 0.0001f, pw, ph, vw, vh);
      break;
    case kManualIntent_PanLeft:
      ManualView_Pan(&s_reader.view, -pan_step, 0.0f, pw, ph, vw, vh); break;
    case kManualIntent_PanRight:
      ManualView_Pan(&s_reader.view, pan_step, 0.0f, pw, ph, vw, vh); break;
    case kManualIntent_PanUp:
      ManualView_Pan(&s_reader.view, 0.0f, -pan_step, pw, ph, vw, vh); break;
    case kManualIntent_PanDown:
      ManualView_Pan(&s_reader.view, 0.0f, pan_step, pw, ph, vw, vh); break;
    case kManualIntent_Close: ManualReader_Close(); break;
    case kManualIntent_None: break;
  }
}

/* The viewport the last frame was drawn into. Input arrives on the main thread
 * with no viewport of its own, and the zoom and pan clamps are meaningless
 * without one; taking the drawn rectangle keeps the clamps agreeing with what is
 * on screen. Stale by at most one frame, and only after a resize. */
static SDL_Rect s_last_viewport = { 0, 0, 640, 480 };

static bool Zoomed(void) { return s_reader.view.zoom > 1.001f; }

bool ManualReader_HandleKey(SDL_Keycode key, bool pressed, bool repeat) {
  if (!s_reader.open) return false;
  (void)repeat;
  if (pressed) s_hint_device = kManualHintDevice_Keyboard;
  if (!pressed) return true;   /* modal: swallow the release too */

  /* NEVER SWALLOWED. Escape leaves the reader, and if the reader is somehow
   * broken that has to keep being true -- this is the path that guarantees a
   * player cannot be trapped in a window with nothing in it. Returning false
   * would hand Escape to the overlay, which would close the whole menu; the
   * reader consumes it and steps back one level instead. */
  if (key == SDLK_ESCAPE) {
    ManualReader_Close();
    return true;
  }
  /* F1 deliberately NOT consumed: it is the ungated toggle that closes the menu
   * outright, and leaving it alone means one key always exits everything. */
  if (key == SDLK_F1) return false;

  ApplyIntent(ManualInput_KeyIntent(key, Zoomed()), s_last_viewport);
  return true;
}

bool ManualReader_HandleGamepadEvent(const SDL_Event *event) {
  if (!s_reader.open || !event) return false;

  if (event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
    const float value = ManualInput_StickAxis(event->gaxis.value,
                                              g_settings.input_stick_deadzone);
    if (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) s_stick_x = value;
    else if (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) s_stick_y = value;
    else return true;              /* other axes: swallowed, see below */
    /* Only a stick actually off centre counts as input. Otherwise the hint
     * would be held up permanently by a stick resting inside its deadzone. */
    if (value != 0.0f) {
      s_hint_device = kManualHintDevice_Gamepad;
      BumpHint();
    }
    return true;
  }

  if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
    s_hint_device = kManualHintDevice_Gamepad;
    ApplyIntent(ManualInput_PadIntent((SDL_GamepadButton)event->gbutton.button,
                                      Zoomed()),
                s_last_viewport);
    return true;
  }

  /* EVERY OTHER PAD EVENT IS SWALLOWED TOO, including button releases and the
   * triggers. The reader is modal, and the overlay reads "not consumed" as
   * "mine" -- so returning false here does not merely ignore the event, it hands
   * it to the settings menu underneath, which then moves its selection behind a
   * reader that is covering it. That is what axis motion used to do. */
  return true;
}

bool ManualReader_HandleMouse(const SDL_Event *event) {
  if (!s_reader.open || !event) return false;
  /* The mouse shares the keyboard's line: same sitting-at-a-desk case, and the
   * click and drag hints only make sense beside the key ones. */
  s_hint_device = kManualHintDevice_Keyboard;
  const SDL_Rect viewport = s_last_viewport;

  /* Window units to renderer-output pixels. Not the same thing on a high-DPI
   * display, and the reader's geometry is all in output pixels. */
  int x = 0, y = 0;
  bool have_point = false;
  if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
      event->type == SDL_EVENT_MOUSE_BUTTON_UP)
    have_point = HostDisplay_WindowPointToOutput((int)event->button.x,
                                                 (int)event->button.y, &x, &y);
  else if (event->type == SDL_EVENT_MOUSE_MOTION)
    have_point = HostDisplay_WindowPointToOutput((int)event->motion.x,
                                                 (int)event->motion.y, &x, &y);

  switch (event->type) {
    case SDL_EVENT_MOUSE_WHEEL:
      ApplyIntent(event->wheel.y > 0 ? kManualIntent_ZoomIn
                                     : kManualIntent_ZoomOut,
                  viewport);
      return true;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (event->button.button != SDL_BUTTON_LEFT) return true;
      if (!have_point) return true;
      s_drag_x = x;
      s_drag_y = y;
      if (Zoomed()) { s_dragging = true; return true; }
      /* CLICK TO TURN, on the half of the spread you clicked -- the gesture a
       * reader expects from a book on screen. Only when not zoomed, because a
       * zoomed page needs the drag for panning, and a click that both panned and
       * turned would be unusable. */
      ApplyIntent(x < viewport.x + viewport.w / 2 ? kManualIntent_PageBack
                                                  : kManualIntent_PageForward,
                  viewport);
      return true;

    case SDL_EVENT_MOUSE_BUTTON_UP:
      s_dragging = false;
      return true;

    case SDL_EVENT_MOUSE_MOTION:
      if (s_dragging && have_point) {
        BumpHint();
        int pw = 0, ph = 0;
        LayoutDimensions(&pw, &ph);
        /* Delta between converted points rather than the event's own xrel/yrel,
         * which are in window units and would drag at the wrong rate on a
         * high-DPI display. Dragging moves the PAGE with the cursor, so the pan
         * is the negative of that delta. */
        ManualView_Pan(&s_reader.view, (float)(s_drag_x - x),
                       (float)(s_drag_y - y), pw, ph,
                       viewport.w > 0 ? viewport.w : 1,
                       viewport.h > 0 ? viewport.h : 1);
        s_drag_x = x;
        s_drag_y = y;
      }
      return true;

    default:
      return false;
  }
}

/* ── Textures ──────────────────────────────────────────────────────────────
 *
 * PRESENT THREAD ONLY, all of this.
 */

static bool AlreadyFailed(int page) {
  for (int i = 0; i < s_reader.failed_count; i++)
    if (s_reader.failed[i] == page) return true;
  return false;
}

static PageTexture *FindCached(int page) {
  for (int i = 0; i < kCacheSlots; i++)
    if (s_reader.cache[i].texture && s_reader.cache[i].page == page) {
      s_reader.cache[i].used_at = ++s_reader.clock;
      return &s_reader.cache[i];
    }
  return NULL;
}

static bool CachedProbe(int page, void *user) {
  (void)user;
  /* Deliberately does NOT touch used_at: this asks what is resident, and a
   * probe that also counted as a use would make the LRU order depend on how
   * often the budget looked rather than on what was drawn. */
  for (int i = 0; i < kCacheSlots; i++)
    if (s_reader.cache[i].texture && s_reader.cache[i].page == page) return true;
  /* A page that cannot be decoded counts as resident, or the budget retries it
   * every frame forever and never spends the frame's decode on a page that
   * could actually succeed. */
  return AlreadyFailed(page);
}

static PageTexture *DecodePage(int page) {
  if (page < 0 || page >= s_reader.index.count) return NULL;
  if (AlreadyFailed(page)) return NULL;

  PageTexture *slot = NULL;
  for (int i = 0; i < kCacheSlots; i++) {
    if (!s_reader.cache[i].texture) { slot = &s_reader.cache[i]; break; }
  }
  if (!slot) {
    slot = &s_reader.cache[0];
    for (int i = 1; i < kCacheSlots; i++)
      if (s_reader.cache[i].used_at < slot->used_at) slot = &s_reader.cache[i];
  }

  const ManualPageEntry *entry = &s_reader.index.pages[page];
  int w = 0, h = 0, channels = 0;
  stbi_uc *pixels = stbi_load_from_memory(s_reader.file + entry->offset,
                                          (int)entry->length, &w, &h,
                                          &channels, 4);
  if (!pixels) {
    fprintf(stderr, "[manual] page %d: decode failed (%s)\n", page + 1,
            stbi_failure_reason());
    if (s_reader.failed_count < kMaxFailures)
      s_reader.failed[s_reader.failed_count++] = page;
    return NULL;
  }

  SDL_Texture *texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888,
                                           SDL_TEXTUREACCESS_STATIC, w, h);
  if (!texture) {
    fprintf(stderr, "[manual] page %d: texture failed (%s)\n", page + 1,
            SDL_GetError());
    if (s_reader.failed_count < kMaxFailures)
      s_reader.failed[s_reader.failed_count++] = page;
    stbi_image_free(pixels);
    return NULL;
  }
  if (!SDL_UpdateTexture(texture, NULL, pixels, w * 4)) {
    fprintf(stderr, "[manual] page %d: texture upload failed (%s)\n", page + 1,
            SDL_GetError());
    if (s_reader.failed_count < kMaxFailures)
      s_reader.failed[s_reader.failed_count++] = page;
    SDL_DestroyTexture(texture);
    stbi_image_free(pixels);
    return NULL;
  }
  SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
  stbi_image_free(pixels);

  if (slot->texture) SDL_DestroyTexture(slot->texture);
  slot->texture = texture;
  slot->page = page;
  slot->used_at = ++s_reader.clock;
  return slot;
}

/* ── Draw ──────────────────────────────────────────────────────────────────
 *
 * The same order the proof of concept established, and for the same reason:
 * SDL_RenderGeometry has no depth test and no backface culling, so correctness
 * comes from ONE fixed draw order -- backdrop, settled pages, shadow, leaf --
 * which is only valid because the turning leaf never dips behind a settled page.
 * manual_pages.c guarantees that and its test asserts it across the whole turn.
 */

static void BuildIndices(const ManualMesh *density, int *out_verts,
                         int *out_indices) {
  const int stride = density->columns + 1;
  int n = 0;
  /* COLUMN-MAJOR (u outer). Load-bearing: the bowed sheet overlaps itself, and
   * with no depth test that resolves correctly only because depth rises with u,
   * so a later-emitted triangle is a nearer one. Row-major emission resets u on
   * every row and paints far over near. */
  for (int iu = 0; iu < density->columns; iu++) {
    for (int iv = 0; iv < density->rows; iv++) {
      const int base = iv * stride + iu;
      s_reader.indices[n++] = base;
      s_reader.indices[n++] = base + 1;
      s_reader.indices[n++] = base + stride;
      s_reader.indices[n++] = base + 1;
      s_reader.indices[n++] = base + stride + 1;
      s_reader.indices[n++] = base + stride;
    }
  }
  *out_indices = n;
  *out_verts = stride * (density->rows + 1);
}

static bool BuildFlatPage(const ManualMesh *density, const float matrix[16],
                          int view_w, int view_h, float x0, float x1,
                          float half_y, float pan_x, float pan_y,
                          int *out_verts, int *out_indices) {
  int v = 0;
  for (int iv = 0; iv <= density->rows; iv++) {
    for (int iu = 0; iu <= density->columns; iu++) {
      const float u = (float)iu / (float)density->columns;
      const float t = (float)iv / (float)density->rows;
      Scene3DPoint screen;
      if (!Scene3D_ProjectWorldPoint(matrix, (x0 + (x1 - x0) * u) - pan_x,
                                     (0.5f - t) * 2.0f * half_y + pan_y, 0.0f,
                                     view_w, view_h, &screen))
        return false;
      s_reader.verts[v].position.x = screen.x;
      s_reader.verts[v].position.y = screen.y;
      s_reader.verts[v].tex_coord.x = u;
      s_reader.verts[v].tex_coord.y = t;
      s_reader.verts[v].color = (SDL_FColor){ 1.0f, 1.0f, 1.0f, 1.0f };
      v++;
    }
  }
  BuildIndices(density, out_verts, out_indices);
  return true;
}

static bool BuildLeaf(const ManualMesh *density, float turn,
                      const float matrix[16], int view_w, int view_h,
                      const ManualSheet *sheet, float pan_x, float pan_y,
                      bool mirrored, float alpha, int *out_verts,
                      int *out_indices) {
  int v = 0;
  for (int iv = 0; iv <= density->rows; iv++) {
    for (int iu = 0; iu <= density->columns; iu++) {
      const float u = (float)iu / (float)density->columns;
      const float t = (float)iv / (float)density->rows;
      float lx = 0.0f, ly = 0.0f, lz = 0.0f;
      ManualTurn_LeafPoint(turn, u, t, &lx, &ly, &lz);
      Scene3DPoint screen;
      /* One bad vertex rejects the WHOLE leaf. Scene3D_ProjectWorldPoint
       * refuses points at or behind the camera plane, and drawing a primitive
       * with a partially-projected vertex set turns it inside out. */
      if (!Scene3D_ProjectWorldPoint(
              matrix, ManualTurn_LeafWorldX(sheet, turn, lx) - pan_x,
              -ly * 2.0f * sheet->half_y + pan_y, lz * 2.0f * sheet->width,
              view_w, view_h, &screen))
        return false;
      const float shade = ManualTurn_LeafShade(turn, u);
      s_reader.verts[v].position.x = screen.x;
      s_reader.verts[v].position.y = screen.y;
      s_reader.verts[v].tex_coord.x = mirrored ? 1.0f - u : u;
      s_reader.verts[v].tex_coord.y = t;
      s_reader.verts[v].color = (SDL_FColor){ shade, shade, shade, alpha };
      v++;
    }
  }
  BuildIndices(density, out_verts, out_indices);
  return true;
}

/* ── The hint line ─────────────────────────────────────────────────────────
 *
 * The reader has no chrome, so every control is discoverable only by being told
 * -- but a permanent bar across a page of a manual is the one thing a manual
 * reader must not have. So it is shown, held, and then faded out, and any input
 * brings it back.
 *
 * Drawn in the GAME'S OWN MENU FONT, through the overlay that owns the atlas.
 * SDL_RenderDebugText is an 8-pixel developer font: fine in a standalone demo
 * window, and unreadably small inside a menu on a large display, which is
 * exactly how it looked. */
enum {
  kHintHoldMs = 3200,   /* fully lit after any input */
  kHintFadeMs = 900,    /* then out over this long */
};

/* Bumped by every input, on the MAIN thread; read by the draw on the present
 * thread. A word-sized store either lands or does not, and the only consequence
 * of losing the race is that the hint fades one frame late. */
static uint64_t s_hint_shown_ns;

static void BumpHint(void) { s_hint_shown_ns = SDL_GetTicksNS(); }

static uint8_t HintAlpha(uint64_t now) {
  const uint64_t shown = s_hint_shown_ns;
  if (shown == 0) return 0;
  const double age_ms = (double)(now - shown) / 1e6;
  if (age_ms <= (double)kHintHoldMs) return 255;
  const double faded = age_ms - (double)kHintHoldMs;
  if (faded >= (double)kHintFadeMs) return 0;
  return (uint8_t)(255.0 * (1.0 - faded / (double)kHintFadeMs));
}

static void DrawHint(SDL_Rect viewport, uint64_t now, bool spread) {
  const uint8_t alpha = HintAlpha(now);
  if (alpha == 0) return;

  char hint[192];
  snprintf(hint, sizeof hint, "%s %d/%d   %s",
           spread ? "OPENING" : "PAGE", s_reader.view.item + 1, ItemCount(),
           ManualInput_HintText(s_hint_device, Zoomed()));

  /* Scaled to the window rather than fixed, so the line is the same physical
   * size on a 720p handheld and a 4K display instead of shrinking to nothing on
   * the one where there is most room for it. */
  int scale = viewport.h / 320;
  if (scale < 1) scale = 1;
  if (scale > 4) scale = 4;
  const int text_w = SettingsOverlay_GameTextWidth(hint, scale);
  const int glyph = kSettingsOverlayGlyphSize * scale;
  const int pad = glyph / 2;
  const int bar_h = glyph + pad * 2;
  const int x = viewport.x + (viewport.w - text_w) / 2;
  const int y = viewport.y + viewport.h - bar_h + pad;

  /* The backing plate fades with the text; a bar that outlived it would be a
   * black stripe across the page for no reason. Alpha is scaled rather than
   * fixed so the plate never survives the words it exists to make readable. */
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, (Uint8)(alpha * 150 / 255));
  SDL_RenderFillRect(g_renderer, &(SDL_FRect){
      (float)viewport.x, (float)(viewport.y + viewport.h - bar_h),
      (float)viewport.w, (float)bar_h });
  SettingsOverlay_DrawGameText(x, y, scale, alpha, hint);
}

static void DrawSheet(int page, const ManualMesh *density,
                      const float matrix[16], int view_w, int view_h, float x0,
                      float x1, float half_y, float pan_x, float pan_y) {
  if (page < 0) return;
  PageTexture *texture = FindCached(page);
  /* NOT decoded here. A page that is not resident simply does not draw this
   * frame; the budget below will have it ready for the next one. */
  if (!texture) return;
  int verts = 0, indices = 0;
  if (!BuildFlatPage(density, matrix, view_w, view_h, x0, x1, half_y, pan_x,
                     pan_y, &verts, &indices))
    return;
  SDL_RenderGeometry(g_renderer, texture->texture, s_reader.verts, verts,
                     s_reader.indices, indices);
}

void ManualReader_Render(SDL_Rect viewport) {
  if (!s_reader.open) return;
  s_last_viewport = viewport;

  const int view_w = viewport.w > 0 ? viewport.w : 1;
  const int view_h = viewport.h > 0 ? viewport.h : 1;

  /* Advance the turn from the PRESENT thread's clock. This is the thread that
   * actually puts frames on the display, so pacing the animation from anywhere
   * else times it against something the player is not watching. */
  const uint64_t now = SDL_GetTicksNS();
  if (s_reader.last_tick_ns != 0) {
    const float elapsed = (float)((double)(now - s_reader.last_tick_ns) / 1e9);
    /* Clamped: a frame that took longer than a turn -- a stall, a breakpoint,
     * a window drag -- must not teleport the animation past its own end. */
    const float step = elapsed < 0.25f ? elapsed : 0.25f;
    ManualView_AdvanceTurn(&s_reader.view, step, 0.34f);

    /* Analog pan, per frame and scaled by time so its speed does not depend on
     * the frame rate. Only when zoomed: at fit there is no overhang, so this
     * would be a no-op that still cost a clamp every frame. */
    if ((s_stick_x != 0.0f || s_stick_y != 0.0f) && Zoomed()) {
      int pw = 0, ph = 0;
      LayoutDimensions(&pw, &ph);
      /* A full-deflection sweep crosses about one view height per second --
       * enough to cross a zoomed spread without overshooting a column of text. */
      const float speed = (float)view_h * 1.1f * step;
      ManualView_Pan(&s_reader.view, s_stick_x * speed, s_stick_y * speed,
                     pw, ph, view_w, view_h);
    }
  }
  s_reader.last_tick_ns = now;

  /* The backdrop is opaque: the reader is a fullscreen mode, and letting the
   * paused game show through behind a page of text makes both unreadable. */
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_NONE);
  SDL_SetRenderDrawColor(g_renderer, 18, 16, 22, 255);
  SDL_RenderFillRect(g_renderer, &(SDL_FRect){ (float)viewport.x,
                                               (float)viewport.y,
                                               (float)viewport.w,
                                               (float)viewport.h });
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);

  const bool spread = SpreadMode();
  ManualTurnFrame frame;
  if (!ManualTurn_ResolveFrame(&s_reader.view, s_reader.index.count, spread,
                               &frame))
    return;

  /* THE BOOK'S geometry, from the index -- not whichever page happens to be
   * decoded. Otherwise the layout depends on decode timing, and with a budgeted
   * decoder that is a size change on the frame a page lands. */
  int nominal_w = 0, nominal_h = 0;
  if (!ManualPages_NominalGeometry(&s_reader.index, &nominal_w, &nominal_h))
    return;

  const int fit_w = nominal_w * ManualPages_LayoutPageWidths(spread);
  float page_w = 0.0f, page_h = 0.0f;
  ManualView_FittedSize(fit_w, nominal_h, view_w, view_h, s_reader.view.zoom,
                        &page_w, &page_h);

  /* FLAT. The 3D tilt the proof of concept offered was judged worse for a page
   * of dense text, so the shipped reader has no tilt to plumb -- the camera is
   * square-on and the lens is the only thing solved. */
  Scene3DCamera camera = {
    .tilt_x = 0.0f,
    .tilt_y = 0.0f,
    .distance = 2.6f,
    /* A sheet lifts by its own width, so a wide page at the preferred lens
     * swings through the camera plane and the leaf is dropped for failing to
     * project. This narrows the lens only when that would happen. */
    .fov_y = ManualSheet_CameraFov(ManualSheet_PixelWidth(page_w, spread),
                                   view_h, 0.9f),
  };
  float matrix[16];
  Scene3D_BuildViewProjection(&camera, view_w, view_h, matrix);

  ManualSheet sheet;
  if (!ManualSheet_Solve(matrix, view_w, view_h, page_w, page_h, spread, &sheet))
    return;

  ManualMesh density;
  ManualTurn_SolveMesh(matrix, &sheet,
                       (float)kManualMeshBudgetCentipixels / 100.0f, &density);

  const float pan_world_x = page_w > 0.0f
      ? s_reader.view.pan_x * (2.0f * sheet.half_x / page_w) : 0.0f;
  const float pan_world_y = page_h > 0.0f
      ? s_reader.view.pan_y * (2.0f * sheet.half_y / page_h) : 0.0f;

  /* THIS FRAME'S DECODE, before anything is drawn with it. Leaf first: it is the
   * page in motion, and the one whose absence is most visible. */
  const int wanted[] = { frame.leaf_page, frame.right_page, frame.left_page };
  for (int spent = 0; spent < kDecodesPerFrame; spent++) {
    const int next = ManualInput_NextDecode(
        wanted, (int)(sizeof wanted / sizeof wanted[0]), CachedProbe, NULL);
    if (next < 0) break;
    DecodePage(next);
  }

  /* ORDER IS THE CORRECTNESS ARGUMENT, and it never changes mid-turn. */
  if (spread) {
    DrawSheet(frame.left_page, &density, matrix, view_w, view_h, -sheet.half_x,
              0.0f, sheet.half_y, pan_world_x, pan_world_y);
    DrawSheet(frame.right_page, &density, matrix, view_w, view_h, 0.0f,
              sheet.half_x, sheet.half_y, pan_world_x, pan_world_y);
  } else {
    const int page = frame.right_page >= 0 ? frame.right_page : frame.left_page;
    DrawSheet(page, &density, matrix, view_w, view_h, -sheet.half_x,
              sheet.half_x, sheet.half_y, pan_world_x, pan_world_y);
  }

  if (s_reader.view.turn != 0.0f && frame.leaf_page >= 0) {
    PageTexture *leaf = FindCached(frame.leaf_page);
    if (leaf) {
      int verts = 0, indices = 0;
      /* The shadow, offset toward the side the sheet is falling away from, as a
       * FRACTION of the sheet -- a fixed pixel drop is calibrated to whichever
       * page shape the author had. */
      if (BuildLeaf(&density, s_reader.view.turn, matrix, view_w, view_h, &sheet,
                    pan_world_x, pan_world_y, frame.leaf_mirrored, 0.30f,
                    &verts, &indices)) {
        const float dx = (frame.leaf_on_right ? 0.0135f : -0.0135f) *
                         sheet.pixels_w;
        const float dy = 0.0189f * sheet.pixels_w;
        for (int i = 0; i < verts; i++) {
          s_reader.verts[i].position.x += dx;
          s_reader.verts[i].position.y += dy;
          s_reader.verts[i].color = (SDL_FColor){ 0.0f, 0.0f, 0.0f, 0.30f };
        }
        SDL_RenderGeometry(g_renderer, NULL, s_reader.verts, verts,
                           s_reader.indices, indices);
      }
      if (BuildLeaf(&density, s_reader.view.turn, matrix, view_w, view_h, &sheet,
                    pan_world_x, pan_world_y, frame.leaf_mirrored, 1.0f, &verts,
                    &indices))
        SDL_RenderGeometry(g_renderer, leaf->texture, s_reader.verts, verts,
                           s_reader.indices, indices);
    }
  }

  DrawHint(viewport, now, spread);
}
