#ifndef DIORAMA_SKYBOX_UV_H
#define DIORAMA_SKYBOX_UV_H

#include <stdbool.h>
#include <stdint.h>

#include "action/action_bg_plan.h"

/* Fix B (SPEC-backdrop-clip.md): the skybox quad fills the viewport and maps its
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
 * without a ROM or a renderer (precedent: diorama_scroll_math.c).
 */

/* Exact row-banded form used by the BH6 frame handoff. Action background edge
 * bands are expressed in authentic scanlines, while a captured diorama plane
 * can begin with `authentic_y0` rows of vertical extension. Each result entry
 * therefore describes both a half-open capture-row interval and the valid
 * texture-column interval for those rows.
 *
 * Adjacent rows with the same horizontal span are coalesced. Four input bands
 * can introduce at most eight boundaries, hence the fixed 2*N+1 capacity. */
enum { kDioramaBgMaxValidSpans = kActionBgMaxBands * 2 + 1 };

typedef struct DioramaBgValidSpan {
  int y0, y1;
  int x0, x1;
} DioramaBgValidSpan;

typedef struct DioramaBgValidSpanPlan {
  uint8_t count;
  DioramaBgValidSpan spans[kDioramaBgMaxValidSpans];
} DioramaBgValidSpanPlan;

void DioramaBgValidSpanPlan_Build(
    int ws_extra, int budget, int live_left, int live_right,
    bool pad_captured_to_budget, const ActionBgLayerPlan *layer,
    int authentic_y0, int capture_height, int tex_width,
    DioramaBgValidSpanPlan *out);

/* Map a texture-column span to the skybox quad's U range.
 *
 * The blur shader samples up to `blur_radius` texels either side of each
 * fragment, so the range is inset by radius+1 texels: without it the blur would
 * pull the still-black columns back across the new boundary. */
void DioramaSkyboxUvRange(int tex_width, int valid_x0, int valid_x1,
                          float blur_radius, float *out_u0, float *out_u1);

#endif /* DIORAMA_SKYBOX_UV_H */
