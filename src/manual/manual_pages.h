#ifndef MANUAL_PAGES_H
#define MANUAL_PAGES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "constants.h"

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

/* The geometry the whole booklet lays out to: the size shared by the most pages,
 * ties going to the earliest.
 *
 * THE LAYOUT MUST NOT DEPEND ON WHICH PAGE IS DECODED. Taking the fit from
 * whatever texture happens to be resident makes the view rescale as the reader
 * pages through a book whose scans are not all identical, and makes the layout a
 * function of decode TIMING -- the page size can change on the frame an
 * asynchronous decode lands. The index already carries every page's dimensions,
 * so the book's size is known before a single byte is decoded.
 *
 * For an album that passed LooksLikeAlbum this is just page 0's geometry, since
 * uniformity is one of that predicate's tests. It earns its keep on the mixed
 * albums a caller admits deliberately.
 *
 * NO ASPECT IS PRIVILEGED. A tall scan, a square one and a wide one are all just
 * a width and a height here; nothing downstream may assume portrait. Returns
 * false for an empty index, leaving the outputs untouched. */
bool ManualPages_NominalGeometry(const ManualPageIndex *index,
                                 int *out_w, int *out_h);

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
   * every pan would be a no-op. */
  kManualZoomMinPermille = kPermilleScale,
  /* How far past the scan's OWN resolution the reader will magnify, in
   * thousandths. Past this a page is showing JPEG blocks and paper grain rather
   * than text, and the exact multiple where that happens is a property of the
   * scan, not of the window.
   *
   * This replaces a flat 6x ceiling whose comment justified it as "above 6x a
   * 739x1080 scan is showing paper texture" -- true of that scan, and wrong for
   * every other one. A 900x900 GBC scan reaches the same mush at a much lower
   * multiple of fit, because it starts closer to fit. */
  kManualZoomNativePermille = 4000,
  /* An absolute ceiling for the degenerate case: a tiny scan in a huge window
   * fits at a large scale already, and the native rule alone would then permit
   * an unbounded zoom. */
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

/* The zoom range this page allows in this view.
 *
 * The ceiling is DERIVED FROM THE SCAN, not fixed: fit already displays the page
 * at some multiple of its own pixels, and the useful limit is a fixed multiple of
 * NATIVE resolution beyond that. A 739x1080 scan fitted into a small window is
 * being shrunk, so it has room to magnify; the same window showing a 900x900 GBC
 * scan is already near 1:1 and has almost none. One constant cannot serve both,
 * which is what the old flat 6x was.
 *
 * Falls back to the absolute bounds when the geometry is degenerate, and always
 * returns `*out_max >= *out_min` so a caller can clamp with it unconditionally. */
void ManualView_ZoomLimit(int page_w, int page_h, int view_w, int view_h,
                          float *out_min, float *out_max);

