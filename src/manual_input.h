#ifndef MANUAL_INPUT_H
#define MANUAL_INPUT_H

#include <SDL3/SDL.h>
#include <stdbool.h>

/* What the manual reader's inputs MEAN, separated from what it does about them.
 *
 * PURE: this reads no state and touches no renderer, so the whole of the
 * reader's control scheme is decidable in a test. It is its own translation unit
 * rather than part of manual_reader.c because that file owns textures, an image
 * decoder and the window -- linking all of it to assert that PageDown turns a
 * page would be absurd, and the assertions would not get written.
 *
 * It uses SDL's key and button enumerations and so is not SDL-free the way
 * manual_pages.c is. That is the line: manual_pages.c is arithmetic about paper,
 * this is policy about input devices, and neither is plumbing.
 */

typedef enum ManualIntent {
  kManualIntent_None = 0,
  kManualIntent_PageForward,
  kManualIntent_PageBack,
  kManualIntent_First,
  kManualIntent_Last,
  kManualIntent_ZoomIn,
  kManualIntent_ZoomOut,
  kManualIntent_ZoomReset,
  kManualIntent_PanLeft,
  kManualIntent_PanRight,
  kManualIntent_PanUp,
  kManualIntent_PanDown,
  kManualIntent_Close,
} ManualIntent;

/* What a key means to the reader.
 *
 * `zoomed` changes the answer for the arrows, and that is the whole subtlety
 * here. At fit there is no overhang, so panning would be a silent no-op and the
 * obvious key would appear broken; zoomed in, paging instead of panning would
 * make the reader unusable at exactly the magnification someone needed. PageUp
 * and PageDown page at ANY zoom, so there is always a way to turn without
 * zooming back out first. */
ManualIntent ManualInput_KeyIntent(SDL_Keycode key, bool zoomed);

/* The same for a gamepad button. The shoulders are the pad's PageUp/PageDown --
 * they page at any zoom, which is what frees the d-pad to follow the same
 * zoom rule the arrows do rather than inventing a second scheme. */
ManualIntent ManualInput_PadIntent(SDL_GamepadButton button, bool zoomed);

/* Which page to spend this frame's decode on, or -1 for none.
 *
 * DECODE MUST NOT SIT ON THE DRAW PATH. A 739x1080 baseline JPEG costs ~4.7 ms
 * scalar against a budget of refresh/2 -- 8.3 ms at 60 Hz, and 5.6 ms at the
 * Steam Deck's 90 -- so decoding the two or three pages a turn reveals in one
 * frame drops that frame outright, on precisely the frame an animation begins.
 * At most ONE page per presented frame, with the previous page still shown until
 * its replacement is ready, so a turn costs a frame of LATENCY instead.
 *
 * `wanted` is in priority order and may hold -1s and duplicates. `cached` is
 * asked what is already resident, and must also answer true for a page that has
 * permanently failed -- otherwise the budget retries it every frame forever and
 * never spends the frame on a page that could have succeeded. */
int ManualInput_NextDecode(const int *wanted, int wanted_count,
                           bool (*cached)(int page, void *user), void *user);

/* ── What to TELL the player ──────────────────────────────────────────────
 *
 * The reader has no chrome, so its hint line is the only place its controls are
 * ever named -- which makes a hint line that names the wrong device worse than
 * none. On a handheld the pad is the only input there is, and a line reading
 * "ESC BACK" is then instructions for a keyboard nobody is holding.
 *
 * The label lives here, beside the mapping it describes, so the two are edited
 * together and a test can hold them to each other. */
typedef enum ManualHintDevice {
  /* Keyboard and mouse share a line: they are the same sitting-at-a-desk case,
   * and both are usually present together. */
  kManualHintDevice_Keyboard = 0,
  kManualHintDevice_Gamepad,
} ManualHintDevice;

/* The controls half of the hint line -- no page counter, which is the caller's.
 * `zoomed` selects the wording because the controls themselves change with it:
 * the arrows and the d-pad pan when zoomed and page when not. Never NULL. */
const char *ManualInput_HintText(ManualHintDevice device, bool zoomed);

#endif  /* MANUAL_INPUT_H */
