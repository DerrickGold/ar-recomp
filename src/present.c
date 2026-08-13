/* Present-time rendering is isolated from live game state. This file must NOT
 * declare or extern g_ppu, g_settings, g_snes_width, g_ws_extra,
 * g_active_pixel_aspect, or call Settings_Visible*() — every present-time
 * decision comes from the `const FrameSlot *` handed in. Leaving those symbols
 * undeclared makes a stray live read a compile error.
 *
 * Presentation resources are different: the renderer, window, textures, and
 * raw pixel buffers are boot-owned and used synchronously on the render/main
 * thread. The isolation rule applies to mutable game state, not these resources. */

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "action/action_bg_tuner.h"
#include "action/action_effect_projection.h"
#include "action/action_obj_apron.h"
#include "present.h"
#include "action/action_effect_render.h"
#include "constants.h"
#include "crt_post.h"
#include "types.h"
#include "diorama/diorama.h"
#include "diorama/diorama_skybox_uv.h"
#include "diorama/diorama_planes.h"
#include "diorama/diorama_scroll_math.h"
#include "hd_replacement_host.h"
#include "settings_overlay.h"
#include "dev/scene_inspector.h"
#include "scene3d_math.h"
#include "render_capabilities.h"
#include "sim/sim_render_atlas.h"
#include "sim/sim_town_canvas.h"
#include "sim/sim_world_map.h"
#include "sim/sim_world_navigation_capture.h"
#include "sim/sim3d.h"

/* kPixelAspect_Crt43 and kDioramaCam_Free/kDioramaCam_Dynamic are plain enum
 * constants (not live state) — fine to pull in just for those. */
#include "settings.h"
#include "present_internal.h"
#include "presentation_geometry.h"


extern SDL_Renderer *g_renderer;
extern SDL_Texture *g_texture;
extern SDL_Texture *g_hud_bg_texture;
extern SDL_Texture *g_hud_obj_texture;
extern uint8_t g_pixels[];
extern uint8_t g_hud_bg_pixels[];
extern uint8_t g_hud_obj_pixels[];
extern uint8_t g_action_bg2_mask_pixels[];
extern SDL_Texture *g_diorama_textures[kDioramaPlane_Count];
extern uint8_t *g_diorama_layer_pixels[kDioramaPlane_Count];
extern SDL_Texture *g_sim_obj_atlas_texture;

extern SDL_Texture *g_sim3d_layer_textures[kSim3DPlane_Count];
extern SDL_Texture *g_sim3d_flat_texture;
static uint32_t s_diorama_uploaded_plane_mask;
static SDL_Texture *s_action_bg2_mask_texture;
static SDL_Texture *s_action_bg2_effect_target;
static int s_action_bg2_effect_w, s_action_bg2_effect_h;
static bool s_action_bg2_blend_supported = true;

static void DisableActionBg2Effect(const char *operation) {
  if (!s_action_bg2_blend_supported) return;
  s_action_bg2_blend_supported = false;
  fprintf(stderr,
          "[action-fx] flat BG2 effect unavailable at %s (%s); disabled\n",
          operation ? operation : "unknown operation", SDL_GetError());
}

SDL_FRect ToFRect(SDL_Rect r) {
  return (SDL_FRect){ (float)r.x, (float)r.y, (float)r.w, (float)r.h };
}

static int ScaledHudPixels(int pixels, double scale) {
  int result = (int)(pixels * scale + 0.5);
  return result > 0 ? result : 1;
}

static void RenderHudChunk(SDL_Texture *texture, SDL_Rect src, SDL_Rect dst) {
  if (!texture || src.w <= 0 || src.h <= 0 || dst.w <= 0 || dst.h <= 0)
    return;
  SDL_FRect src_f = ToFRect(src), dst_f = ToFRect(dst);
  SDL_RenderTexture(g_renderer, texture, &src_f, &dst_f);
}

static void AddHudPresentationChunk(HudPresentationChunk *chunks, int *count,
                                    SDL_Texture *texture,
                                    SDL_Rect texture_source,
                                    SDL_Rect screen_source,
                                    SDL_Rect output_destination,
                                    InspectorPresentationKind kind,
                                    int inspector_x_bias) {
  if (!chunks || !count || *count >= kHudPresentationChunkCapacity ||
      !texture || texture_source.w <= 0 || texture_source.h <= 0 ||
      screen_source.w <= 0 || screen_source.h <= 0 ||
      output_destination.w <= 0 || output_destination.h <= 0)
    return;
  chunks[(*count)++] = (HudPresentationChunk){
    texture, texture_source, screen_source, output_destination,
    kind, inspector_x_bias,
  };
}

/* One pure geometry description drives both compositing and hit-testing.
 * Presentation supplies FrameSlot-derived inputs; DevTools_InspectWindowPoint
 * supplies a live snapshot. */
int BuildHudPresentationChunks(SDL_Rect viewport,
                               const HudProjectionInputs *in,
                               HudPresentationChunk *chunks) {
  if (!in->hud_bg_texture || !in->hud_split_height) return 0;

  int count = 0;
  double scale_y, scale_x;
  if (in->hud_scale_percent == 0) {
    /* Auto: derived from the viewport, so already in physical output pixels. */
    scale_y = (double)viewport.h / in->snes_height;
    scale_x = (double)viewport.w / in->visible_width;
  } else {
    /* Pinned: the percentage is SNES-pixels-per-OUTPUT-pixel, and the output is
     * physical under SDL_WINDOW_HIGH_PIXEL_DENSITY. The FrameSlot already
     * carries the density-corrected value (D6 — no live settings read here). */
    scale_y = in->hud_scale_percent / (double)kPercentScale;
    scale_x = scale_y * (in->pixel_aspect == kPixelAspect_Crt43 ? 7.0 / 6.0 : 1.0);
  }

  int tex_extra = (in->snes_width - kFrameSlotAuthenticWidth) / 2;
  int height = in->hud_split_height;
  int player_y = in->hud_player_row_y;
  int enemy_y = in->hud_left_only_y;
  if (player_y > height) player_y = height;
  if (enemy_y > height) enemy_y = height;
  if (player_y > enemy_y) player_y = enemy_y;

  /* Band 1: top row (ACT/TIME/SCORE) — 3-way left/center/right split. */
  int upper_h = player_y;
  int upper_dh = ScaledHudPixels(upper_h, scale_y);

  SDL_Rect src = { tex_extra, 0, in->hud_left_end, upper_h };
  SDL_Rect dst = { viewport.x, viewport.y,
                   ScaledHudPixels(src.w, scale_x), upper_dh };
  AddHudPresentationChunk(
      chunks, &count, in->hud_bg_texture, src,
      (SDL_Rect){ 0, 0, src.w, src.h }, dst,
      kInspectorPresentation_HudBg, -in->extra_left_right);

  if (in->hud_left_end < in->hud_right_start) {
    src.x = tex_extra + in->hud_left_end;
    src.w = in->hud_right_start - in->hud_left_end;
    dst.w = ScaledHudPixels(src.w, scale_x);
    dst.x = viewport.x + (viewport.w - dst.w) / 2;
    AddHudPresentationChunk(
        chunks, &count, in->hud_bg_texture, src,
        (SDL_Rect){ in->hud_left_end, 0, src.w, src.h }, dst,
        kInspectorPresentation_HudBg, 0);
  }

  int right_source_w = kFrameSlotAuthenticWidth - in->hud_right_start;
  int right_dest_w = ScaledHudPixels(right_source_w, scale_x);
  src.x = tex_extra + in->hud_right_start;
  src.w = right_source_w;
  dst.x = viewport.x + viewport.w - right_dest_w;
  dst.w = right_dest_w;
  AddHudPresentationChunk(
      chunks, &count, in->hud_bg_texture, src,
      (SDL_Rect){ in->hud_right_start, 0, src.w, src.h }, dst,
      kInspectorPresentation_HudBg, in->extra_left_right);

  /* Band 2: player row (PLAYER health + magic-scroll) — left+right split
   * at hud_right_start so health pips stay left-anchored and scroll tiles
   * stay right-anchored regardless of HP level. */
  if (player_y < enemy_y) {
    int mid_h = enemy_y - player_y;
    int mid_dh = ScaledHudPixels(mid_h, scale_y);
    int mid_dy = viewport.y + ScaledHudPixels(player_y, scale_y);

    src.x = tex_extra;
    src.y = player_y;
    src.w = in->hud_right_start;
    src.h = mid_h;
    dst.x = viewport.x;
    dst.y = mid_dy;
    dst.w = ScaledHudPixels(src.w, scale_x);
    dst.h = mid_dh;
    AddHudPresentationChunk(
        chunks, &count, in->hud_bg_texture, src,
        (SDL_Rect){ 0, player_y, src.w, src.h }, dst,
        kInspectorPresentation_HudBg, -in->extra_left_right);

    src.x = tex_extra + in->hud_right_start;
    src.w = kFrameSlotAuthenticWidth - in->hud_right_start;
    dst.x = viewport.x + viewport.w - ScaledHudPixels(src.w, scale_x);
    dst.w = ScaledHudPixels(src.w, scale_x);
    AddHudPresentationChunk(
        chunks, &count, in->hud_bg_texture, src,
        (SDL_Rect){ in->hud_right_start, player_y, src.w, src.h }, dst,
        kInspectorPresentation_HudBg, in->extra_left_right);
  }

  /* Band 3: enemy row — full-width left-anchored (boss health spans the
   * entire screen). */
  if (enemy_y < height) {
    int low_h = height - enemy_y;
    src.x = tex_extra;
    src.y = enemy_y;
    src.w = kFrameSlotAuthenticWidth;
    src.h = low_h;
    dst.x = viewport.x;
    dst.y = viewport.y + ScaledHudPixels(enemy_y, scale_y);
    dst.w = ScaledHudPixels(src.w, scale_x);
    dst.h = ScaledHudPixels(low_h, scale_y);
    AddHudPresentationChunk(
        chunks, &count, in->hud_bg_texture, src,
        (SDL_Rect){ 0, enemy_y, src.w, src.h }, dst,
        kInspectorPresentation_HudBg, -in->extra_left_right);
  }

  /* Band 4 (diorama): everything on BG3 BELOW the status bar — the act-title
   * card and the pause text. Unlike the bands above, this content is authored
   * for the authentic 256px screen and has no left/right anchor semantics, so
   * it is drawn as a single centered chunk at its authentic Y. Only present
   * when the capture side extended BG3's rectangle past the split (diorama +
   * diorama_hud_flat); in flat mode these rows are still drawn by the game
   * into the framebuffer and hud_body_y1 stays 0. */
  if (in->hud_body_y1 > height) {
    int body_h = in->hud_body_y1 - height;
    int body_dw = ScaledHudPixels(kFrameSlotAuthenticWidth, scale_x);
    SDL_Rect body_src = { tex_extra, height, kFrameSlotAuthenticWidth, body_h };
    SDL_Rect body_dst = { viewport.x + (viewport.w - body_dw) / 2,
                          viewport.y + ScaledHudPixels(height, scale_y),
                          body_dw, ScaledHudPixels(body_h, scale_y) };
    AddHudPresentationChunk(
        chunks, &count, in->hud_bg_texture, body_src,
        (SDL_Rect){ 0, height, kFrameSlotAuthenticWidth, body_h }, body_dst,
        kInspectorPresentation_HudBg, 0);
  }

  /* Action's selected-magic icon (4 OAM), simulation's hourglass (4 OAM), and
   * Sky Palace's magic icon (4 OAM for Magical Fire, 1 for the other three
   * spells) are separately validated OAM signatures; the caller resolves the
   * icon x/y (from live oam/highOam or the FrameSlot snapshot) and passes it in
   * already resolved, so this function stays free of oam[]/highOam[] entirely.
   * Every one of those signatures covers the same 16x16 footprint, which is why
   * the slot COUNT never reaches this far and one chunk size serves them all. */
  if (in->hud_obj_texture && in->obj_icon_valid && in->obj_icon_x < kFrameSlotAuthenticWidth) {
    int x = in->obj_icon_x, y = in->obj_icon_y;
    int icon_w = 16, icon_h = 16;
    SDL_Rect obj_src = { tex_extra + x, y, icon_w, icon_h };
    SDL_Rect obj_dst = {
      viewport.x + viewport.w - right_dest_w - ScaledHudPixels(20, scale_x),
      viewport.y + ScaledHudPixels(y, scale_y),
      ScaledHudPixels(icon_w, scale_x),
      ScaledHudPixels(icon_h, scale_y),
    };
    AddHudPresentationChunk(
        chunks, &count, in->hud_obj_texture, obj_src,
        (SDL_Rect){ x, y, icon_w, icon_h }, obj_dst,
        kInspectorPresentation_HudObj, 0);
  }
  return count;
}

