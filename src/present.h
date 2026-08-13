#ifndef PRESENT_H
#define PRESENT_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL.h>
#include "types.h"
#include "hd_replacements.h"
#include "diorama/diorama.h"
#include "sim/sim_render_metadata.h"
#include "action/action_effects.h"
#include "action/action_bg_plan.h"

/* M5 (ar-recomp-threading-impl.md Appendix D). FrameSlot is the ONE contract
 * for everything present-time rendering reads: it is populated by the single
 * writer FrameSlot_Capture (D5), called on the game thread immediately after
 * RtlDrawPpuFrame() returns, and consumed only by present.c (D6) — present.c
 * must not read g_ppu/g_settings/g_snes_width/etc. live, only slot fields.
 *
 * Pixel buffers (g_pixels, g_hud_bg_pixels, g_hud_obj_pixels,
 * g_m7_overlay_pixels, g_diorama_layer_pixels[], g_sim_obj_atlas_pixels) are
 * deliberately NOT copied
 * here. Safety for those comes from single-threaded ordering: since Phase 0
 * removed the present thread (#18/P13), Upload reads them on the same thread
 * that produced them (RtlDrawPpuFrame), before the next tick overwrites them,
 * so there is no reader/writer race to close. FrameSlot still carries the
 * small scalar/derived state Composite needs, because that keeps present.c
 * off live g_ppu/g_settings (the D6 isolation rule), not for any thread
 * reason (§2.8/D3). */

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
  /* Mirrors ppu.h's kPpuSurfaceWidth (the SURFACE allocation width, which is
   * kPpuBufWidth plus the apron per side), for the same D6 reason as the
   * overlay enum above: present-time code must not include ppu.h. It is the
   * ALLOCATED width of every layer texture, and therefore the denominator that
   * normalizes the U axis — the capture occupies only the leading snes_width
   * columns of it. Anything computing a normalized U offset must divide by this,
   * not by snes_width; see IJ1 in diorama_scroll_math.c for the artifact that
   * mistake produced. Cross-checked by FrameSlot_Capture's _Static_assert
   * against the real constant. */
  kFrameSlotLayerTextureWidth = 640,  /* kPpuSurfaceWidth (512 + 64*2) */
  /* The authentic SNES screen dimensions, mirroring actraiser_game.h's
   * kActRaiserAuthenticWidth/Height for the same D6 reason as the constants above:
   * present-time code must not include actraiser_game.h, which declares g_ram and
   * a pile of live WRAM accessors this side of the wall must not touch.
   *
   * This is the width of the AUTHENTIC image inside a possibly-wider framebuffer.
   * The widescreen layout is [extra][256][extra], so `(snes_width - this) / 2` is
   * the left margin and HUD source rects are expressed against this rather than
   * against snes_width -- the HUD is authored for the authentic window and is
   * anchored, not stretched. Do not confuse it with snes_width (the whole
   * framebuffer, which varies) or with kFrameSlotLayerTextureWidth (the allocated
   * texture, which is fixed at 640 and is the U-axis denominator).
   *
   * Both are cross-checked against their actraiser_game.h counterparts by
   * _Static_asserts in frame_slot.c. */
  kFrameSlotAuthenticWidth = 256,  /* kActRaiserAuthenticWidth */
  kFrameSlotAuthenticHeight = 224, /* kActRaiserAuthenticHeight */
};
enum {
  kFrameSlotOverlayFlag_RemoveFromGame = 1,
  kFrameSlotOverlayFlag_MarkFullAddSubscreen = 16,
};

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
  /* SDL_Texture*, copied for convenience. NOT stable after boot, despite what
   * this comment used to claim: HdReplacementHost_ReloadTextures destroys and
   * recreates every one on SDL_EVENT_RENDER_TARGETS_RESET /
   * _DEVICE_RESET. Within a single present that is harmless (the slot is
   * captured and consumed in one synchronous call), but a RETAINED slot — see
   * R17/C2's g_repr, which a between-ticks re-present re-composites — would
   * hold dangling handles across a reset. That is why the reset arm calls
   * HostDisplay_InvalidatePresentHistory(). Do not retain a FrameSlot anywhere else
   * without the same invalidation. */
  void *texture;
} FrameSlotHdEntry;

