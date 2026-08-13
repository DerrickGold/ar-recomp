#include "manual_pages.h"

#include <math.h>

#include "scene3d_math.h"
#include <string.h>

/* ── Album carving ─────────────────────────────────────────────────────────── */

/* JPEG SOF0 (baseline) carries the geometry we need. Progressive (SOF2) and the
 * arithmetic-coded variants are deliberately NOT accepted: stb_image decodes
 * baseline only, so recording a page it cannot later decode would turn a clear
 * "this is not an album" into a page that fails at display time. */
static bool JpegGeometry(const uint8_t *data, size_t size,
                         uint16_t *out_w, uint16_t *out_h) {
  if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;
  size_t at = 2;
  while (at + 3 < size) {
    if (data[at] != 0xFF) { at++; continue; }   /* resync over fill bytes */
    const uint8_t marker = data[at + 1];
    if (marker == 0xFF) { at++; continue; }
    /* Standalone markers carry no length payload. */
    if (marker == 0xD8 || marker == 0x01 ||
        (marker >= 0xD0 && marker <= 0xD7)) { at += 2; continue; }
    if (at + 3 >= size) return false;
    const size_t seg = ((size_t)data[at + 2] << 8) | data[at + 3];
    if (seg < 2 || at + 2 + seg > size) return false;
    if (marker == 0xC0 || marker == 0xC1) {     /* SOF0/SOF1: baseline */
      if (seg < 7) return false;
      *out_h = (uint16_t)(((uint16_t)data[at + 5] << 8) | data[at + 6]);
      *out_w = (uint16_t)(((uint16_t)data[at + 7] << 8) | data[at + 8]);
      return *out_w > 0 && *out_h > 0;
    }
    if (marker == 0xDA) return false;           /* scan data: no SOF seen */
    at += 2 + seg;
  }
  return false;
}

/* End of a JPEG starting at `start`: the EOI that closes it.
 *
 * Scanning for the FIRST 0xFFD9 is wrong -- a thumbnail embedded in an EXIF/JFIF
 * APPn segment contains its own EOI, so the page would be cut short at the
 * thumbnail and the rest of the real image discarded. Walking the marker
 * segments skips those payloads wholesale, then the entropy-coded scan is
 * scanned bytewise for the terminator. */
static bool JpegExtent(const uint8_t *data, size_t size, size_t start,
                       size_t *out_end) {
  size_t at = start + 2;
  while (at + 1 < size) {
    if (data[at] != 0xFF) { at++; continue; }
    const uint8_t marker = data[at + 1];
    if (marker == 0xFF) { at++; continue; }
    if (marker == 0xD9) { *out_end = at + 2; return true; }
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) { at += 2; continue; }
    if (marker == 0xDA) {
      /* Start of scan: entropy-coded bytes follow, where 0xFF00 and the restart
       * markers are stuffing rather than structure. Only a real EOI ends it. */
      if (at + 3 >= size) return false;
      const size_t seg = ((size_t)data[at + 2] << 8) | data[at + 3];
      if (seg < 2 || at + 2 + seg > size) return false;
      at += 2 + seg;
      bool resume_segments = false;
      while (at + 1 < size) {
        if (data[at] != 0xFF) { at++; continue; }
        const uint8_t next = data[at + 1];
        if (next == 0xD9) { *out_end = at + 2; return true; }
        /* In entropy-coded data an 0xFF is either STUFFED (0xFF00) or a restart
         * marker; both are payload, not structure. */
        if (next == 0x00 || (next >= 0xD0 && next <= 0xD7)) { at += 2; continue; }
        if (next == 0xFF) { at++; continue; }        /* fill byte */
        /* Any other marker legitimately ends this scan (a multi-scan image), so
         * resume the segment walk. Note this is also where CORRUPT data lands --
         * an unstuffed 0xFF -- and the segment walk's own length validation
         * rejects the candidate there. Rejecting is right: a stream we cannot
         * delimit is one stb_image could not decode either. */
        resume_segments = true;
        break;
      }
      if (!resume_segments) return false;   /* ran off the end with no EOI */
      continue;
    }
    if (at + 3 >= size) return false;
    const size_t seg = ((size_t)data[at + 2] << 8) | data[at + 3];
    if (seg < 2 || at + 2 + seg > size) return false;
    at += 2 + seg;
  }
  return false;
}