SDL_Rect ComputePresentationViewport(SDL_Renderer *renderer,
                                     bool ignore_aspect_ratio,
                                     int pixel_aspect, int visible_width,
                                     int snes_height) {
  int out_w = 0, out_h = 0;
  SDL_GetRenderOutputSize(renderer, &out_w, &out_h);
  return PresentationGeometry_CalculateViewport(
      out_w, out_h, ignore_aspect_ratio,
      pixel_aspect == kPixelAspect_Crt43, visible_width, snes_height);
}

void ApplyLogicalPresentation(const FrameSlot *slot) {
  if (!g_renderer) return;
  /* The explicit Stretch path draws directly in output coordinates, and must
   * leave logical presentation disabled so readback and host-owned overlays
   * continue to use physical output pixels. Aspect-fit mode needs SDL's
   * letterbox transform for the base framebuffer. */
  if (slot->ignore_aspect_ratio) {
    SDL_SetRenderLogicalPresentation(g_renderer, 0, 0,
                                     SDL_LOGICAL_PRESENTATION_DISABLED);
    return;
  }
  PresentationGeometry_ApplyLogical(
      g_renderer, false,
      slot->pixel_aspect == kPixelAspect_Crt43,
      slot->visible_width, slot->snes_height);
}

static HudProjectionInputs BuildProjectionInputsFromSlot(const FrameSlot *slot) {
  HudProjectionInputs in = {0};
  in.hud_bg_texture = g_hud_bg_texture;
  in.hud_obj_texture = g_hud_obj_texture;
  in.hud_scale_percent = slot->hud_scale_percent;
  in.pixel_aspect = slot->pixel_aspect;
  in.snes_width = slot->snes_width;
  in.snes_height = slot->snes_height;
  in.visible_width = slot->visible_width;
  in.hud_split_height = slot->hud_split_height;
  in.hud_left_end = slot->hud_left_end;
  in.hud_right_start = slot->hud_right_start;
  in.hud_player_row_y = slot->hud_player_row_y;
  in.hud_left_only_y = slot->hud_left_only_y;
  in.extra_left_right = slot->extra_left_right;
  {
    const FrameSlotOverlayCapture *bg3 =
        &slot->overlay_captures[kFrameSlotOverlay_Bg3];
    if (bg3->y1 > (int16_t)slot->hud_split_height && bg3->y1 <= 240)
      in.hud_body_y1 = (uint8_t)bg3->y1;
  }

  /* The promote's own latched range, NOT overlay_captures[Obj].oamFirst/Count:
   * in diorama mode that capture describes the full-frame 0..127 scene claim
   * that legitimately overwrote the icon's capture, so keying off it dropped
   * obj_icon_valid and the icon fell back to whatever the scene did with it
   * (drawn tilted and centered rather than anchored beside the right group).
   *
   * Any nonzero count, not ==4: the promote only ever latches a range it has
   * validated as a 16x16 HUD icon, and Sky Palace spends 1 slot on that icon
   * for three of the four spells and 4 for Magical Fire. Demanding 4 here was
   * the second half of the bug that stranded those three at centre screen. */
  if (slot->oam_valid && slot->hud_icon_count) {
    int first = slot->hud_icon_first;
    in.obj_icon_x = (slot->oam[first * 2] & 0xff) |
        ((slot->high_oam[first >> 2] >> ((first & 3) * 2)) & 1) << 8;
    in.obj_icon_y = slot->oam[first * 2] >> 8;
    in.obj_icon_valid = true;
  }
  return in;
}

static void PresentHudOverlay(const FrameSlot *slot, SDL_Rect viewport) {
  HudProjectionInputs in = BuildProjectionInputsFromSlot(slot);
  HudPresentationChunk chunks[kHudPresentationChunkCapacity];
  int count = BuildHudPresentationChunks(viewport, &in, chunks);
  for (int i = 0; i < count; i++)
    RenderHudChunk(chunks[i].texture, chunks[i].texture_source,
                   chunks[i].output_destination);
}

/* A7 (followup doc), diorama variant. A straight port of PresentHudOverlay
 * into the diorama branch produced visible seams between the ACT/TIME/SCORE
 * bands because the scene and overlay followed different viewport policies.
 * Both branches now receive the same aspect-fit viewport; retaining a single
 * composite texture also makes per-chunk rounding self-contained.
 *
 * Reconstruct the whole HUD into one dedicated texture first (recreated
 * whenever the output size changes — same resolution the chunks would have
 * rendered at, just isolated so any residual per-chunk rounding stays
 * self-contained instead of visible against the tilted scene), then draw
 * that single texture as a plain, undistorted screen overlay — same
 * screen-space blit as the flat branch, just one draw call instead of up to
 * kHudPresentationChunkCapacity. */
static SDL_Texture *s_hud_composite_texture;
static int s_hud_composite_w, s_hud_composite_h;

static SDL_Texture *EnsureHudCompositeTexture(int w, int h) {
  if (!g_renderer || w <= 0 || h <= 0) return NULL;
  if (s_hud_composite_texture && s_hud_composite_w == w &&
      s_hud_composite_h == h)
    return s_hud_composite_texture;
  if (s_hud_composite_texture) SDL_DestroyTexture(s_hud_composite_texture);
  s_hud_composite_texture = SDL_CreateTexture(
      g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
  s_hud_composite_w = w;
  s_hud_composite_h = h;
  if (s_hud_composite_texture) {
    SDL_SetTextureBlendMode(s_hud_composite_texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(s_hud_composite_texture, SDL_SCALEMODE_NEAREST);
  }
  return s_hud_composite_texture;
}

void PresentHudOverlayComposited(const FrameSlot *slot,
                                        SDL_Rect viewport) {
  SDL_Texture *composite = EnsureHudCompositeTexture(viewport.w, viewport.h);
  if (!composite) return;

  HudProjectionInputs in = BuildProjectionInputsFromSlot(slot);
  HudPresentationChunk chunks[kHudPresentationChunkCapacity];
  SDL_Rect local_viewport = { 0, 0, viewport.w, viewport.h };
  int count = BuildHudPresentationChunks(local_viewport, &in, chunks);
  if (count <= 0) return;

  SDL_SetRenderTarget(g_renderer, composite);
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_NONE);
  SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 0);
  SDL_RenderClear(g_renderer);
  for (int i = 0; i < count; i++)
    RenderHudChunk(chunks[i].texture, chunks[i].texture_source,
                   chunks[i].output_destination);
  SDL_SetRenderTarget(g_renderer, CrtPost_BaseTarget());

  SDL_FRect dst = ToFRect(viewport);
  SDL_RenderTexture(g_renderer, composite, NULL, &dst);
}

static void PresentMode7Composite(const FrameSlot *slot, SDL_Rect viewport) {
  if (!g_m7_texture || !slot->m7_active) return;
  SDL_Rect src = { slot->visible_x0 * kHdMode7Scale, 0,
                   slot->visible_width * kHdMode7Scale,
                   slot->snes_height * kHdMode7Scale };
  SDL_FRect src_f = ToFRect(src), viewport_f = ToFRect(viewport);
  SDL_RenderTexture(g_renderer, g_m7_texture, &src_f, &viewport_f);
}