/* Multiply the zoom about the view centre, clamped to ManualView_ZoomLimit, and
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
 * The turning sheet is hinged on one edge and BOWS as it lifts, like paper. It
 * does not curl freely: SDL_RenderGeometry has NO DEPTH TEST and no backface
 * culling, so correctness comes from draw ORDER alone, and the bow is bounded so
 * that order is provably right.
 *
 * TWO INVARIANTS, both bounded by the same number, both asserted by tests:
 *
 *   1. z >= 0 everywhere, so the leaf never dips behind a settled page and the
 *      fixed draw order (backdrop -> destination -> shadow -> leaf) holds for
 *      every frame with no mid-animation reordering.
 *
 *   2. z is MONOTONICALLY NON-DECREASING in u. This is the subtle one, and it is
 *      what makes a bow safe at all. A bowed sheet DOES fold back on itself in
 *      screen x near the hinge when it is edge-on -- measured 14 px of overlap at
 *      the amplitude below, so it genuinely self-occludes. Painter's order still
 *      renders it correctly, but ONLY IF THE RENDERER EMITS ITS MESH WITH u AS
 *      THE OUTER LOOP (column-major). Depth rises monotonically with u, so a
 *      later-emitted triangle is a nearer one -- but row-major emission resets u
 *      to 0 on every row, making depth jump BACKWARD at each row boundary and a
 *      far triangle paint over a near one. Measured 6 out-of-order steps per
 *      frame that way, versus 0 column-major. The sheet is constant in v, so
 *      ordering by column costs nothing.
 *
 *      This is a CONSTRAINT ON THE RENDERER, not a property of this module, and
 *      the module cannot enforce it -- ManualTurn_DepthRisesWithU exists so a
 *      renderer can assert the premise it depends on.
 *
 * Both hold exactly while amplitude <= 0.5/pi ~= 0.159, and THE BINDING CASE IS
 * THE HINGE, not the free edge. Writing the depth out,
 *
 *     z(u) = sin(a) * [ u/2 + A*sin(pi*u)*cos(a) ]
 *
 * the bracket is worst at a = pi, where it is u/2 - A*sin(pi*u). At the free edge
 * sin(pi*u) is ZERO, so that end is trivially safe and cannot be the worst case;
 * as u -> 0 the sine goes like pi*u and the bracket goes like u*(1/2 - A*pi),
 * which is where 0.5 - A*pi >= 0 actually comes from. Measured: at A = 0.160,
 * one thousandth over the bound, the first violation appears at u = 0.0285 --
 * against the hinge, as the algebra says. (An earlier revision of this comment
 * attributed the bound to u=1. The BOUND was right; the attribution pointed at
 * the one place on the sheet where the bow vanishes.)
 *
 * kManualCurlPermille is set well under the bound, so it is a guard rail rather
 * than a cliff edge.
 *
 * This is why the bow is a BOW and not a curl: a freely curled sheet -- one whose
 * far half rolls back over its near half -- breaks invariant 2, and then no
 * ordering of whole strips can fix it. It would need the mesh split along the
 * silhouette, which this project has no precedent for.
 */

enum {
  /* Bow amplitude, in thousandths of the sheet's width. 70 is a visible lift
   * with better than 2x headroom under the limit below, because the amplitude
   * interacts with the projection and a value that is merely *provably* safe is
   * not the same as a comfortable one. Raising this past the limit turns the
   * geometry tests red rather than producing a subtly wrong image. */
  kManualCurlPermille = 70,
  /* THE DERIVED CEILING, floor(1000 * 0.5/pi). This is not a tuning knob and
   * must never be raised to accommodate a larger amplitude: it is what the
   * invariant proof above ALLOWS, so moving it does not make a bigger bow safe,
   * it just stops the guard from reporting that the bow is unsafe.
   *
   * The guard used to be a plain `<=` comparison in a test, which the obvious
   * edit -- raising the amplitude and the ceiling together -- satisfied happily.
   * Two things stop that now: the static assertion below fires at COMPILE time
   * for the amplitude, and a test independently recomputes floor(500/pi) and
   * compares it to this constant, so moving the ceiling fails on its own. */
  kManualCurlLimitPermille = 159,
};

_Static_assert(kManualCurlPermille <= kManualCurlLimitPermille,
               "bow amplitude exceeds the 0.5/pi bound that makes the reader's "
               "fixed draw order correct; raise nothing, lower kManualCurlPermille");

/* Hinge rotation for a turn phase, in radians: 0 at rest, pi when the sheet has
 * landed. Eased so the sheet accelerates off the spine and settles rather than
 * stopping dead. */
float ManualTurn_HingeAngle(float turn);

/* Bow displacement along the sheet's own normal at `u`, for a turn phase, in
 * sheet-width units. Zero at both ends of the turn and at both edges of the
 * sheet, peaking mid-sweep and mid-span -- paper bends in the middle and is held
 * flat where it is hinged and where it is about to land. */
float ManualTurn_BowOffset(float turn, float u);

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

/* ── Where the turning sheet sits ──────────────────────────────────────────── */

/* The solved placement of one opening and of the sheet that turns off it.
 *
 * ONE SHEET IS NOT ONE LAYOUT AREA, and conflating them is why single-page mode
 * turned a HALF-WIDTH leaf. In spread layout the area is two pages wide, the
 * hinge is the gutter at its centre, and the sheet is one half of it. In
 * single-page layout the area is ONE page, the hinge is the page's own outer
 * edge, and the sheet is the whole area -- twice the half-extent, not one of
 * them. The renderer previously scaled both by `half_x`, which is correct for
 * spreads and renders half a page in single-page mode.
 *
 * Everything here is in world units except `pixels`, so a renderer can place the
 * sheet without re-deriving any of it -- and so the placement is testable, which
 * a hand-written expression at the draw site was not. */
