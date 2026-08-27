#ifndef ACTION_OBJ_APRON_H
#define ACTION_OBJ_APRON_H

#include <stdbool.h>
#include <stdint.h>

#include "snesrecomp/runner.h"

/* ── The action-side OBJ apron channel ────────────────────────────────────
 *
 * A captured OBJ plane is WIDER than the span the diorama displays: the extra
 * kPpuObjApron columns per side are RESOLVE headroom, never shown. This module
 * owns what goes in them.
 *
 * Why it exists, stated precisely (the plan's Phase 3 correction, 2026-08-06 --
 * an earlier framing claimed the apron makes a sprite "slide in whole", which
 * it cannot):
 *
 *   - Clipping at the shown edge is INHERENT to a finite shown region and is
 *     not what this fixes. What it fixes is the shown edge being the same
 *     instant as the BUFFER edge, so a part straddling it was abandoned
 *     mid-write. Object $10E0 in Fillmore act 1 shows the signature: column
 *     occupancy ramping 0,0,6,10,22,27,28,29,35,37 into the plane's final
 *     column -- a sprite fading out of the buffer, not a sprite of that shape.
 *   - With the apron filled, the display edge has real neighbouring texels, so
 *     linear filtering, the crisp-path supersample and the DOF/edge-AA shaders
 *     stop blending the last real texel against nothing. DrawDioramaSkybox
 *     documents this exact failure for BG2 and works around it by insetting the
 *     UV range; the apron fixes it properly instead.
 *   - It is the machinery the sim synthetic part channel needs (plan Phases
 *     5-6), built where a byte-identity gate can prove it.
 *
 * The apron can only ever hold OBJ pixels. The widened background line buffer
 * ends at kPpuExtraLeftRight=128 and the live view ends at kWsExtraMax=120;
 * neither extends into these additional 64 columns. That is why the apron is
 * not displayed -- showing it would show sprites over empty background.
 *
 * INVARIANT, and the reason this is a separate channel rather than a wider
 * emit window: real OAM is NEVER widened. A part outside the display window
 * stays parked in the OAM shadow exactly as the ROM left it, and rides here
 * instead, carrying its EXACT position rather than the lossy 9-bit/8-bit
 * encoding. */

enum {
  /* Capacity, not a truncation policy: overflow is counted and reported, never
   * silently dropped. Sized well above the peak measured at the screen edge. */
  kActionApronMaxParts = 128,
};

/* Both margins in columns per side. `apron` is kPpuObjApron; `ws_extra` is the
 * DISPLAY margin (g_ws_extra), which is what the surface layout is keyed to --
 * deliberately not extraLeftCur/extraRightCur, which shrink at level bounds
 * while the surface geometry does not. */
typedef struct ActionApronGeometry {
  int ws_extra;
  int apron;
} ActionApronGeometry;

/* Allocation width of a captured plane under this geometry. */
int ActionApron_SurfaceWidth(const ActionApronGeometry *g);

/* ── The two facts every apron-aware surface consumer needs ────────────────
 *
 * These exist because getting them wrong is not hypothetical: on 2026-08-06
 * three separate consumers read apron-wide surfaces with the DISPLAY width and
 * produced three distinct visible regressions (a sheared HUD, a black stripe
 * down the backdrop, and a HUD icon loose in the scene). The arithmetic is
 * trivial; the bug is always "which width am I holding?", so the fix is to make
 * every call site name the answer.
 *
 * `display_width` is ALWAYS 256 + 2*ws_extra -- the span that is shown. `apron`
 * is 0 for a surface bound at scanline width. */

/* Row stride, in bytes, of a surface carrying `apron` columns per side. */
static inline size_t ActionApron_SurfacePitch(int display_width, int apron) {
  return (size_t)(display_width + apron * 2) * 4;
}

/* Byte offset from a surface's base to the first DISPLAYED column. Add this
 * before handing the surface to anything that expects screen x = -ws_extra at
 * column 0 (the flat upload, Sim3D's capture, the metadata trace, dev-tools). */
static inline size_t ActionApron_DisplayOffset(int apron) {
  return (size_t)apron * 4;
}

/* Screen x -> surface column. Screen x = 0 sits at column apron + ws_extra. */
int ActionApron_SurfaceColumn(const ActionApronGeometry *g, int screen_x);

/* Half-open SCREEN-x spans of the two apron bands -- the only columns this
 * channel is ever allowed to write. Everything between them belongs to
 * scanout, and writing there would break the display-window byte-identity
 * gate. */
void ActionApron_LeftSpan(const ActionApronGeometry *g, int *x0, int *x1);
void ActionApron_RightSpan(const ActionApronGeometry *g, int *x0, int *x1);

/* Does a part occupying [x, x+size) overlap either apron band? A part that
 * fails the OAM window can still miss the apron entirely (it may be far
 * outside, or rejected on the other axis), and recording it would waste a
 * slot. */
bool ActionApron_PartTouchesApron(const ActionApronGeometry *g, int x,
                                  int size);

/* Does a part carry OBJ colour-math alpha? Mirrors PpuCapturedOverlayColor:
 * CGADSUB's OBJ bit applies only to palettes 4-7, which the capture encodes as
 * layer id 4. Here the palette comes straight from the part's attribute word,
 * so the same rule is a pure function of tile_attr. */
bool ActionApron_PartUsesColorMath(uint16_t tile_attr);

/* ── Per-frame channel ─────────────────────────────────────────────────────
 * Filled by the emitter during the object scan (game logic), drained at
 * capture time after scanout. Cleared at the start of every action frame, so a
 * frame that emits nothing leaves an empty list rather than last frame's. */

void ActionApron_BeginFrame(void);
/* Returns false when the part was not recorded -- either it misses the apron
 * (not an error) or the list is full (counted as overflow). */
bool ActionApron_AddPart(const ActionApronGeometry *g, int screen_x,
                         int screen_y, uint16_t tile_attr, uint8_t size);
int ActionApron_Count(void);
int ActionApron_Overflow(void);
int ActionApron_PeakCount(void);
const SrPpuObjPart *ActionApron_Parts(void);

#endif  /* ACTION_OBJ_APRON_H */