/* Draw every active HD replacement over the region its capture removed this
 * frame. Master brightness is resolved on the host texture so INIDISP fades
 * apply to the substituted art; forced blank suppresses it entirely. */
static void PresentHdReplacements(const FrameSlot *slot, SDL_Rect viewport) {
  if (slot->inidisp & 0x80) return;

  int vis_w = slot->visible_width;
  int vis_x0 = slot->visible_x0;
  int extra = (slot->snes_width - kFrameSlotAuthenticWidth) / 2;
  double scale_x = (double)viewport.w / vis_w;
  double scale_y = (double)viewport.h / slot->snes_height;

  for (int i = 0; i < slot->hd_entry_count; i++) {
    const FrameSlotHdEntry *entry = &slot->hd_entries[i];
    if (!entry->active || !entry->texture) continue;
    const FrameSlotOverlayCapture *capture =
        &slot->overlay_captures[entry->source];
    if (capture->x1 <= capture->x0 || capture->y1 <= capture->y0 ||
        !(capture->flags & kFrameSlotOverlayFlag_RemoveFromGame))
      continue;
    int dx0 = (int)((capture->x0 + extra - vis_x0) * scale_x + 0.5);
    int dx1 = (int)((capture->x1 + extra - vis_x0) * scale_x + 0.5);
    int dy0 = (int)(capture->y0 * scale_y + 0.5);
    int dy1 = (int)(capture->y1 * scale_y + 0.5);
    SDL_Rect dst = { viewport.x + dx0, viewport.y + dy0,
                     dx1 - dx0, dy1 - dy0 };
    if (dst.w <= 0 || dst.h <= 0) continue;

    SDL_Texture *texture = (SDL_Texture *)entry->texture;
    Uint8 mod = entry->brightness_mod
        ? (Uint8)((slot->inidisp & 0xf) * 255 / 15) : 255;
    SDL_SetTextureColorMod(texture, mod, mod, mod);
    SDL_FRect dst_f = ToFRect(dst);
    SDL_RenderTexture(g_renderer, texture, NULL, &dst_f);
  }
}

static int InspectorScreenToOutputX(SDL_Rect viewport, double screen_x,
                                    const FrameSlot *slot) {
  int visible_left = slot->visible_x0 - slot->ws_extra;
  return viewport.x + (int)((screen_x - visible_left) * viewport.w /
                            slot->visible_width + 0.5);
}

static int InspectorScreenToOutputY(SDL_Rect viewport, double screen_y,
                                    const FrameSlot *slot) {
  return viewport.y + (int)(screen_y * viewport.h / slot->snes_height + 0.5);
}

static int HudSourceToOutputX(const HudPresentationChunk *chunk, double source_x) {
  return chunk->output_destination.x +
      (int)((source_x - chunk->screen_source.x) *
            chunk->output_destination.w / chunk->screen_source.w + 0.5);
}

static int HudSourceToOutputY(const HudPresentationChunk *chunk, double source_y) {
  return chunk->output_destination.y +
      (int)((source_y - chunk->screen_source.y) *
            chunk->output_destination.h / chunk->screen_source.h + 0.5);
}

static bool HudHighlightToOutput(const HudPresentationChunk *chunk,
                                 int x0, int y0, int x1, int y1,
                                 SDL_Rect *output) {
  if (!chunk || !output) return false;
  x0 -= chunk->inspector_x_bias;
  x1 -= chunk->inspector_x_bias;
  const SDL_Rect source = chunk->screen_source;
  if (x0 < source.x) x0 = source.x;
  if (y0 < source.y) y0 = source.y;
  if (x1 > source.x + source.w) x1 = source.x + source.w;
  if (y1 > source.y + source.h) y1 = source.y + source.h;
  if (x1 <= x0 || y1 <= y0) return false;
  int output_x0 = HudSourceToOutputX(chunk, x0);
  int output_y0 = HudSourceToOutputY(chunk, y0);
  int output_x1 = HudSourceToOutputX(chunk, x1);
  int output_y1 = HudSourceToOutputY(chunk, y1);
  *output = (SDL_Rect){
    output_x0, output_y0, output_x1 - output_x0, output_y1 - output_y0,
  };
  return output->w > 0 && output->h > 0;
}

static bool FindSelectedHudChunk(const FrameSlot *slot, SDL_Rect viewport,
                                 HudPresentationChunk *selected) {
  if (slot->inspector_selection.kind == kInspectorPresentation_Base)
    return false;
  HudProjectionInputs in = BuildProjectionInputsFromSlot(slot);
  HudPresentationChunk chunks[kHudPresentationChunkCapacity];
  int count = BuildHudPresentationChunks(viewport, &in, chunks);
  for (int i = count - 1; i >= 0; i--) {
    const SDL_Rect source = chunks[i].screen_source;
    if (chunks[i].inspector_kind != slot->inspector_selection.kind ||
        slot->inspector_selection.source_x < source.x ||
        slot->inspector_selection.source_x >= source.x + source.w ||
        slot->inspector_selection.source_y < source.y ||
        slot->inspector_selection.source_y >= source.y + source.h)
      continue;
    if (selected) *selected = chunks[i];
    return true;
  }
  return false;
}

static void PresentSceneInspector(const FrameSlot *slot, SDL_Rect viewport) {
  if (!slot->scene_inspector_enabled || !SceneInspector_HasSelection())
    return;
  int x = 0, y = 0;
  if (!SceneInspector_GetPoint(&x, &y)) return;
  HudPresentationChunk hud_chunk;
  bool hud_selection = FindSelectedHudChunk(slot, viewport, &hud_chunk);
  int projected_px = hud_selection
      ? HudSourceToOutputX(&hud_chunk, slot->inspector_selection.source_x)
      : InspectorScreenToOutputX(viewport, slot->inspector_selection.source_x, slot);
  int projected_py = hud_selection
      ? HudSourceToOutputY(&hud_chunk, slot->inspector_selection.source_y)
      : InspectorScreenToOutputY(viewport, slot->inspector_selection.source_y, slot);
  int output_width = 0, output_height = 0;
  SDL_GetRenderOutputSize(g_renderer, &output_width, &output_height);
  bool same_output = output_width == slot->inspector_selection.output_width &&
                     output_height == slot->inspector_selection.output_height;
  int px = same_output ? slot->inspector_selection.output_x : projected_px;
  int py = same_output ? slot->inspector_selection.output_y : projected_py;
  int anchor_dx = px - projected_px;
  int anchor_dy = py - projected_py;

  SDL_BlendMode old_blend = SDL_BLENDMODE_NONE;
  Uint8 old_r = 0, old_g = 0, old_b = 0, old_a = 0;
  SDL_GetRenderDrawBlendMode(g_renderer, &old_blend);
  SDL_GetRenderDrawColor(g_renderer, &old_r, &old_g, &old_b, &old_a);
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(g_renderer, 255, 192, 32, 255);
  /* Crosshair arms scale with the output (7 SNES pixels' worth at the
   * current viewport scale, min the historical 7px) — a fixed 7 output
   * pixels is near-invisible at 4K/high-density output. */
  enum { kInspectorCrosshairMinimumArmPixels = 7 };
  int arm = viewport.h > 0
      ? (viewport.h * kInspectorCrosshairMinimumArmPixels +
         kFrameSlotAuthenticHeight / 2) /
          kFrameSlotAuthenticHeight
      : kInspectorCrosshairMinimumArmPixels;
  if (arm < kInspectorCrosshairMinimumArmPixels)
    arm = kInspectorCrosshairMinimumArmPixels;
  SDL_RenderLine(g_renderer, (float)(px - arm), (float)py, (float)(px + arm), (float)py);
  SDL_RenderLine(g_renderer, (float)px, (float)(py - arm), (float)px, (float)(py + arm));

  int x0, y0, x1, y1;
  if (SceneInspector_GetHighlight(&x0, &y0, &x1, &y1)) {
    SDL_Rect rect;
    bool have_rect = hud_selection &&
        HudHighlightToOutput(&hud_chunk, x0, y0, x1, y1, &rect);
    if (!hud_selection) {
      rect = (SDL_Rect){
        InspectorScreenToOutputX(viewport, x0, slot),
        InspectorScreenToOutputY(viewport, y0, slot),
        InspectorScreenToOutputX(viewport, x1, slot) -
            InspectorScreenToOutputX(viewport, x0, slot),
        InspectorScreenToOutputY(viewport, y1, slot) -
            InspectorScreenToOutputY(viewport, y0, slot),
      };
      have_rect = rect.w > 0 && rect.h > 0;
    }
    if (have_rect) {
      rect.x += anchor_dx;
      rect.y += anchor_dy;
      SDL_FRect rect_f = ToFRect(rect);
      SDL_RenderRect(g_renderer, &rect_f);
    }
  }
  SDL_SetRenderDrawBlendMode(g_renderer, old_blend);
  SDL_SetRenderDrawColor(g_renderer, old_r, old_g, old_b, old_a);
  SettingsOverlay_RenderDebugPanel(
      "SCENE INSPECTOR", SceneInspector_PanelText(), (SDL_Point){ px, py });
}

