#ifndef PRESENT_H
#define PRESENT_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL.h>
#include "types.h"
#include "hd_replacements.h"

/* M5 (ar-recomp-threading-impl.md Appendix D). FrameSlot is the ONE contract
 * for everything present-time rendering reads: it is populated by the single
 * writer FrameSlot_Capture (D5), called on the game thread immediately after
 * RtlDrawPpuFrame() returns, and consumed only by present.c (D6) — present.c
 * must not read g_ppu/g_settings/g_snes_width/etc. live, only slot fields.
 *
 * Pixel buffers (g_pixels, g_hud_bg_pixels, g_hud_obj_pixels,
 * g_m7_overlay_pixels, g_diorama_layer_pixels[]) are deliberately NOT copied
 * here — see the M5 plan's buffer-ownership note. Safety for those comes from
 * the present-thread handshake (the game thread does not touch them again
 * until the present thread's upload phase has finished reading them), not
 * from copying or double-buffering. FrameSlot only carries the small
 * scalar/derived state that the present thread's COMPOSITE phase reads
 * concurrently with the game thread's NEXT tick — that race is real
 * regardless of buffer strategy (§2.8/D3). */

/* Mirrors ppu.h's kPpuOverlaySource_* / kPpuOverlayFlag_RemoveFromGame.
 * present.c does not include ppu.h (D6), so the order/value is pinned here
 * and cross-checked by FrameSlot_Capture's _Static_assert against the real
 * enum where it's populated (main.c, which does include ppu.h). */
enum {
  kFrameSlotOverlay_Bg1 = 0,
  kFrameSlotOverlay_Bg2 = 1,
  kFrameSlotOverlay_Bg3 = 2,
  kFrameSlotOverlay_Bg4 = 3,
  kFrameSlotOverlay_Obj = 4,
  kFrameSlotOverlaySourceCount = 5,  /* kPpuOverlaySource_Count */
};
enum { kFrameSlotOverlayFlag_RemoveFromGame = 1 };

typedef struct FrameSlotOverlayCapture {
  int16_t x0, x1;
  int16_t y0, y1;
  uint8_t flags;
  uint8_t oamFirst, oamCount;
} FrameSlotOverlayCapture;

typedef struct FrameSlotHdEntry {
  bool active;
  int source;
  bool brightness_mod;
  void *texture;  /* SDL_Texture*; stable after boot, copied for convenience */
} FrameSlotHdEntry;

/* Scene-inspector click anchor (D4-adjacent: shared between the present-time
 * renderer, which reads it from the FrameSlot, and main.c's game-thread click
 * handler InspectWindowPoint, which writes it from a live hit-test). Shared
 * here (rather than kept private to main.c) so present.c can use the same
 * type without redeclaring it. */
typedef enum InspectorPresentationKind {
  kInspectorPresentation_Base,
  kInspectorPresentation_HudBg,
  kInspectorPresentation_HudObj,
} InspectorPresentationKind;

typedef struct InspectorPresentationSelection {
  InspectorPresentationKind kind;
  double source_x;
  double source_y;
  int output_x;
  int output_y;
  int output_width;
  int output_height;
} InspectorPresentationSelection;

typedef struct FrameSlot {
  /* Geometry, resolved (D3 — never call Settings_Visible*()/live globals from
   * present-time code; these are the already-resolved results). */
  int snes_width;
  int snes_height;
  int display_mode;
  int pixel_aspect;
  bool ws_active;
  int ws_extra;
  bool ignore_aspect_ratio;
  int visible_x0;
  int visible_width;
  int hud_scale_percent;

  /* Diorama gate (D14 — Diorama_IsActiveThisFrame() result for this frame). */
  bool diorama_active;

  /* M7 (§6.1): per-frame scroll snapshot for present-time interpolation.
   * Indexed by BG number (0=BG1..3=BG4), matching g_ppu->hScroll/vScroll.
   * timestamp_ns is when THIS slot was captured (FrameSlot_Capture, right
   * after RtlDrawPpuFrame). Mode-7 matrix interpolation is out of scope:
   * diorama (the only consumer) is Mode-1-only by the scope banner, and the
   * flat path's Mode-7 overlay is a single image, not a per-layer mesh to
   * shift.
   *
   * NOTE ON "prev": interpolation needs this slot's data PLUS the
   * previous frame's. It is NOT safe to read g_frame_slots[1-idx] for that —
   * the game thread's next submission targets exactly that alternate slot,
   * and per the M5.3 handshake it is free to start writing it well before
   * the present thread finishes compositing the current one (that's the
   * whole point of decoupling upload from composite). Instead, the present
   * thread keeps its OWN small thread-local DioramaScrollSnapshot (below),
   * updated after each composite from the slot it just showed — no shared
   * memory, no race. */
  uint64_t timestamp_ns;
  int16_t bg_hscroll[4];
  int16_t bg_vscroll[4];
  /* §6.4 turbo edge case: turbo compresses many emulated ticks' worth of
   * scroll into one FrameSlot submission (still at the normal ~16ms
   * submission cadence — see the M7 plan note on why this differs from the
   * doc's literal "multiple rapid submissions" turbo model), so the
   * prev->curr delta is not a valid one-tick velocity estimate. Skip
   * interpolation outright when either slot was captured under turbo. */
  bool turbo_active;

  /* Widescreen HUD split + related PPU scalars (§2.8). */
  uint8_t hud_split_height;
  uint8_t hud_left_end;
  uint8_t hud_right_start;
  uint8_t hud_player_row_y;
  uint8_t hud_left_only_y;
  uint8_t extra_left_right;
  uint8_t inidisp;
  uint8_t bg_mode;  /* PPU_mode(g_ppu) == (g_ppu->bgmode & 7) */

  FrameSlotOverlayCapture overlay_captures[kFrameSlotOverlaySourceCount];

  /* OBJ HUD-icon promotion. Only populated when
   * overlay_captures[Obj].oamCount != 0 (§2.8 cost note); oam_valid says
   * whether this frame actually filled them. */
  bool oam_valid;
  uint16_t oam[0x100];
  uint8_t high_oam[0x20];

  /* Mode-7 override presentation. The src-rect is derived at present time
   * from visible_x0/visible_width/snes_height (already resolved above), so
   * only the active flag needs capturing here. */
  bool m7_active;

  /* HD replacements, resolved per-entry policy for this frame (source index
   * matches overlay_captures[] above). */
  FrameSlotHdEntry hd_entries[kHdMaxReplacements];
  int hd_entry_count;

  /* Scene inspector (present.c must not touch g_settings.scene_inspector or
   * main.c's g_scene_inspector_presentation directly). */
  bool scene_inspector_enabled;
  InspectorPresentationSelection inspector_selection;
} FrameSlot;