/* Scene-inspector click anchor (D4-adjacent: shared between the present-time
 * renderer, which reads it from the FrameSlot, and main.c's game-thread click
 * handler in dev_tools.c, which writes it from a live hit-test). Shared here
 * (rather than kept private to the host) so present.c can use the same
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
  /* Vertical margin the PPU actually rendered for this frame (the transpose of
   * ws_extra). snes_height stays the AUTHENTIC visible height, so the captured
   * surfaces are snes_height + ws_extra_top + ws_extra_bottom rows tall and
   * authentic scanline 0
   * lives at row ws_extra_top -- just as texture column 0 is screen
   * x = -ws_extra. Zero on every non-diorama frame, which is what keeps the
   * flat presentation path (which assumes row 0) correct by construction. */
  int ws_extra_top;
  int ws_extra_bottom;
  /* Columns of RESOLVE apron each captured surface carries per side beyond the
   * displayed span. Distinct from ws_extra, which is DISPLAY margin: the apron
   * is never shown as extra world, it is headroom so a sprite is fully
   * resolved before it reaches the visible plane edge instead of being clipped
   * as it crosses. Screen x = 0 sits at surface column obj_apron + ws_extra. */
  int obj_apron;
  int hud_scale_percent;

  /* Diorama gate (D14 — Diorama_IsActiveThisFrame() result for this frame). */
  bool diorama_active;
  /* Upload policy and exact producer metadata for this captured frame. The
   * request mask snapshots presentation settings; the content mask comes from
   * PPU scanout and marks destinations that received a nontransparent pixel.
   * PresentUpload intersects them without reading live settings or rescanning
   * the CPU surfaces. */
  uint32_t diorama_plane_request_mask;
  uint32_t diorama_plane_content_mask;
  /* Planes containing the resolved TS input to a full SNES colour add. The
   * compositor draws this subset with saturated additive blending after the
   * ordinary main-screen world planes. */
  uint32_t diorama_plane_additive_mask;
  /* Immutable key for scoped diorama layer overrides. The section is derived
   * from positively identified captured scene art, never from present-time
   * live WRAM. */
  uint8_t diorama_map_group;
  uint8_t diorama_map_number;
  uint8_t diorama_layer_section;
  /* True only when the flat-mode PPU capture produced a current BG2-winner
   * mask. Presentation must not reuse stale mask texture content. */
  bool action_bg2_mask_valid;

  /* D1 simulation-town semantic payload.  This value-copy is the only form
   * present-time code may consume; the live HLE producer state stays private
   * (the D6 rule).  All effective visual bits are zero until their render
   * stages land, so adding this contract cannot change the authentic composite. */
  SimFrameData sim;

  /* Action-stage spell and scene lifecycles, captured from WRAM beside the
   * frame they decorate. Payloads are inert outside positively identified
   * source records/map objects. */
  ActionEffectFrame action_effects;
  ActionSceneEffectFrame action_scene_effects;
  bool action_effect_lighting;
  bool action_effect_particles;

  /* "Cycle magic spell" cheat state, snapshotted rather than read live so
   * present-time code never touches g_settings or WRAM (D6). While armed the composite
   * draws a badge naming the current spell: a spell-swapped session must be
   * self-evidently non-stock in any screenshot, recording, or bug report. */
  bool magic_cycle_armed;
  uint8_t magic_cycle_selected;   /* 0 = none, else 1..4 */

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
   * NOTE ON "prev": interpolation needs this slot's data PLUS the previous
   * frame's, so the caller keeps its own DioramaScrollSnapshot (below) and
   * passes it to PresentFrame; the slot itself only ever carries the
   * CURRENT frame. Historically this note warned against reading the
   * alternate entry of a double-buffered slot array (the M5.3 present thread
   * could be writing it); that array and thread are gone (#18/P13), but the
   * separate prev snapshot remains the interface — PresentFrame is given
   * prev explicitly rather than inferring it. */
  uint64_t timestamp_ns;
  /* R17/C3: emulated ticks between the previous capture and this one — the
   * TRUE period of the prev->curr camera pair, clamped to 1..8 by
   * FrameSlot_Capture, and 0 while paused (a frozen re-capture is not a pair).
   * Interpolation divides the sub-tick phase by this: the main loop can drain
   * several ticks in one iteration while the scroll snapshot advances once per
   * present, and a multi-tick pair therefore carries proportionally more
   * camera motion than one tick's worth. Without it, extrapolation overshoots
   * by exactly that factor. (The wall-clock span EMA this replaces normalized
   * the same effect implicitly, by measuring the real elapsed interval.) */
  uint8_t capture_ticks;
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
   * read live from present.c per D6) so PresentFrame knows whether to
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
  /* Host-input orbit layered over the dynamic baseline. Unlike the baseline
   * zoom, these offsets are not persisted and return to zero after release. */
  float diorama_manual_orbit_yaw;
  float diorama_manual_orbit_pitch;
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
  float sim_manual_orbit_yaw;
  float sim_manual_orbit_pitch;
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
  /* Fix B (SPEC-backdrop-clip.md): the LIVE per-side margins the frame was
   * rendered with (extra_left_right above is the fixed budget, which does not
   * narrow at a world bound). Latched by ActRaiser_LiveMargins rather than read
   * from g_ppu, which can be zeroed between the draw and this capture.
   *
   * A zeroed slot therefore means "live span = the authentic 256 only", which
   * over-crops safely. Do NOT reinterpret 0 as "unset, use the full budget" —
   * that reintroduces the black wedge at exactly camera_x == 0, the case this
   * exists to fix. */
  uint8_t extra_left_cur;
  uint8_t extra_right_cur;
  /* BH6/extents: exact plan that produced the captured BG1/BG2 planes.
   * Ordinary action frames preserve canonical source, per-side extents, and
   * row bands; explicit global overrides retain source metadata but project
   * their executed edges. Non-action frames carry a native-source projection
   * of the applied policy. `bg_capture_pad_to_budget` is deliberately
   * separate: it is a frame-level capture execution fact, not a map-specific
   * edge decision. */
  ActionBgPlan action_bg_plan;
  bool bg_capture_pad_to_budget;
  /* Developer-only authoring guide gate, captured beside the exact plan so
   * present code never reads the live tuner singleton. */
  bool action_bg_extent_guides;
  uint8_t inidisp;
  uint8_t bg_mode;  /* PPU_mode(g_ppu) == (g_ppu->bgmode & 7) */

  FrameSlotOverlayCapture overlay_captures[kFrameSlotOverlaySourceCount];

  /* OBJ HUD-icon promotion. The OAM slots ActRaiser_WidescreenHudObjPromote
   * validated this frame, latched from ActRaiser_HudObjIconRange; a zero count
   * means it promoted nothing.
   *
   * Do NOT re-derive this from overlay_captures[Obj].oamFirst/oamCount. That
   * range is whatever policy claimed the ONE OBJ capture slot last, and in
   * diorama mode that is the full-frame 0..127 scene claim, not the icon —
   * which is exactly how the icon used to get lost and fall back to being
   * drawn centered with the scene instead of anchored right. */
  uint8_t hud_icon_first, hud_icon_count;

  /* oam/high_oam are only populated when there is an OBJ overlay or a promoted
   * icon to resolve (§2.8 cost note); oam_valid says whether this frame
   * actually filled them. */
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
   * the live g_scene_inspector_presentation directly). */
  bool scene_inspector_enabled;
  InspectorPresentationSelection inspector_selection;
} FrameSlot;