void PresentUpload(const FrameSlot *slot) {
  if (!g_renderer || !g_texture) return;

  if (slot->diorama_active) {
    uint8_t *pixels[kDioramaPlane_Count];
    memcpy(pixels, g_diorama_layer_pixels, sizeof(pixels));
    pixels[kDioramaPlane_Backdrop] = g_pixels;
    uint32_t upload_mask = slot->diorama_plane_request_mask &
                           slot->diorama_plane_content_mask;
    /* Row 0 is the top of the captured world band. Upload both sides; the
     * authentic frame begins at ws_extra_top and the lower band follows it. */
    s_diorama_uploaded_plane_mask = Diorama_Upload(
        g_diorama_textures, pixels, slot->snes_width + slot->obj_apron * 2,
        slot->snes_height + slot->ws_extra_top + slot->ws_extra_bottom,
        slot->obj_apron, upload_mask);
  } else {
    s_diorama_uploaded_plane_mask = 0;
    SDL_Rect upload = { 0, 0, slot->snes_width, slot->snes_height };
    /* g_pixels is bound apron-wide (it doubles as the diorama backdrop plane),
     * so the authentic frame starts kPpuObjApron columns in. Offset the source
     * and use the real pitch; the upload rect is unchanged. */
    SDL_UpdateTexture(
        g_texture, &upload,
        g_pixels + ActionApron_DisplayOffset(slot->obj_apron),
        ActionApron_SurfacePitch(slot->snes_width, slot->obj_apron));
  }

  if (!slot->diorama_active && slot->action_bg2_mask_valid) {
    if (!s_action_bg2_mask_texture) {
      s_action_bg2_mask_texture = SDL_CreateTexture(
          g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
          kFrameSlotLayerTextureWidth, kFrameSlotAuthenticHeight);
      if (s_action_bg2_mask_texture) {
        SDL_SetTextureScaleMode(
            s_action_bg2_mask_texture, SDL_SCALEMODE_NEAREST);
        SDL_SetTextureBlendMode(
            s_action_bg2_mask_texture, SDL_BLENDMODE_MUL);
      }
    }
    if (s_action_bg2_mask_texture) {
      const SDL_Rect mask = {0, 0, slot->snes_width, slot->snes_height};
      SDL_UpdateTexture(s_action_bg2_mask_texture, &mask,
                        g_action_bg2_mask_pixels, slot->snes_width * 4);
    }
  }

  /* Refresh HUD textures for both presentation paths. Diorama anchors the HUD
   * through the same textures as flat mode; skipping this upload would combine
   * stale pixels with the current frame's split geometry. */
  if (slot->hud_split_height) {
    int split_rows = slot->hud_split_height;
    if (g_hud_bg_texture) {
      int rows = slot->overlay_captures[kFrameSlotOverlay_Bg3].y1;
      if (rows < split_rows) rows = split_rows;
      SDL_Rect hud = { 0, 0, slot->snes_width, rows };
      SDL_UpdateTexture(g_hud_bg_texture, &hud, g_hud_bg_pixels,
                        slot->snes_width * 4);
    }
    if (g_hud_obj_texture) {
      int rows = slot->overlay_captures[kFrameSlotOverlay_Obj].y1;
      if (rows < split_rows) rows = split_rows;
      SDL_Rect hud = { 0, 0, slot->snes_width, rows };
      SDL_UpdateTexture(g_hud_obj_texture, &hud, g_hud_obj_pixels,
                        slot->snes_width * 4);
    }
  }

  if (g_m7_texture && slot->m7_active) {
    SDL_Rect src = { slot->visible_x0 * kHdMode7Scale, 0,
                     slot->visible_width * kHdMode7Scale,
                     slot->snes_height * kHdMode7Scale };
    SDL_UpdateTexture(g_m7_texture, &src,
                      g_m7_overlay_pixels + (size_t)src.x * 4,
                      slot->snes_width * kHdMode7Scale * 4);
  }

  /* D1b: the raw atlas follows the same upload-before-release ownership as
   * every other frame pixel buffer. Only the packed used rectangle is copied;
   * all descriptors in this immutable slot are bounded by that rectangle. */
  if (g_sim_obj_atlas_texture && slot->sim.town && slot->sim.atlas_valid &&
      slot->sim.atlas_used_width && slot->sim.atlas_used_height) {
    SDL_Rect atlas = { 0, 0, slot->sim.atlas_used_width,
                      slot->sim.atlas_used_height };
    SDL_UpdateTexture(g_sim_obj_atlas_texture, &atlas,
                      g_sim_obj_atlas_pixels, kSimObjAtlasPitch);
  }

  if (slot->sim.separated_valid) {
    SDL_Rect frame = { 0, 0, slot->snes_width, slot->snes_height };
    uint32_t plane_upload_mask =
        Sim3D_PlaneTextureUploadMask(slot->sim.effective_features);
    for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
      if ((plane_upload_mask & (1u << plane)) &&
          g_sim3d_layer_textures[plane] && g_sim3d_layer_pixels[plane])
        SDL_UpdateTexture(g_sim3d_layer_textures[plane], &frame,
                          g_sim3d_layer_pixels[plane], slot->snes_width * 4);
    }
    /* Ground projection samples the separated planes directly. Upload the
     * CPU flat composite only for the fallback stage that actually draws it. */
    if (g_sim3d_flat_texture &&
        !(slot->sim.effective_features & kSimFeature_GroundProjection))
      SDL_UpdateTexture(g_sim3d_flat_texture, &frame, g_sim3d_flat_pixels,
                        slot->snes_width * 4);
  }
  UploadSimTownCanvas();
  UploadWorldNavigationComposition(slot);
}

void FrameSlot_ExtractScrollSnapshot(const FrameSlot *slot,
                                    DioramaScrollSnapshot *out) {
  out->timestamp_ns = slot->timestamp_ns;
  out->bg1_camera_x = slot->bg1_camera_x;
  out->bg1_camera_y = slot->bg1_camera_y;
  out->bg2_camera_x = slot->bg2_camera_x;
  out->bg2_camera_y = slot->bg2_camera_y;
  out->bg_mode = slot->bg_mode;
  out->turbo_active = slot->turbo_active;
  out->diorama_active = slot->diorama_active;
}

/* M7 (§6.1-6.4): present-time scroll interpolation, diorama only (the flat
 * path's single baked framebuffer has no separate per-layer scroll to shift
 * — see the M7 plan note). Extrapolates FORWARD from `curr` using the
 * prev->curr delta as a one-tick velocity estimate: at the instant curr was
 * captured (t=0) we show curr as-is; by one whole tick-period later (t=1)
 * we show curr shifted by a full predicted tick's motion. This deliberately
 * differs from the doc's §6.2 literal `prev + t*(curr-prev)` sketch, which
 * lerps FROM prev TO curr — since a present always happens at or after
 * curr's timestamp (curr is by definition the latest captured data), that
 * formula would show prev's stale position at t=0 and only reach curr's
 * actual position a full tick later, i.e. a constant one-tick display lag.
 * Extrapolation is the standard fixed-timestep-render technique for exactly
 * this "render happens after the last tick, before the next one" case. */
static DioramaScrollDelta ComputeDioramaScrollDelta(
    const FrameSlot *curr, const DioramaScrollSnapshot *prev, float alpha) {
  /* R17/C4: the phase comes from the caller (the main loop's accumulator
   * remainder), not from SDL_GetTicksNS() here. Present-time code reading its
   * own clock to guess where it sat between two ticks is what made this
   * corruptible: the guess needed an EMA of a period the loop already knew
   * exactly, and that EMA could be polluted by presents the loop never
   * scheduled (R16). This wrapper survives only because present.c must not
   * include diorama_scroll_math.h's caller-side gate. */
  return ComputeDioramaScrollDeltaAt(curr, prev, alpha);
}

/* Presentation-owned effective camera. Free Cam uses the persisted pose from
 * the FrameSlot. Dynamic Cam eases reactive lean and event kicks around its
 * dedicated baseline. Diorama_Composite receives that resolved pose and never
 * reads producer-owned g_diorama_cam. */
static DioramaCameraPose g_diorama_render_cam;
static int g_diorama_render_cam_mode = -1;    /* -1: no frame composited yet */
static uint64_t g_diorama_render_cam_last_ns;

/* Dynamic-camera response constants. */
static const float kDioramaDampTau = 0.15f;    /* seconds, 1-exp(-dt/tau) */
static const float kDioramaLeanYaw = 0.10f;    /* rad, max yaw lean @ full run speed */
/* Doc's provisional 0.06 rad (half of yaw's 0.10) turned out imperceptible
 * in play (AR_DYNCAM_LOG confirmed the render camera genuinely swings
 * ~2.4 deg during a jump — this isn't a pipeline bug), most likely because
 * pitch reads far less visually salient than yaw in this 3/4 view (weaker
 * differential parallax between layers than a lateral sway produces) and a
 * running jump has both swinging at once, with yaw dominating. Raised to
 * match yaw's peak so a jump reads as clearly as running does. */
static const float kDioramaLeanPitch = 0.12f;  /* rad, max pitch lean @ full vertical speed */

/* B4-kick (followup doc): event-triggered impulses, decaying independently
 * of the baseline+lean damping above (a jolt should feel crisp, not get
 * folded into the slower position-ease target) — added on top of a LOCAL
 * copy of g_diorama_render_cam each frame, never baked into the persisted
 * render-cam state itself. kDioramaKickPitch/kDioramaKickTau are the doc's
 * literal event_kick_magnitude/event_kick_decay.
 *
 * The zoom-punch (kDioramaKickZoom) was originally spec'd for the boost
 * event, but PlayerBoost turned out not to be a clean trigger (fired
 * constantly just holding a direction — disabled, see event_hit/event_land
 * below). Repurposed onto the HIT event instead (live design call, 2026-07-21
 * — the effect itself read well, it just needed a reliable source): a hit
 * uses the reliable invuln-bit edge already relied on elsewhere
 * (AR_NO_KNOCKBACK), and now gets BOTH the jolt and the zoom-punch, making
 * it read as more dramatic than a routine landing (jolt only). */
static float g_diorama_kick_pitch;       /* rad, landing/hit jolt, decays to 0 */
static float g_diorama_kick_zoom;        /* fraction, hit zoom-punch, decays to 0 (negative = closer) */
static uint64_t g_diorama_last_slot_ns;  /* detects a genuinely NEW FrameSlot capture */
static const float kDioramaKickPitch = 0.05f;  /* rad */
static const float kDioramaKickZoom = -0.15f;  /* fraction; "slight" zoom-in */
static const float kDioramaKickTau = 0.20f;    /* seconds, wall-clock exp decay */

/* Host effects use the renderer abstraction's standard additive/alpha blend
 * modes and untextured geometry, not a backend shader. Those are portable
 * API paths,
 * but not a promise of pixel-identical rasterization across Metal, Vulkan,
 * Direct3D and software. Capability is verified at the point of use: SDL may
 * legally substitute the closest blend mode, so a successful set is followed
 * by a get-and-compare. Any rejection or substitution fails the stages closed. */
