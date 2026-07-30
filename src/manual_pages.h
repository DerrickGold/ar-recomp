#ifndef MANUAL_PAGES_H
#define MANUAL_PAGES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Page-image indexing and reader kinematics for the in-game manual.
 *
 * PURE: no SDL, no stdio, no allocation. The host shim owns the renderer, the
 * file handles and the textures; everything decided here is arithmetic and
 * policy, so it is testable without a window -- the same split as
 * diorama_layer_order / scene3d_math, and for the same reason (the render files
 * are in zero test binaries).
 *
 * WHAT A "PAGE PACK" IS. The manual arrives as one image per page. The reader
 * never parses PDF: a PDF that happens to be a scan album can be carved
 * losslessly by the builder, and anything else the builder rasterises or
 * refuses. So this module's input is a page COUNT and a per-page byte range --
 * never a document format.
 *
 * ORDERING IS PART OF THE CONTRACT. Pages are addressed by index, and the index
 * IS the reading order. Recovering that order from filenames is the builder's
 * job (and a real hazard: lexical sort puts page 10 before page 2). Nothing here
 * sorts anything.
 */

enum {
  /* A booklet, not a library. 40 pages is the known real manual; the cap exists
   * so a malformed pack cannot ask for unbounded state. */
  kManualMaxPages = 512,
};

/* One page's bytes inside the album file, as the builder found them. Offsets
 * are absolute in the container so the host can read a page without holding the
 * whole 8 MB file. */
typedef struct ManualPageEntry {
  uint32_t offset;
  uint32_t length;
  uint16_t width;
  uint16_t height;
} ManualPageEntry;

typedef struct ManualPageIndex {
  ManualPageEntry pages[kManualMaxPages];
  int count;
} ManualPageIndex;

/* ── Album carving (the scan-album fast path) ───────────────────────────────
 *
 * Scans `data` for baseline-JPEG page images and records their byte ranges.
 * Returns the number found, and never more than kManualMaxPages.
 *
 * DELIBERATELY NOT A PDF PARSER. It looks for image streams and takes their
 * SOI..EOI extent; a PDF whose pages are vector text yields ZERO, which is the
 * correct answer -- those pages have no image to extract and must be rasterised
 * upstream. Callers MUST treat a low count as "this file is not an album"
 * rather than "here is a short manual": see ManualPages_LooksLikeAlbum, which
 * exists because "found at least one JPEG" is a HARMFUL success test (a vector
 * PDF with a letterhead logo yields exactly one).
 *
 * Dimensions are read from the JPEG's own SOF0 marker, so a page whose stream
 * is truncated or non-baseline is skipped rather than recorded at 0x0. */
int ManualPages_CarveAlbum(const uint8_t *data, size_t size,
                           ManualPageIndex *out_index);

/* True when `index` over a `size`-byte container looks like a real page album
 * rather than a document that merely contains images.
 *
 * Tests COMPLETENESS, not existence: at least two pages, the image bytes
 * dominating the container (>= 80%), and every page sharing one geometry. A
 * cover-plus-letterhead PDF fails all three. This predicate is the difference
 * between shipping the user's manual and shipping their letterhead. */
bool ManualPages_LooksLikeAlbum(const ManualPageIndex *index, size_t size);

/* ── Reader kinematics ─────────────────────────────────────────────────────
 *
 * Zoom is a multiple of fit-to-view; pan is in FIT-RELATIVE units so it means
 * the same thing at every zoom and window size. Both are clamped so the page
 * can never be lost off-screen -- the failure that makes a reader feel broken.
 */

typedef struct ManualView {
  int page;         /* current page, always in [0, count) */
  float zoom;       /* 1.0 == fit to view */
  float pan_x;      /* fit-relative, clamped to the zoomed overhang */
  float pan_y;
  float turn;       /* 0 == settled; ±(0,1] == a turn in flight */
  int turn_target;  /* page the turn lands on; == page when settled */
} ManualView;

enum {
  /* Below 1.0 the page would float inside the view with nothing around it, and
   * every pan would be a no-op. Above 6x a 739x1080 scan is showing paper
   * texture, not text. */
  kManualZoomMinPermille = 1000,
  kManualZoomMaxPermille = 6000,
};

