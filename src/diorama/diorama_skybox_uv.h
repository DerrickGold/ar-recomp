#ifndef AR_DIORAMA_SKYBOX_UV_H
#define AR_DIORAMA_SKYBOX_UV_H

#include <stdbool.h>
#include <stdint.h>

#include "action/action_bg_plan.h"

/* The skybox quad fills the viewport and maps its
 * U range over the FIXED capture span, but the PPU only ever renders within the
 * live per-side margin — which collapses to 0 as a finite world's camera reaches
 * its bound. The never-rendered columns are transparent, and the skybox draws
 * with SDL_BLENDMODE_NONE, so they paint an opaque black wedge.
 *
 * Fix A repairs this at the source wherever BG2's margins are SYNTHESIZED
 * (mirror/repeat padding). Where they are not — a genuinely wide BG2 whose
 * margins come from tilemap, or a clamped BG2 with no margin content at all —
 * nothing can fill those columns, so the quad samples only the valid span.
 * The final aspect fit crops that span without changing the source pixel shape.
 *
 * Pure functions, no SDL and no globals, so the arithmetic is unit-testable
 * without a ROM or a renderer.
 */

/* Exact row-banded form used by the BH6 frame handoff. Action background edge
 * bands are expressed in authentic scanlines, while a captured diorama plane
 * can begin with `authentic_y0` rows of vertical extension. Each result entry
 * therefore describes both a half-open capture-row interval and the valid
 * texture-column interval for those rows.
 *
 * Adjacent rows with the same horizontal span are coalesced. Four input bands
 * can introduce at most eight horizontal-policy boundaries; fixed top/bottom
 * extents can add one transparent interval on each side. */
enum { kDioramaBgMaxValidSpans = kActionBgMaxBands * 2 + 3 };

typedef struct DioramaBgValidSpan {
  int y0, y1;
  int x0, x1;
} DioramaBgValidSpan;

typedef struct DioramaBgValidSpanPlan {
  uint8_t count;
  DioramaBgValidSpan spans[kDioramaBgMaxValidSpans];
} DioramaBgValidSpanPlan;

/* Half-open authentic-screen X bounds shared by the finite source's captured
 * live-world rows. Includes authored terrain and captured raster shifts.
 * Unavailable/cyclic sources leave valid false; padding policy is independent. */
typedef struct DioramaBgSourceBounds {
  int x0, x1;
  bool valid;
} DioramaBgSourceBounds;

/* What a band's single shift cannot carry: each authentic row's own shift
 * minus its band's, in texels. Only set when the bands could not split per
 * rate within the span budget (perspective floors change rate every row). */
typedef struct DioramaRasterRowShift {
  bool active;
  float texels[224];
} DioramaRasterRowShift;

/* Keep camera-driven raster bands with the presented playfield. The native
 * HDMA scrolls some BG2 rows from the playfield camera (sensitivity: pixels
 * per camera pixel, authentic rows 0..223), in whole pixels and from the
 * camera of the frame that built the table (`raster_camera_x`); `subtexel`
 * (nullable) gives how far each row's table was past its last whole pixel
 * there, for rows whose steps are not at floor(rate * camera). Presentation
 * shows the playfield at camera_x - hold_native_x (its edge clamp moves the
 * view hold_native_x native pixels right). Each band is drawn at exactly
 * rate * presented camera: continuous and current, so neither the table's
 * pixel steps nor its one-frame lag can jump when a hold starts or ends.
 * Splits the spans where sensitivity changes and returns each piece's texel
 * shift. When that split would not fit, and `rows` is given, neighbouring
 * pieces of one span merge (closest rates first, the longer piece keeping its
 * exact rate) into at most kDioramaBgMaxValidSpans - 1 spans, leaving one
 * band slot free for drawing rows apart, and `rows` carries the remainder.
 * Rows of a per-row gradient (a perspective floor) keep their native table
 * while nothing is held and ease into the held rows over the first few native
 * pixels of hold; runs of several rows at one rate are held like any band.
 * Returns false (outputs untouched) when no shift is needed or nothing fits;
 * callers then draw the original spans. */
bool DioramaBgValidSpanPlan_HoldRaster(
    const DioramaBgValidSpanPlan *spans, int authentic_y0,
    const float sensitivity[224], const float *subtexel,
    int camera_x, int raster_camera_x, float hold_native_x,
    DioramaBgValidSpanPlan *out, float shift_texels[kDioramaBgMaxValidSpans],
    DioramaRasterRowShift *rows);

/* Capture rows [y0, y1) split into runs of equal row shift. */
typedef struct DioramaRasterRowRun {
  int y0, y1;
  float texels;
} DioramaRasterRowRun;

/* Returns 0 when no row in the range is shifted (or the runs would exceed
 * `capacity`); the band then draws whole. Otherwise the runs cover the
 * range, unshifted rows included. */
unsigned DioramaRasterRowShift_Runs(
    const DioramaRasterRowShift *rows, int authentic_y0, int y0, int y1,
    DioramaRasterRowRun *out, unsigned capacity);