static SDL_AtomicInt s_effect_blend_supported = { .value = 1 };
static SDL_AtomicInt s_effect_geometry_supported = { .value = 1 };

void DisableEffectBlend(const char *operation) {
  if (!SDL_CompareAndSwapAtomicInt(
          &s_effect_blend_supported, 1, 0))
    return;
  fprintf(stderr,
          "[host-effects] effect blend pass unavailable at %s (%s) — "
          "effect lighting and particles disabled\n",
          operation, SDL_GetError());
}

static void DisableEffectGeometry(const char *operation) {
  if (!SDL_CompareAndSwapAtomicInt(
          &s_effect_geometry_supported, 1, 0))
    return;
  fprintf(stderr,
          "[host-effects] geometry pass unavailable at %s (%s) — "
          "effect lighting and particles disabled\n",
          operation, SDL_GetError());
}

bool EffectRendererAvailable(void) {
  return SDL_GetAtomicInt(&s_effect_blend_supported) != 0 &&
      SDL_GetAtomicInt(&s_effect_geometry_supported) != 0;
}

bool Present_EffectRendererSupported(void) {
  return EffectRendererAvailable();
}

static bool BeginEffectBlendMode(EffectRenderState *state,
                                 SDL_BlendMode requested,
                                 const char *operation) {
  if (!state || !EffectRendererAvailable()) return false;
  if (!SDL_GetRenderDrawBlendMode(g_renderer, &state->blend) ||
      !SDL_GetRenderDrawColor(g_renderer, &state->r, &state->g,
                              &state->b, &state->a)) {
    DisableEffectBlend("state capture");
    return false;
  }
  if (!SDL_SetRenderDrawBlendMode(g_renderer, requested)) {
    SDL_SetRenderDrawBlendMode(g_renderer, state->blend);
    DisableEffectBlend(operation);
    return false;
  }
  SDL_BlendMode applied = SDL_BLENDMODE_INVALID;
  if (!SDL_GetRenderDrawBlendMode(g_renderer, &applied) ||
      applied != requested) {
    SDL_SetRenderDrawBlendMode(g_renderer, state->blend);
    DisableEffectBlend(operation);
    return false;
  }
  return true;
}

bool BeginEffectAdd(EffectRenderState *state) {
  return BeginEffectBlendMode(
      state, SDL_BLENDMODE_ADD, "additive blend verification");
}

static bool BeginEffectAlpha(EffectRenderState *state) {
  return BeginEffectBlendMode(
      state, SDL_BLENDMODE_BLEND, "alpha blend verification");
}

void EndEffectBlend(const EffectRenderState *state) {
  if (!state) return;
  bool color_ok = SDL_SetRenderDrawColor(
      g_renderer, state->r, state->g, state->b, state->a);
  bool blend_ok = SDL_SetRenderDrawBlendMode(g_renderer, state->blend);
  if (!color_ok || !blend_ok) DisableEffectBlend("state restore");
}

bool SubmitEffectBatch(EffectBatch *batch) {
  if (!batch || batch->overflow) {
    static bool logged;
    if (!logged) {
      logged = true;
      fprintf(stderr,
              "[host-effects] internal geometry batch capacity exceeded — "
              "effect pass skipped\n");
    }
    return false;
  }
  if (!batch->index_count) return true;
  if (SDL_RenderGeometry(g_renderer, NULL, batch->vertices,
                         batch->vertex_count, batch->indices,
                         batch->index_count))
    return true;
  DisableEffectGeometry("geometry submit");
  return false;
}

/* ── Action-stage presentation effects ────────────────────────────────── */

_Static_assert(kActionEffectObjPriorityCount ==
                   kDioramaObjectPriorityCount,
               "action effects and diorama must agree on OBJ bands");

static void DrawActionEffects(const FrameSlot *slot, SDL_Rect viewport,
                              const DioramaProjection *diorama_projection) {
  if (!slot || (!slot->action_effects.visible_count &&
                !slot->action_scene_effects.visible_count &&
                !slot->action_scene_effects.decoration_visible_count) ||
      (!slot->action_effect_lighting && !slot->action_effect_particles) ||
      !EffectRendererAvailable())
    return;

  ActionEffectProjectionContext projection = {
    .bg1_camera_x = slot->bg1_camera_x,
    .bg1_camera_y = slot->bg1_camera_y,
    .bg2_camera_x = slot->bg2_camera_x,
    .bg2_camera_y = slot->bg2_camera_y,
    .ws_extra = slot->ws_extra,
    .ws_extra_top = slot->ws_extra_top,
    .visible_x0 = slot->visible_x0,
    .visible_width = slot->visible_width,
    .snes_height = slot->snes_height,
    .diorama_projection = diorama_projection,
    .viewport = viewport,
  };
  ActionEffectRenderBatch geometry;
  ActionSceneEffectRenderBatch scene_geometry;
  geometry.vertex_count = geometry.index_count = 0;
  scene_geometry.vertex_count = scene_geometry.index_count = 0;
  if ((slot->action_effects.visible_count &&
       !ActionEffectRender_Build(
           &slot->action_effects, slot->action_effect_lighting,
           slot->action_effect_particles,
           ActionEffectProjection_ProjectPoint, &projection, &geometry)) ||
      (slot->action_scene_effects.visible_count &&
       !ActionSceneEffectRender_Build(
           &slot->action_scene_effects, slot->action_effect_lighting,
           slot->action_effect_particles,
           ActionEffectProjection_ProjectPoint, &projection,
           &scene_geometry)))
    return;
  const int actor_vertex_count = scene_geometry.vertex_count;
  const int actor_index_count = scene_geometry.index_count;

  EffectBatch spell_batch = {
    .vertices = geometry.vertices,
    .indices = geometry.indices,
    .vertex_count = geometry.vertex_count,
    .index_count = geometry.index_count,
    .vertex_capacity = kActionEffectRenderMaxVertices,
    .index_capacity = kActionEffectRenderMaxIndices,
  };
  EffectBatch scene_batch = {
    .vertices = scene_geometry.vertices,
    .indices = scene_geometry.indices,
    .vertex_count = scene_geometry.vertex_count,
    .index_count = scene_geometry.index_count,
    .vertex_capacity = kActionSceneEffectRenderMaxVertices,
    .index_capacity = kActionSceneEffectRenderMaxIndices,
  };
  EffectRenderState state;
  bool spell_submitted = true;
  bool scene_submitted = true;
  if (spell_batch.index_count || scene_batch.index_count) {
    if (!BeginEffectAdd(&state)) return;
    spell_submitted = SubmitEffectBatch(&spell_batch);
    scene_submitted = SubmitEffectBatch(&scene_batch);
    EndEffectBlend(&state);
  }

  /* Map-derived world decorations own a separate captured list and reuse the
   * same scratch batch after actor submission. This preserves the actor
   * budget without increasing the stack peak. BG2 decorations and bottom
   * atmosphere are submitted by their dedicated depth-ordered passes. */
  bool decoration_submitted = false;
  if (slot->action_scene_effects.decoration_visible_count &&
      ActionSceneDecorationRender_Build(
          &slot->action_scene_effects,
          kActionEffectRenderLayer_WorldOverlay,
          slot->action_effect_lighting, slot->action_effect_particles,
          ActionEffectProjection_ProjectPoint, &projection,
          &scene_geometry) && scene_geometry.index_count) {
    scene_batch.vertex_count = scene_geometry.vertex_count;
    scene_batch.index_count = scene_geometry.index_count;
    if (BeginEffectAdd(&state)) {
      decoration_submitted = SubmitEffectBatch(&scene_batch);
      EndEffectBlend(&state);
    }
  }
  /* One line, once per process: the whole path (WRAM identity -> capture ->
   * projection -> geometry submit) either produced pixels or it did not, and
   * a run's console.log should say which without anyone re-deriving it. The
   * silent version of this is what let a 16-bit read of the animation-bank
   * BYTE reject every spell with no visible symptom but "nothing happens". */
  static bool announced;
  if (!announced && geometry.index_count && spell_submitted) {
    announced = true;
    fprintf(stderr, "[action-fx] first spell geometry submitted: %u effect(s), "
            "%d vertices / %d indices (lighting=%d particles=%d)\n",
            slot->action_effects.visible_count, geometry.vertex_count,
            geometry.index_count, slot->action_effect_lighting,
            slot->action_effect_particles);
  }
  static bool announced_scene;
  if (!announced_scene && actor_index_count && scene_submitted) {
    announced_scene = true;
    fprintf(stderr,
            "[action-fx] first scene accent geometry submitted: %u effect(s), "
            "%d vertices / %d indices (lighting=%d particles=%d)\n",
            slot->action_scene_effects.visible_count,
            actor_vertex_count, actor_index_count,
            slot->action_effect_lighting, slot->action_effect_particles);
  }
  static bool announced_decorations;
  if (!announced_decorations && decoration_submitted) {
    announced_decorations = true;
    fprintf(stderr,
            "[action-fx] first map decoration geometry submitted\n");
  }
}

typedef struct ActionDioramaPlaneEffectContext {
  const FrameSlot *slot;
  SDL_Rect viewport;
} ActionDioramaPlaneEffectContext;

