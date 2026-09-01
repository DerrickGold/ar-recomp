#ifndef DIORAMA_SKYBOX_UV_H
#define DIORAMA_SKYBOX_UV_H

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
 * nothing can fill those columns, so the quad must instead sample only the span
 * that is actually valid and let the sky stretch slightly.
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

/* A captured skybox is an enveloping presentation surface, not a literal
 * continuation of every source layer. When the layer has fewer live vertical
 * margin rows than the primary playfield, crop the unavailable capture rows
 * and stretch the remaining BG across the complete output. Texture V remains
 * separate from capture Y because the PPU surface has fixed allocation
 * headroom above and below the active capture. */
typedef struct DioramaSkyboxVerticalMapping {
  int capture_y0;
  int capture_y1;
  float texture_v0;
  float texture_v1;
} DioramaSkyboxVerticalMapping;

void DioramaBgValidSpanPlan_Build(
    int ws_extra, int budget, int live_left, int live_right,
    bool pad_captured_to_budget, const ActionBgLayerPlan *layer,
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
    const DioramaSkyboxVerticalMapping *mapping, int capture_y);

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

#endif /* DIORAMA_SKYBOX_UV_H */
