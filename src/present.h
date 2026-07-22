#ifndef PRESENT_H
#define PRESENT_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL.h>
#include "types.h"
#include "hd_replacements.h"
#include "diorama.h"
#include "sim_render_metadata.h"

/* M5 (ar-recomp-threading-impl.md Appendix D). FrameSlot is the ONE contract
 * for everything present-time rendering reads: it is populated by the single
 * writer FrameSlot_Capture (D5), called on the game thread immediately after
 * RtlDrawPpuFrame() returns, and consumed only by present.c (D6) — present.c
 * must not read g_ppu/g_settings/g_snes_width/etc. live, only slot fields.
 *
 * Pixel buffers (g_pixels, g_hud_bg_pixels, g_hud_obj_pixels,
 * g_m7_overlay_pixels, g_diorama_layer_pixels[], g_sim_obj_atlas_pixels) are
 * deliberately NOT copied
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

  /* D1 simulation-town semantic payload.  This value-copy is the only form
   * the present thread may consume; the HLE producer remains game-thread
   * private.  All effective visual bits are zero until their render stages
   * land, so adding this contract cannot change the authentic composite. */
  SimFrameData sim;

  /* M7 (§6.1)/B1b (followup doc): per-frame camera snapshot for present-time
   * interpolation. timestamp_ns is when THIS slot was captured
   * (FrameSlot_Capture, right after RtlDrawPpuFrame). Mode-7 matrix
   * interpolation is out of scope: diorama (the only consumer) is
   * Mode-1-only by the scope banner, and the flat path's Mode-7 overlay is a
   * single image, not a per-layer mesh to shift.
   *
   * B1b replaced the original g_ppu->hScroll[]/vScroll[] snapshot with
   * these: ActRaiser's BG2 parallax is HDMA-driven (per-scanline register
   * rewrites), so hScroll[1]/vScroll[1] is whatever the last HDMA line left
   * behind by the time FrameSlot_Capture runs — not a stable "camera
   * position" — and interpolating between two such snapshots vibrated the
   * whole layer with no real camera motion (confirmed live). These are the
   * game's own STABLE logical camera in WRAM (kActRaiserWram_Bg1/2CameraX/Y,
   * $0022/$0024/$0026/$0028), read via ActRaiser_ReadWram16 — the
   * game-authored position BEFORE HDMA touches the PPU registers, so it
   * doesn't carry the residue. Only BG1/BG2 have a WRAM camera (BG3 is UI,
   * not world content — see ComputeDioramaScrollDelta, its delta is always
   * 0). WRAM camera values wrap naturally in ordinary int16 arithmetic
   * (unlike the 10-bit modular PPU scroll registers this replaces), so no
   * wrap-correction is needed when differencing them.
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
  int16_t bg1_camera_x, bg1_camera_y;
  int16_t bg2_camera_x, bg2_camera_y;
  /* §6.4 turbo edge case: turbo compresses many emulated ticks' worth of
   * scroll into one FrameSlot submission (still at the normal ~16ms
   * submission cadence — see the M7 plan note on why this differs from the
   * doc's literal "multiple rapid submissions" turbo model), so the
   * prev->curr delta is not a valid one-tick velocity estimate. Skip
   * interpolation outright when either slot was captured under turbo. */
  bool turbo_active;
  /* kSettingCat_Graphics "Scroll interpolation" row, snapshotted here (not
   * read live from present.c per D6) so PresentComposite knows whether to
   * even attempt interpolation for this frame. */
  bool interp_setting_enabled;
  /* A5 (followup doc) "Flat HUD" row, snapshotted here (not read live from
   * present.c per D6): true = diorama's PresentHudOverlayComposited call
   * (A7) runs, drawing the anchored flat HUD; false = skip it — BG3 was
   * left in the game-thread capture instead (actraiser_rtl.c), so it
   * renders as diorama.c's ordinary tilted BG3 layer. */
  bool diorama_hud_flat;
  /* B4-split (followup doc): DioramaCameraMode (settings.h) plus both
   * candidate authored poses, resolved at present-composite time into
   * whichever is active this frame — see present.c's g_diorama_render_cam
   * and the DioramaCameraPose comment (diorama.h) for the full rationale.
   * Snapshotting BOTH poses (rather than resolving on the game thread) keeps
   * FrameSlot_Capture a plain field-by-field mirror of g_settings, matching
   * every other row here. diorama_reactive_strength rides along now so
   * later B4 checkpoints (velocity-lean, pan, kicks) don't need another
   * FrameSlot edit. */
  int diorama_camera_mode;
  DioramaCameraPose diorama_free_pose;
  DioramaCameraPose diorama_dyncam_baseline;
  int diorama_reactive_strength;
  /* B4-vellean (followup doc): PlayerVelocityX/Y, self-calibrated against a
   * running per-session max and clamped to [-1,1] — see FrameSlot_Capture
   * (main.c) for why normalization happens there (it owns the WRAM read and
   * the running-max state) rather than here. yaw follows horizontal
   * velocity (running), pitch follows vertical velocity (jump/fall), naming
   * matches which DioramaCameraPose field each drives in present.c's sway
   * formula. Not yet multiplied by k_run/k_pitch/reactive_strength — that
   * formula is present-side (D6: present.c owns the actual sway math, this
   * is just the clamped raw signal). */
  float diorama_dyncam_lean_yaw;
  float diorama_dyncam_lean_pitch;
  /* B4-kick (followup doc): rising-edge event flags, computed on the game
   * thread (FrameSlot_Capture, main.c — it owns the WRAM reads and the
   * prior-state needed to detect an edge). True only on the ONE FrameSlot
   * capture where the underlying signal transitioned; present.c triggers a
   * fresh decaying impulse only when it sees a slot whose timestamp_ns it
   * hasn't already processed (a present redraw of the same slot must not
   * re-trigger). event_hit: PlayerFlags invuln bit rising edge (taking a
   * hit). event_land: PlayerVelocityY falling-then-settled in one tick.
   * event_boost: PlayerBoost 0-to-nonzero rising edge. */
  bool diorama_dyncam_event_hit;
  bool diorama_dyncam_event_land;
  bool diorama_dyncam_event_boost;

  /* Sim-town dynamic camera. Same shape as the diorama fields above and for
   * the same reasons: the game thread owns the WRAM reads and the per-frame
   * state an edge or a running average needs, present.c owns the actual
   * camera formula.
   *
   * The signals differ from action mode's because the mode does. There is no
   * jump and no ground, so "vertical velocity" is just the other axis of a
   * planar drift: yaw leans toward horizontal travel and pitch toward
   * vertical travel, and both come from the angel record's own +$1A/+$1C
   * planar velocities rather than from PlayerVelocity, which is an
   * action-stage concept. */
  /* SimCameraMode. Free Cam's pose reaches present.c the ordinary way, inside
   * sim.projection_*, because the game thread already resolved which pose is
   * active; this only says whether the reactive offsets apply on top. */
  int sim_camera_mode;
  int sim_dyncam_strength;
  float sim_dyncam_lean_yaw;
  float sim_dyncam_lean_pitch;
  bool sim_dyncam_event_hit;

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
  int16_t bg1_camera_x, bg1_camera_y;
  int16_t bg2_camera_x, bg2_camera_y;
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