static void DrawActionDioramaPlaneEffect(
    void *userdata, int plane, const DioramaProjection *diorama_projection) {
  ActionDioramaPlaneEffectContext *context =
      (ActionDioramaPlaneEffectContext *)userdata;
  if (!context || !context->slot ||
      !context->slot->action_scene_effects.decoration_visible_count ||
      plane != kPpuOverlaySource_Bg2 || !diorama_projection ||
      !diorama_projection->bg2_plane.valid || !EffectRendererAvailable())
    return;
  const FrameSlot *slot = context->slot;
  ActionEffectProjectionContext projection = {
    .bg1_camera_x = slot->bg1_camera_x,
    .bg1_camera_y = slot->bg1_camera_y,
    .bg2_camera_x = slot->bg2_camera_x,
    .bg2_camera_y = slot->bg2_camera_y,
    .ws_extra = slot->ws_extra,
    .ws_extra_top = slot->ws_extra_top,
    .visible_x0 = slot->visible_x0,
    .visible_width = slot->visible_width,
    .snes_height = slot->snes_height,
    .diorama_projection = diorama_projection,
    .viewport = context->viewport,
  };
  ActionSceneEffectRenderBatch geometry;
  if (!ActionSceneDecorationRender_Build(
          &slot->action_scene_effects, kActionEffectRenderLayer_Bg2Plane,
          slot->action_effect_lighting, slot->action_effect_particles,
          ActionEffectProjection_ProjectPoint, &projection, &geometry) ||
      !geometry.index_count)
    return;
  EffectBatch batch = {
    .vertices = geometry.vertices,
    .indices = geometry.indices,
    .vertex_count = geometry.vertex_count,
    .index_count = geometry.index_count,
    .vertex_capacity = kActionSceneEffectRenderMaxVertices,
    .index_capacity = kActionSceneEffectRenderMaxIndices,
  };
  EffectRenderState state;
  if (!BeginEffectAdd(&state)) return;
  const bool submitted = SubmitEffectBatch(&batch);
  EndEffectBlend(&state);
  static bool announced;
  if (!announced && submitted) {
    announced = true;
    fprintf(stderr,
            "[action-fx] first BG2-local waterfall geometry submitted "
            "(Diorama)\n");
  }

  /* The finite-backdrop gap exists only in Diorama's vertical extension.
   * Submit its unmasked atmosphere from the same after-BG2 callback, before
   * later BG1 and OBJ planes, so source sprites remain in front. */
  if (!ActionSceneDecorationRender_Build(
          &slot->action_scene_effects,
          kActionEffectRenderLayer_Atmosphere,
          slot->action_effect_lighting, slot->action_effect_particles,
          ActionEffectProjection_ProjectPoint, &projection, &geometry) ||
      !geometry.index_count)
    return;
  batch.vertex_count = geometry.vertex_count;
  batch.index_count = geometry.index_count;
  /* Mist needs to obscure the finite BG2/skybox discontinuity, not merely
   * brighten both sides of it. Standard source-alpha blending lets the
   * staggered zero-alpha rims feather that boundary; the ordinary waterfall
   * veil and all luminous effects remain additive. */
  if (!BeginEffectAlpha(&state)) return;
  const bool atmosphere_submitted = SubmitEffectBatch(&batch);
  EndEffectBlend(&state);
  static bool announced_atmosphere;
  if (!announced_atmosphere && atmosphere_submitted) {
    announced_atmosphere = true;
    fprintf(stderr,
            "[action-fx] first waterfall bottom atmosphere submitted "
            "(Diorama)\n");
  }
}

static SDL_Texture *EnsureActionBg2EffectTarget(int w, int h) {
  if (!g_renderer || w <= 0 || h <= 0) return NULL;
  if (s_action_bg2_effect_target && s_action_bg2_effect_w == w &&
      s_action_bg2_effect_h == h)
    return s_action_bg2_effect_target;
  SDL_DestroyTexture(s_action_bg2_effect_target);
  s_action_bg2_effect_target = SDL_CreateTexture(
      g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
  s_action_bg2_effect_w = w;
  s_action_bg2_effect_h = h;
  if (s_action_bg2_effect_target) {
    SDL_SetTextureScaleMode(
        s_action_bg2_effect_target, SDL_SCALEMODE_NEAREST);
    SDL_BlendMode applied = SDL_BLENDMODE_INVALID;
    if (!SDL_SetTextureBlendMode(
            s_action_bg2_effect_target, SDL_BLENDMODE_ADD_PREMULTIPLIED) ||
        !SDL_GetTextureBlendMode(s_action_bg2_effect_target, &applied) ||
        applied != SDL_BLENDMODE_ADD_PREMULTIPLIED) {
      SDL_DestroyTexture(s_action_bg2_effect_target);
      s_action_bg2_effect_target = NULL;
      DisableActionBg2Effect("premultiplied blend verification");
    }
  }
  return s_action_bg2_effect_target;
}

static void DrawActionBg2EffectFlat(const FrameSlot *slot,
                                    SDL_Rect viewport) {
  if (!slot || !slot->action_bg2_mask_valid ||
      !slot->action_scene_effects.decoration_visible_count ||
      !s_action_bg2_mask_texture || !s_action_bg2_blend_supported ||
      !EffectRendererAvailable())
    return;
  ActionEffectProjectionContext projection = {
    .bg1_camera_x = slot->bg1_camera_x,
    .bg1_camera_y = slot->bg1_camera_y,
    .bg2_camera_x = slot->bg2_camera_x,
    .bg2_camera_y = slot->bg2_camera_y,
    .ws_extra = slot->ws_extra,
    .visible_x0 = slot->visible_x0,
    .visible_width = slot->visible_width,
    .snes_height = slot->snes_height,
    .viewport = viewport,
  };
  ActionSceneEffectRenderBatch geometry;
  if (!ActionSceneDecorationRender_Build(
          &slot->action_scene_effects, kActionEffectRenderLayer_Bg2Plane,
          slot->action_effect_lighting, slot->action_effect_particles,
          ActionEffectProjection_ProjectPoint, &projection, &geometry) ||
      !geometry.index_count)
    return;
  int output_w = 0, output_h = 0;
  if (!SDL_GetRenderOutputSize(g_renderer, &output_w, &output_h)) return;
  SDL_Texture *target = EnsureActionBg2EffectTarget(output_w, output_h);
  if (!target) return;

  SDL_Texture *saved_target = SDL_GetRenderTarget(g_renderer);
  SDL_BlendMode saved_blend = SDL_BLENDMODE_INVALID;
  Uint8 saved_r = 0, saved_g = 0, saved_b = 0, saved_a = 0;
  if (!SDL_GetRenderDrawBlendMode(g_renderer, &saved_blend) ||
      !SDL_GetRenderDrawColor(
          g_renderer, &saved_r, &saved_g, &saved_b, &saved_a)) {
    DisableActionBg2Effect("render-state capture");
    return;
  }
  if (!SDL_SetRenderTarget(g_renderer, target)) {
    DisableActionBg2Effect("effect-target bind");
    return;
  }
  bool target_ready =
      SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_NONE) &&
      SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 0) &&
      SDL_RenderClear(g_renderer);
  if (!target_ready)
    DisableActionBg2Effect("effect-target clear");
  EffectBatch batch = {
    .vertices = geometry.vertices,
    .indices = geometry.indices,
    .vertex_count = geometry.vertex_count,
    .index_count = geometry.index_count,
    .vertex_capacity = kActionSceneEffectRenderMaxVertices,
    .index_capacity = kActionSceneEffectRenderMaxIndices,
  };
  EffectRenderState state;
  bool submitted = false;
  bool masked = false;
  if (target_ready && BeginEffectAdd(&state)) {
    submitted = SubmitEffectBatch(&batch);
    EndEffectBlend(&state);
  }
  if (submitted && s_action_bg2_blend_supported) {
    SDL_BlendMode applied = SDL_BLENDMODE_INVALID;
    if (!SDL_SetTextureBlendMode(
            s_action_bg2_mask_texture, SDL_BLENDMODE_MUL) ||
        !SDL_GetTextureBlendMode(s_action_bg2_mask_texture, &applied) ||
        applied != SDL_BLENDMODE_MUL) {
      DisableActionBg2Effect("multiply blend verification");
    } else {
      const SDL_FRect src = {
        (float)slot->visible_x0, 0.0f,
        (float)slot->visible_width, (float)slot->snes_height,
      };
      const SDL_FRect dst = ToFRect(viewport);
      masked = SDL_RenderTexture(
          g_renderer, s_action_bg2_mask_texture, &src, &dst);
      if (!masked) DisableActionBg2Effect("winner-mask draw");
    }
  }
  const bool target_restored = SDL_SetRenderTarget(g_renderer, saved_target);
  const bool blend_restored =
      SDL_SetRenderDrawBlendMode(g_renderer, saved_blend);
  const bool color_restored = SDL_SetRenderDrawColor(
      g_renderer, saved_r, saved_g, saved_b, saved_a);
  if (!target_restored || !blend_restored || !color_restored) {
    DisableActionBg2Effect("render-state restore");
    return;
  }
  bool composited = false;
  if (masked && s_action_bg2_blend_supported) {
    composited = SDL_RenderTexture(g_renderer, target, NULL, NULL);
    if (!composited) DisableActionBg2Effect("masked-target composite");
  }
  static bool announced;
  if (!announced && composited) {
    announced = true;
    fprintf(stderr,
            "[action-fx] first BG2-local waterfall geometry submitted "
            "(flat, winner-masked)\n");
  }
}

/* ── Cheat visibility badge ────────────────────────────────────────────── */

/* An armed spell-cycle silently makes the game behave in a way no cartridge
 * can, which is exactly the state a screenshot or a bug report must not be
 * able to hide. Drawn last-but-one — above the game and the HUD, below the
 * settings overlay, in every presentation path — so it cannot be scrolled,
 * masked, or projected out of frame. */
static void PresentCheatBadge(const FrameSlot *slot, SDL_Rect viewport) {
  if (!slot || !slot->magic_cycle_armed) return;

  static const char *const kSpells[] = {
    "NONE", "FIRE", "STARDUST", "AURA", "LIGHT",
  };
  char text[64];
  uint8_t selected = slot->magic_cycle_selected;
  SDL_snprintf(text, sizeof(text), "CHEAT: SPELL CYCLE %s",
               selected <= 4 ? kSpells[selected] : "NONE");

  /* One glyph of inset from the viewport's top-left, at whatever scale keeps
   * the run legible on this output without ever exceeding the viewport. */
  int scale = viewport.h >= 720 ? 2 : 1;
  if (SettingsOverlay_GameTextWidth(text, scale) + 2 * kSettingsOverlayGlyphSize
      > viewport.w && scale > 1)
    scale = 1;
  int x = viewport.x + kSettingsOverlayGlyphSize;
  int y = viewport.y + kSettingsOverlayGlyphSize;
  SettingsOverlay_DrawGameText(x, y, scale, 255, text);
}