/* Sole writer (D5). Populates every field above; call once per frame,
 * immediately after RtlDrawPpuFrame() returns, on the game thread. Lives in
 * main.c (it legitimately reads live g_ppu/g_settings — it IS the boundary,
 * not a present.c function). */
void FrameSlot_Capture(FrameSlot *dst);

/* --- Shared pure geometry (D4): no global reads, so either the present
 * thread (fed from a FrameSlot) or the game thread's mouse hit-test
 * (InspectWindowPoint, fed from live state) can call these with the same
 * math and get the same answer for the same inputs. Defined in present.c. */

typedef struct HudProjectionInputs {
  SDL_Texture *hud_bg_texture;
  SDL_Texture *hud_obj_texture;
  int hud_scale_percent;   /* g_settings.hud_scale_percent */
  int pixel_aspect;        /* g_active_pixel_aspect */
  int snes_width;
  int snes_height;
  int visible_width;
  uint8_t hud_split_height, hud_left_end, hud_right_start;
  uint8_t hud_player_row_y, hud_left_only_y, extra_left_right;
  /* Resolved OBJ HUD-icon slot (computed by the caller from oam/highOam —
   * see main.c's InspectWindowPoint / FrameSlot's oam[]/high_oam[]). */
  bool obj_icon_valid;
  int obj_icon_x, obj_icon_y;
} HudProjectionInputs;

typedef struct HudPresentationChunk {
  SDL_Texture *texture;
  SDL_Rect texture_source;
  SDL_Rect screen_source;
  SDL_Rect output_destination;
  InspectorPresentationKind inspector_kind;
  int inspector_x_bias;
} HudPresentationChunk;

enum { kHudPresentationChunkCapacity = 7 };

int BuildHudPresentationChunks(SDL_Rect viewport,
                               const HudProjectionInputs *inputs,
                               HudPresentationChunk *chunks);

SDL_Rect ComputePresentationViewport(SDL_Renderer *renderer, bool ws_active,
                                     bool ignore_aspect_ratio,
                                     int pixel_aspect, int visible_width,
                                     int snes_height);

/* M7: the small subset of a FrameSlot that scroll interpolation needs from
 * the PREVIOUS frame. Deliberately its own tiny type rather than a second
 * `const FrameSlot *` — see the long comment on FrameSlot's timestamp_ns
 * field for why reading a second live FrameSlot for "prev" would race the
 * game thread. The present thread keeps exactly one of these as its own
 * thread-local state, updated after each composite via
 * FrameSlot_ExtractScrollSnapshot. */
typedef struct DioramaScrollSnapshot {
  uint64_t timestamp_ns;
  int16_t bg_hscroll[4];
  int16_t bg_vscroll[4];
  uint8_t bg_mode;
  bool turbo_active;
  bool diorama_active;
} DioramaScrollSnapshot;

void FrameSlot_ExtractScrollSnapshot(const FrameSlot *slot,
                                    DioramaScrollSnapshot *out);

/* --- Present-time entry points (M5.2: still called synchronously from the
 * game thread; M5.3 moves these onto the present thread). Split into an
 * Upload phase (SDL_UpdateTexture only) and a Composite phase (SDL_RenderTexture
 * + SDL_RenderPresent) per the M5 plan's buffer-ownership decision: the game
 * thread is safe to redraw into the pixel buffers as soon as Upload returns,
 * well before Composite's vsync block finishes. */
void PresentUpload(const FrameSlot *slot);
/* prev_scroll: the scroll snapshot from the frame shown immediately before
 * this one (M7 interpolation, diorama mode only). NULL disables
 * interpolation for this call (screenshots, the no-present-thread
 * synchronous fallback, or simply "no previous frame yet"). */
void PresentComposite(const FrameSlot *slot,
                      const DioramaScrollSnapshot *prev_scroll);

#endif
