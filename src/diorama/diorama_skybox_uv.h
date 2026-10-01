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