/* Developer authoring overlay for Settings > Layers > BG Extents. Segments
 * arrive in authentic-screen coordinates from the immutable plan in FrameSlot;
 * mapping them here keeps the pure row/guide model independent of SDL and keeps
 * present.c isolated from the live tuner singleton. BG1 is cyan, BG2 orange. */
static void PresentActionBgExtentGuides(const FrameSlot *slot,
                                        SDL_Rect viewport) {
  if (!slot || !slot->action_bg_extent_guides || viewport.w <= 0 ||
      viewport.h <= 0 || slot->visible_width <= 0)
    return;
  ActionBgTunerGuide guides[kActionBgTunerGuideMax];
  int count = ActionBgTuner_BuildGuides(
      &slot->action_bg_plan, guides, kActionBgTunerGuideMax);
  if (!count) return;

  SDL_BlendMode old_blend = SDL_BLENDMODE_NONE;
  Uint8 old_r = 0, old_g = 0, old_b = 0, old_a = 0;
  SDL_GetRenderDrawBlendMode(g_renderer, &old_blend);
  SDL_GetRenderDrawColor(g_renderer, &old_r, &old_g, &old_b, &old_a);
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
  const float scale_x = (float)viewport.w / (float)slot->visible_width;
  const float scale_y =
      (float)viewport.h / (float)kFrameSlotAuthenticHeight;
  const float authentic_x0 =
      ((float)slot->visible_width - (float)kFrameSlotAuthenticWidth) * 0.5f;
  for (int i = 0; i < count; i++) {
    const ActionBgTunerGuide *guide = &guides[i];
    if (guide->layer == 0)
      SDL_SetRenderDrawColor(g_renderer, 48, 220, 255, 220);
    else
      SDL_SetRenderDrawColor(g_renderer, 255, 96, 48, 220);
    float x0 = viewport.x + (authentic_x0 + guide->x0) * scale_x;
    float x1 = viewport.x + (authentic_x0 + guide->x1) * scale_x;
    float y0 = viewport.y + guide->y0 * scale_y;
    float y1 = viewport.y + guide->y1 * scale_y;
    SDL_RenderLine(g_renderer, x0, y0, x1, y1);
    /* One adjacent line remains legible over both bright and dark pixel art. */
    if (guide->x0 == guide->x1)
      SDL_RenderLine(g_renderer, x0 + 1.0f, y0, x1 + 1.0f, y1);
    else
      SDL_RenderLine(g_renderer, x0, y0 + 1.0f, x1, y1 + 1.0f);
  }
  SDL_SetRenderDrawColor(g_renderer, old_r, old_g, old_b, old_a);
  SDL_SetRenderDrawBlendMode(g_renderer, old_blend);
}

/* Terminal host UI is deliberately outside the CRT scene. One scoped physical
 * output coordinate space covers the inspector marker/panel, cheat disclosure,
 * manual and settings menu, then restores the backbuffer state for callers. */
void PresentHostUi(const FrameSlot *slot, SDL_Rect viewport) {
  if (!slot || !g_renderer) return;
  PresentationOutputState output_state;
  if (!PresentationGeometry_PushFullOutput(g_renderer, &output_state)) return;
  PresentActionBgExtentGuides(slot, viewport);
  PresentSceneInspector(slot, viewport);
  PresentCheatBadge(slot, viewport);
  SettingsOverlay_Render(viewport);
  PresentationGeometry_PopFullOutput(g_renderer, &output_state);
}

bool Present_SimRimMaskSupported(void) {
  return SDL_GetAtomicInt(&s_sim_rim_mask_supported) != 0;
}

/* Called from the SDL_EVENT_RENDER_TARGETS_RESET / _DEVICE_RESET arm and once
 * during orderly shutdown.
 *
 * SDL_events.h documents _DEVICE_RESET as "The device has been reset and all
 * textures need to be recreated". This includes the size-keyed render targets
 * above as well as resources written only when a game-side serial changes
 * (underlay/canvas) or exactly once at creation (cloud noise). None of those
 * cache keys has any dependence on GPU device state, so without this call the
 * caches short-circuit forever and keep handing back textures whose contents
 * the driver discarded. In a settled town the underlay serial can stay fixed
 * indefinitely, so the damage does not self-heal; only changing town would
 * clear it.
 *
 * The symptom is already documented for this exact texture class in
 * UploadSimTownCanvas below ("it showed as magenta"): freshly reallocated
 * STREAMING storage is uninitialized. Never reproducible on macOS/Metal, which
 * does not emit _DEVICE_RESET at all — this is a Windows-D3D and
 * Vulkan/SDL_GPU (Steam Deck) bug. */
void PresentRendererResources_Reset(void) {
  if (s_hud_composite_texture)
    SDL_DestroyTexture(s_hud_composite_texture);
  s_hud_composite_texture = NULL;
  s_hud_composite_w = s_hud_composite_h = 0;
  SDL_DestroyTexture(s_action_bg2_mask_texture);
  SDL_DestroyTexture(s_action_bg2_effect_target);
  s_action_bg2_mask_texture = NULL;
  s_action_bg2_effect_target = NULL;
  s_action_bg2_effect_w = s_action_bg2_effect_h = 0;
  s_action_bg2_blend_supported = true;
  SDL_SetAtomicInt(&s_effect_blend_supported, 1);
  SDL_SetAtomicInt(&s_effect_geometry_supported, 1);
  PresentSim3D_ResetResources();
}

