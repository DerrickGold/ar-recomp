#ifndef MANUAL_READER_H
#define MANUAL_READER_H

#include <SDL3/SDL.h>
#include <stdbool.h>

/* Input policy and the decode budget: pure, and tested on their own. */
#include "manual_input.h"

/* The in-game manual, as the game hosts it.
 *
 * manual_pages.c decides everything that is arithmetic -- page indexing, the
 * spread layout, zoom and pan, the turn's kinematics and geometry. THIS file is
 * the part that cannot be tested without a window: the file, the textures, the
 * draw calls and the event plumbing. The split is the same one diorama_host.c
 * has against diorama_layer_order.c, and for the same reason.
 *
 * NESTED INSIDE THE SETTINGS OVERLAY, deliberately. Opening the reader does not
 * close the overlay: SettingsOverlay_IsOpen() stays true, so the ~18 places that
 * ask it whether the game is suspended, whether input is grabbed and whether the
 * HUD is theirs to draw need no edits at all, and cannot disagree with each
 * other about a third state. The reader is a mode the overlay is in, not a peer.
 *
 * The main thread owns the reader's input, view state, textures, and rendering.
 * Keep every SDL renderer call inside ManualReader_Render or its resource
 * helpers so renderer ownership remains obvious. In particular, do not preload
 * a page from an input handler: loading/indexing is renderer-free, while page
 * decoding and texture creation are lazy presentation work.
 */

/* Load and index the manual. Safe to call repeatedly; the work happens once.
 *
 * NO RENDERER INVOLVED, on purpose: carving is a byte walk over the container,
 * so the whole question of whether a manual exists is answerable on the main
 * thread before any texture exists. Decoding remains in the render path. */
bool ManualReader_Load(void);

/* True when a manual was found and looks like a real page album, loading it on
 * the first query. The overlay uses this to omit the whole Manual section rather
 * than offering an action that can only fail. */
bool ManualReader_Available(void);

/* One line on what happened: the page count, or why there is no manual. Owned
 * here, valid until the next ManualReader_Load. */
const char *ManualReader_Status(void);

/* Number of pages, or 0. */
int ManualReader_PageCount(void);

bool ManualReader_IsOpen(void);
/* Opens the reader, loading the manual first if needed. False -- and no state
 * change -- when there is no manual to show. */
bool ManualReader_Open(void);
void ManualReader_Close(void);

/* Release textures on the main thread that owns the renderer. */
void ManualReader_DestroyTextures(void);

/* ── Input ─────────────────────────────────────────────────────────────────
 *
 * Each returns true when it consumed the event. The reader is modal while open,
 * so it consumes nearly everything -- with ONE exception that is a safety
 * property rather than a preference: the keys that close the overlay are never
 * swallowed. A reader that failed to load a page, or drew nothing for any other
 * reason, must not be able to trap the player in a window they cannot leave. */
bool ManualReader_HandleKey(SDL_Keycode key, bool pressed, bool repeat);
bool ManualReader_HandleGamepadEvent(const SDL_Event *event);
/* Mouse coordinates arrive in WINDOW units while the reader draws in renderer
 * OUTPUT pixels, and on a high-DPI display those differ; the conversion happens
 * inside, against the rectangle the last frame was actually drawn into, so a
 * click lands on the half of the spread it looks like it landed on. */
bool ManualReader_HandleMouse(const SDL_Event *event);

/* ── Draw ──────────────────────────────────────────────────────────────────
 *
 * Draws the reader over `viewport` on the main renderer thread and advances the
 * page turn from the presentation clock. */
void ManualReader_Render(SDL_Rect viewport);

#endif  /* MANUAL_READER_H */
