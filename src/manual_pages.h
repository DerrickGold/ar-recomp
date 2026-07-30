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

/* ── Spread layout ─────────────────────────────────────────────────────────
 *
 * A real booklet is read as OPENINGS, not pages: the covers stand alone and
 * everything between them is seen two-up. That is not decoration -- artwork
 * (maps especially) is drawn across the gutter, and showing those halves on
 * separate screens cuts the picture in two.
 *
 * So a 40-page manual is 21 openings:
 *
 *     opening  0 :  [   ] [ 1 ]     front cover, alone
 *     opening  1 :  [ 2 ] [ 3 ]
 *     opening  2 :  [ 4 ] [ 5 ]
 *        ...
 *     opening 19 :  [38 ] [39 ]
 *     opening 20 :  [40 ] [   ]     back cover, alone
 *
 * (1-based above for readability; the API is 0-based.)
 *
 * PARITY IS THE WHOLE POINT AND IT IS EASY TO GET WRONG. Off by one and every
 * interior opening pairs the wrong halves -- a map's right half sits beside the
 * next page's left -- which looks almost plausible and is completely wrong. Hence
 * a pure, tested layout rather than arithmetic inline at the draw site. */

typedef struct ManualSpread {
  /* Page index, or -1 for "no page on this side" -- the front cover has no left,
   * the back cover no right, and an odd interior count leaves one lone page. */
  int left;
  int right;
} ManualSpread;

/* Number of openings for a booklet of `page_count` pages. Zero for an empty or
 * negative count. */
int ManualPages_SpreadCount(int page_count);

/* The pages visible at `spread`. False if the index is out of range, leaving
 * `out` untouched. */
bool ManualPages_SpreadAt(int page_count, int spread, ManualSpread *out);

/* The opening that shows `page`, or -1 if the page is out of range. The inverse
 * of SpreadAt, so a "go to page N" jump can land on the right opening. */
int ManualPages_SpreadForPage(int page_count, int page);

/* True when the opening shows a single page (either cover, or a lone interior
 * page).
 *
 * NOTE FOR RENDERERS: this says how many pages are PRESENT, not how wide to lay
 * the view out. Sizing a single-page opening to one page and a two-up one to two
 * makes the whole view rescale by 2x the moment a cover is turned -- everything
 * jumps mid-animation. In spread layout the area is ALWAYS two pages wide and a
 * lone page occupies one half of it, which is also what the paper does: a front
 * cover is one leaf of the opening it swings away from. */
bool ManualSpread_IsSingle(const ManualSpread *spread);

/* Which half a single-page opening occupies: true for the right (a front cover,
 * which opens leftward), false for the left (a back cover). Meaningless, and
 * false, for a two-up opening.
 *
 * The distinction is what keeps a cover on the correct side of the gutter while
 * the layout width stays constant. */
bool ManualSpread_SingleOnRight(const ManualSpread *spread);

/* ── Reader kinematics ─────────────────────────────────────────────────────
 *
 * Zoom is a multiple of fit-to-view; pan is in FIT-RELATIVE units so it means
 * the same thing at every zoom and window size. Both are clamped so the page
 * can never be lost off-screen -- the failure that makes a reader feel broken.
 */

typedef struct ManualView {
  /* The current ITEM. In spread layout that is an OPENING index, not a page
   * index -- named `item` rather than `page` because a field called `page`
   * holding a spread number is exactly the lie that produces off-by-one bugs
   * later. Every count passed to the functions below must match: page count in
   * single layout, ManualPages_SpreadCount() in spread layout. */
  int item;
  float zoom;       /* 1.0 == fit to view */
  float pan_x;      /* fit-relative, clamped to the zoomed overhang */
  float pan_y;
  float turn;       /* 0 == settled; ±(0,1] == a turn in flight */
  int turn_target;  /* item the turn lands on; == item when settled */
} ManualView;

enum {
  /* Below 1.0 the page would float inside the view with nothing around it, and
   * every pan would be a no-op. Above 6x a 739x1080 scan is showing paper
   * texture, not text. */
  kManualZoomMinPermille = 1000,
  kManualZoomMaxPermille = 6000,
};

void ManualView_Init(ManualView *view);

/* Width, in page-widths, that the view must be laid out to.
 *
 * ALWAYS 2 in spread layout and 1 otherwise -- deliberately NOT a function of the
 * current opening. Sizing a single-page opening to one page and a two-up one to
 * two rescales the whole view by 2x the instant a cover is turned, so every page
 * visibly jumps mid-animation. A lone page occupies one half of the constant area
 * instead, which is also what the paper does.
 *
 * Lives here rather than at the draw site because it is exactly the kind of
 * one-line layout decision that looks obviously right and is the reason the
 * animation glitched on covers. */
int ManualPages_LayoutPageWidths(bool spread_mode);

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

