/* CRT post-processing: one fullscreen shader pass over the finished frame.
 *
 * PresentFrame() owns the one integration point every render mode funnels
 * through — flat 2D, diorama, sim3D enhanced and world-navigation 3D — so
 * redirecting its scene stage into an offscreen target and resolving it here
 * gives every mode the effect at once.
 *
 * Usage, around the existing composite call:
 *
 *     PresentFrame(...);  // begin -> scene -> resolve -> host UI
 *     SDL_RenderPresent(renderer);
 *
 * Begin/End are no-ops when the effect is off, leaving the scene path
 * byte-for-byte untouched.
 *
 * IMPORTANT for any code that renders to its own target: while this is engaged,
 * "restore the previous target" is NOT SDL_SetRenderTarget(renderer, NULL) —
 * that would drop drawing onto the backbuffer, behind the resolve, and it would
 * silently vanish. Use CrtPost_BaseTarget() instead, which returns NULL when
 * the effect is off and the scene target when it is on. */
#ifndef AR_CRT_POST_H
#define AR_CRT_POST_H

#include <SDL3/SDL.h>
#include <stdbool.h>

/* Redirect rendering into the offscreen scene target. Returns false without
 * side effects when disabled. A selected-mode setup failure also returns
 * false, but latches SessionFatal so the caller can end the frame/session. */
bool CrtPost_Begin(SDL_Renderer *renderer);

/* Resolve the scene target to the backbuffer through the CRT shader. Safe to
 * call unconditionally; returns `image` unchanged if Begin did not engage.
 *
 * `scan_columns`/`scan_lines` are the SOURCE dimensions (visible_width and
 * g_snes_height), not pixel counts on screen — the beam profile and the
 * horizontal bandwidth limit are properties of the signal, not the window, so
 * they must hold still as it scales.
 * `image` is the letterboxed viewport the game picture occupies inside the
 * target, so curvature and scanlines apply to the picture and not to the
 * black bars around it.
 *
 * Returns the exact image rectangle used for the resolve. Flat presentation
 * may replace the caller's calculated fallback with SDL's per-target logical
 * presentation rectangle; post-resolve host UI must consume this return value
 * so both passes share one geometry truth. */
SDL_Rect CrtPost_End(SDL_Renderer *renderer,
                     int scan_columns, int scan_lines, SDL_Rect image);

/* The target that "back to the base surface" means right now: NULL when the
 * effect is off, the scene target while it is engaged. */
SDL_Texture *CrtPost_BaseTarget(void);

void CrtPost_Shutdown(void);

#endif /* AR_CRT_POST_H */