typedef struct ManualSheet {
  float half_x;    /* half-width of the LAYOUT area (both pages, in spreads) */
  float half_y;    /* half-height of the layout area */
  float hinge_x;   /* the turning sheet's hinge, for a FORWARD turn */
  float width;     /* the turning sheet's own full width */
  /* The sheet on screen. Anything a renderer would otherwise size in fixed
   * pixels -- a drop shadow, a mesh cell -- has to come from these, or it is
   * calibrated to whatever page shape happened to be in front of the author. */
  float pixels_w;
  float pixels_h;
} ManualSheet;

/* Solve the placement for a layout area that is `page_w x page_h` pixels on
 * screen -- the WHOLE area, both pages in spread mode. False if the projection
 * cannot be probed, in which case nothing should be drawn. */
bool ManualSheet_Solve(const float matrix[16], int view_w, int view_h,
                       float page_w, float page_h, bool spread_mode,
                       ManualSheet *out);

enum {
  /* How close to the camera the sheet's highest point may come, in thousandths
   * of the camera distance. 700 is not a taste value: it is just above the 661
   * the ActRaiser manual already reaches. It preserves the reference layout's
   * camera and adjusts only shapes that would otherwise cross the camera. */
  kManualLiftClearancePermille = 700,
};

/* The vertical field of view to build the reader's camera with, given a sheet
 * that is `sheet_pixels` wide on screen in a `view_h`-tall view.
 *
 * A SHEET LIFTS BY ITS OWN WIDTH. Rotating it about its hinge puts the free edge
 * a full sheet-width off the page at the halfway point, so the wider the sheet
 * is ON SCREEN, the deeper it reaches toward the camera. With a fixed fov that
 * depth is a fixed fraction of the camera distance times the sheet's share of
 * the view:
 *
 *     lift / distance = 2 * sheet_pixels * tan(fov/2) / view_h
 *
 * The ActRaiser spread lands at 0.66 of the distance, which is dramatic and
 * works. A wide, short manual in single-page layout lands at 1.72 -- the leaf
 * passes THROUGH the camera plane, Scene3D_ProjectWorldPoint refuses those
 * vertices, and the renderer's documented response is to drop the whole leaf.
 * The page turn simply does not draw.
 *
 * MOVING THE CAMERA CANNOT FIX THIS. The extents are solved from the projection,
 * so pulling back scales the sheet up by the same factor and the ratio above is
 * invariant. The lens is the only free variable: a wider sheet needs a longer
 * one. Returns `preferred_fov` whenever it is already safe, so this is inert for
 * every shape that already worked. */
float ManualSheet_CameraFov(float sheet_pixels, int view_h, float preferred_fov);

/* The sheet's on-screen width, before a camera exists to solve extents with.
 *
 * Needed because ManualSheet_CameraFov must run BEFORE the projection is built,
 * and ManualSheet.pixels_w is only available after. Same rule, one definition:
 * the layout area is two pages wide in spread mode and the sheet is one half. */
float ManualSheet_PixelWidth(float page_w, bool spread_mode);

/* World x of a leaf point whose unit-sheet x is `leaf_x` (as ManualTurn_LeafPoint
 * returns it), for a turn of this sign.
 *
 * The hinge mirrors with the direction: a forward turn lifts from `hinge_x`, a
 * backward one from its reflection, which is what puts the hinge on the page's
 * far edge when the sheet swings the other way. `leaf_x` is already signed by
 * ManualTurn_LeafPoint, so this only has to place and scale it. */
float ManualTurn_LeafWorldX(const ManualSheet *sheet, float turn, float leaf_x);

/* ── Mesh density ──────────────────────────────────────────────────────────── */

