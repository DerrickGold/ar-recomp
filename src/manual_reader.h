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
 * TWO THREADS TOUCH THIS, and which one does what is not negotiable:
 *
 *   main thread     ManualReader_Open/Close and the three Handle* functions.
 *                   These mutate the view and nothing else.
 *   present thread  ManualReader_Render, and ONLY it. Every SDL_Render call and
 *                   every texture the reader owns is created, updated and
 *                   destroyed there, because this project has already been bitten
 *                   by renderer calls off the present thread (a black window on
 *                   Linux GL, worked around by forcing Vulkan). Do not "just
 *                   preload" a page from an input handler.
 *
 * The view is written by input and read by the draw with no lock, exactly as the
 * settings overlay's own row and tab state already are. That is sound here only
 * because every write is a word-sized store and the one ordering that matters --
 * ManualView_BeginTurn publishing turn_target BEFORE turn -- is already the
 * order manual_pages.c writes them in. A torn read therefore shows the previous
 * frame's turn, never a new turn against a stale target.
 */

/* Load and index the manual. Safe to call repeatedly; the work happens once.
 *
 * NO RENDERER INVOLVED, on purpose: carving is a byte walk over the container,
 * so the whole question of whether a manual exists is answerable on the main
 * thread before any texture exists. Only decoding needs the present thread. */
bool ManualReader_Load(void);

/* True when a manual was found and looks like a real page album. The menu row
 * reads this to explain itself rather than opening onto nothing. */
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

/* Release the textures. PRESENT THREAD ONLY. */
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
 * PRESENT THREAD ONLY. Draws the reader over `viewport` and advances the turn
 * from its own clock -- frame-paced where it is presented, rather than from the
 * main thread's, which is not the thing being animated. */
void ManualReader_Render(SDL_Rect viewport);

#endif  /* MANUAL_READER_H */