int ManualPages_CarveAlbum(const uint8_t *data, size_t size,
                           ManualPageIndex *out_index) {
  if (!out_index) return 0;
  memset(out_index, 0, sizeof *out_index);
  if (!data || size < 4) return 0;

  size_t at = 0;
  while (at + 1 < size && out_index->count < kManualMaxPages) {
    if (data[at] != 0xFF || data[at + 1] != 0xD8) { at++; continue; }
    size_t end = 0;
    uint16_t w = 0, h = 0;
    if (JpegExtent(data, size, at, &end) &&
        JpegGeometry(data + at, end - at, &w, &h)) {
      ManualPageEntry *entry = &out_index->pages[out_index->count++];
      entry->offset = (uint32_t)at;
      entry->length = (uint32_t)(end - at);
      entry->width = w;
      entry->height = h;
      at = end;                 /* never rescan inside a recorded page */
      continue;
    }
    at += 2;                    /* a false SOI in binary noise */
  }
  return out_index->count;
}

bool ManualPages_LooksLikeAlbum(const ManualPageIndex *index, size_t size) {
  if (!index || index->count < 2 || size == 0) return false;

  /* The images must BE the document, not decorate it. A vector PDF carrying a
   * letterhead strip on every page can reach a high page count, but those
   * images are a rounding error of the file. */
  uint64_t image_bytes = 0;
  for (int i = 0; i < index->count; i++) image_bytes += index->pages[i].length;
  if (image_bytes * 100u < (uint64_t)size * 80u) return false;

  /* One geometry throughout. A scan album is a stack of identical sheets; mixed
   * sizes mean logos, figures, or a document that merely embeds photographs. */
  const uint16_t w = index->pages[0].width, h = index->pages[0].height;
  for (int i = 1; i < index->count; i++) {
    if (index->pages[i].width != w || index->pages[i].height != h) return false;
  }
  return true;
}

bool ManualPages_NominalGeometry(const ManualPageIndex *index,
                                 int *out_w, int *out_h) {
  if (!index || index->count <= 0) return false;

  /* The most common geometry, ties to the earliest. Quadratic over a list capped
   * at kManualMaxPages and computed once per book, which is cheaper than the
   * sort it would take to do better and keeps the tie rule obvious. */
  int best = -1, best_count = 0;
  for (int i = 0; i < index->count; i++) {
    /* A zero dimension is not a geometry. The carver cannot record one -- it
     * reads the SOF and rejects a page without it -- but a hand-built index can
     * hold one, and it must not win the vote and take the whole book with it. */
    if (index->pages[i].width == 0 || index->pages[i].height == 0) continue;
    int count = 0;
    for (int j = 0; j < index->count; j++) {
      if (index->pages[j].width == index->pages[i].width &&
          index->pages[j].height == index->pages[i].height) count++;
    }
    if (count > best_count) { best_count = count; best = i; }
  }
  if (best < 0) return false;
  if (out_w) *out_w = (int)index->pages[best].width;
  if (out_h) *out_h = (int)index->pages[best].height;
  return true;
}

/* ── Spread layout ─────────────────────────────────────────────────────────── */

/* Interior pages are everything but the two covers, paired two-up. Derived in
 * one place so SpreadCount, SpreadAt and SpreadForPage cannot disagree. */
static int InteriorSpreadCount(int page_count) {
  if (page_count <= 2) return 0;
  const int interior = page_count - 2;
  return (interior + 1) / 2;          /* a lone last page still needs an opening */
}

int ManualPages_SpreadCount(int page_count) {
  if (page_count <= 0) return 0;
  if (page_count == 1) return 1;      /* a cover and nothing else */
  /* front cover + interior openings + back cover */
  return 1 + InteriorSpreadCount(page_count) + 1;
}

bool ManualPages_SpreadAt(int page_count, int spread, ManualSpread *out) {
  if (!out || page_count <= 0) return false;
  const int total = ManualPages_SpreadCount(page_count);
  if (spread < 0 || spread >= total) return false;

  if (spread == 0) {                  /* front cover, right-hand side */
    out->left = -1;
    out->right = 0;
    return true;
  }
  if (spread == total - 1) {          /* back cover, left-hand side */
    out->left = page_count - 1;
    out->right = -1;
    return true;
  }
  /* Interior opening `spread` (1-based among interiors) shows pages
   * 2*spread-1 and 2*spread. The last one may have no right half. */
  const int left = 2 * spread - 1;
  const int right = left + 1;
  out->left = left;
  out->right = (right <= page_count - 2) ? right : -1;
  return true;
}

