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

/* How far BG2's captured margins actually extend on the frame being drawn. */
typedef enum DioramaBg2MarginSource {
  /* Raw tilemap fetch: valid out to the live per-side margin only. */
  kBg2Margin_Live = 0,
  /* Mirror/repeat synthesis. With Fix A active this reaches the full budget. */
  kBg2Margin_Padded = 1,
  /* Layer clamped to the authentic 256: no margin content exists at all. */
  kBg2Margin_Clamped = 2,
} DioramaBg2MarginSource;

/* Classify BG2's margin extent from the PPU's per-layer policy masks.
 *
 * `bg2_repeat_band` is true when BG2 carries a per-scanline repeat BAND, which
 * makes the extent vary by row. A single span cannot express that, so the band
 * is reported CONSERVATIVELY as Clamped: cropping the sky is never worse than
 * the black band that is there today.
 *
 * Masks are the raw ppu->wsLayerClamp / wsLayerMirror / wsLayerRepeat bytes;
 * only BG2's bit (1u << 1) is consulted, so BG1's policy cannot leak in. */
int DioramaBg2MarginSource_Classify(uint8_t ws_clamp, uint8_t ws_mirror,
                                    uint8_t ws_repeat, bool bg2_repeat_band);

/* Half-open span [*out_x0, *out_x1) of BG2's valid captured content, in TEXTURE
 * columns. The authentic 256 columns begin at `ws_extra` because the capture
 * starts at screen x = -ws_extra.
 *
 * `budget` is the fixed centering budget (ppu->extraLeftRight); `live_left` and
 * `live_right` are this frame's live margins. Both are clamped defensively into
 * [0, budget], and the result into [0, tex_width]. */
void DioramaBg2ValidSpan(int ws_extra, int budget, int live_left, int live_right,
                         int margin_source, int tex_width,
                         int *out_x0, int *out_x1);

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