void PresentCompositeScene(const FrameSlot *slot,
                           const DioramaScrollSnapshot *prev_scroll,
                           float alpha) {
  if (!g_renderer || !g_texture) return;

  /* The action map group becomes live while the world-to-action transition
   * is still holding the SNES in hardware forced blank. That makes Diorama's
   * host-side gate true before the first action frame is actually visible.
   * Unlike the ordinary PPU scanout, Diorama does not pass through INIDISP:
   * its navy clear, shoebox, skybox, HUD, and host overlays would therefore
   * leak through an otherwise fully blank transition (the gf=976 snapshot is
   * the captured example). Treat forced blank as the master output gate it is
   * on hardware and return before drawing any host-owned layer or overlay. */
  if (slot->diorama_active && (slot->inidisp & 0x80)) {
    SDL_SetRenderTarget(g_renderer, CrtPost_BaseTarget());
    SDL_SetRenderLogicalPresentation(g_renderer, 0, 0,
                                     SDL_LOGICAL_PRESENTATION_DISABLED);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    return;
  }

  if (slot->sim.view == kSimView_Enhanced && slot->sim.separated_valid) {
    PresentSim3D(slot);
    return;
  }
  if (slot->sim.view == kSimView_WorldNavigation &&
      PresentWorldNavigation3D(slot))
    return;

  if (slot->diorama_active) {
    uint8_t *pixels[kDioramaPlane_Count];
    memcpy(pixels, g_diorama_layer_pixels, sizeof(pixels));
    pixels[kDioramaPlane_Backdrop] = g_pixels;
    /* PresentUpload recorded exactly which requested/content-bearing surfaces
     * uploaded successfully before releasing their producer. A NULL entry is
     * already Diorama_Composite's established "plane absent" contract, and
     * also prevents stale texture contents from resurfacing after an empty
     * priority band or failed upload. */
    for (int plane = 0; plane < kDioramaPlane_Count; plane++)
      if (!(s_diorama_uploaded_plane_mask & (1u << plane)))
        pixels[plane] = NULL;
    /* M7 interpolation (kSettingCat_Graphics "Scroll interpolation" row) is
     * OFF by default. Observed cause of a real bug: ActRaiser's BG2
     * parallax layer in action stages appears to be HDMA-driven
     * (per-scanline register rewrites), so the single end-of-frame
     * hScroll[1]/vScroll[1] value this snapshots is not a stable "camera
     * position" — it's whatever the last HDMA write left behind, which can
     * differ frame-to-frame with no real camera motion at all.
     * Interpolating between two such snapshots produces a visible
     * whole-layer "vibration" on exactly that layer (confirmed by hand: BG2
     * parallax jitters while genuinely static). §6.4 anticipated HDMA
     * scroll as a smoothness limitation but not this failure mode. Needs
     * either (a) detecting HDMA-driven BGs and excluding them from
     * interpolation, or (b) a different scroll source than the raw register
     * snapshot, before re-enabling by default.
     *
     * Read from the FrameSlot (D6 — present.c must not read g_settings
     * live), snapshotted by FrameSlot_Capture on the game thread. */
    DioramaScrollDelta scroll_delta = slot->interp_setting_enabled
        ? ComputeDioramaScrollDelta(slot, prev_scroll, alpha)
        : (DioramaScrollDelta){0};
    /* B4-split (followup doc): resolve which authored pose is active this
     * frame. Free Cam: the live authored pose, unchanged from B4-split.
     * Dynamic Cam (B4-vellean): baseline + a small velocity-driven lean —
     * yaw toward horizontal run direction, pitch with vertical velocity —
     * scaled by reactive_strength/kPercentScale (0 disables sway, reproducing
     * B4-baseline's "snaps to the fixed pose" test). */
    bool dynamic = slot->diorama_camera_mode == kDioramaCam_Dynamic;
    DioramaCameraPose target;
    if (dynamic) {
      float gain =
          (float)slot->diorama_reactive_strength / (float)kPercentScale;
      target = slot->diorama_dyncam_baseline;
      target.tilt_y += kDioramaLeanYaw * gain * slot->diorama_dyncam_lean_yaw;
      target.tilt_x += kDioramaLeanPitch * gain * slot->diorama_dyncam_lean_pitch;
    } else {
      target = slot->diorama_free_pose;
    }

    /* B4-damp: Free Cam stays a direct snap (manual orbit must feel
     * immediate, and this preserves B4-split's byte-identical regression
     * test). Dynamic Cam eases toward the target with a wall-clock
     * exponential — NOT a fixed per-frame factor, since B1a makes the
     * present rate monitor-dependent and a fixed factor would be twice as
     * stiff at 120Hz as at 60Hz. The one exception: the frame a mode change
     * lands on (or the very first composited frame) snaps immediately —
     * that's what makes switching TO Dynamic Cam snap straight to the
     * baseline pose (already verified in B4-baseline) rather than easing in
     * from wherever Free Cam was left. */
    bool mode_changed = g_diorama_render_cam_mode != slot->diorama_camera_mode;
    g_diorama_render_cam_mode = slot->diorama_camera_mode;
    uint64_t now_ns = SDL_GetTicksNS();
    float dt = 0.0f;
    if (g_diorama_render_cam_last_ns != 0) {
      dt = (float)(now_ns - g_diorama_render_cam_last_ns) / 1e9f;
      if (dt < 0.0f) dt = 0.0f;
      if (dt > 1.0f) dt = 1.0f;   /* sanity clamp (e.g. resuming after a pause) */
    }
    if (!dynamic || mode_changed || g_diorama_render_cam_last_ns == 0) {
      g_diorama_render_cam = target;
    } else {
      float alpha = 1.0f - expf(-dt / kDioramaDampTau);
      g_diorama_render_cam.tilt_x +=
          (target.tilt_x - g_diorama_render_cam.tilt_x) * alpha;
      g_diorama_render_cam.tilt_y +=
          (target.tilt_y - g_diorama_render_cam.tilt_y) * alpha;
      g_diorama_render_cam.distance +=
          (target.distance - g_diorama_render_cam.distance) * alpha;
    }
    g_diorama_render_cam_last_ns = now_ns;

    /* B4-kick: trigger a fresh impulse only on a genuinely NEW FrameSlot
     * capture (not a re-presentation of one already processed—see
     * the FrameSlot field comment, present.h), and only in Dynamic Cam
     * (event kicks are part of the reactive system, same scoping as
     * vellean/pan). Impulses stack additively (a hit while already mid-jolt
     * gets stronger, not replaced) so back-to-back events still read. Decay
     * runs every present frame regardless, on the same wall-clock exponential
     * basis as the position damping above. */
    bool new_slot = dynamic && slot->timestamp_ns != g_diorama_last_slot_ns;
    g_diorama_last_slot_ns = slot->timestamp_ns;
    if (new_slot) {
      float gain =
          (float)slot->diorama_reactive_strength / (float)kPercentScale;
      if (slot->diorama_dyncam_event_hit || slot->diorama_dyncam_event_land)
        g_diorama_kick_pitch += kDioramaKickPitch * gain;
      /* Hit gets the zoom-punch too (see the section comment above) — a
       * discrete, reliable edge, unlike PlayerBoost. */
      if (slot->diorama_dyncam_event_hit)
        g_diorama_kick_zoom += kDioramaKickZoom * gain;
      /* DISABLED (2026-07-21, live report): PlayerBoost ($08C4) fired
       * constantly while just holding a direction — it isn't a clean
       * "boost activated" edge the way the invuln bit is for hits; more
       * likely a counter/cycling value that's nonzero (or repeatedly
       * revisits zero) during ordinary movement, not a discrete ability
       * trigger. slot->diorama_dyncam_event_boost is still captured
       * (FrameSlot/AR_DYNCAM_LOG's evt(boost=...) field) for whenever this
       * gets revisited with real investigation into what the byte means. */
    }
    if (!dynamic) {
      g_diorama_kick_pitch = 0.0f;
      g_diorama_kick_zoom = 0.0f;
    } else if (dt > 0.0f) {
      float kick_decay = expf(-dt / kDioramaKickTau);
      g_diorama_kick_pitch *= kick_decay;
      g_diorama_kick_zoom *= kick_decay;
    }
    DioramaCameraPose final_cam = g_diorama_render_cam;
    float distance_scale = 1.0f;
    if (dynamic) {
      final_cam.tilt_x += g_diorama_kick_pitch +
          slot->diorama_manual_orbit_pitch;
      final_cam.tilt_y += slot->diorama_manual_orbit_yaw;
      distance_scale = 1.0f + g_diorama_kick_zoom;
    }

    /* AR_DYNCAM_LOG=1: diagnose "no visible sway" reports — prints the raw
     * self-calibrated lean signal, the gain, the computed target, and the
     * actual (possibly still-damping) render camera every present, same
     * pattern as AR_INTERP_LOG above. */
    static int dyncam_log_on = -1;
    if (dyncam_log_on < 0) {
      const char *e = getenv("AR_DYNCAM_LOG");
      dyncam_log_on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (dyncam_log_on && dynamic) {
      fprintf(stderr,
        "[dyncam] mode=%d gain=%.3f lean_yaw=%.3f lean_pitch=%.3f "
        "target(x=%.4f y=%.4f d=%.3f) render(x=%.4f y=%.4f d=%.3f) "
        "kick(pitch=%.4f zoom=%.4f) evt(hit=%d land=%d boost=%d)\n",
        slot->diorama_camera_mode,
        (float)slot->diorama_reactive_strength / (float)kPercentScale,
        slot->diorama_dyncam_lean_yaw, slot->diorama_dyncam_lean_pitch,
        target.tilt_x, target.tilt_y, target.distance,
        g_diorama_render_cam.tilt_x, g_diorama_render_cam.tilt_y,
        g_diorama_render_cam.distance,
        g_diorama_kick_pitch, g_diorama_kick_zoom,
        slot->diorama_dyncam_event_hit, slot->diorama_dyncam_event_land,
        slot->diorama_dyncam_event_boost);
    }

    /* Fix B/BH6: resolve BG2's row-banded valid capture spans from the slot
     * alone (D6 — this file never reads live g_ppu). ws_extra, not
     * extra_left_right, is the offset: the capture pitch and Diorama_Upload's
     * rect are both derived from ws_extra, so it is what texture column 0
     * corresponds to. Keep the two concepts distinct even when their values are
     * equal, so either can change without altering the other's meaning. */
    DioramaBgValidSpanPlan bg2_valid_spans;
    /* + obj_apron: each span is in SURFACE columns, and screen x = 0 sits at
     * column obj_apron + ws_extra now that the surfaces carry resolve headroom
     * on both sides. Without it the skybox would crop its sky an apron early. */
    DioramaBgValidSpanPlan_Build(
        slot->ws_extra + slot->obj_apron,
        slot->extra_left_right,
        slot->extra_left_cur, slot->extra_right_cur,
        slot->bg_capture_pad_to_budget,
        &slot->action_bg_plan.layer[kActionBgPlanLayerCount - 1],
        slot->ws_extra_top,
        slot->snes_height + slot->ws_extra_top + slot->ws_extra_bottom,
        kFrameSlotLayerTextureWidth, &bg2_valid_spans);
    SDL_Rect viewport = ComputePresentationViewport(
        g_renderer, slot->ignore_aspect_ratio, slot->pixel_aspect,
        slot->visible_width, slot->snes_height);
    DioramaProjection action_projection;
    ActionDioramaPlaneEffectContext plane_effect = {slot, viewport};
    const uint8_t required_effect_obj_priorities =
        (slot->action_effect_lighting || slot->action_effect_particles)
            ? ActionEffectProjection_RequiredObjPriorityMask(
                  &slot->action_effects, &slot->action_scene_effects)
            : 0;
    const uint8_t effect_obj_priority_mask =
        Diorama_FilterObjEffectProjectionMask(
            required_effect_obj_priorities,
            slot->diorama_plane_request_mask,
            slot->diorama_plane_content_mask,
            s_diorama_uploaded_plane_mask);
    if (!Diorama_Composite(g_renderer, slot->snes_width,
                           slot->snes_height + slot->ws_extra_top +
                               slot->ws_extra_bottom,
                           slot->ws_extra_top,
                           slot->obj_apron,
                           slot->pixel_aspect, slot->ignore_aspect_ratio,
                           slot->visible_width, viewport,
                           g_diorama_textures, pixels,
                           &scroll_delta, &final_cam, distance_scale,
                           slot->diorama_plane_additive_mask &
                               s_diorama_uploaded_plane_mask,
                           effect_obj_priority_mask,
                           slot->diorama_map_group,
                           slot->diorama_map_number,
                           slot->diorama_layer_section,
                           &bg2_valid_spans,
                           DrawActionDioramaPlaneEffect, &plane_effect,
                           &action_projection))
      return;
    DrawActionEffects(slot, viewport, &action_projection);
    /* Flat HUD mode leaves BG3 in the same RemoveFromGame capture used by flat
     * presentation. Reconstruct its split pieces into one texture before
     * drawing the screen-space overlay; drawing them directly creates seams
     * (see PresentHudOverlayComposited).
     *
     * With diorama_hud_flat off, capture rebinds BG3 into the diorama layer
     * buffer so it renders as the ordinary tilted BG3 plane in
     * Diorama_Composite's own
     * per-layer loop above — skip the anchored overlay entirely here so the
     * two don't both draw a HUD. */
    if (slot->diorama_hud_flat)
      PresentHudOverlayComposited(slot, viewport);
    return;
  }

  ApplyLogicalPresentation(slot);
  SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
  SDL_RenderClear(g_renderer);
  SDL_Rect src = { slot->visible_x0, 0, slot->visible_width, slot->snes_height };
  SDL_FRect src_f = ToFRect(src);
  SDL_RenderTexture(g_renderer, g_texture, &src_f, NULL);

  SDL_Rect viewport = ComputePresentationViewport(
      g_renderer, slot->ignore_aspect_ratio,
      slot->pixel_aspect, slot->visible_width, slot->snes_height);
  SDL_SetRenderLogicalPresentation(g_renderer, 0, 0,
                                   SDL_LOGICAL_PRESENTATION_DISABLED);
  PresentMode7Composite(slot, viewport);
  DrawActionBg2EffectFlat(slot, viewport);
  DrawActionEffects(slot, viewport, NULL);
  PresentHudOverlay(slot, viewport);
  PresentHdReplacements(slot, viewport);
  ApplyLogicalPresentation(slot);
}