int ManualPages_SpreadForPage(int page_count, int page) {
  if (page_count <= 0 || page < 0 || page >= page_count) return -1;
  if (page == 0) return 0;
  if (page == page_count - 1) return ManualPages_SpreadCount(page_count) - 1;
  return (page + 1) / 2;              /* inverse of 2*spread-1 / 2*spread */
}

bool ManualSpread_IsSingle(const ManualSpread *spread) {
  if (!spread) return false;
  return (spread->left < 0) != (spread->right < 0);
}

bool ManualSpread_SingleOnRight(const ManualSpread *spread) {
  if (!ManualSpread_IsSingle(spread)) return false;
  return spread->right >= 0;
}

/* ── Reader kinematics ─────────────────────────────────────────────────────── */

int ManualPages_LayoutPageWidths(bool spread_mode) {
  return spread_mode ? 2 : 1;
}

static float ClampF(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

void ManualView_Init(ManualView *view) {
  if (!view) return;
  memset(view, 0, sizeof *view);
  view->zoom = 1.0f;
}

void ManualView_FittedSize(int page_w, int page_h, int view_w, int view_h,
                           float zoom, float *out_w, float *out_h) {
  if (out_w) *out_w = 0.0f;
  if (out_h) *out_h = 0.0f;
  if (page_w <= 0 || page_h <= 0 || view_w <= 0 || view_h <= 0) return;
  const float sx = (float)view_w / (float)page_w;
  const float sy = (float)view_h / (float)page_h;
  const float fit = sx < sy ? sx : sy;
  if (out_w) *out_w = (float)page_w * fit * zoom;
  if (out_h) *out_h = (float)page_h * fit * zoom;
}

void ManualView_PanLimit(const ManualView *view,
                         int page_w, int page_h, int view_w, int view_h,
                         float *out_x, float *out_y) {
  if (out_x) *out_x = 0.0f;
  if (out_y) *out_y = 0.0f;
  if (!view) return;
  float w = 0.0f, h = 0.0f;
  ManualView_FittedSize(page_w, page_h, view_w, view_h, view->zoom, &w, &h);
  /* Overhang is half the excess: pan is measured from the centre. Negative
   * excess (the page is smaller than the view) clamps to zero, so a fitted page
   * cannot be panned at all. */
  const float ox = (w - (float)view_w) * 0.5f;
  const float oy = (h - (float)view_h) * 0.5f;
  if (out_x) *out_x = ox > 0.0f ? ox : 0.0f;
  if (out_y) *out_y = oy > 0.0f ? oy : 0.0f;
}

void ManualView_Pan(ManualView *view, float dx, float dy,
                    int page_w, int page_h, int view_w, int view_h) {
  if (!view) return;
  float lx = 0.0f, ly = 0.0f;
  ManualView_PanLimit(view, page_w, page_h, view_w, view_h, &lx, &ly);
  view->pan_x = ClampF(view->pan_x + dx, -lx, lx);
  view->pan_y = ClampF(view->pan_y + dy, -ly, ly);
}

void ManualView_ZoomLimit(int page_w, int page_h, int view_w, int view_h,
                          float *out_min, float *out_max) {
  const float lo = (float)kManualZoomMinPermille / 1000.0f;
  const float ceiling = (float)kManualZoomMaxPermille / 1000.0f;
  if (out_min) *out_min = lo;
  if (out_max) *out_max = ceiling;
  if (page_w <= 0 || page_h <= 0 || view_w <= 0 || view_h <= 0) return;

  /* Screen pixels per SOURCE pixel at zoom 1 -- the same fit ManualView_FittedSize
   * applies, so the two cannot disagree about what "fit" means. */
  const float sx = (float)view_w / (float)page_w;
  const float sy = (float)view_h / (float)page_h;
  const float fit = sx < sy ? sx : sy;
  if (!(fit > 0.0f) || !isfinite(fit)) return;

  /* Zoom is a multiple of fit, so the multiple of NATIVE resolution is fit*zoom.
   * Solving that for the native ceiling is the whole rule: a scan being shrunk to
   * fit has room to magnify, one already near 1:1 has almost none. */
  float hi = ((float)kManualZoomNativePermille / 1000.0f) / fit;
  if (hi > ceiling) hi = ceiling;
  /* A scan displayed past the native ceiling at fit would compute a maximum
   * below the minimum. Fit still has to be reachable, so the floor wins and the
   * range collapses to exactly 1.0 rather than inverting. */
  if (hi < lo) hi = lo;
  if (out_max) *out_max = hi;
}

void ManualView_Zoom(ManualView *view, float factor,
                     int page_w, int page_h, int view_w, int view_h) {
  if (!view || !(factor > 0.0f) || !isfinite(factor)) return;
  float lo = 0.0f, hi = 0.0f;
  ManualView_ZoomLimit(page_w, page_h, view_w, view_h, &lo, &hi);
  view->zoom = ClampF(view->zoom * factor, lo, hi);
  /* Re-clamp: zooming OUT shrinks the overhang, so a pan that was legal at the
   * old zoom would otherwise leave the page hanging off-centre with no input
   * from the user. Passing (0,0) applies the new limits to the existing pan. */
  ManualView_Pan(view, 0.0f, 0.0f, page_w, page_h, view_w, view_h);
}

bool ManualView_BeginTurn(ManualView *view, int direction, int count) {
  if (!view || count <= 0 || direction == 0) return false;
  if (view->turn != 0.0f) return false;   /* already in flight */
  const int target = view->item + (direction > 0 ? 1 : -1);
  if (target < 0 || target >= count) return false;
  view->turn_target = target;
  view->turn = direction > 0 ? 1e-6f : -1e-6f;  /* nonzero == in flight */
  /* Land at fit, not wherever the previous page was zoomed to. */
  view->zoom = 1.0f;
  view->pan_x = 0.0f;
  view->pan_y = 0.0f;
  return true;
}

bool ManualView_AdvanceTurn(ManualView *view, float elapsed_seconds,
                            float turn_seconds) {
  if (!view || view->turn == 0.0f) return false;
  if (!(turn_seconds > 0.0f)) {          /* degenerate duration: land at once */
    view->item = view->turn_target;
    view->turn = 0.0f;
    return false;
  }
  const float step = elapsed_seconds / turn_seconds;
  const bool forward = view->turn > 0.0f;
  float progress = fabsf(view->turn) + step;
  if (progress >= 1.0f) {
    view->item = view->turn_target;
    view->turn = 0.0f;
    return false;
  }
  view->turn = forward ? progress : -progress;
  return true;
}

void ManualView_GoTo(ManualView *view, int item, int count) {
  if (!view || count <= 0) return;
  view->item = item < 0 ? 0 : (item >= count ? count - 1 : item);
  view->turn = 0.0f;
  view->turn_target = view->item;
  view->zoom = 1.0f;
  view->pan_x = 0.0f;
  view->pan_y = 0.0f;
}

/* ── Turn geometry ─────────────────────────────────────────────────────────── */

float ManualTurn_HingeAngle(float turn) {
  const float t = ClampF(fabsf(turn), 0.0f, 1.0f);
  /* Smoothstep: zero derivative at both ends, so the sheet leaves the spine and
   * settles onto the stack instead of starting and stopping abruptly. */
  const float eased = t * t * (3.0f - 2.0f * t);
  return eased * (float)M_PI;
}

float ManualTurn_BowOffset(float turn, float u) {
  const float cu = ClampF(u, 0.0f, 1.0f);
  const float angle = ManualTurn_HingeAngle(turn);
  const float amplitude = (float)kManualCurlPermille / 1000.0f;
  /* sin(pi*u) pins the bow to zero at the hinge and the free edge; sin(angle)
   * pins it to zero at rest and at the landing, so a settled sheet is exactly
   * flat and coincides with the page beneath it. */
  return amplitude * sinf((float)M_PI * cu) * sinf(angle);
}

void ManualTurn_LeafPoint(float turn, float u, float v,
                          float *out_x, float *out_y, float *out_z) {
  const float cu = ClampF(u, 0.0f, 1.0f);
  const float cv = ClampF(v, 0.0f, 1.0f);
  const float angle = ManualTurn_HingeAngle(turn);
  /* Rigid rotation about the hinge, which is the sheet's own left edge for a
   * forward turn. The sheet stays flat at every phase -- no curl -- which keeps
   * it from self-intersecting and lets ONE fixed draw order be correct for the
   * whole animation.
   *
   * `cu` is the distance from the hinge across the full sheet width, so the free
   * edge travels the sheet's whole span rather than half of it. Hinging at the
   * origin instead would sweep only the right half and leave the left half of
   * the page behind it permanently exposed. */
  /* THE HINGE IS THE GUTTER, at x = 0 -- the book's spine, not the sheet's outer
   * edge. A forward turn lifts the RIGHT leaf (x in [0, 0.5]) and lays it down on
   * the left; `span` is measured from the gutter outward, so the free edge sweeps
   * from +0.5 through the lift and lands at -0.5, exactly covering the facing
   * page.
   *
   * Hinging on the sheet's own outer edge instead sent the free edge to x = -1.5
   * -- a full sheet-width past the page, flipping onto empty space. In a
   * single-page layout the caller maps this half-sheet onto the whole page, so
   * one geometry serves both. */
  const float span = cu * 0.5f;                /* 0 at the gutter, 0.5 at the edge */
  /* The sheet BOWS along its own normal (-sin a, 0, cos a) as it lifts, so it
   * reads as paper rather than a rotating board. Bounded: see the invariants in
   * the header -- the bow folds the sheet slightly in screen x, and painter's
   * order stays correct only because z remains monotonic in u. */
  const float bow = ManualTurn_BowOffset(turn, cu);
  float x = span * cosf(angle) - bow * sinf(angle);
  float z = span * sinf(angle) + bow * cosf(angle);
  /* THE INVARIANT: the leaf never goes behind a page. Load-bearing, but as a
   * NUMERICAL guard rather than a structural one, and the difference matters to
   * anyone tempted to delete it.
   *
   * At the shipped amplitude the algebra leaves 0.5 - A*pi = 0.28 of margin (see
   * the header), so z cannot go meaningfully negative. What it does go is
   * -4.4e-08, at u=1 and a full turn: M_PI rounded to float is a hair ABOVE pi,
   * so sinf() of it is a small NEGATIVE number, and z = 0.5*sinf(pi) inherits
   * the sign. The clamp absorbs exactly that.
   *
   * An audit read the clamp as making the z>=0 test a tautology and proposed
   * removing it; doing so turns the suite red in 200 places, all of them this
   * one ulp. It is not a tautology and it is not covering for the bound. */
  if (z < 0.0f) z = 0.0f;
  /* A backward turn lifts the LEFT leaf and lays it to the right. */
  if (turn < 0.0f) x = -x;
  if (out_x) *out_x = x;
  if (out_y) *out_y = cv - 0.5f;   /* centred, like the settled page */
  if (out_z) *out_z = z;
}

bool ManualTurn_SheetExtents(const float matrix[16],
                             int view_w, int view_h,
                             float page_w, float page_h,
                             float *out_half_x, float *out_half_y) {
  if (!matrix || view_w <= 0 || view_h <= 0) return false;
  if (!(page_w > 0.0f) || !(page_h > 0.0f)) return false;
  /* Finite, not merely positive. +Inf is > 0 and sails through the test above,
   * then divides to +Inf extents and puts every vertex at NaN, producing a
   * silently blank reader. Reject it at this boundary. */
  if (!isfinite(page_w) || !isfinite(page_h)) return false;

  /* At z=0 the projection is linear in world x and y, so the scale is exactly
   * the screen displacement of a one-unit step -- no search, no tuning. */
  Scene3DPoint origin, unit_x, unit_y;
  if (!Scene3D_ProjectWorldPoint(matrix, 0.0f, 0.0f, 0.0f,
                                 view_w, view_h, &origin) ||
      !Scene3D_ProjectWorldPoint(matrix, 1.0f, 0.0f, 0.0f,
                                 view_w, view_h, &unit_x) ||
      !Scene3D_ProjectWorldPoint(matrix, 0.0f, 1.0f, 0.0f,
                                 view_w, view_h, &unit_y))
    return false;

  /* Each axis's own screen component, not the hypotenuse of its step. With the
   * reader's yaw a +x step does also move slightly in screen y, but the effect
   * is 0.3% at this camera -- so this is a correctness detail, not a visible
   * one, and the earlier comment's "would over-shrink the sheet" oversold it. */
  const float pixels_per_x = fabsf(unit_x.x - origin.x);
  const float pixels_per_y = fabsf(origin.y - unit_y.y);
  if (!(pixels_per_x > 0.0f) || !(pixels_per_y > 0.0f)) return false;
  if (!isfinite(pixels_per_x) || !isfinite(pixels_per_y)) return false;

  /* A camera can be degenerate enough (a near-zero fov) to project a unit step
   * to a sliver, which divides out to an enormous extent. Enormous is arguably
   * arithmetically right; NOT FINITE is not, and it is the one that reaches the
   * renderer as NaN vertices. Reject on the result rather than trying to
   * enumerate the cameras that produce it. */
  const float half_x = page_w * 0.5f / pixels_per_x;
  const float half_y = page_h * 0.5f / pixels_per_y;
  if (!isfinite(half_x) || !isfinite(half_y)) return false;

  if (out_half_x) *out_half_x = half_x;
  if (out_half_y) *out_half_y = half_y;
  return true;
}

/* ── Where the turning sheet sits ──────────────────────────────────────────── */

bool ManualSheet_Solve(const float matrix[16], int view_w, int view_h,
                       float page_w, float page_h, bool spread_mode,
                       ManualSheet *out) {
  if (!out) return false;
  memset(out, 0, sizeof *out);
  float half_x = 0.0f, half_y = 0.0f;
  if (!ManualTurn_SheetExtents(matrix, view_w, view_h, page_w, page_h,
                               &half_x, &half_y))
    return false;

  out->half_x = half_x;
  out->half_y = half_y;
  if (spread_mode) {
    /* Two pages wide, hinged on the gutter at the centre. The sheet is one half
     * of the area -- which is one page. */
    out->hinge_x = 0.0f;
    out->width = half_x;
    out->pixels_w = page_w * 0.5f;
  } else {
    /* One page wide, hinged on the page's own left edge, and the sheet is the
     * WHOLE area: two half-extents, not one. Scaling it by half_x -- the spread
     * rule applied to a layout that is not a spread -- is what drew a leaf half
     * a page wide, hinged down the middle of the text. */
    out->hinge_x = -half_x;
    out->width = 2.0f * half_x;
    out->pixels_w = page_w;
  }
  out->pixels_h = page_h;
  return true;
}

float ManualSheet_PixelWidth(float page_w, bool spread_mode) {
  return spread_mode ? page_w * 0.5f : page_w;
}

float ManualSheet_CameraFov(float sheet_pixels, int view_h, float preferred_fov) {
  if (!(preferred_fov > 0.0f) || !isfinite(preferred_fov)) return preferred_fov;
  if (!(sheet_pixels > 0.0f) || !isfinite(sheet_pixels) || view_h <= 0)
    return preferred_fov;

  /* Invert lift/distance = 2*sheet_pixels*tan(fov/2)/view_h for the fov at which
   * the lift exactly meets the clearance. */
  const float clearance = (float)kManualLiftClearancePermille / 1000.0f;
  const float half_tangent = clearance * (float)view_h / (2.0f * sheet_pixels);
  if (!isfinite(half_tangent) || !(half_tangent > 0.0f)) return preferred_fov;
  const float fov = 2.0f * atanf(half_tangent);
  if (!isfinite(fov) || !(fov > 0.0f)) return preferred_fov;
  /* Only ever NARROWS. A sheet with room to spare keeps the preferred lens, so
   * every page shape that already framed well is untouched by this. */
  return fov < preferred_fov ? fov : preferred_fov;
}

float ManualTurn_LeafWorldX(const ManualSheet *sheet, float turn, float leaf_x) {
  if (!sheet) return 0.0f;
  /* ManualTurn_LeafPoint has already mirrored leaf_x for a backward turn, so the
   * hinge has to mirror with it or the sheet lifts from one edge and lands a
   * whole width away from where the page it covers actually is. */
  const float hinge = (turn < 0.0f) ? -sheet->hinge_x : sheet->hinge_x;
  return hinge + leaf_x * 2.0f * sheet->width;
}

/* ── Mesh density ──────────────────────────────────────────────────────────── */

/* Homogeneous depth of a leaf point, placed and scaled exactly as the renderer
 * places it. Sharing this with nothing would let the measurement drift away from
 * the geometry it claims to measure. */
static float LeafClipDepth(const float matrix[16], const ManualSheet *sheet,
                           float turn, float u, float v) {
  float lx = 0.0f, ly = 0.0f, lz = 0.0f;
  ManualTurn_LeafPoint(turn, u, v, &lx, &ly, &lz);
  return Scene3D_ClipDepth(matrix,
                           ManualTurn_LeafWorldX(sheet, turn, lx),
                           -ly * 2.0f * sheet->half_y,
                           lz * 2.0f * sheet->width);
}

/* The midpoint deviation of affine from perspective-correct interpolation over
 * one cell, in parameter space: |du| * |dw| / (2*(w0+w1)). Derived by equating
 * the two at screen-space s=0.5; it is exact, not an approximation. */
static float CellUvError(float span, float w0, float w1) {
  if (!(w0 > 0.0f) || !(w1 > 0.0f)) return 0.0f;   /* behind the camera: skipped */
  return span * fabsf(w1 - w0) / (2.0f * (w0 + w1));
}

bool ManualTurn_MeshUvError(const float matrix[16], const ManualSheet *sheet,
                            int columns, int rows,
                            float *out_column_px, float *out_row_px) {
  if (out_column_px) *out_column_px = 0.0f;
  if (out_row_px) *out_row_px = 0.0f;
  if (!matrix || !sheet || columns < 1 || rows < 1) return false;
  if (!isfinite(sheet->width) || !isfinite(sheet->half_y)) return false;

  float worst_column = 0.0f, worst_row = 0.0f;
  /* Sample the turn rather than solving for its worst phase: the depth spread
   * peaks somewhere mid-sweep whose location moves with the camera, and 33
   * phases is far cheaper than being clever once per layout change. */
  enum { kPhases = 33 };
  for (int p = 0; p <= kPhases; p++) {
    const float turn = (float)p / (float)kPhases;
    for (int c = 0; c < columns; c++) {
      const float u0 = (float)c / (float)columns;
      const float u1 = (float)(c + 1) / (float)columns;
      const float e = CellUvError(u1 - u0,
                                  LeafClipDepth(matrix, sheet, turn, u0, 0.5f),
                                  LeafClipDepth(matrix, sheet, turn, u1, 0.5f)) *
                      sheet->pixels_w;
      if (e > worst_column) worst_column = e;
    }
    for (int r = 0; r < rows; r++) {
      const float v0 = (float)r / (float)rows;
      const float v1 = (float)(r + 1) / (float)rows;
      const float e = CellUvError(v1 - v0,
                                  LeafClipDepth(matrix, sheet, turn, 0.5f, v0),
                                  LeafClipDepth(matrix, sheet, turn, 0.5f, v1)) *
                      sheet->pixels_h;
      if (e > worst_row) worst_row = e;
    }
  }
  if (!isfinite(worst_column) || !isfinite(worst_row)) return false;
  if (out_column_px) *out_column_px = worst_column;
  if (out_row_px) *out_row_px = worst_row;
  return true;
}

static int ClampI(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

/* First estimate for a density: the deviation falls as 1/N^2 on each axis, so
 * one measurement solves for N instead of searching for it. */
static int EstimateDensity(int measured_at, float measured_px, float budget_px,
                           int lo, int hi) {
  if (!(measured_px > budget_px)) return lo;
  const float scale = sqrtf(measured_px / budget_px);
  if (!isfinite(scale)) return hi;
  return ClampI((int)ceilf((float)measured_at * scale), lo, hi);
}

void ManualTurn_SolveMesh(const float matrix[16], const ManualSheet *sheet,
                          float budget_px, ManualMesh *out) {
  if (!out) return;
  /* The coarsest mesh is the fallback, deliberately. A sheet drawn slightly too
   * coarse is a mild artefact; a sheet not drawn is a broken reader. */
  out->columns = kManualMeshMinColumns;
  out->rows = kManualMeshMinRows;
  if (!matrix || !sheet || !(budget_px > 0.0f) || !isfinite(budget_px)) return;

  float column_px = 0.0f, row_px = 0.0f;
  if (!ManualTurn_MeshUvError(matrix, sheet, kManualMeshMinColumns,
                              kManualMeshMinRows, &column_px, &row_px))
    return;

  int columns = EstimateDensity(kManualMeshMinColumns, column_px, budget_px,
                                kManualMeshMinColumns, kManualMeshMaxColumns);
  int rows = EstimateDensity(kManualMeshMinRows, row_px, budget_px,
                             kManualMeshMinRows, kManualMeshMaxRows);

  /* VERIFY, then bump. The 1/N^2 law is the leading term, not an identity --
   * measured 8 -> 16 columns improves by 3.76x where the law predicts 4 -- so
   * the estimate can land just over budget. Re-measuring and stepping up is a
   * handful of iterations and turns "should be within budget" into "is", which
   * is the difference between this being a bound and being a guess. */
  for (int i = 0; i < 6; i++) {
    if (!ManualTurn_MeshUvError(matrix, sheet, columns, rows,
                                &column_px, &row_px))
      break;
    const bool columns_over = column_px > budget_px && columns < kManualMeshMaxColumns;
    const bool rows_over = row_px > budget_px && rows < kManualMeshMaxRows;
    if (!columns_over && !rows_over) break;
    if (columns_over) columns = ClampI(columns + columns / 4 + 1,
                                       kManualMeshMinColumns, kManualMeshMaxColumns);
    if (rows_over) rows = ClampI(rows + rows / 4 + 1,
                                 kManualMeshMinRows, kManualMeshMaxRows);
  }

  out->columns = columns;
  out->rows = rows;
}

float ManualTurn_LeafShade(float turn, float u) {
  const float cu = ClampF(u, 0.0f, 1.0f);
  const float angle = ManualTurn_HingeAngle(turn);
  /* Brightest flat, dimmest edge-on: |cos| is 1 at rest and at the landing, and
   * 0 halfway. Floored so a sheet mid-turn is never pure black. */
  const float facing = fabsf(cosf(angle));

  /* The bow's own contribution. Its slope along the sheet tilts the surface, so
   * the highlight travels ACROSS the page as it turns instead of the whole sheet
   * dimming uniformly -- without this the bow is present in the geometry but
   * nearly invisible, since a bow of this amplitude moves few pixels.
   * d/du of sin(pi*u) is pi*cos(pi*u): positive on the hinge half, negative on
   * the outer half, zero at the crest. */
  const float amplitude = (float)kManualCurlPermille / 1000.0f;
  const float slope = amplitude * (float)M_PI * cosf((float)M_PI * cu) *
                      sinf(angle);
  const float bend = 1.0f - 0.55f * slope;

  const float across = 0.85f + 0.15f * cu;
  return ClampF((0.55f + 0.45f * facing) * across * bend, 0.0f, 1.0f);
}

bool ManualTurn_DepthRisesWithU(float turn, int samples) {
  if (samples < 2) return false;
  float previous = -1e30f;
  for (int i = 0; i < samples; i++) {
    const float u = (float)i / (float)(samples - 1);
    float x = 0.0f, y = 0.0f, z = 0.0f;
    ManualTurn_LeafPoint(turn, u, 0.5f, &x, &y, &z);
    if (z < previous - 1e-5f) return false;
    previous = z;
  }
  return true;
}

bool ManualTurn_FrontFaceVisible(float turn) {
  return ManualTurn_HingeAngle(turn) <= (float)M_PI * 0.5f;
}

bool ManualTurn_ResolveFrame(const ManualView *view, int page_count,
                             bool spread_mode, ManualTurnFrame *out) {
  if (!view || !out || page_count <= 0) return false;
  memset(out, 0, sizeof *out);
  out->left_page = -1;
  out->right_page = -1;
  out->leaf_page = -1;

  const int items = spread_mode ? ManualPages_SpreadCount(page_count)
                                : page_count;
  if (items <= 0) return false;
  const int settled_item = view->item < 0 ? 0
                         : (view->item >= items ? items - 1 : view->item);

  /* Single-page mode is the same shape with an empty left side, so the caller
   * needs no second path. */
  ManualSpread settled = { -1, settled_item };
  if (spread_mode && !ManualPages_SpreadAt(page_count, settled_item, &settled))
    return false;

  const bool turning = view->turn != 0.0f;
  if (!turning) {
    out->left_page = settled.left;
    out->right_page = settled.right;
    return true;
  }

  const int target_item = view->turn_target < 0 ? 0
                        : (view->turn_target >= items ? items - 1
                                                      : view->turn_target);
  ManualSpread target = { -1, target_item };
  if (spread_mode && !ManualPages_SpreadAt(page_count, target_item, &target))
    return false;

  const bool forward = view->turn > 0.0f;
  const bool front = ManualTurn_FrontFaceVisible(view->turn);
  out->leaf_on_right = forward;

  if (forward) {
    /* The right leaf lifts. The left page is UNCHANGED -- it stays visible under
     * the descending sheet -- and only the newly exposed right side advances. */
    out->left_page = settled.left;
    out->right_page = target.right;
    out->leaf_page = front ? settled.right : target.left;
  } else {
    /* Mirror image: the left leaf lifts, the right page is unchanged. */
    out->right_page = settled.right;
    out->left_page = target.left;
    out->leaf_page = front ? settled.left : target.right;
  }

  /* In single-page mode a "spread" has only a right page, so a backward turn
   * would find nothing to lift. Fall back to whichever side exists. */
  if (out->leaf_page < 0)
    out->leaf_page = front
        ? (settled.right >= 0 ? settled.right : settled.left)
        : (target.right >= 0 ? target.right : target.left);

  /* THE XOR. The leaf's u runs 0 at the gutter; a left page's gutter is its
   * RIGHT edge, so its texture must be flipped. Which pages land on which face
   * swaps with the direction, so mirroring on the face alone reverses every
   * backward turn -- the bug this replaces. */
  out->leaf_mirrored = (forward != front);
  return true;
}