/* Where a finite skybox view starts in BG2's world for one frame.
 *
 * BG2's whole width is spread across the playfield camera's travel. Placing
 * the view from the playfield camera makes the sky stop exactly when the
 * playfield does, and keeps the view inside BG2's world so it is never pinned
 * at an edge while the playfield still moves. */
typedef struct DioramaSkyboxScrollInput {
  int camera_x;                    /* playfield (BG1) camera */
  int camera_min, camera_max;      /* the playfield camera's travel */
  int world_width;                 /* BG2's world */
  int view_width;                  /* requested view */
} DioramaSkyboxScrollInput;

/* Returns false when the caller's 1:1 BG2 placement applies: no camera travel
 * or no room to move. */
bool DioramaSkyboxScroll_Place(const DioramaSkyboxScrollInput *input,
                               float *world_left);

/* Accumulate one actually captured live-world row, accounting for its PPU
 * mosaic group (1 when disabled). Unavailable vertical rows and synthetic
 * edge families do not constrain the finite source interval. */
void DioramaBgSourceBounds_AddRow(
    DioramaBgSourceBounds *bounds, const ActionBgLayerPlan *layer,
    int authentic_y, int x0, int x1, unsigned mosaic_size);

/* A captured skybox is an enveloping presentation surface, not a literal
 * continuation of every source layer. When the layer has fewer live vertical
 * margin rows than the primary playfield, crop the unavailable capture rows
 * before fitting the remaining BG to the output aspect. Texture V remains
 * separate from capture Y because the PPU surface has fixed allocation
 * headroom above and below the active capture. */
typedef struct DioramaSkyboxVerticalMapping {
  float capture_y0;
  float capture_y1;
  float texture_v0;
  float texture_v1;
} DioramaSkyboxVerticalMapping;

void DioramaBgValidSpanPlan_Build(
    int ws_extra, int budget, int live_left, int live_right,
    bool pad_captured_to_budget, const ActionBgLayerPlan *layer,
    const DioramaBgSourceBounds *source_bounds,
    int authentic_y0, int capture_height, int tex_width,
    DioramaBgValidSpanPlan *out);

/* Bounding capture rows containing at least one drawable BG2 column. This is
 * also the authoritative content edge for an attached plane continuation: the
 * texture allocation/capture rectangle can extend past it with intentionally
 * transparent rows. */
bool DioramaBgValidSpanPlan_DrawableRowBounds(
    const DioramaBgValidSpanPlan *plan, int *out_y0, int *out_y1);

/* Resolve the drawable vertical capture interval and its blur-safe texture
 * range. A complete capture is bit-identical to the legacy V mapping; only a
 * genuinely clipped interval receives the radius+1 inset used by U. */
bool DioramaSkyboxVerticalMapping_Build(
    const DioramaBgValidSpanPlan *plan, int capture_height,
    int texture_height, float blur_radius,
    DioramaSkyboxVerticalMapping *out);

/* Normalize one capture-row boundary into the skybox's full-output axis. */
float DioramaSkyboxVerticalMapping_Fraction(
    const DioramaSkyboxVerticalMapping *mapping, float capture_y);

/* Keep the visible native window independent of extension-row redistribution.
 * The parallax displacement follows the same virtual camera as the playfield;
 * an independent BG edge may stop it sooner, without sampling unavailable art. */
void DioramaSkyboxVerticalMapping_FollowCamera(
    DioramaSkyboxVerticalMapping *mapping, int texture_height,
    int authentic_y0, float camera_delta);

/* Fit the skybox to the output with the requested source pixel shape. Crop
 * around the current view centre, never stretch one axis independently. The
 * returned source width is centred within each band's blur-safe U interval;
 * available_width is the narrowest visible interval, so all bands share one
 * vertical scale. Invalid inputs return zero without changing the mapping. */
float DioramaSkyboxVerticalMapping_FitAspect(
    DioramaSkyboxVerticalMapping *mapping, int texture_height,
    float available_width, float output_aspect, float pixel_aspect);

/* Map a texture-column span to the skybox quad's U range.
 *
 * The blur shader samples up to `blur_radius` texels either side of each
 * fragment, so the range is inset by radius+1 texels: without it the blur would
 * pull the still-black columns back across the new boundary. */
void DioramaSkyboxUvRange(int tex_width, int valid_x0, int valid_x1,
                          float blur_radius, float *out_u0, float *out_u1);

/* A ROM skybox is a tight map page rather than a widescreen PPU capture.
 * Repeat enough of that page to cover the current displayed width; vertical
 * mapping remains the complete page [0,1]. */
void DioramaRomSkyboxUvRange(int display_width, int source_width,
                             float *out_u0, float *out_u1);

/* Invert a flat world plane at one normalized output point. The result is
 * normalized capture space, so periodic skyboxes can share the foreground's
 * exact camera/scale instead of magnifying camera motion by fitting a crop. */
bool DioramaSkyboxWorldPoint(const float matrix[16], float z,
                             float aspect_x, float height_scale,
                             float output_x, float output_y,
                             float *capture_x, float *capture_y);

#endif /* AR_DIORAMA_SKYBOX_UV_H */