/* Sole writer (D5). Populates every field above; call once per frame,
 * immediately after RtlDrawPpuFrame() returns, on the game thread. Lives in
 * main.c (it legitimately reads live g_ppu/g_settings — it IS the boundary,
 * not a present.c function). */
void FrameSlot_Capture(FrameSlot *dst);

/* --- Shared pure geometry (D4): no global reads, so either the present
 * thread (fed from a FrameSlot) or dev_tools.c's game-thread mouse hit-test
 * (fed from live state) can call these with the same
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
  /* Bottom row (exclusive) of the BG3 capture when it extends BELOW
   * hud_split_height — diorama mode captures the whole authentic height so
   * the act-title card and pause text (same layer as the HUD, just further
   * down the screen) can be drawn as a flat overlay instead of vanishing
   * behind the tilted scene planes. 0 (or <= hud_split_height) means the
   * capture is the status bar only, as in flat mode. */
  uint8_t hud_body_y1;
  /* Resolved OBJ HUD-icon slot (computed by the caller from oam/highOam —
   * see DevTools_InspectWindowPoint / FrameSlot's oam[]/high_oam[]). */
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

/* 3 (top band) + 2 (player row) + 1 (enemy row) + 1 (BG3 body: act title /
 * pause text) + 1 (OBJ icon). */
