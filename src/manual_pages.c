#include "manual_pages.h"

#include <math.h>
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

/* ── Reader kinematics ─────────────────────────────────────────────────────── */

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

void ManualView_Zoom(ManualView *view, float factor,
                     int page_w, int page_h, int view_w, int view_h) {
  if (!view || !(factor > 0.0f) || !isfinite(factor)) return;
  const float lo = (float)kManualZoomMinPermille / 1000.0f;
  const float hi = (float)kManualZoomMaxPermille / 1000.0f;
  view->zoom = ClampF(view->zoom * factor, lo, hi);
  /* Re-clamp: zooming OUT shrinks the overhang, so a pan that was legal at the
   * old zoom would otherwise leave the page hanging off-centre with no input
   * from the user. Passing (0,0) applies the new limits to the existing pan. */
  ManualView_Pan(view, 0.0f, 0.0f, page_w, page_h, view_w, view_h);
}

bool ManualView_BeginTurn(ManualView *view, int direction, int count) {
  if (!view || count <= 0 || direction == 0) return false;
  if (view->turn != 0.0f) return false;   /* already in flight */
  const int target = view->page + (direction > 0 ? 1 : -1);
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
    view->page = view->turn_target;
    view->turn = 0.0f;
    return false;
  }
  const float step = elapsed_seconds / turn_seconds;
  const bool forward = view->turn > 0.0f;
  float progress = fabsf(view->turn) + step;
  if (progress >= 1.0f) {
    view->page = view->turn_target;
    view->turn = 0.0f;
    return false;
  }
  view->turn = forward ? progress : -progress;
  return true;
}

void ManualView_GoTo(ManualView *view, int page, int count) {
  if (!view || count <= 0) return;
  view->page = page < 0 ? 0 : (page >= count ? count - 1 : page);
  view->turn = 0.0f;
  view->turn_target = view->page;
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

void ManualTurn_LeafPoint(float turn, float u, float v,
                          float *out_x, float *out_y, float *out_z) {
  const float cu = ClampF(u, 0.0f, 1.0f);
  const float cv = ClampF(v, 0.0f, 1.0f);
  const float angle = ManualTurn_HingeAngle(turn);
  /* Rigid rotation about the hinge (the x=0 edge). The sheet is flat for every
   * phase -- no curl -- which is what keeps it non-self-intersecting and lets
   * one fixed draw order be correct for the whole animation. */
  const float span = cu;
  float x = span * cosf(angle);
  float z = span * sinf(angle);
  if (z < 0.0f) z = 0.0f;   /* the invariant: the leaf never goes behind a page */
  /* Mirror for a backward turn so the hinge sits on the opposite edge. */
  if (turn < 0.0f) x = -x;
  if (out_x) *out_x = x;
  if (out_y) *out_y = cv;
  if (out_z) *out_z = z;
}

float ManualTurn_LeafShade(float turn, float u) {
  const float angle = ManualTurn_HingeAngle(turn);
  /* Brightest flat, dimmest edge-on: |cos| is 1 at rest and at the landing, and
   * 0 halfway. Floored so a sheet mid-turn is never pure black. */
  const float facing = fabsf(cosf(angle));
  const float across = 0.85f + 0.15f * ClampF(u, 0.0f, 1.0f);
  return ClampF((0.55f + 0.45f * facing) * across, 0.0f, 1.0f);
}

bool ManualTurn_FrontFaceVisible(float turn) {
  return ManualTurn_HingeAngle(turn) <= (float)M_PI * 0.5f;
}