/* Jump straight to an item with no animation, clamped into range. */
void ManualView_GoTo(ManualView *view, int item, int count);

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

/* Position of a point on the leaf, in UNIT-SHEET space: x and y each run over
 * [-0.5, 0.5] at rest so the sheet is exactly the same rectangle a settled page
 * occupies, and z lifts out of the page toward the viewer.
 *
 * `u` runs 0 (hinge edge) to 1 (free edge) and `v` 0 to 1 down the page. The
 * hinge sits on the sheet's own LEFT edge for a forward turn (x = -0.5) and its
 * right edge for a backward one -- not at the origin, which would hinge the
 * sheet about its middle and leave half the page behind it uncovered.
 *
 * At rest the sheet coincides with the settled page EXACTLY. That is what makes
 * the start of a turn invisible; a mismatch here reads as the page changing size
 * the moment it is touched. z is never negative, which is the ordering invariant
 * above. */
void ManualTurn_LeafPoint(float turn, float u, float v,
                          float *out_x, float *out_y, float *out_z);

/* Half-extents, in world units, at which a z=0 unit sheet projects to exactly
 * `page_w x page_h` pixels under `matrix`.
 *
 * THE REASON THIS EXISTS. A settled page and a turning leaf must occupy the same
 * rectangle, and they cannot if one is drawn in pixel space and the other through
 * a perspective projection with hand-picked scale factors -- the page visibly
 * resized at the start of every turn. So the projection is the single source of
 * truth for BOTH, and this solves for the extents rather than guessing them:
 * at z=0 the mapping from world x/y to pixels is linear, so two probe points
 * give the pixels-per-world-unit exactly.
 *
 * Returns false if the camera cannot project the probes at all, in which case
 * the caller must fall back to drawing flat rather than draw something wrong. */
bool ManualTurn_SheetExtents(const float matrix[16],
                             int view_w, int view_h,
                             float page_w, float page_h,
                             float *out_half_x, float *out_half_y);

/* Shade multiplier across the leaf, 0..1: the sheet catches less light as it
 * lifts, which is what sells the fold without a lighting model. */
float ManualTurn_LeafShade(float turn, float u);

/* Which face of the leaf is toward the viewer at this phase. With no backface
 * culling BOTH faces rasterise, so the host must pick the texture -- past the
 * halfway point that is the destination page's reverse. */
bool ManualTurn_FrontFaceVisible(float turn);

/* ── What is on screen during a turn ───────────────────────────────────────
 *
 * A page turn moves ONE sheet of paper. Its front is the page you were reading;
 * its back is the page that ends up facing you. Everything else stays where it
 * is. Getting that wrong in either direction produces a visible pop, so the
 * whole assignment is resolved here and tested, rather than as conditionals at
 * the draw site.
 *
 * FORWARD, opening N -> N+1. The right leaf lifts and falls to the left:
 *
 *     left side   L(N)      UNCHANGED -- it is still there, being covered
 *     right side  R(N+1)    revealed as the leaf lifts away
 *     leaf front  R(N)      the page you were reading
 *     leaf back   L(N+1)    what the sheet's reverse carries
 *
 * At turn=1 the leaf's back has landed exactly where the new opening's left page
 * is, and the right side already shows R(N+1) -- so nothing changes at the moment
 * the animation ends. That continuity IS the correctness condition. Setting BOTH
 * sides to the target when the turn starts (the obvious implementation) pops the
 * left page to its new value on the first frame.
 *
 * BACKWARD is the mirror: the LEFT leaf lifts and falls to the right, the right
 * side is unchanged, and the left side reveals the target's left page.
 */

typedef struct ManualTurnFrame {
  /* Pages to draw flat, beneath the leaf. -1 for an empty side. */
  int left_page;
  int right_page;
  /* The page whose image the leaf shows right now, and whether its texture must
   * be mirrored in u. A LEFT page meets the gutter on its right edge and a RIGHT
   * page on its left, while the leaf's u always runs 0 at the gutter -- so
   * whether u must be flipped depends on BOTH the direction of the turn and
   * which face is toward the viewer. It is an exclusive-or, not a property of
   * the face alone; mirroring on the face only leaves every backward turn
   * reversed. */
  int leaf_page;
  bool leaf_mirrored;
  /* Which side of the gutter the leaf occupies: true when it is lifting from the
   * right (a forward turn). The host needs this only for the shadow's offset. */
  bool leaf_on_right;
} ManualTurnFrame;

/* Resolve everything visible for `view` over a `page_count`-page booklet.
 *
 * `spread_mode` selects openings versus single pages; in single-page mode the
 * "left" side is always -1 and one page occupies the view, so the caller has one
 * code path. Returns false if the view cannot be resolved at all, in which case
 * nothing should be drawn. */
bool ManualTurn_ResolveFrame(const ManualView *view, int page_count,
                             bool spread_mode, ManualTurnFrame *out);

#endif  /* MANUAL_PAGES_H */
