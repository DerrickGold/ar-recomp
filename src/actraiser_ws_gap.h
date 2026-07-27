#ifndef ACTRAISER_WS_GAP_H
#define ACTRAISER_WS_GAP_H

#include <stddef.h>
#include <stdint.h>

/* Fix C (SPEC-backdrop-clip.md): the compositor writes only the ACTIVE window,
 * so a finite world whose camera has reached its bound leaves unwritten strips
 * at both framebuffer edges — the difference between the fixed centering budget
 * and the narrowed live margin. Those strips must be filled every frame; left
 * alone they would hold the previous frame's pixels (stale ghost strips).
 *
 * Historically they were memset to 0. In flat widescreen that is correct — a
 * black strip reads as the intended pillarbox at a world edge. In diorama mode
 * the same framebuffer becomes the backdrop PLANE, drawn with SDL_BLENDMODE_NONE
 * behind the whole tilted stack, so those zeros paint an opaque black wedge
 * across roughly a fifth of the screen. Filling them with the scene's own
 * backdrop colour instead makes the plane continuous.
 *
 * Extracted as a pure function so the geometry is unit-testable without a ROM,
 * a PPU, or a renderer (precedent: diorama_scroll_math.c, host_display_pacing.c).
 *
 * `rows`   first row of the framebuffer, ARGB8888.
 * `pitch`  bytes per row.
 * `height` rows to fill.
 * `budget` the fixed per-side centering budget (ppu->extraLeftRight).
 * `live_left` / `live_right` this frame's live per-side margins
 *          (ppu->extraLeftCur / extraRightCur). A live margin equal to the
 *          budget means that side has no gap.
 * `fill_argb` the colour to write. Passing 0 reproduces the previous
 *          memset-to-black behaviour exactly, which is what makes the A/B
 *          toggle a true comparison.
 *
 * Out-of-range inputs (negative, or a live margin exceeding the budget) write
 * nothing rather than clamping: a caller that computed a nonsense margin should
 * not silently paint over real pixels.
 */
void ActRaiserFillMarginGaps(uint8_t *rows, size_t pitch, int height,
                             int budget, int live_left, int live_right,
                             uint32_t fill_argb);

#endif /* ACTRAISER_WS_GAP_H */