/* SDL_RenderGeometry interpolates texture coordinates AFFINELY -- there is no
 * perspective-correct interpolation and no way to ask for it. Across one mesh
 * cell the true, perspective-correct texture parameter and the affine one
 * deviate most at the cell's midpoint, by
 *
 *     |du| * |dw| / (2 * (w0 + w1))
 *
 * in parameter space, where w is the homogeneous depth at each end. Times the
 * sheet's on-screen size that is a distance in PIXELS: the amount by which the
 * scan's content is drawn in the wrong place. The scan's strokes are 1-2 px, so
 * a few pixels of slip is legible as a smear.
 *
 * THE DENSITY CANNOT BE A CONSTANT, which is what it was. The deviation grows
 * with the sheet's on-screen size and with how much depth it spans, so it is a
 * function of the page's shape AND the window's:
 *
 *   - a fixed 16 columns measured 1.1 px on the 739x1080 page in a 1600x900
 *     window -- the case it was tuned on -- and 4.0 px on a square or wide page
 *     in the same window, because those fill the width instead of the height.
 *   - the same 16 columns measured 6.5 px at 2560x1440. The constant was tuned
 *     against one page in one window and silently degrades in both directions.
 *   - along v the untilted sheet spans no depth at all, so rows cost nothing --
 *     but with the 3D tilt on, 6 rows measured 4.7 px at 1600x900 and 7.5 px at
 *     2560x1440. Anyone judging whether the tilt is worth keeping was judging a
 *     mesh far too coarse for it.
 *
 * So the renderer solves the density for the sheet and camera it actually has.
 * The error falls as 1/N^2 on each axis, which is what makes one measurement
 * enough to solve for N rather than search. */

enum {
  /* Bounds on the solved density. The minimum is where the error is MEASURED
   * before being scaled, so it must be fine enough for that measurement to be
   * meaningful; the maximum bounds a renderer's vertex arrays, which is why it
   * belongs in the header rather than at the call site. */
  kManualMeshMinColumns = 8,
  kManualMeshMaxColumns = 40,
  kManualMeshMinRows = 4,
  /* 16 leaves the tilted portrait page at 1.10 px on a 1440p display -- clamped
   * rather than solved, and just over budget. 20 solves it. */
  kManualMeshMaxRows = 20,
  /* Target for the worst cell, in hundredths of a pixel. Under the scan's own
   * 1-2 px stroke width, so the slip stays inside a stroke. */
  kManualCentipixelsPerPixel = 100,
  kManualMeshBudgetCentipixels = kManualCentipixelsPerPixel,
};

typedef struct ManualMesh {
  int columns;   /* subdivisions along u, across the bend */
  int rows;      /* subdivisions along v, down the page */
} ManualMesh;

/* Worst affine-vs-perspective texture deviation over a whole turn, in screen
 * pixels, for a `columns x rows` mesh of this sheet under this projection.
 * Reported per axis: they are independent, and only the u axis carries the bend.
 * Either output may be NULL. False if the projection cannot be probed. */
bool ManualTurn_MeshUvError(const float matrix[16], const ManualSheet *sheet,
                            int columns, int rows,
                            float *out_column_px, float *out_row_px);

/* Solve the coarsest mesh whose deviation stays within `budget_px`, clamped to
 * the bounds above. Falls back to the minimum density on a projection it cannot
 * probe -- a coarse sheet is a mild artefact, and refusing to draw is not. */
void ManualTurn_SolveMesh(const float matrix[16], const ManualSheet *sheet,
                          float budget_px, ManualMesh *out);

/* Shade multiplier across the leaf, 0..1: the sheet catches less light as it
 * lifts, which is what sells the fold without a lighting model. */
float ManualTurn_LeafShade(float turn, float u);

/* True when the leaf's depth rises monotonically with u at this phase, sampled
 * `samples` times across the sheet.
 *
 * This is the premise a painter's-order renderer depends on: it is what makes the
 * later-emitted triangle the nearer one, and therefore what makes the bowed
 * sheet's self-overlap resolve correctly with no depth test. Exported so a
 * renderer (or its test) can assert the property rather than assume it -- the
 * claim used to live only in this comment, and the comment was wrong about which
 * way the mesh was emitted. */
bool ManualTurn_DepthRisesWithU(float turn, int samples);

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