void ManualView_Init(ManualView *view);

/* Fit `page_w x page_h` inside `view_w x view_h` preserving aspect, then apply
 * `zoom`. Writes the on-screen size in pixels. A page always fits at zoom 1
 * with at least one axis exact, so "fit" is the same on any window. */
void ManualView_FittedSize(int page_w, int page_h, int view_w, int view_h,
                           float zoom, float *out_w, float *out_h);

/* Multiply the zoom about the view centre, clamped to the permille bounds, and
 * re-clamp pan so zooming out cannot leave the page parked off-centre. */
void ManualView_Zoom(ManualView *view, float factor,
                     int page_w, int page_h, int view_w, int view_h);

/* Pan by a fit-relative delta, clamped to the overhang at the current zoom.
 * At zoom 1 there is no overhang, so this is a no-op -- which is why panning a
 * fitted page correctly does nothing at all. */
void ManualView_Pan(ManualView *view, float dx, float dy,
                    int page_w, int page_h, int view_w, int view_h);

/* Largest |pan| the current zoom allows on each axis. Zero when the page fits.
 * Exposed so a caller can show a scroll affordance without duplicating the
 * clamp arithmetic. */
void ManualView_PanLimit(const ManualView *view,
                         int page_w, int page_h, int view_w, int view_h,
                         float *out_x, float *out_y);

/* Begin a turn toward `page + direction`, or return false at the ends of the
 * booklet (and while a turn is already in flight -- a queued turn would let a
 * held key outrun the animation and skip pages invisibly). Turning resets zoom
 * and pan: arriving mid-page on a new sheet is disorienting, and it is also
 * what makes the fixed draw order safe, since both sheets are then at fit. */
bool ManualView_BeginTurn(ManualView *view, int direction, int count);

/* Advance a turn in flight. Returns true while still animating; on completion
 * `page` becomes `turn_target` and `turn` returns to 0. Clock-driven rather
 * than frame-driven so the animation reads the same at 60 and 144 Hz. */
bool ManualView_AdvanceTurn(ManualView *view, float elapsed_seconds,
                            float turn_seconds);

/* Jump straight to a page with no animation, clamped into range. */
void ManualView_GoTo(ManualView *view, int page, int count);

/* ── Turn geometry ─────────────────────────────────────────────────────────
 *
 * The turning sheet is a rigid leaf hinged on one edge. Rigid is not a
 * simplification for its own sake: SDL_RenderGeometry has NO DEPTH TEST and no
 * backface culling, so correctness has to come from draw ORDER, and order is
 * only provable if the leaf cannot intersect what it passes over. A curled
 * sheet self-occludes within a single quad, which no ordering fixes.
 *
 * Hence the invariant this module guarantees and its test asserts: the leaf's
 * z is >= 0 for every turn phase, while settled pages sit at negative z. The
 * host can then draw backdrop -> destination -> shadow -> leaf, once, with no
 * mid-animation reordering.
 */

/* Hinge rotation for a turn phase, in radians: 0 at rest, pi when the sheet has
 * landed. Eased so the sheet accelerates off the spine and settles rather than
 * stopping dead. */
float ManualTurn_HingeAngle(float turn);

/* Position of a point on the leaf. `u` runs 0 (hinge) to 1 (free edge), `v` 0
 * to 1 down the page. Writes leaf-local (x, y, z) in fit-relative units.
 * z is never negative, which is the ordering invariant above. */
void ManualTurn_LeafPoint(float turn, float u, float v,
                          float *out_x, float *out_y, float *out_z);

/* Shade multiplier across the leaf, 0..1: the sheet catches less light as it
 * lifts, which is what sells the fold without a lighting model. */
float ManualTurn_LeafShade(float turn, float u);

/* Which face of the leaf is toward the viewer at this phase. With no backface
 * culling BOTH faces rasterise, so the host must pick the texture -- past the
 * halfway point that is the destination page's reverse. */
bool ManualTurn_FrontFaceVisible(float turn);

#endif  /* MANUAL_PAGES_H */
