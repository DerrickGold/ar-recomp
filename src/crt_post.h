/* CRT post-processing: one fullscreen shader pass over the finished frame.
 *
 * The whole effect hangs off a single integration point. PresentComposite() is
 * the one function every render mode funnels through — flat 2D, diorama, sim3D
 * enhanced and world-navigation 3D — so redirecting it into an offscreen target
 * and resolving that target through a shader gives every mode the effect at
 * once, with no per-mode work.
 *
 * Usage, around the existing composite call:
 *
 *     CrtPost_Begin(renderer);
 *     PresentComposite(...);
 *     CrtPost_End(renderer, scan_lines, image_rect);
 *     SDL_RenderPresent(renderer);
 *
 * Both calls are no-ops when the effect is off, leaving the original path
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

/* Redirect rendering into the offscreen scene target. Returns false (and
 * changes nothing) when the effect is disabled or unavailable. */
bool CrtPost_Begin(SDL_Renderer *renderer);

/* Resolve the scene target to the backbuffer through the CRT shader. Safe to
 * call unconditionally; does nothing if Begin did not engage.
 *
 * `scan_columns`/`scan_lines` are the SOURCE dimensions (visible_width and
 * g_snes_height), not pixel counts on screen — the beam profile and the
 * horizontal bandwidth limit are properties of the signal, not the window, so
 * they must hold still as it scales.
 * `image` is the letterboxed viewport the game picture occupies inside the
 * target, so curvature and scanlines apply to the picture and not to the
 * black bars around it. */
void CrtPost_End(SDL_Renderer *renderer, int scan_columns, int scan_lines,
                 SDL_Rect image);

/* The target that "back to the base surface" means right now: NULL when the
 * effect is off, the scene target while it is engaged. */
SDL_Texture *CrtPost_BaseTarget(void);

void CrtPost_Shutdown(void);

#endif /* AR_CRT_POST_H */