enum { kHudPresentationChunkCapacity = 8 };

int BuildHudPresentationChunks(SDL_Rect viewport,
                               const HudProjectionInputs *inputs,
                               HudPresentationChunk *chunks);

SDL_Rect ComputePresentationViewport(SDL_Renderer *renderer,
                                     bool ignore_aspect_ratio,
                                     int pixel_aspect, int visible_width,
                                     int snes_height);

/* M7: the small subset of a FrameSlot that scroll interpolation needs from
 * the PREVIOUS frame. Deliberately its own tiny type rather than a second
 * `const FrameSlot *` — see the long comment on FrameSlot's timestamp_ns
 * field for why reusing a live FrameSlot for "prev" would misread the
 * current frame's data. The caller keeps exactly one of these, updated after
 * each composite via FrameSlot_ExtractScrollSnapshot. */
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

/* --- Present-time entry points. Called synchronously on the render/main
 * thread (Phase 0 removed the present thread, #18/P13). Still split into an
 * Upload phase (SDL_UpdateTexture only) and a resolved frame phase
 * (SDL_RenderTexture + post-process + host UI): the split keeps the texture
 * uploads grouped ahead of the vsync-blocking present, not for any cross-thread
 * handoff. */
void PresentUpload(const FrameSlot *slot);
/* prev_scroll: the scroll snapshot from the frame shown immediately before
 * this one (M7 interpolation, diorama mode only). NULL disables
 * interpolation for this call (screenshots, or simply "no previous frame
 * yet").
 *
 * alpha (R17/C4): the sub-tick phase this present sits at — the main loop's
 * accumulator remainder over kFrameNs, in [0,1) — or kInterpPhaseNone
 * (diorama_scroll_math.h) for a present that has no meaningful phase. Passed in
 * rather than read from a clock here, so present-time code cannot disagree with
 * the loop that owns the tick schedule. It is host timing, not game state, so
 * it is a parameter rather than a FrameSlot field: the slot stays immutable
 * after capture, and one retained slot can be re-composited at several
 * different phases (which is the whole point of the re-present). */
/* Owns the complete draw order: scene target begin, mode-specific scene,
 * post-process resolve, then full-output host UI. The returned viewport is the
 * exact image rect selected by the resolve (SDL's logical rect where active,
 * otherwise the calculated fallback). SDL_RenderPresent remains caller-owned. */
SDL_Rect PresentFrame(const FrameSlot *slot,
                      const DioramaScrollSnapshot *prev_scroll,
                      float alpha);

/* Drops renderer-owned present caches (HUD composite, sim shadow/rim targets,
 * town/world-navigation canvases, underlays and cloud fields) so the next
 * present rebuilds them.
 *
 * MUST be called from the SDL_EVENT_RENDER_TARGETS_RESET / _DEVICE_RESET arm.
 * Each of those textures is (re)written only when a GAME-side serial changes, or
 * once at creation — never in response to device state — so after a reset the
 * caches would short-circuit forever and keep presenting textures whose
 * contents the driver discarded. See the comment on the definition. */
void PresentRendererResources_Reset(void);

#endif
