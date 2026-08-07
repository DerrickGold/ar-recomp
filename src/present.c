/* M5 (ar-recomp-threading-impl.md Appendix D6): present-time rendering,
 * physically isolated from main.c's live game state. This file must NOT
 * declare or extern g_ppu, g_settings, g_snes_width, g_ws_extra,
 * g_active_pixel_aspect, or call Settings_Visible*() — every present-time
 * decision comes from the `const FrameSlot *` handed in. A stray live read
 * of any of those becomes an undeclared-symbol compile error, which is the
 * point (D6) — it's cheaper than relying on discipline/grep.
 *
 * It's fine to extern the presentation *resources* below (renderer, window,
 * textures, the raw pixel buffers) — see the M5 plan's buffer-ownership
 * note: those are boot-created once and, thanks to the present-thread
 * handshake (real SDL_CreateThread, condition-var protocol, quiesce,
 * synchronous headless fallback), are exclusively read here between the
 * game thread's Upload release and its next RtlDrawPpuFrame call. That's a
 * different race class than g_ppu/g_settings, and it's closed by the
 * handshake, not by this file's isolation. */

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "action_obj_apron.h"
#include "present.h"
#include "action_effect_render.h"
#include "crt_post.h"
#include "types.h"
#include "diorama.h"
#include "diorama_skybox_uv.h"
#include "diorama_planes.h"
#include "diorama_scroll_math.h"
#include "hd_replacement_host.h"
#include "settings_overlay.h"
#include "scene_inspector.h"
#include "scene3d_math.h"
#include "render_capabilities.h"
#include "sim_render_atlas.h"
#include "sim_town_canvas.h"
#include "sim_world_map.h"
#include "sim_world_navigation_capture.h"
#include "sim3d.h"

/* kPixelAspect_Crt43 and kDioramaCam_Free/kDioramaCam_Dynamic are plain enum
 * constants (not live state) — fine to pull in just for those. */
#include "settings.h"

extern SDL_Renderer *g_renderer;
extern SDL_Texture *g_texture;
extern SDL_Texture *g_hud_bg_texture;
extern SDL_Texture *g_hud_obj_texture;
extern uint8_t g_pixels[];
extern uint8_t g_hud_bg_pixels[];
extern uint8_t g_hud_obj_pixels[];
extern SDL_Texture *g_diorama_textures[kDioramaPlane_Count];
extern uint8_t *g_diorama_layer_pixels[kDioramaPlane_Count];
extern SDL_Texture *g_sim_obj_atlas_texture;
static void UploadSimTownCanvas(void);

extern SDL_Texture *g_sim3d_layer_textures[kSim3DPlane_Count];
extern SDL_Texture *g_sim3d_flat_texture;

static SDL_Texture *s_world_navigation_palace_texture;
static SDL_Texture *s_world_navigation_ui_texture;
static bool s_world_navigation_composition_alloc_failed;
static bool s_world_navigation_composition_upload_valid;
static uint32_t s_diorama_uploaded_plane_mask;

static SDL_FRect ToFRect(SDL_Rect r) {
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

static SDL_Texture *EnsureWorldNavigationCompositionTexture(
    SDL_Texture **texture) {
  if (*texture || s_world_navigation_composition_alloc_failed)
    return *texture;
  *texture = SDL_CreateTexture(
      g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      kSimWorldNavigationCompositionWidth,
      kSimWorldNavigationCompositionHeight);
  if (!*texture) {
    s_world_navigation_composition_alloc_failed = true;
    fprintf(stderr,
            "[world-navigation] composition texture unavailable: %s\n",
            SDL_GetError());
    return NULL;
  }
  SDL_SetTextureBlendMode(*texture, SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(*texture, SDL_SCALEMODE_NEAREST);
  return *texture;
}

static void UploadWorldNavigationComposition(const FrameSlot *slot) {
  s_world_navigation_composition_upload_valid = false;
  if (!slot || slot->sim.view != kSimView_WorldNavigation) return;
  const SimWorldNavigationComposition *composition =
      &slot->sim.world_navigation_scene.composition;
  if (!composition->valid) return;
  if (composition->empty_animation) {
    s_world_navigation_composition_upload_valid = true;
    return;
  }

  SDL_Texture *palace = EnsureWorldNavigationCompositionTexture(
      &s_world_navigation_palace_texture);
  SDL_Texture *ui = EnsureWorldNavigationCompositionTexture(
      &s_world_navigation_ui_texture);
  if (!palace || !ui) return;
  SDL_Rect palace_rect = {
    0, 0, composition->palace.width, composition->palace.height,
  };
  SDL_Rect ui_rect = {
    0, 0, composition->ui.width, composition->ui.height,
  };
  if (!SDL_UpdateTexture(
          palace, &palace_rect, g_sim_world_navigation_palace_pixels,
          kSimWorldNavigationCompositionPitch) ||
      !SDL_UpdateTexture(
          ui, &ui_rect, g_sim_world_navigation_ui_pixels,
          kSimWorldNavigationCompositionPitch))
    return;
  s_world_navigation_composition_upload_valid = true;
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

/* One geometry description drives both compositing and hit-testing (D4):
 * pure, no globals — the present thread feeds it from a FrameSlot, and
 * DevTools_InspectWindowPoint feeds it from live state. Ported verbatim from
 * the pre-M5 main.c version, just reading `inputs` instead of g_ppu/g_settings/
 * g_snes_width/g_snes_height directly. */
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
    scale_y = in->hud_scale_percent / 100.0;
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

  /* Action's selected-magic icon (4 OAM, 16x16), simulation's hourglass
   * (4 OAM, 16x16), and Sky Palace's magic icon (2 OAM, 16x8) are separately
   * validated OAM signatures; the caller resolves the icon x/y (from live
   * oam/highOam or the FrameSlot snapshot) and passes it in already
   * resolved, so this function stays free of oam[]/highOam[] entirely. */
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

SDL_Rect ComputePresentationViewport(SDL_Renderer *renderer, bool ws_active,
                                     bool ignore_aspect_ratio,
                                     int pixel_aspect, int visible_width,
                                     int snes_height) {
  int out_w = 0, out_h = 0;
  SDL_GetRenderOutputSize(renderer, &out_w, &out_h);
  SDL_Rect viewport = { 0, 0, out_w, out_h };
  if (!ws_active || ignore_aspect_ratio || out_w <= 0 || out_h <= 0)
    return viewport;

  int logical_w = visible_width * (pixel_aspect == kPixelAspect_Crt43 ? 7 : 1);
  int logical_h = snes_height * (pixel_aspect == kPixelAspect_Crt43 ? 6 : 1);
  if ((int64_t)out_w * logical_h > (int64_t)out_h * logical_w) {
    viewport.w = (int)((int64_t)out_h * logical_w / logical_h);
    viewport.x = (out_w - viewport.w) / 2;
  } else {
    viewport.h = (int)((int64_t)out_w * logical_h / logical_w);
    viewport.y = (out_h - viewport.h) / 2;
  }
  return viewport;
}

static void ApplyLogicalPresentation(const FrameSlot *slot) {
  if (!g_renderer) return;
  if (slot->ws_active && !slot->ignore_aspect_ratio) {
    if (slot->pixel_aspect == kPixelAspect_Crt43)
      SDL_SetRenderLogicalPresentation(g_renderer, slot->visible_width * 7,
                                       slot->snes_height * 6,
                                       SDL_LOGICAL_PRESENTATION_LETTERBOX);
    else
      SDL_SetRenderLogicalPresentation(g_renderer, slot->visible_width,
                                       slot->snes_height,
                                       SDL_LOGICAL_PRESENTATION_LETTERBOX);
  } else {
    SDL_SetRenderLogicalPresentation(g_renderer, 0, 0,
                                     SDL_LOGICAL_PRESENTATION_DISABLED);
  }
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
   * The 4-slot count still gates the 16x16 chunk below — Sky Palace's 2-slot
   * 16x8 icon is handled by sim3d's own anchored overlay, not here. */
  if (slot->oam_valid && slot->hud_icon_count == 4) {
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
 * into the diorama branch (multiple chunk blits directly against the
 * diorama's raw full-output viewport) produced visible seams between the
 * ACT/TIME/SCORE bands. Root cause: BuildHudPresentationChunks derives
 * scale_x/scale_y from the viewport it's handed, and the flat branch always
 * hands it the aspect-locked viewport from ComputePresentationViewport
 * (letterboxed to preserve pixel_aspect); the diorama branch instead hands
 * it the UNCORRECTED full output rect (diorama.c's 3D scene intentionally
 * fills edge-to-edge, ignoring pixel_aspect), so scale_x and scale_y came
 * out mismatched and the bands didn't line up.
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

static void PresentHudOverlayComposited(const FrameSlot *slot,
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
  int arm = viewport.h > 0 ? (viewport.h * 7 + 112) / 224 : 7;
  if (arm < 7) arm = 7;
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
    /* The captured surfaces are taller than the authentic frame by exactly the
     * vertical margin, and their row 0 IS the top of that band -- so the upload
     * covers snes_height + ws_extra_top rows starting at row 0. */
    s_diorama_uploaded_plane_mask = Diorama_Upload(
        g_diorama_textures, pixels, slot->snes_width + slot->obj_apron * 2,
        slot->snes_height + slot->ws_extra_top, slot->obj_apron, upload_mask);
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

  /* A7 (followup doc): this used to sit behind the diorama branch's early
   * `return` above, so g_hud_bg_texture/g_hud_obj_texture were NEVER
   * refreshed in diorama mode — PresentHudOverlayComposited was sampling
   * whatever stale texture content happened to be left over from the last
   * flat-mode frame (or uninitialized memory if the game booted straight
   * into diorama), read with THIS frame's hud_split geometry. That mismatch
   * between stale pixels and fresh geometry is what produced the garbled/
   * misaligned HUD text. Needed in both branches now that diorama mode
   * anchors its HUD through the same g_hud_bg_texture flat mode uses. */
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
    for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
      if (g_sim3d_layer_textures[plane] && g_sim3d_layer_pixels[plane])
        SDL_UpdateTexture(g_sim3d_layer_textures[plane], &frame,
                          g_sim3d_layer_pixels[plane], slot->snes_width * 4);
    }
    if (g_sim3d_flat_texture)
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

/* B4-split (followup doc): the present-thread-owned "effective render
 * camera" — Free Cam: the authored/persisted pose (snapshotted through
 * FrameSlot every frame, so Free Cam behavior/output is unchanged). Dynamic
 * Cam: baseline pose today (direct snap; B4-vellean/B4-damp add sway +
 * easing on top in a later checkpoint). A plain file-scope static, not
 * thread-local: PresentComposite (like every other present.c function) is
 * only ever called from the present thread, the same reasoning that already
 * covers s_hud_composite_texture above. Diorama_Composite's camera parameter
 * comes from here in Dynamic mode, from the slot's authored pose in Free
 * mode — g_diorama_cam (diorama.c, game-thread-owned) is never read here. */
static DioramaCameraPose g_diorama_render_cam;
static int g_diorama_render_cam_mode = -1;    /* -1: no frame composited yet */
static uint64_t g_diorama_render_cam_last_ns;

/* B4-vellean/B4-damp (followup doc) provisional constants — literals from
 * the doc, not yet author-tuned (tuning is an explicit follow-up pass). */
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

/* D5a cull fade. One boundary drives two independent ground treatments:
 * `fade` hands town ground over to the underlay, while `dim` multiplies the
 * surviving colour toward black. Every ground draw that can cover the town
 * canvas must use this same description, or a later opaque draw will restore
 * the square captured-texture boundary over the rounded fade. */
typedef struct SimCullFade {
  int lead;
  int corner;
  int lift_inset;
  float fade;
  float dim;
  /* The full-resolution town map is finite even when the projected ground is
   * not. Feather its own extent into the half-resolution world underlay so
   * the last canvas texel cannot form a hard vertical/horizontal seam. */
  float extent_x0, extent_y0, extent_x1, extent_y1;
  float extent_feather;
  int margin_left, margin_right;
  int screen_x0;
} SimCullFade;

/* Cull proximity at a captured-texture point, 0..1. The conversion back to
 * the emitter's biased coordinates keeps the visual boundary identical to the
 * cull predicate instead of maintaining a second approximation of it. */
static float SimCullProximityAt(const SimCullFade *fade, float texture_x,
                                float texture_y, SDL_Rect source) {
  if (!fade) return 0.0f;
  int16_t biased_x = (int16_t)(texture_x - (float)fade->screen_x0 + 16.0f);
  int16_t biased_y = (int16_t)(texture_y - (float)source.y + 17.0f);
  return Sim3D_CullProximity(biased_x, biased_y, fade->margin_left,
                             fade->margin_right, fade->lead,
                             fade->corner, fade->lift_inset);
}

/* Opacity of full-resolution town ground at the finite town-map extent.
 * Smoothstep has zero slope at both ends, avoiding a second crease where the
 * feather reaches full opacity. A zero feather disables this independent
 * mask (as it must for the world-map underlay itself). */
static float SimGroundExtentAlphaAt(const SimCullFade *fade, float texture_x,
                                    float texture_y) {
  if (!fade || fade->extent_feather <= 0.0f) return 1.0f;
  float edge_x = fminf(texture_x - fade->extent_x0,
                       fade->extent_x1 - texture_x);
  float edge_y = fminf(texture_y - fade->extent_y0,
                       fade->extent_y1 - texture_y);
  float edge = edge_x < edge_y ? edge_x : edge_y;
  float ramp = edge / fade->extent_feather;
  if (ramp <= 0.0f) return 0.0f;
  if (ramp >= 1.0f) return 1.0f;
  return ramp * ramp * (3.0f - 2.0f * ramp);
}

/* Inserts a texture-space split into a sorted mesh axis. Coordinates at or
 * outside the existing extent do not create a new segment. */
static int InsertSimGroundCoordinate(float *coordinates, int count,
                                     int capacity, float coordinate) {
  if (count <= 0 || count >= capacity ||
      coordinate <= coordinates[0] ||
      coordinate >= coordinates[count - 1])
    return count;
  int index = 1;
  while (index < count && coordinates[index] < coordinate) index++;
  if (fabsf(coordinates[index] - coordinate) < 0.0001f) return count;
  memmove(&coordinates[index + 1], &coordinates[index],
          (size_t)(count - index) * sizeof(coordinates[0]));
  coordinates[index] = coordinate;
  return count + 1;
}

enum {
  /* The cull tint is sampled at these vertices. The old 8x6 projection mesh
   * was adequate for affine UVs but too coarse for a 96px rounded corner: its
   * interpolation would turn the newly shared tint back into a visibly
   * polygonal box. Match the extension mesh density so both layers resolve
   * one boundary. */
  kSimGroundColumns = 64,
  kSimGroundRows = 48,
  kSimGroundVertexCount = (kSimGroundColumns + 1) * (kSimGroundRows + 1),
  kSimGroundIndexCount = kSimGroundColumns * kSimGroundRows * 6,
};

static void DrawSimGroundPlane(SDL_Texture *texture, SDL_Rect source,
                               SDL_Rect viewport,
                               const float matrix[16],
                               const SimCullFade *fade) {
  if (!texture || source.w <= 0 || source.h <= 0 ||
      viewport.w <= 0 || viewport.h <= 0)
    return;

  float aspect = (float)viewport.w / (float)viewport.h;
  /* Reused on the present thread; together these are too large for a routine
   * stack allocation at the density the rounded boundary requires. */
  static SDL_Vertex vertices[kSimGroundVertexCount];
  static int indices[kSimGroundIndexCount];
  int vertex_count = 0, index_count = 0;

  for (int row = 0; row <= kSimGroundRows; row++) {
    float fy = (float)row / (float)kSimGroundRows;
    for (int column = 0; column <= kSimGroundColumns; column++) {
      float fx = (float)column / (float)kSimGroundColumns;
      float texture_x = source.x + fx * source.w;
      float texture_y = source.y + fy * source.h;
      Scene3DPoint projected;
      if (!Scene3D_ProjectWorldPoint(
              matrix, (fx - 0.5f) * aspect, 0.5f - fy, 0.0f,
              viewport.w, viewport.h, &projected))
        return;
      float away = SimCullProximityAt(fade, texture_x, texture_y, source);
      float bright = fade ? 1.0f - away * fade->dim : 1.0f;
      float extent_alpha =
          SimGroundExtentAlphaAt(fade, texture_x, texture_y);
      SDL_FColor tint = {
        bright, bright, bright,
        (fade ? 1.0f - away * fade->fade : 1.0f) * extent_alpha,
      };
      vertices[vertex_count++] = (SDL_Vertex){
        { viewport.x + projected.x, viewport.y + projected.y }, tint,
        { texture_x / (float)kSim3DMaxWidth,
          texture_y / (float)kSim3DMaxHeight },
      };
    }
  }
  for (int row = 0; row < kSimGroundRows; row++) {
    for (int column = 0; column < kSimGroundColumns; column++) {
      int top_left = row * (kSimGroundColumns + 1) + column;
      int bottom_left = top_left + kSimGroundColumns + 1;
      indices[index_count++] = top_left;
      indices[index_count++] = top_left + 1;
      indices[index_count++] = bottom_left + 1;
      indices[index_count++] = top_left;
      indices[index_count++] = bottom_left + 1;
      indices[index_count++] = bottom_left;
    }
  }
  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  SDL_RenderGeometry(g_renderer, texture, vertices, vertex_count,
                     indices, index_count);
}

/* The ground quad spans exactly one world unit vertically over `source.h`
 * captured rows, so a D3c virtual height in authentic SNES pixels converts to
 * world units with the same scale the flat view uses for sprite size. Lifting
 * along +Z leaves the ground anchor (z = 0) available to the D4 shadow pass. */
static float SimHeightWorldUnits(SDL_Rect source, int virtual_height,
                                 unsigned height_scale_x100) {
  if (source.h <= 0 || !virtual_height) return 0.0f;
  return (float)virtual_height * (float)height_scale_x100 /
      (100.0f * (float)source.h);
}

static bool ProjectSimTexturePoint(
    const float matrix[16], SDL_Rect source, SDL_Rect viewport,
    float texture_x, float texture_y, float height_world,
    Scene3DPoint *out_point) {
  float fx = (texture_x - source.x) / source.w;
  float fy = (texture_y - source.y) / source.h;
  float aspect = (float)viewport.w / (float)viewport.h;
  Scene3DPoint point;
  if (!Scene3D_ProjectWorldPoint(
          matrix, (fx - 0.5f) * aspect, 0.5f - fy, height_world,
          viewport.w, viewport.h, &point))
    return false;
  point.x += viewport.x;
  point.y += viewport.y;
  *out_point = point;
  return true;
}

static float SimTexturePointDepthScale(
    const float matrix[16], SDL_Rect source, SDL_Rect viewport,
    float texture_x, float texture_y, float height_world,
    float reference_depth) {
  float fx = (texture_x - source.x) / source.w;
  float fy = (texture_y - source.y) / source.h;
  float aspect = (float)viewport.w / (float)viewport.h;
  return Scene3D_ProjectBillboardScale(
      matrix, (fx - 0.5f) * aspect, 0.5f - fy, height_world,
      reference_depth);
}

/* Host effects use the renderer abstraction's standard additive blend and
 * untextured geometry, not a backend shader. That is a portable API path,
 * but not a promise of pixel-identical rasterization across Metal, Vulkan,
 * Direct3D and software. Capability is verified at the point of use: SDL may
 * legally substitute the closest blend mode, so a successful set is followed
 * by a get-and-compare. Any rejection or substitution fails the stages closed. */
static SDL_AtomicInt s_effect_add_supported = { .value = 1 };
static SDL_AtomicInt s_effect_geometry_supported = { .value = 1 };

typedef struct EffectRenderState {
  SDL_BlendMode blend;
  Uint8 r, g, b, a;
} EffectRenderState;

static void DisableEffectAdd(const char *operation) {
  if (!SDL_CompareAndSwapAtomicInt(
          &s_effect_add_supported, 1, 0))
    return;
  fprintf(stderr,
          "[host-effects] additive pass unavailable at %s (%s) — "
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

static bool EffectRendererAvailable(void) {
  return SDL_GetAtomicInt(&s_effect_add_supported) != 0 &&
      SDL_GetAtomicInt(&s_effect_geometry_supported) != 0;
}

bool Present_EffectRendererSupported(void) {
  return EffectRendererAvailable();
}

static bool BeginEffectAdd(EffectRenderState *state) {
  if (!state || !EffectRendererAvailable()) return false;
  if (!SDL_GetRenderDrawBlendMode(g_renderer, &state->blend) ||
      !SDL_GetRenderDrawColor(g_renderer, &state->r, &state->g,
                              &state->b, &state->a)) {
    DisableEffectAdd("state capture");
    return false;
  }
  if (!SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_ADD)) {
    SDL_SetRenderDrawBlendMode(g_renderer, state->blend);
    DisableEffectAdd("blend set");
    return false;
  }
  SDL_BlendMode applied = SDL_BLENDMODE_INVALID;
  if (!SDL_GetRenderDrawBlendMode(g_renderer, &applied) ||
      applied != SDL_BLENDMODE_ADD) {
    SDL_SetRenderDrawBlendMode(g_renderer, state->blend);
    DisableEffectAdd("blend verification");
    return false;
  }
  return true;
}

static void EndEffectAdd(const EffectRenderState *state) {
  if (!state) return;
  bool color_ok = SDL_SetRenderDrawColor(
      g_renderer, state->r, state->g, state->b, state->a);
  bool blend_ok = SDL_SetRenderDrawBlendMode(g_renderer, state->blend);
  if (!color_ok || !blend_ok) DisableEffectAdd("state restore");
}

static bool ProjectSimAnchorAndScale(
    const float matrix[16], SDL_Rect source, SDL_Rect viewport,
    float texture_x, float texture_y, float height_world,
    float reference_depth, Scene3DPoint *anchor,
    float *scale_x, float *scale_y) {
  if (!ProjectSimTexturePoint(matrix, source, viewport, texture_x, texture_y,
                              height_world, anchor))
    return false;
  float depth_scale = SimTexturePointDepthScale(
      matrix, source, viewport, texture_x, texture_y, height_world,
      reference_depth);
  if (depth_scale <= 0.0f) return false;
  if (scale_x) *scale_x = (float)viewport.w / source.w * depth_scale;
  if (scale_y) *scale_y = (float)viewport.h / source.h * depth_scale;
  return true;
}

static float SimBillboardHeightPop(SDL_Rect source, float height_world,
                                   unsigned height_pop_pct);

static bool ProjectSimEffectPoint(
    const FrameSlot *slot, const SimEffectInstance *effect,
    const SimEffectLocalPoint *local, SDL_Rect source, SDL_Rect viewport,
    const Scene3DCamera *camera, const float matrix[16],
    Scene3DPoint *point, float *scale_x, float *scale_y) {
  if (!local || effect->geometry.kind != kSimEffectGeometry_Point) return false;
  int record_screen_x = (int16_t)(uint16_t)(
      effect->world_x - slot->sim.camera_x);
  int record_screen_y = (int16_t)(uint16_t)(
      effect->world_y - slot->sim.camera_y);
  float texture_x = slot->ws_extra + record_screen_x;
  float texture_y = record_screen_y;
  float height_world = SimHeightWorldUnits(
      source, local->height, slot->sim.height_scale_x100);

  switch ((SimEffectGeometrySpace)effect->geometry.space) {
    case kSimEffectSpace_Screen:
      point->x = viewport.x + local->x * (float)viewport.w / source.w;
      point->y = viewport.y + local->y * (float)viewport.h / source.h;
      if (scale_x) *scale_x = (float)viewport.w / source.w;
      if (scale_y) *scale_y = (float)viewport.h / source.h;
      return true;
    case kSimEffectSpace_WorldLocal:
      texture_x += local->x;
      texture_y += local->y;
      return ProjectSimAnchorAndScale(
          matrix, source, viewport, texture_x, texture_y, height_world,
          Scene3D_AutoFitDistance(camera->fov_y), point, scale_x, scale_y);
    case kSimEffectSpace_RecordLocal:
      break;
    default:
      return false;
  }

  Scene3DPoint anchor;
  float sx, sy;
  if (!ProjectSimAnchorAndScale(
          matrix, source, viewport, texture_x, texture_y, height_world,
          Scene3D_AutoFitDistance(camera->fov_y), &anchor, &sx, &sy))
    return false;
  float height_pop = SimBillboardHeightPop(
      source, height_world, slot->sim.height_pop_pct);
  sx *= height_pop;
  sy *= height_pop;
  point->x = anchor.x + local->x * sx;
  point->y = anchor.y + local->y * sy;
  if (scale_x) *scale_x = sx;
  if (scale_y) *scale_y = sy;
  return true;
}

enum { kSimMaxParticlesPerEffect = 12 };

typedef enum SimEffectParticleMotion {
  kSimEffectParticle_Burst,
  kSimEffectParticle_Flame,
} SimEffectParticleMotion;

typedef struct SimEffectStyle {
  float strength;
  SDL_FColor glow_center;
  SDL_FColor glow_edge;
  SDL_FColor particle_color;
  SDL_FColor flash_color;
  float glow_radius_x;
  float glow_radius_y;
  /* Screen-upright visual offset from the semantic contact point. The
   * metadata keeps the exact gameplay/art attachment while tall sprites can
   * center their light within the visible body. */
  float glow_origin_lift;
  /* Screen-upright distance, in authentic billboard pixels, from the
   * semantic attachment/contact point to the particle source. Lighting keeps
   * its own independently styled origin. Keeping the two roles separate
   * prevents a grounded semantic contact from forcing embers to emerge
   * underneath tall source art. */
  float particle_origin_lift;
  uint8_t particle_count;
  uint8_t particle_motion;
  bool scene_flash;
} SimEffectStyle;

static SimEffectStyle SimLightningBaseStyle(float strength,
                                            float radius_x,
                                            float radius_y) {
  return (SimEffectStyle){
    .strength = strength,
    .glow_center = { 0.48f, 0.72f, 1.0f, 0.46f },
    .glow_edge = { 0.20f, 0.42f, 1.0f, 0.0f },
    .particle_color = { 0.62f, 0.82f, 1.0f, 1.0f },
    .flash_color = { 0.38f, 0.59f, 1.0f, 1.0f },
    .glow_radius_x = radius_x,
    .glow_radius_y = radius_y,
    .particle_count = kSimMaxParticlesPerEffect,
    .particle_motion = kSimEffectParticle_Burst,
    .scene_flash = true,
  };
}

static bool SimEffectStyleFor(const SimEffectInstance *effect,
                              SimEffectStyle *style) {
  if (!effect || !style) return false;
  SimEffectPhase phase = (SimEffectPhase)effect->phase;
  switch ((SimEffectKind)effect->kind) {
    case kSimEffect_LightningMiracle: {
      float strength = 1.0f;
      if (phase == kSimEffectPhase_LightningLead)
        strength = 210.0f / 255.0f;
      else if (phase == kSimEffectPhase_LightningBranch)
        strength = 235.0f / 255.0f;
      *style = SimLightningBaseStyle(strength, 30.0f, 12.0f);
      return true;
    }
    case kSimEffect_BlueDragonLightning: {
      float strength = phase == kSimEffectPhase_BlueDragonBoltA ? 0.82f :
          phase == kSimEffectPhase_BlueDragonBoltB ? 0.92f : 1.0f;
      *style = SimLightningBaseStyle(strength, 24.0f, 11.0f);
      return true;
    }
    case kSimEffect_TownCreationLightning: {
      float strength = phase == kSimEffectPhase_TownCreationBoltA ? 0.82f :
          phase == kSimEffectPhase_TownCreationBoltB ? 0.90f :
          phase == kSimEffectPhase_TownCreationBoltC ? 0.96f : 1.0f;
      *style = SimLightningBaseStyle(strength, 27.0f, 12.0f);
      return true;
    }
    case kSimEffect_RedDemonFire: {
      float strength = phase == kSimEffectPhase_RedFireSmall ? 0.62f :
          phase == kSimEffectPhase_RedFireMedium ? 0.82f : 1.0f;
      *style = (SimEffectStyle){
        .strength = strength,
        .glow_center = { 1.0f, 0.46f, 0.08f, 0.38f },
        .glow_edge = { 1.0f, 0.12f, 0.01f, 0.0f },
        .particle_color = { 1.0f, 0.60f, 0.16f, 1.0f },
        .glow_radius_x = 18.0f,
        .glow_radius_y = 12.0f,
        .particle_count = 8,
        .particle_motion = kSimEffectParticle_Flame,
      };
      return true;
    }
    case kSimEffect_GroundFire: {
      float strength = phase == kSimEffectPhase_GroundFireA ? 0.78f :
          phase == kSimEffectPhase_GroundFireB ? 0.90f : 1.0f;
      switch ((SimEffectColorFamily)effect->color_family) {
        case kSimEffectColor_FireRed:
          *style = (SimEffectStyle){
            .strength = strength,
            .glow_center = { 1.0f, 0.40f, 0.04f, 0.46f },
            .glow_edge = { 1.0f, 0.09f, 0.0f, 0.0f },
            .particle_color = { 1.0f, 0.66f, 0.12f, 1.0f },
            .glow_radius_x = 21.0f,
            .glow_radius_y = 13.0f,
            .particle_count = 10,
            .particle_motion = kSimEffectParticle_Flame,
          };
          return true;
        case kSimEffectColor_FireBlue:
          *style = (SimEffectStyle){
            .strength = strength,
            .glow_center = { 0.24f, 0.68f, 1.0f, 0.58f },
            .glow_edge = { 0.04f, 0.20f, 1.0f, 0.0f },
            .particle_color = { 0.68f, 0.94f, 1.0f, 1.0f },
            .glow_radius_x = 24.0f,
            .glow_radius_y = 15.0f,
            .particle_count = kSimMaxParticlesPerEffect,
            .particle_motion = kSimEffectParticle_Flame,
          };
          return true;
        case kSimEffectColor_None:
        case kSimEffectColor_LightningBlue:
          return false;
      }
      return false;
    }
    case kSimEffect_HouseFire: {
      float strength = phase == kSimEffectPhase_HouseFireA ? 0.78f :
          phase == kSimEffectPhase_HouseFireB ? 0.90f : 1.0f;
      *style = (SimEffectStyle){
        .strength = strength,
        .glow_center = { 1.0f, 0.42f, 0.05f, 0.48f },
        .glow_edge = { 1.0f, 0.08f, 0.0f, 0.0f },
        .particle_color = { 1.0f, 0.68f, 0.14f, 1.0f },
        .glow_radius_x = 22.0f,
        .glow_radius_y = 14.0f,
        /* Run 20260803-134746 shows the youngest diamonds straddling the
         * bottom of this 16px sprite when both use ground contact (8,16).
         * The old capture also places the radial light below the visible
         * body, so center both presentation origins at local y=12 while the
         * semantic contact remains (8,16). */
        .glow_origin_lift = 4.0f,
        .particle_origin_lift = 4.0f,
        .particle_count = 10,
        .particle_motion = kSimEffectParticle_Flame,
      };
      return true;
    }
    case kSimEffect_None:
      return false;
  }
  return false;
}

typedef struct EffectBatch {
  SDL_Vertex *vertices;
  int *indices;
  int vertex_count, index_count;
  int vertex_capacity, index_capacity;
  bool overflow;
} EffectBatch;

static bool EffectBatchReserve(EffectBatch *batch,
                               int vertices, int indices) {
  if (!batch || vertices < 0 || indices < 0 ||
      batch->vertex_count + vertices > batch->vertex_capacity ||
      batch->index_count + indices > batch->index_capacity) {
    if (batch) batch->overflow = true;
    return false;
  }
  return true;
}

static bool SubmitEffectBatch(EffectBatch *batch) {
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

static const float kEffectCircle32[32][2] = {
  { 1.0f, 0.0f }, { 0.980785f, 0.195090f },
  { 0.923880f, 0.382683f }, { 0.831470f, 0.555570f },
  { 0.707107f, 0.707107f }, { 0.555570f, 0.831470f },
  { 0.382683f, 0.923880f }, { 0.195090f, 0.980785f },
  { 0.0f, 1.0f }, { -0.195090f, 0.980785f },
  { -0.382683f, 0.923880f }, { -0.555570f, 0.831470f },
  { -0.707107f, 0.707107f }, { -0.831470f, 0.555570f },
  { -0.923880f, 0.382683f }, { -0.980785f, 0.195090f },
  { -1.0f, 0.0f }, { -0.980785f, -0.195090f },
  { -0.923880f, -0.382683f }, { -0.831470f, -0.555570f },
  { -0.707107f, -0.707107f }, { -0.555570f, -0.831470f },
  { -0.382683f, -0.923880f }, { -0.195090f, -0.980785f },
  { 0.0f, -1.0f }, { 0.195090f, -0.980785f },
  { 0.382683f, -0.923880f }, { 0.555570f, -0.831470f },
  { 0.707107f, -0.707107f }, { 0.831470f, -0.555570f },
  { 0.923880f, -0.382683f }, { 0.980785f, -0.195090f },
};

static bool AppendSimEffectGlow(
    EffectBatch *batch, const FrameSlot *slot,
    const SimEffectInstance *effect, const SimEffectStyle *style,
    SDL_Rect source, SDL_Rect viewport, const Scene3DCamera *camera,
    const float matrix[16]) {
  enum { kSegments = 32 };
  if (!EffectBatchReserve(batch, kSegments + 1, kSegments * 3))
    return false;
  Scene3DPoint strike;
  float scale_x, scale_y;
  if (!ProjectSimEffectPoint(
          slot, effect, &effect->geometry.data.point, source, viewport,
          camera, matrix, &strike, &scale_x, &scale_y))
    return true;
  strike.y -= style->glow_origin_lift * scale_y;

  int base_vertex = batch->vertex_count;
  SDL_FColor center = style->glow_center;
  center.a *= style->strength;
  batch->vertices[batch->vertex_count++] = (SDL_Vertex){
    { strike.x, strike.y },
    center,
    { 0.0f, 0.0f },
  };
  float radius_x = style->glow_radius_x * scale_x;
  float radius_y = style->glow_radius_y * scale_y;
  if (radius_x < 3.0f) radius_x = 3.0f;
  if (radius_y < 2.0f) radius_y = 2.0f;
  for (int i = 0; i < kSegments; i++) {
    batch->vertices[batch->vertex_count++] = (SDL_Vertex){
      { strike.x + kEffectCircle32[i][0] * radius_x,
        strike.y + kEffectCircle32[i][1] * radius_y },
      style->glow_edge,
      { 0.0f, 0.0f },
    };
    batch->indices[batch->index_count++] = base_vertex;
    batch->indices[batch->index_count++] = base_vertex + i + 1;
    batch->indices[batch->index_count++] =
        base_vertex + (i + 1) % kSegments + 1;
  }
  return true;
}

static uint32_t EffectHash(uint32_t value) {
  value ^= value >> 16;
  value *= 0x7FEB352Du;
  value ^= value >> 15;
  value *= 0x846CA68Bu;
  return value ^ (value >> 16);
}

static bool AppendSimEffectParticles(
    EffectBatch *batch, const FrameSlot *slot,
    const SimEffectInstance *effect, const SimEffectStyle *style,
    SDL_Rect source, SDL_Rect viewport, const Scene3DCamera *camera,
    const float matrix[16]) {
  if (!effect->pulse_generation || effect->ticks_since_visible > 5) return true;
  Scene3DPoint strike;
  float scale_x, scale_y;
  if (!ProjectSimEffectPoint(
          slot, effect, &effect->geometry.data.point, source, viewport,
          camera, matrix, &strike, &scale_x, &scale_y))
    return true;
  strike.y -= style->particle_origin_lift * scale_y;
  float output_scale = (fabsf(scale_x) + fabsf(scale_y)) * 0.5f;
  if (output_scale < 0.5f) output_scale = 0.5f;

  for (uint32_t i = 0; i < style->particle_count; i++) {
    uint32_t seed = EffectHash(
        (uint32_t)effect->record_address * 0x9E3779B9u ^
        effect->generation * 0x85EBCA6Bu ^
        effect->pulse_generation * 0xC2B2AE35u ^
        (uint32_t)effect->kind * 0x165667B1u ^ i * 0x27D4EB2Fu);
    bool flame = style->particle_motion == kSimEffectParticle_Flame;
    unsigned lifetime = flame
        ? 24u + ((seed >> 4) & 15u)
        : 6u + (seed & 3u);
    unsigned age;
    if (flame) {
      /* A flame is already populated when its authentic sprite appears. Seed
       * each ember at a different point in a slower 0.4-0.65 second cycle;
       * the old 6-9 tick loop made every fire look frantic and repeatedly
       * emptied the emitter during its short visible animation. */
      age = (effect->pulse_ticks + seed % lifetime) % lifetime;
    } else {
      unsigned spawn_delay = i / 4u;
      if (effect->pulse_ticks < spawn_delay) continue;
      age = effect->pulse_ticks - spawn_delay;
      if (age >= lifetime) continue;
    }
    if (!EffectBatchReserve(batch, 4, 6)) return false;

    float t = lifetime > 1 ? (float)age / (float)(lifetime - 1) : 1.0f;
    const float *direction = kEffectCircle32[(seed >> 16) & 31u];
    float x, y, size;
    if (flame) {
      float lateral = direction[0] * (2.0f + 6.0f * t) * output_scale;
      x = strike.x + lateral;
      y = strike.y - (2.0f + 19.0f * t) * output_scale;
      size = (2.20f - 1.20f * t) * output_scale;
    } else {
      float distance = (4.0f + 22.0f * t) * output_scale;
      float arc = 4.0f * t * (1.0f - t);
      x = strike.x + direction[0] * distance;
      y = strike.y + direction[1] * distance * 0.42f -
          arc * 12.0f * output_scale;
      size = (1.8f - 1.15f * t) * output_scale;
    }
    if (size < 0.75f) size = 0.75f;
    SDL_FColor color = style->particle_color;
    color.a *= (1.0f - t) * style->strength;
    int base_vertex = batch->vertex_count;
    batch->vertices[batch->vertex_count++] = (SDL_Vertex){
      { x, y - size }, color, { 0.0f, 0.0f } };
    batch->vertices[batch->vertex_count++] = (SDL_Vertex){
      { x + size, y }, color, { 0.0f, 0.0f } };
    batch->vertices[batch->vertex_count++] = (SDL_Vertex){
      { x, y + size }, color, { 0.0f, 0.0f } };
    batch->vertices[batch->vertex_count++] = (SDL_Vertex){
      { x - size, y }, color, { 0.0f, 0.0f } };
    static const int diamond[] = { 0, 1, 2, 0, 2, 3 };
    for (int n = 0; n < 6; n++)
      batch->indices[batch->index_count++] = base_vertex + diamond[n];
  }
  return true;
}

static void DrawSimEffectLocalLighting(
    const FrameSlot *slot, bool lighting, SDL_Rect source, SDL_Rect viewport,
    const Scene3DCamera *camera, const float matrix[16]) {
  if (!lighting || !slot->sim.effect_visible_count ||
      !EffectRendererAvailable())
    return;
  enum {
    kVertices = kSimMaxEffectInstances * 33,
    kIndices = kSimMaxEffectInstances * 32 * 3,
  };
  SDL_Vertex vertices[kVertices];
  int indices[kIndices];
  EffectBatch batch = {
    .vertices = vertices, .indices = indices,
    .vertex_capacity = kVertices, .index_capacity = kIndices,
  };
  for (uint8_t i = 0; i < slot->sim.effect_count; i++) {
    const SimEffectInstance *effect = &slot->sim.effects[i];
    SimEffectStyle style;
    if (!(effect->flags & kSimEffectFlag_Visible) ||
        !SimEffectStyleFor(effect, &style))
      continue;
    if (!AppendSimEffectGlow(&batch, slot, effect, &style, source,
                             viewport, camera, matrix))
      break;
  }
  if (!batch.index_count && !batch.overflow) return;
  EffectRenderState state;
  if (!BeginEffectAdd(&state)) return;
  SubmitEffectBatch(&batch);
  EndEffectAdd(&state);
}

static void DrawSimEffectSceneFlash(const FrameSlot *slot, bool lighting,
                                    SDL_Rect viewport) {
  if (!lighting || !slot->sim.effect_visible_count ||
      !EffectRendererAvailable())
    return;
  SimEffectStyle strongest_style = {0};
  for (uint8_t i = 0; i < slot->sim.effect_count; i++) {
    SimEffectStyle style;
    if ((slot->sim.effects[i].flags & kSimEffectFlag_Visible) &&
        SimEffectStyleFor(&slot->sim.effects[i], &style) &&
        style.scene_flash && style.strength > strongest_style.strength)
      strongest_style = style;
  }
  if (strongest_style.strength <= 0.0f) return;
  EffectRenderState state;
  if (!BeginEffectAdd(&state)) return;
  bool color_ok = SDL_SetRenderDrawColor(
      g_renderer,
      (Uint8)(strongest_style.flash_color.r * 255.0f),
      (Uint8)(strongest_style.flash_color.g * 255.0f),
      (Uint8)(strongest_style.flash_color.b * 255.0f),
      (Uint8)(strongest_style.strength * 12.0f));
  SDL_FRect flash = ToFRect(viewport);
  bool fill_ok = color_ok && SDL_RenderFillRect(g_renderer, &flash);
  if (!fill_ok) DisableEffectAdd("scene flash");
  EndEffectAdd(&state);
}

static void DrawSimEffectParticles(
    const FrameSlot *slot, bool particles, SDL_Rect source, SDL_Rect viewport,
    const Scene3DCamera *camera, const float matrix[16]) {
  if (!particles || !slot->sim.effect_count ||
      !EffectRendererAvailable())
    return;
  enum {
    kVertices = kSimMaxEffectInstances * kSimMaxParticlesPerEffect * 4,
    kIndices = kSimMaxEffectInstances * kSimMaxParticlesPerEffect * 6,
  };
  SDL_Vertex vertices[kVertices];
  int indices[kIndices];
  EffectBatch batch = {
    .vertices = vertices, .indices = indices,
    .vertex_capacity = kVertices, .index_capacity = kIndices,
  };
  for (uint8_t i = 0; i < slot->sim.effect_count; i++) {
    const SimEffectInstance *effect = &slot->sim.effects[i];
    SimEffectStyle style;
    if (!SimEffectStyleFor(effect, &style)) continue;
    if (!AppendSimEffectParticles(&batch, slot, effect, &style, source,
                                  viewport, camera, matrix))
      break;
  }
  if (!batch.index_count && !batch.overflow) return;
  EffectRenderState state;
  if (!BeginEffectAdd(&state)) return;
  SubmitEffectBatch(&batch);
  EndEffectAdd(&state);
}

/* ── Action-stage spell effects ────────────────────────────────────────── */

typedef struct ActionEffectProjectionContext {
  const FrameSlot *slot;
  const DioramaProjection *diorama_projection;
  SDL_Rect viewport;
} ActionEffectProjectionContext;

_Static_assert(kActionEffectObjPriorityCount ==
                   kDioramaObjectPriorityCount,
               "action effects and diorama must agree on OBJ bands");

static bool ProjectActionEffectPoint(
    void *userdata, const ActionEffectInstance *effect,
    float local_x, float local_y, SDL_FPoint *point) {
  const ActionEffectProjectionContext *context = userdata;
  const FrameSlot *slot = context ? context->slot : NULL;
  if (!slot || !effect || !point || slot->visible_width <= 0 ||
      slot->snes_height <= 0 || context->viewport.w <= 0 ||
      context->viewport.h <= 0)
    return false;

  int screen_x = (int16_t)(uint16_t)(
      (uint16_t)effect->world_x - (uint16_t)slot->bg1_camera_x);
  int screen_y = (int16_t)(uint16_t)(
      (uint16_t)effect->world_y - (uint16_t)slot->bg1_camera_y);
  float capture_x = (float)slot->ws_extra + screen_x + local_x;
  float capture_y = (float)screen_y + local_y;

  if (context->diorama_projection)
    /* +ws_extra_top for the same reason capture_x carries +ws_extra: the
     * diorama samples TEXTURE space, whose row 0 is screen y = -ws_extra_top.
     * The flat path below keeps the authentic screen y, and never sees a
     * non-zero vertical margin anyway. */
    return Diorama_ProjectCapturedPoint(
        context->diorama_projection, capture_x,
        capture_y + (float)slot->ws_extra_top,
        effect->obj_priority, point, NULL, NULL);

  point->x = context->viewport.x +
      (capture_x - (float)slot->visible_x0) * context->viewport.w /
          (float)slot->visible_width;
  point->y = context->viewport.y + capture_y * context->viewport.h /
      (float)slot->snes_height;
  return true;
}

static void DrawActionEffects(const FrameSlot *slot, SDL_Rect viewport,
                              const DioramaProjection *diorama_projection) {
  if (!slot || !slot->action_effects.visible_count ||
      (!slot->action_effect_lighting && !slot->action_effect_particles) ||
      !EffectRendererAvailable())
    return;

  ActionEffectProjectionContext projection = {
    .slot = slot,
    .diorama_projection = diorama_projection,
    .viewport = viewport,
  };
  ActionEffectRenderBatch geometry;
  if (!ActionEffectRender_Build(
          &slot->action_effects, slot->action_effect_lighting,
          slot->action_effect_particles, ProjectActionEffectPoint,
          &projection, &geometry) ||
      !geometry.index_count)
    return;

  EffectBatch batch = {
    .vertices = geometry.vertices,
    .indices = geometry.indices,
    .vertex_count = geometry.vertex_count,
    .index_count = geometry.index_count,
    .vertex_capacity = kActionEffectRenderMaxVertices,
    .index_capacity = kActionEffectRenderMaxIndices,
  };
  EffectRenderState state;
  if (!BeginEffectAdd(&state)) return;
  SubmitEffectBatch(&batch);
  EndEffectAdd(&state);

  /* One line, once per process: the whole path (WRAM identity -> capture ->
   * projection -> geometry submit) either produced pixels or it did not, and
   * a run's console.log should say which without anyone re-deriving it. The
   * silent version of this is what let a 16-bit read of the animation-bank
   * BYTE reject every spell with no visible symptom but "nothing happens". */
  static bool announced;
  if (!announced) {
    announced = true;
    fprintf(stderr, "[action-fx] first spell geometry submitted: %u effect(s), "
            "%d vertices / %d indices (lighting=%d particles=%d)\n",
            slot->action_effects.visible_count, geometry.vertex_count,
            geometry.index_count, slot->action_effect_lighting,
            slot->action_effect_particles);
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

static bool SimObjectIsPromotedHud(const FrameSlot *slot,
                                   const SimRenderObject *object) {
  const FrameSlotOverlayCapture *capture =
      &slot->overlay_captures[kFrameSlotOverlay_Obj];
  return object->tier == kSimRecordTier_Fixed && capture->oamCount &&
      object->oam_first >= capture->oamFirst &&
      object->oam_first + object->oam_count <=
          capture->oamFirst + capture->oamCount;
}

static void DrawSimMapPlaneObject(const SimRenderObject *object,
                                  int screen_origin_x, int screen_origin_y,
                                  SDL_Rect source, SDL_Rect viewport,
                                  const float matrix[16]) {
  float x0 = (float)(object->local_x0 + screen_origin_x);
  float y0 = (float)(object->local_y0 + screen_origin_y);
  float x1 = (float)(object->local_x1 + screen_origin_x);
  float y1 = (float)(object->local_y1 + screen_origin_y);
  Scene3DPoint points[4];
  if (!ProjectSimTexturePoint(matrix, source, viewport, x0, y0, 0.0f,
                              &points[0]) ||
      !ProjectSimTexturePoint(matrix, source, viewport, x1, y0, 0.0f,
                              &points[1]) ||
      !ProjectSimTexturePoint(matrix, source, viewport, x0, y1, 0.0f,
                              &points[2]) ||
      !ProjectSimTexturePoint(matrix, source, viewport, x1, y1, 0.0f,
                              &points[3]))
    return;
  float u0 = object->atlas_x / (float)kSimObjAtlasWidth;
  float v0 = object->atlas_y / (float)kSimObjAtlasHeight;
  float u1 = (object->atlas_x + object->atlas_w) /
      (float)kSimObjAtlasWidth;
  float v1 = (object->atlas_y + object->atlas_h) /
      (float)kSimObjAtlasHeight;
  const SDL_FColor white = { 1.0f, 1.0f, 1.0f, 1.0f };
  SDL_Vertex vertices[] = {
    {{points[0].x, points[0].y}, white, {u0, v0}},
    {{points[1].x, points[1].y}, white, {u1, v0}},
    {{points[2].x, points[2].y}, white, {u0, v1}},
    {{points[3].x, points[3].y}, white, {u1, v1}},
  };
  const int indices[] = { 0, 1, 3, 0, 3, 2 };
  SDL_RenderGeometry(g_renderer, g_sim_obj_atlas_texture,
                     vertices, 4, indices, 6);
}

/* The directional light, resolved into shear per world unit of height (the
 * light is infinitely far and the ground is always z=0, so only the ratio
 * matters). Elevation 90 is straight overhead and shears nothing.
 *
 * The shipped default is deliberately near-overhead: the ground point then
 * sits directly under the lifted billboard on screen, so the gap between actor
 * and shadow reads unambiguously as altitude. A strongly angled light slides
 * the shadow sideways, where the eye reads the offset as lateral position
 * instead. Both angles are player settings, so this is a starting point rather
 * than a constraint. */
static const float kPi = 3.14159265f;

static void SimShadowLight(const FrameSlot *slot, float *light_x,
                           float *light_y) {
  float elevation = (float)slot->sim.light_elevation_deg * kPi / 180.0f;
  float azimuth = (float)slot->sim.light_azimuth_deg * kPi / 180.0f;
  float sine = sinf(elevation);
  /* cot(elevation), clamped so a near-horizon light cannot throw a shadow to
   * infinity and blow out the mask. */
  float shear = sine > 0.05f ? cosf(elevation) / sine : 20.0f;
  if (shear > 4.0f) shear = 4.0f;
  if (shear < 0.0f) shear = 0.0f;
  *light_x = shear * cosf(azimuth);
  *light_y = shear * sinf(azimuth);
}
/* Footprint shrink per world unit of height. A caster on the standard 24px
 * flight plane sits at height_world = 24/224, so 6.0 puts its shadow at
 * 1/(1 + 24/224*6) = ~61% -- enough to read as "up there" without the shadow
 * losing its shape (4.0, the original value, gave ~70% and read as too close
 * to the ground). This is the right knob for that reading rather than
 * `height_scale_x100`: the height dial lifts the sprite AND shears its shadow,
 * reframing the actor against the map, while this only resizes the footprint.
 *
 * Only casters with a nonzero classified height are affected, which is exactly
 * the flying class -- the angel and enemy classes $12-$15 (Sim3D_ClassifyObject,
 * sim_render_metadata.c) -- plus the 8px semi-grounded Napper pluck. Grounded
 * townspeople, scene composites, and every fixed-tier object stay at
 * footprint 1.0, so their silhouettes still meet their own feet. */
static const float kSimShadowHeightShrink = 6.0f;

/* Extra billboard scale on top of the perspective scale the lift already
 * produces. That true component is only about 1.5% at the standard flight
 * plane -- correct, and far too subtle to read -- so `height_pop_pct` adds a
 * deliberate presentation pop, normalized against the catalogue flight plane
 * so the setting's percentage means what it says. It scales the sprite in
 * place rather than biasing its depth: pulling a flyer toward the camera would
 * move it back down-screen and close the very gap to its own shadow that sells
 * the altitude. Paired with kSimShadowHeightShrink, a rising actor grows while
 * its shadow shrinks, which is what reads as height.
 *
 * The alternative -- shrinking the ground and grounded actors instead -- was
 * considered and rejected: scaling the ground with its actors is just a camera
 * zoom-out (same relative effect, but it reframes the town and fights the
 * distance setting), and shrinking grounded actors alone breaks their
 * footprint against the map tiles they stand on. A flyer has no such fixed
 * reference, so it is the cheap place to put the difference. */
static float SimBillboardHeightPop(SDL_Rect source, float height_world,
                                   unsigned height_pop_pct) {
  if (height_world <= 0.0f || !height_pop_pct || source.h <= 0) return 1.0f;
  float reference = (float)kSimVirtualHeight_Flying / (float)source.h;
  if (reference <= 0.0f) return 1.0f;
  return 1.0f + (height_world / reference) * (float)height_pop_pct / 100.0f;
}
/* How much of a caster's art height becomes ground depth. A billboard has no
 * depth, so shearing its silhouette along the light the way a solid body would
 * collapses it to a sliver under this shallow camera; the silhouette is
 * instead laid flat on the ground, foreshortened by the same projection as the
 * ground texture, and shortened here because the light is high. */
static const float kSimShadowFootprintDepth = 0.6f;

static SDL_Texture *s_sim_shadow_texture;
static SDL_Texture *s_sim_shadow_scratch;
static int s_sim_shadow_w, s_sim_shadow_h;
static bool s_sim_shadow_alloc_failed;
static bool s_sim_shadow_scratch_alloc_failed;

static SDL_Texture *CreateSimShadowTarget(int w, int h) {
  SDL_Texture *texture = SDL_CreateTexture(
      g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
  if (texture) {
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
  }
  return texture;
}

static SDL_Texture *EnsureSimShadowTexture(int w, int h) {
  if (!g_renderer || w <= 0 || h <= 0) return NULL;
  if (s_sim_shadow_texture && s_sim_shadow_w == w && s_sim_shadow_h == h)
    return s_sim_shadow_texture;
  if (s_sim_shadow_texture) SDL_DestroyTexture(s_sim_shadow_texture);
  if (s_sim_shadow_scratch) SDL_DestroyTexture(s_sim_shadow_scratch);
  s_sim_shadow_scratch = NULL;
  s_sim_shadow_scratch_alloc_failed = false;
  s_sim_shadow_texture = CreateSimShadowTarget(w, h);
  s_sim_shadow_w = w;
  s_sim_shadow_h = h;
  if (!s_sim_shadow_texture && !s_sim_shadow_alloc_failed) {
    /* Geometry must survive a target-allocation failure; only the shadow
     * stage drops out. Logged once so it cannot flood a play session. */
    s_sim_shadow_alloc_failed = true;
    fprintf(stderr, "[sim3d-d4] shadow mask target unavailable: %s\n",
            SDL_GetError());
  }
  return s_sim_shadow_texture;
}

/* D4b separable blur. The shadow mask is pure alpha, so a box blur is exactly
 * a weighted sum of alpha taps -- no shader needed. A custom blend mode keeps
 * the destination colour (black) and *adds* source alpha, and each tap carries
 * its weight in the texture alpha mod, so N taps at 1/N each average rather
 * than saturate. Doing it with ordinary blended draws means soft shadows work
 * on every renderer backend, not only where the Metal GPU path is available.
 *
 * Two passes over one axis each, ping-ponging through the scratch target:
 * cost is 2N draws instead of the N*N a single-pass box would need. */
enum { kSimShadowBlurTaps = 7 };

static SDL_BlendMode SimShadowAccumulateBlend(void) {
  static SDL_BlendMode mode = SDL_BLENDMODE_INVALID;
  if (mode == SDL_BLENDMODE_INVALID)
    mode = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
  return mode;
}

static void BlurSimShadowAxis(SDL_Texture *source, SDL_Texture *destination,
                              int w, int h, float radius, bool horizontal) {
  SDL_SetRenderTarget(g_renderer, destination);
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_NONE);
  SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 0);
  SDL_RenderClear(g_renderer);

  SDL_BlendMode accumulate = SimShadowAccumulateBlend();
  if (accumulate == SDL_BLENDMODE_INVALID) {
    /* Without the custom blend the taps would composite instead of average,
     * which reads as a smeared double image rather than a soft edge. Copy the
     * mask through unchanged and leave the shadow hard. */
    SDL_SetTextureBlendMode(source, SDL_BLENDMODE_NONE);
    SDL_RenderTexture(g_renderer, source, NULL, NULL);
    SDL_SetTextureBlendMode(source, SDL_BLENDMODE_BLEND);
    return;
  }
  SDL_SetTextureBlendMode(source, accumulate);
  int half = kSimShadowBlurTaps / 2;
  for (int tap = -half; tap <= half; tap++) {
    float offset = radius * (float)tap / (float)half;
    SDL_FRect destination_rect = {
      horizontal ? offset : 0.0f, horizontal ? 0.0f : offset,
      (float)w, (float)h,
    };
    SDL_SetTextureAlphaMod(source, (Uint8)(255 / kSimShadowBlurTaps));
    SDL_RenderTexture(g_renderer, source, NULL, &destination_rect);
  }
  SDL_SetTextureAlphaMod(source, 255);
  SDL_SetTextureBlendMode(source, SDL_BLENDMODE_BLEND);
}

/* Softness is a radius in output pixels, scaled with the viewport so the look
 * is resolution-independent rather than shrinking as the window grows. */
static void BlurSimShadowMask(SDL_Texture *mask, int w, int h,
                              unsigned softness_pct) {
  if (!softness_pct) return;
  float radius = (float)softness_pct / 100.0f * (float)h * 0.02f;
  if (radius < 0.5f) return;
  /* D4b's separable blur alone needs the second full-viewport target. Keep a
   * hard-shadow run from reserving it, which is 31.6 MiB at 4K. Allocation
   * failure degrades softness without touching the primary mask. */
  if (!s_sim_shadow_scratch && !s_sim_shadow_scratch_alloc_failed) {
    s_sim_shadow_scratch = CreateSimShadowTarget(w, h);
    s_sim_shadow_scratch_alloc_failed = !s_sim_shadow_scratch;
  }
  if (!s_sim_shadow_scratch) return;
  BlurSimShadowAxis(mask, s_sim_shadow_scratch, w, h, radius, true);
  BlurSimShadowAxis(s_sim_shadow_scratch, mask, w, h, radius, false);
}

/* Silhouettes are accumulated into a transparent mask and composited once, so
 * overlapping casters cannot double-darken the ground and the darkened result
 * can never touch sky, dialogs, HUD, or settings. */
static void DrawSimShadowMask(
    const FrameSlot *slot, bool virtual_height, bool soft_shadows,
    SDL_Rect source, SDL_Rect viewport, const float matrix[16]) {
  if (!g_sim_obj_atlas_texture || !slot->sim.atlas_valid) return;
  if (!slot->sim.shadow_opacity_pct) return;
  bool any_caster = false;
  for (size_t i = 0; i < slot->sim.object_count; i++) {
    if (Sim3D_ObjectCastsShadow(&slot->sim.objects[i])) {
      any_caster = true;
      break;
    }
  }
  /* An empty mask contributes nothing. Avoid a full-viewport target clear,
   * optional fourteen-draw blur and full-viewport composite on such frames. */
  if (!any_caster) return;
  SDL_Texture *mask = EnsureSimShadowTexture(viewport.w, viewport.h);
  if (!mask) return;

  SDL_Rect local_viewport = { 0, 0, viewport.w, viewport.h };
  float unit_x = ((float)viewport.w / (float)viewport.h) / (float)source.w;
  float unit_y = 1.0f / (float)source.h;
  float light_x, light_y;
  SimShadowLight(slot, &light_x, &light_y);

  /* The clip rect belongs to the current target, and the caller may have set
   * one on the output before this runs; the mask must be built unclipped, and
   * the composite below then re-clips it. */
  SDL_Rect saved_clip;
  bool clipped = SDL_RenderClipEnabled(g_renderer);
  if (clipped) SDL_GetRenderClipRect(g_renderer, &saved_clip);

  SDL_SetRenderTarget(g_renderer, mask);
  SDL_SetRenderClipRect(g_renderer, NULL);
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_NONE);
  SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 0);
  SDL_RenderClear(g_renderer);
  SDL_SetTextureBlendMode(g_sim_obj_atlas_texture, SDL_BLENDMODE_BLEND);
  SDL_SetTextureAlphaMod(g_sim_obj_atlas_texture, 255);

  for (size_t i = 0; i < slot->sim.object_count; i++) {
    const SimRenderObject *object = &slot->sim.objects[i];
    if (!Sim3D_ObjectCastsShadow(object)) continue;

    bool foot_anchor = !(object->traits & kSimObjectTrait_RecordOriginAnchor);
    int foot_dx = foot_anchor ? object->foot_x - (int)object->world_x : 0;
    int foot_dy = foot_anchor ? object->foot_y - (int)object->world_y : 0;
    int screen_anchor_x = (int16_t)(uint16_t)(
        object->world_x + foot_dx - slot->sim.camera_x);
    int screen_anchor_y = (int16_t)(uint16_t)(
        object->world_y + foot_dy - slot->sim.camera_y);
    float texture_anchor_x = slot->ws_extra + screen_anchor_x;
    float texture_anchor_y = screen_anchor_y;
    float anchor_world_x =
        ((texture_anchor_x - source.x) / source.w - 0.5f) *
        ((float)viewport.w / (float)viewport.h);
    float anchor_world_y =
        0.5f - (texture_anchor_y - source.y) / source.h;
    float height_world = virtual_height
        ? SimHeightWorldUnits(source, object->virtual_height,
                              slot->sim.height_scale_x100)
        : 0.0f;

    /* The silhouette is laid flat on the ground about the caster's foot: art
     * to the left stays left, art above the foot extends away from the camera.
     * The whole quad then shears along the light by the caster's height, so a
     * grounded actor is shadowed under its own feet and a flying one throws
     * its shadow clear of itself. */
    float offset_x[2] = {
      (float)(object->local_x0 - foot_dx),
      (float)(object->local_x1 - foot_dx),
    };
    float offset_y[2] = {
      (float)(object->local_y0 - foot_dy),
      (float)(object->local_y1 - foot_dy),
    };
    /* Height shrinks the footprint about the caster's ground point, so the
     * classified height and the player's height-scale tuning both feed the
     * shadow's size as well as its offset. */
    float footprint = Scene3D_ShadowFootprintScale(height_world,
                                                   kSimShadowHeightShrink);
    Scene3DPoint corner[4];
    bool shadow_visible = true;
    for (int c = 0; c < 4; c++) {
      if (!Scene3D_ProjectShadowPoint(
              matrix, anchor_world_x + offset_x[c & 1] * unit_x * footprint,
              anchor_world_y - offset_y[c >> 1] * unit_y *
                  kSimShadowFootprintDepth * footprint,
              height_world, light_x, light_y,
              local_viewport.w, local_viewport.h, &corner[c])) {
        shadow_visible = false;
        break;
      }
    }
    if (!shadow_visible) continue;

    float u0 = object->atlas_x / (float)kSimObjAtlasWidth;
    float v0 = object->atlas_y / (float)kSimObjAtlasHeight;
    float u1 = (object->atlas_x + object->atlas_w) /
        (float)kSimObjAtlasWidth;
    float v1 = (object->atlas_y + object->atlas_h) /
        (float)kSimObjAtlasHeight;
    const SDL_FColor black = { 0.0f, 0.0f, 0.0f, 1.0f };
    SDL_Vertex vertices[] = {
      {{corner[0].x, corner[0].y}, black, {u0, v0}},
      {{corner[1].x, corner[1].y}, black, {u1, v0}},
      {{corner[2].x, corner[2].y}, black, {u0, v1}},
      {{corner[3].x, corner[3].y}, black, {u1, v1}},
    };
    const int indices[] = { 0, 1, 3, 0, 3, 2 };
    SDL_RenderGeometry(g_renderer, g_sim_obj_atlas_texture,
                       vertices, 4, indices, 6);
  }

  if (soft_shadows)
    BlurSimShadowMask(mask, viewport.w, viewport.h,
                      slot->sim.shadow_softness_pct);

  SDL_SetRenderTarget(g_renderer, CrtPost_BaseTarget());
  if (clipped) SDL_SetRenderClipRect(g_renderer, &saved_clip);

  SDL_SetTextureAlphaMod(
      mask, (Uint8)(slot->sim.shadow_opacity_pct * 255 / 100));
  SDL_FRect dst = ToFRect(viewport);
  SDL_RenderTexture(g_renderer, mask, NULL, &dst);
  SDL_SetTextureAlphaMod(mask, 255);
}

/* D4c draws the same billboards a second and third time to build a rim band,
 * so the geometry lives in one loop rather than being re-derived. A NULL pass
 * is the ordinary coloured draw. */
typedef enum SimBillboardPassKind {
  kSimBillboardPass_Fill,  /* light-coloured silhouette, offset toward the light */
  kSimBillboardPass_Mask,  /* keep only the part inside the sprite's own body */
} SimBillboardPassKind;

/* Which tier a billboard band draws. The world and the menu now composite at
 * different depths -- the menu group is deferred past the atmospheric effects
 * -- so a band has to be able to draw one without the other. */
typedef enum SimObjectTierFilter {
  kSimTierFilter_World,
  kSimTierFilter_Fixed,
} SimObjectTierFilter;

typedef struct SimBillboardPass {
  SimBillboardPassKind kind;
  float offset_x, offset_y;
  /* W4-1: the blend mode this pass needs on the atlas texture.
   *
   * It has to travel WITH the pass rather than be set by the caller
   * beforehand, because DrawSimObjectPriority sets the atlas blend mode itself
   * on entry (it is the ordinary draw's mode, and the two main-path callers
   * rely on that). A caller that set a mode and then called in had it silently
   * overwritten before anything was drawn — which is exactly how the rim-light
   * mask pass lost its blend mode and stopped trimming the rim. */
  SDL_BlendMode blend;
} SimBillboardPass;

/* Defined with the rim-light code below, since the capability it latches belongs
 * to that effect; declared here because the billboard draw is the only caller. */
static bool SimApplyAtlasBlendMode(SDL_BlendMode blend);

/* Single source of truth for "what blend mode does this draw use", so the
 * caller-side pass setup and the callee-side set cannot disagree. A NULL pass is
 * the ordinary coloured draw. */
static SDL_BlendMode SimBillboardPassBlend(const SimBillboardPass *pass) {
  return (pass && pass->blend != SDL_BLENDMODE_INVALID)
      ? pass->blend
      : SDL_BLENDMODE_BLEND;
}

/* Strict "a must be drawn after b" for the in-band painter sort. Strict, not
 * "greater or equal": returning true for equal keys would make the insertion
 * sort unstable and lose the reverse-OAM tiebreak that keeps a multi-part
 * actor's authored overlap. */
static bool SimObjectSortsAfter(const SimRenderObject *a,
                                const SimRenderObject *b,
                                uint16_t camera_y) {
  bool a_overhead = (a->traits & kSimObjectTrait_Overhead) != 0;
  bool b_overhead = (b->traits & kSimObjectTrait_Overhead) != 0;
  if (a_overhead != b_overhead) return a_overhead;
  int a_row = (int16_t)(uint16_t)(a->world_y - camera_y);
  int b_row = (int16_t)(uint16_t)(b->world_y - camera_y);
  return a_row > b_row;
}

static void DrawSimObjectPriority(
    const FrameSlot *slot, int priority, SimObjectTierFilter tier_filter,
    bool project_world,
    bool virtual_height, SDL_Rect source, SDL_Rect viewport,
    const Scene3DCamera *camera, const float matrix[16],
    const SimBillboardPass *pass) {
  if (!g_sim_obj_atlas_texture || !slot->sim.atlas_valid) return;
  /* W4-2: a rejected blend mode means this pass cannot draw correctly, so bail
   * rather than draw with whatever mode happened to be set. */
  if (!SimApplyAtlasBlendMode(SimBillboardPassBlend(pass))) return;
  float flat_scale_x = (float)viewport.w / source.w;
  float flat_scale_y = (float)viewport.h / source.h;

  /* Earlier OAM slots own overlapping opaque pixels. SDL's later draw wins,
   * so the base traversal is reverse OAM order.
   *
   * Projected billboards additionally sort back-to-front by ground depth
   * within the band. On the flat SNES screen, OAM order alone decides overlap
   * and the result is correct because everything shares one plane; once the
   * map is projected, two actors on different map rows genuinely are at
   * different distances, and honouring OAM order there lets a far actor paint
   * over a near one. Sorting is confined to the band, so the hardware priority
   * bands still decide the coarse layering, and reverse OAM order remains the
   * tiebreak so co-located sprites (multi-part actors) keep their authored
   * overlap and the order stays stable frame to frame. */
  int order[kSimMaxRenderObjects];
  int order_count = 0;
  for (int i = (int)slot->sim.object_count - 1; i >= 0; i--) {
    const SimRenderObject *object = &slot->sim.objects[i];
    bool fixed = object->tier != kSimRecordTier_World;
    if (!object->atlas_valid || object->priority != priority ||
        fixed != (tier_filter == kSimTierFilter_Fixed) ||
        SimObjectIsPromotedHud(slot, object))
      continue;
    order[order_count++] = i;
  }
  if (project_world) {
    /* Sort key: overhead art last, then ascending screen row.
     *
     * The row is the record's row on the captured screen, not a world Y: the
     * ground quad maps row 0 to the far edge, so a smaller row is farther away
     * and must be drawn first. Overhead art is exempt from that entirely --
     * its composition hangs above the row the record sits on, so ordering it
     * by that row lets a nearer ground object draw over a cloud.
     *
     * Insertion sort: a band holds a few dozen objects at most, and a stable
     * sort is what preserves the OAM tiebreak above -- including among the
     * overhead objects themselves, which keep the ROM's authored overlap. */
    for (int i = 1; i < order_count; i++) {
      int index = order[i];
      int j = i - 1;
      while (j >= 0 && SimObjectSortsAfter(&slot->sim.objects[order[j]],
                                           &slot->sim.objects[index],
                                           slot->sim.camera_y)) {
        order[j + 1] = order[j];
        j--;
      }
      order[j + 1] = index;
    }
  }
  for (int n = 0; n < order_count; n++) {
    const SimRenderObject *object = &slot->sim.objects[order[n]];

    /* The rim is a silhouette effect: it must not inherit the sprite's
     * colour-math alpha, and map-plane art lies on the ground rather than
     * standing up, so it has no silhouette to light. */
    if (pass && (object->traits & kSimObjectTrait_MapPlane)) continue;
    bool half_add = !pass && slot->sim.object_half_add &&
        object->color_math_eligible;
    SDL_SetTextureAlphaMod(g_sim_obj_atlas_texture, half_add ? 128 : 255);

    int record_screen_x = (int16_t)(uint16_t)(
        object->world_x - slot->sim.camera_x);
    int record_screen_y = (int16_t)(uint16_t)(
        object->world_y - slot->sim.camera_y);
    if (project_world && (object->traits & kSimObjectTrait_MapPlane)) {
      DrawSimMapPlaneObject(object, slot->ws_extra + record_screen_x,
                            record_screen_y, source, viewport, matrix);
      continue;
    }

    /* The classified anchor is part of the object descriptor, not of the
     * VirtualHeight switch: projectiles and ground-targeted effects keep the
     * record origin even when their height resolves to zero. */
    bool foot_anchor = object->tier == kSimRecordTier_World &&
        !(object->traits & kSimObjectTrait_RecordOriginAnchor);
    int foot_dx = foot_anchor ? object->foot_x - (int)object->world_x : 0;
    int foot_dy = foot_anchor ? object->foot_y - (int)object->world_y : 0;
    int screen_anchor_x = (int16_t)(uint16_t)(
        object->world_x + foot_dx - slot->sim.camera_x);
    int screen_anchor_y = (int16_t)(uint16_t)(
        object->world_y + foot_dy - slot->sim.camera_y);
    float texture_anchor_x = slot->ws_extra + screen_anchor_x;
    float texture_anchor_y = screen_anchor_y;
    float scale_x = flat_scale_x;
    float scale_y = flat_scale_y;
    Scene3DPoint anchor;
    if (project_world && object->tier == kSimRecordTier_World) {
      float height_world = virtual_height
          ? SimHeightWorldUnits(source, object->virtual_height,
                                slot->sim.height_scale_x100)
          : 0.0f;
      if (!ProjectSimAnchorAndScale(
              matrix, source, viewport, texture_anchor_x, texture_anchor_y,
              height_world, Scene3D_AutoFitDistance(camera->fov_y),
              &anchor, &scale_x, &scale_y))
        continue;
      float height_pop = SimBillboardHeightPop(
          source, height_world, slot->sim.height_pop_pct);
      scale_x *= height_pop;
      scale_y *= height_pop;
    } else {
      anchor.x = viewport.x +
          (texture_anchor_x - source.x) * flat_scale_x;
      anchor.y = viewport.y +
          (texture_anchor_y - source.y) * flat_scale_y;
    }

    SDL_FRect atlas = {
      object->atlas_x, object->atlas_y,
      object->atlas_w, object->atlas_h,
    };
    SDL_FRect destination = {
      anchor.x + (object->local_x0 - foot_dx) * scale_x,
      anchor.y + (object->local_y0 - foot_dy) * scale_y,
      (object->local_x1 - object->local_x0) * scale_x,
      (object->local_y1 - object->local_y0) * scale_y,
    };
    if (pass) {
      destination.x += pass->offset_x;
      destination.y += pass->offset_y;
    }
    if (destination.w > 0.0f && destination.h > 0.0f)
      SDL_RenderTexture(g_renderer, g_sim_obj_atlas_texture,
                        &atlas, &destination);
  }
  SDL_SetTextureAlphaMod(g_sim_obj_atlas_texture, 255);
}

/* D4c rim light. Sprites have no normals, so the only physically meaningful
 * lighting product left is an edge: the band of a silhouette that faces the
 * light. It is built with two silhouette draws rather than a shader — one
 * offset toward the light, then intersected with the sprite's own body —
 * leaving a band just inside the lit edge, composited additively.
 *
 * Intersecting rather than subtracting is what keeps this honest. The first
 * version subtracted, which put the band OUTSIDE the silhouette: a halo
 * painted onto the background, which reads as the sprite glowing rather than
 * being lit, and which scales with strength so no amount of dialling it back
 * fixes the look. The action-stage rim shader (src/shaders/rim.frag.glsl)
 * has the same in-place property by construction — its `edge` term is
 * multiplied by the pixel's own alpha — so both paths now light only pixels
 * the sprite already owns.
 *
 * Restricted to world billboards by construction: the pass loop skips
 * map-plane art, and the band is composited immediately after its own priority
 * band, so it can never light the ground, the HUD, or a later band's sprite. */
static SDL_Texture *s_sim_rim_texture;
static int s_sim_rim_w, s_sim_rim_h;

static const SDL_Color kSimRimColor = { 255, 244, 214, 255 };

static SDL_Texture *EnsureSimRimTexture(int w, int h) {
  if (!g_renderer || w <= 0 || h <= 0) return NULL;
  if (s_sim_rim_texture && s_sim_rim_w == w && s_sim_rim_h == h)
    return s_sim_rim_texture;
  if (s_sim_rim_texture) SDL_DestroyTexture(s_sim_rim_texture);
  s_sim_rim_texture = CreateSimShadowTarget(w, h);
  s_sim_rim_w = w;
  s_sim_rim_h = h;
  return s_sim_rim_texture;
}

/* Multiplies destination alpha by source alpha while leaving destination
 * colour, i.e. keeps only the overlap. Applied to the offset silhouette with
 * the sprite at its true position, this trims the rim band back inside the
 * sprite so it can never touch a background pixel.
 *
 * W4-2: SDL_ComposeCustomBlendMode only COMPOSES a value — SDL_blendmode.h
 * documents that "not all renderers support" custom modes and directs callers to
 * the per-renderer support notes, and the composing call itself cannot report
 * that. Support is discovered only when the mode is handed to
 * SDL_SetTextureBlendMode, whose bool return we must therefore check. Until that
 * happens the mode is "composed but unproven", which is why this returns
 * SDL_BLENDMODE_INVALID once a set has actually failed rather than optimistically
 * forever. */
/* Reads true until a set actually fails. Settings sees this only through the
 * atomic, read-only capability accessor below. */
static SDL_AtomicInt s_sim_rim_mask_supported = { .value = 1 };

bool Present_SimRimMaskSupported(void) {
  return SDL_GetAtomicInt(&s_sim_rim_mask_supported) != 0;
}

static SDL_BlendMode SimRimMaskBlend(void) {
  if (!Present_SimRimMaskSupported()) return SDL_BLENDMODE_INVALID;
  static SDL_BlendMode mode = SDL_BLENDMODE_INVALID;
  if (mode == SDL_BLENDMODE_INVALID)
    mode = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_SRC_ALPHA,
        SDL_BLENDOPERATION_ADD);
  return mode;
}

/* Applies a pass's blend mode and reports whether the renderer accepted it.
 * A custom mode that the backend cannot honour must disable the effect rather
 * than silently draw with whatever mode was set before — that would produce
 * exactly the untrimmed silhouette W4-1 fixed. */
static bool SimApplyAtlasBlendMode(SDL_BlendMode blend) {
  if (SDL_SetTextureBlendMode(g_sim_obj_atlas_texture, blend)) return true;
  if (SDL_CompareAndSwapAtomicInt(&s_sim_rim_mask_supported, 1, 0)) {
    fprintf(stderr,
            "[sim3d-rim] this renderer rejected the rim mask blend mode (%s) — "
            "rim light disabled\n", SDL_GetError());
  }
  return false;
}

/* Screen-space direction the rim sits on. The lateral part is the opposite of
 * the shadow shear (the light is on the far side from its own shadow), plus a
 * constant upward bias so an overhead light — the shipped default, where the
 * shear is nearly zero — still lights the top edge rather than nothing. */
static void SimRimOffset(const FrameSlot *slot, float distance,
                         float *offset_x, float *offset_y) {
  float light_x, light_y;
  SimShadowLight(slot, &light_x, &light_y);
  float x = -light_x;
  /* +world y is up-screen, so a light biased away from the camera lifts the
   * rim; the constant term is the overhead component. */
  float y = -(light_y + 1.0f);
  float length = sqrtf(x * x + y * y);
  if (length < 0.0001f) { *offset_x = 0.0f; *offset_y = -distance; return; }
  *offset_x = x / length * distance;
  *offset_y = y / length * distance;
}

static void DrawSimRimLight(
    const FrameSlot *slot, int priority, bool virtual_height,
    SDL_Rect source, SDL_Rect viewport, const Scene3DCamera *camera,
    const float matrix[16]) {
  if (!slot->sim.rim_strength_pct) return;
  if (!g_sim_obj_atlas_texture || !slot->sim.atlas_valid) return;
  bool any_rim = false;
  for (size_t i = 0; i < slot->sim.object_count; i++) {
    const SimRenderObject *object = &slot->sim.objects[i];
    if (object->atlas_valid && object->priority == priority &&
        object->tier == kSimRecordTier_World &&
        !(object->traits & kSimObjectTrait_MapPlane)) {
      any_rim = true;
      break;
    }
  }
  /* The painter loop visits all four hardware OBJ priorities, but a town frame
   * commonly uses only one or two. An empty band used to clear and composite a
   * full-output target anyway; three of four bands were empty on every frame
   * in the representative replay. */
  if (!any_rim) return;
  SDL_Texture *rim = EnsureSimRimTexture(viewport.w, viewport.h);
  SDL_BlendMode mask_blend = SimRimMaskBlend();
  if (!rim || mask_blend == SDL_BLENDMODE_INVALID) return;
  /*
   * Probe the custom mask blend before drawing the fill. If the renderer
   * rejects it, compositing after the mask pass would otherwise expose the
   * unmasked fill for one priority band.
   */
  if (!SimApplyAtlasBlendMode(mask_blend)) return;

  /* Band width scales with the output so the rim does not thin out to nothing
   * as the window grows. */
  float distance = (float)viewport.h / (float)source.h * 1.25f;
  /* The fill lays down the offset silhouette with ordinary alpha blending; the
   * mask then multiplies it down to the part inside the sprite's own body,
   * which is what makes it read as a rim rather than a drop shadow in reverse.
   * W4-1: each pass carries its own blend mode, because the callee sets the
   * atlas mode on entry and would otherwise overwrite one set here. */
  SimBillboardPass fill = { kSimBillboardPass_Fill, 0.0f, 0.0f,
                            SDL_BLENDMODE_BLEND };
  SimRimOffset(slot, distance, &fill.offset_x, &fill.offset_y);
  SimBillboardPass mask = { kSimBillboardPass_Mask, 0.0f, 0.0f, mask_blend };

  SDL_Rect saved_clip;
  bool clipped = SDL_RenderClipEnabled(g_renderer);
  if (clipped) SDL_GetRenderClipRect(g_renderer, &saved_clip);

  SDL_SetRenderTarget(g_renderer, rim);
  SDL_SetRenderClipRect(g_renderer, NULL);
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_NONE);
  SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 0);
  SDL_RenderClear(g_renderer);

  SDL_SetTextureColorMod(g_sim_obj_atlas_texture, kSimRimColor.r,
                         kSimRimColor.g, kSimRimColor.b);
  SDL_Rect local_viewport = { 0, 0, viewport.w, viewport.h };
  DrawSimObjectPriority(slot, priority, kSimTierFilter_World, true,
                        virtual_height, source,
                        local_viewport, camera, matrix, &fill);
  DrawSimObjectPriority(slot, priority, kSimTierFilter_World, true,
                        virtual_height, source,
                        local_viewport, camera, matrix, &mask);
  /* Restore the shared atlas state this function borrowed. The blend mode is
   * left at the ordinary draw mode rather than whatever the mask pass used. */
  SDL_SetTextureColorMod(g_sim_obj_atlas_texture, 255, 255, 255);
  SDL_SetTextureBlendMode(g_sim_obj_atlas_texture, SDL_BLENDMODE_BLEND);

  SDL_SetRenderTarget(g_renderer, CrtPost_BaseTarget());
  if (clipped) SDL_SetRenderClipRect(g_renderer, &saved_clip);

  SDL_SetTextureBlendMode(rim, SDL_BLENDMODE_ADD);
  SDL_SetTextureAlphaMod(rim, (Uint8)(slot->sim.rim_strength_pct * 255 / 100));
  SDL_FRect destination = ToFRect(viewport);
  SDL_RenderTexture(g_renderer, rim, NULL, &destination);
  SDL_SetTextureAlphaMod(rim, 255);
  SDL_SetTextureBlendMode(rim, SDL_BLENDMODE_BLEND);
}

/* World-map underlay (ground extension).
 *
 * The town's ground quad is the captured 256-or-wider screen window; this
 * draws the same ground plane carried on past that window, textured with the
 * owned developed world map at half the town's linear resolution. The whole
 * mapping is one affine chain in authentic pixels:
 *
 *   town pixel  = camera + (captured column - screen_x0)
 *   world pixel = origin tile * 8 + town pixel / 2
 *
 * so a captured-texture coordinate converts straight to a world-map UV, and
 * the existing ProjectSimTexturePoint puts it in the same world units the
 * ground mesh uses. Alignment therefore cannot drift from the town: both are
 * driven by the same camera and the same transform. */
enum {
  /* How far past the visible window the extension reaches, in authentic town
   * pixels. One town is 512, so this is exactly enough to show a neighbouring
   * region and no more; the horizon guard below usually clips it sooner. */
  kSimUnderlayMarginPixels = 512,
  /* The extension is much larger than the unit ground quad, and
   * SDL_RenderGeometry interpolates UVs affinely, so it needs a finer mesh
   * than the ground's 8x6 to keep the perspective foreshortening honest. */
  /* Dense enough to resolve the cull boundary, not just the perspective.
   *
   * 24x18 was chosen for affine UV correctness alone, and over an extent that
   * runs source +/- 512px that is roughly 60px per cell -- coarser than the
   * corner radius, so the rounded window was interpolated back into a
   * straight-edged box and the smoothstep feather was flattened with it. The
   * fade is sampled at these vertices, so the mesh has to be finer than the
   * smallest feature the fade is meant to show. */
  kSimUnderlayColumns = 64,
  kSimUnderlayRows = 48,
  /* The canvas can insert both sides of the live captured rectangle into
   * each axis, then omit its alpha-masked cells. Exact split coordinates keep
   * the independently drawn meshes watertight. */
  kSimUnderlayMaxColumns = kSimUnderlayColumns + 2,
  kSimUnderlayMaxRows = kSimUnderlayRows + 2,
  kSimUnderlayVertexCount =
      (kSimUnderlayMaxColumns + 1) * (kSimUnderlayMaxRows + 1),
  kSimUnderlayIndexCount =
      kSimUnderlayMaxColumns * kSimUnderlayMaxRows * 6,
  /* One world-map tile occupies 16 town pixels. Cross-fading over that exact
   * footprint hides the resolution handoff without smearing multiple terrain
   * features together. */
  kSimTownExtentFeatherPixels =
      kSimWorldMapTilePixels * kSimWorldMapTownScale,
  /* Box-downsample factor for the out-of-focus copy of the world map. Four
   * is enough to lose the 8x8 tile grid -- the detail that reads as "nearby"
   * -- while keeping coastlines and landmasses legible as shapes. */
  kSimUnderlayBlurDivisor = 4,
  kSimUnderlayBlurPixels = kSimWorldMapPixels / kSimUnderlayBlurDivisor,
};

/* Nothing may be drawn closer to the camera plane than this: past it the
 * perspective divide inverts and the mesh folds back over the scene. */
static const float kSimUnderlayMinClipDepth = 0.35f;

static SDL_Texture *s_sim_underlay_texture;
/* Downsampled copy of the same bake, upscaled with linear filtering to stand
 * in for a blur. The far field is out of focus rather than merely dim: a
 * distant thing that is sharp reads as a small thing nearby, which is exactly
 * the wrong statement about ground the camera can never reach. */
static SDL_Texture *s_sim_underlay_blur_texture;
static uint32_t s_sim_underlay_serial;
static bool s_sim_underlay_alloc_failed;
static SDL_Texture *s_sim_canvas_texture;
static bool s_sim_canvas_alloc_failed;
static SDL_Texture *s_sim_cloud_texture;
static bool s_sim_cloud_alloc_failed;
static SDL_Texture *s_world_navigation_cloud_texture;
static bool s_world_navigation_cloud_alloc_failed;

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
  if (s_sim_shadow_texture) SDL_DestroyTexture(s_sim_shadow_texture);
  s_sim_shadow_texture = NULL;
  if (s_sim_shadow_scratch) SDL_DestroyTexture(s_sim_shadow_scratch);
  s_sim_shadow_scratch = NULL;
  s_sim_shadow_w = s_sim_shadow_h = 0;
  s_sim_shadow_alloc_failed = false;
  s_sim_shadow_scratch_alloc_failed = false;
  if (s_sim_rim_texture) SDL_DestroyTexture(s_sim_rim_texture);
  s_sim_rim_texture = NULL;
  s_sim_rim_w = s_sim_rim_h = 0;
  SDL_SetAtomicInt(&s_sim_rim_mask_supported, 1);
  SDL_SetAtomicInt(&s_effect_add_supported, 1);
  SDL_SetAtomicInt(&s_effect_geometry_supported, 1);
  if (s_sim_underlay_texture) SDL_DestroyTexture(s_sim_underlay_texture);
  s_sim_underlay_texture = NULL;
  if (s_sim_underlay_blur_texture)
    SDL_DestroyTexture(s_sim_underlay_blur_texture);
  s_sim_underlay_blur_texture = NULL;
  s_sim_underlay_serial = 0;
  s_sim_underlay_alloc_failed = false;
  if (s_sim_canvas_texture) SDL_DestroyTexture(s_sim_canvas_texture);
  s_sim_canvas_texture = NULL;
  s_sim_canvas_alloc_failed = false;
  if (s_sim_cloud_texture) SDL_DestroyTexture(s_sim_cloud_texture);
  s_sim_cloud_texture = NULL;
  s_sim_cloud_alloc_failed = false;
  if (s_world_navigation_cloud_texture)
    SDL_DestroyTexture(s_world_navigation_cloud_texture);
  s_world_navigation_cloud_texture = NULL;
  s_world_navigation_cloud_alloc_failed = false;
  if (s_world_navigation_palace_texture)
    SDL_DestroyTexture(s_world_navigation_palace_texture);
  s_world_navigation_palace_texture = NULL;
  if (s_world_navigation_ui_texture)
    SDL_DestroyTexture(s_world_navigation_ui_texture);
  s_world_navigation_ui_texture = NULL;
  s_world_navigation_composition_alloc_failed = false;
  s_world_navigation_composition_upload_valid = false;
}

/* Uploaded at the frame-slot handoff, like every other game-thread pixel
 * buffer, and only over the region written since the last upload — a still
 * camera in a quiet town uploads nothing at all. */
static void UploadSimTownCanvas(void) {
  if (s_sim_canvas_alloc_failed || !SimTownCanvas_Serial()) return;
  if (!s_sim_canvas_texture) {
    s_sim_canvas_texture = SDL_CreateTexture(
        g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        kSimTownCanvasPixels, kSimTownCanvasPixels);
    if (!s_sim_canvas_texture) {
      s_sim_canvas_alloc_failed = true;
      fprintf(stderr, "[sim3d-canvas] town canvas texture unavailable: %s\n",
              SDL_GetError());
      return;
    }
    SDL_SetTextureBlendMode(s_sim_canvas_texture, SDL_BLENDMODE_BLEND);
    /* Matches the ground mesh's own sampling: this is the same captured
     * pixels, just held in town space instead of screen space. */
    SDL_SetTextureScaleMode(s_sim_canvas_texture, SDL_SCALEMODE_LINEAR);
    /* A new streaming texture holds uninitialized memory, and from here on
     * only dirty sub-rectangles are uploaded — so anything the camera never
     * covers would keep whatever garbage the driver allocated (it showed as
     * magenta). Publish the whole zeroed canvas once so unseen town pixels
     * really are transparent and the world map shows through them. */
    SDL_UpdateTexture(s_sim_canvas_texture, NULL, SimTownCanvas_Pixels(),
                      kSimTownCanvasPixels * (int)sizeof(uint32_t));
  }
  int x = 0, y = 0, w = 0, h = 0;
  if (!SimTownCanvas_TakeDirtyRect(&x, &y, &w, &h)) return;
  const uint32_t *pixels = SimTownCanvas_Pixels();
  SDL_UpdateTexture(s_sim_canvas_texture, &(SDL_Rect){ x, y, w, h },
                    pixels + (size_t)y * kSimTownCanvasPixels + x,
                    kSimTownCanvasPixels * (int)sizeof(uint32_t));
}

/* Rebuilt only when the baked image would differ, which the serial reports.
 * The image is town-independent — only where it is sampled changes when the
 * player moves between towns — so a town change costs nothing here. */
static SDL_Texture *EnsureSimUnderlayTexture(const FrameSlot *slot) {
  if (s_sim_underlay_texture &&
      s_sim_underlay_serial == slot->sim.underlay_serial)
    return s_sim_underlay_texture;
  if (s_sim_underlay_alloc_failed) return NULL;

  if (!s_sim_underlay_texture) {
    s_sim_underlay_texture = SDL_CreateTexture(
        g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        kSimWorldMapPixels, kSimWorldMapPixels);
    if (!s_sim_underlay_texture) {
      s_sim_underlay_alloc_failed = true;
      fprintf(stderr, "[sim3d-underlay] world map texture unavailable: %s\n",
              SDL_GetError());
      return NULL;
    }
    SDL_SetTextureBlendMode(s_sim_underlay_texture, SDL_BLENDMODE_BLEND);
    /* Nearest keeps the world map's own 8x8 tile grid crisp under the 2x
     * upscale, which reads as a deliberate lower-detail layer rather than a
     * blurred copy of the town. */
    SDL_SetTextureScaleMode(s_sim_underlay_texture, SDL_SCALEMODE_NEAREST);
  }

  if (!s_sim_underlay_blur_texture) {
    s_sim_underlay_blur_texture = SDL_CreateTexture(
        g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        kSimUnderlayBlurPixels, kSimUnderlayBlurPixels);
    if (s_sim_underlay_blur_texture) {
      SDL_SetTextureBlendMode(s_sim_underlay_blur_texture,
                              SDL_BLENDMODE_BLEND);
      /* Linear is the whole trick: the box-downsampled image scaled back up
       * with bilinear filtering is a cheap, stable blur, and it costs one
       * texture rather than a multi-tap pass over the full 1024 square. */
      SDL_SetTextureScaleMode(s_sim_underlay_blur_texture,
                              SDL_SCALEMODE_LINEAR);
    }
  }

  void *pixels = NULL;
  int pitch = 0;
  if (!SDL_LockTexture(s_sim_underlay_texture, NULL, &pixels, &pitch))
    return NULL;
  bool baked = SimWorldMap_Bake((uint32_t *)pixels,
                                pitch / (int)sizeof(uint32_t));
  /* Unlock BEFORE touching the blur texture. The previous version kept this
   * lock open and read the just-baked pixels back out of it to build the mip,
   * which is a documented contract violation twice over: SDL_LockTexture is
   * WRITE-ONLY ("the pixels made available for editing don't necessarily
   * contain the old texture data", SDL_render.h), and two streaming locks were
   * held at once with the inner one released first.
   *
   * It worked on macOS and garbled on Steam Deck for the reason that class of
   * bug always does: Metal hands back a persistently-mapped buffer that reads
   * back fine, while a Vulkan/Mesa backend can hand back write-combined or
   * staging memory whose reads return unpredictable content — so the mip was
   * built from partly-garbage source and the upscaled blur smeared it across
   * the top of the 1024 square. Same hazard as finding O2 (the town canvas
   * bake), in the read direction rather than the write direction.
   *
   * The mip now comes from SimWorldMap_Downsample, which reads the module's own
   * persistent CPU image. That costs no extra memory: the image already exists
   * and is what this lock was a copy OF. */
  SDL_UnlockTexture(s_sim_underlay_texture);
  if (!baked) return NULL;
  if (s_sim_underlay_blur_texture) {
    void *blur_pixels = NULL;
    int blur_pitch = 0;
    if (SDL_LockTexture(s_sim_underlay_blur_texture, NULL, &blur_pixels,
                        &blur_pitch)) {
      if (!SimWorldMap_Downsample((uint32_t *)blur_pixels,
                                  blur_pitch / (int)sizeof(uint32_t),
                                  kSimUnderlayBlurDivisor)) {
        /* Leave the mip as-is rather than presenting a half-written lock. */
        fprintf(stderr, "[sim3d-underlay] world map downsample failed\n");
      }
      SDL_UnlockTexture(s_sim_underlay_blur_texture);
    }
  }
  s_sim_underlay_serial = slot->sim.underlay_serial;
  return s_sim_underlay_texture;
}

/* Draws one texture as an extension of the ground plane. `texture_x_at_zero`
 * is the captured-texture column that samples the texture's left edge, and
 * `span` is how many captured columns the whole texture covers — the two
 * numbers that place any town-space image under the same camera as the town's
 * own ground mesh. `fade` is optional; NULL draws at a uniform alpha.
 *
 * `exclude` identifies the live BG1 rectangle. Canvas cells in that rectangle
 * are omitted only where an alpha mask is active: drawing both layers there
 * would apply the same feather twice, while omitting the fully opaque backing
 * would expose the underlay through transparent BG1 priority pixels. */
static void DrawSimGroundExtension(SDL_Texture *texture,
                                   float texture_x_at_zero,
                                   float texture_y_at_zero, float span,
                                   uint8_t alpha, SDL_Rect source,
                                   SDL_Rect viewport, const float matrix[16],
                                   const SimCullFade *fade,
                                   const SDL_FRect *exclude) {
  if (!texture || !alpha || source.w <= 0 || source.h <= 0) return;

  /* Clamp the extension to the world map's own edges so every UV stays inside
   * the texture, then to the requested margin around the visible window. */
  float x0 = texture_x_at_zero, x1 = texture_x_at_zero + span;
  float y0 = texture_y_at_zero, y1 = texture_y_at_zero + span;
  float margin = (float)kSimUnderlayMarginPixels;
  if (x0 < source.x - margin) x0 = (float)source.x - margin;
  if (x1 > source.x + source.w + margin)
    x1 = (float)(source.x + source.w) + margin;
  if (y0 < source.y - margin) y0 = (float)source.y - margin;
  if (y1 > source.y + source.h + margin)
    y1 = (float)(source.y + source.h) + margin;
  if (x1 - x0 < 1.0f || y1 - y0 < 1.0f) return;

  /* Keep every vertex in front of the camera plane. With the ground tilted
   * away, depth grows with world y, so the edge that folds is the near one —
   * the largest captured row, y1 — not the horizon edge. Clamp whichever end
   * the camera says is dangerous; both corners are tested because yaw makes
   * the boundary depend on x. */
  float aspect = (float)viewport.w / (float)viewport.h;
  float world_y0 = 0.5f - (y0 - source.y) / source.h;  /* far */
  float world_y1 = 0.5f - (y1 - source.y) / source.h;  /* near */
  for (int corner = 0; corner < 2; corner++) {
    float texture_x = corner ? x1 : x0;
    float world_x = ((texture_x - source.x) / source.w - 0.5f) * aspect;
    float boundary = 0.0f;
    bool increasing = false;
    if (!Scene3D_GroundDepthBoundaryY(matrix, world_x,
                                      kSimUnderlayMinClipDepth, &boundary,
                                      &increasing))
      continue;
    if (increasing) {
      if (world_y1 < boundary) world_y1 = boundary;
    } else if (world_y0 > boundary) {
      world_y0 = boundary;
    }
  }
  if (world_y0 - world_y1 < 1.0f / source.h) return;
  y0 = source.y + (0.5f - world_y0) * source.h;
  y1 = source.y + (0.5f - world_y1) * source.h;

  float base_alpha = (float)alpha / 255.0f;
  /* File-scope rather than automatic: at this density the vertex/index pair
   * is well past a sane frame-stack allocation. Since Phase 0 every caller
   * runs on the render/main thread — the only thread that issues SDL render
   * calls — so these shared statics stay single-threaded-safe. */
  static SDL_Vertex vertices[kSimUnderlayVertexCount];
  static int indices[kSimUnderlayIndexCount];
  int vertex_count = 0, index_count = 0;
  float x_coordinates[kSimUnderlayMaxColumns + 1];
  float y_coordinates[kSimUnderlayMaxRows + 1];
  int x_count = kSimUnderlayColumns + 1;
  int y_count = kSimUnderlayRows + 1;
  for (int column = 0; column < x_count; column++)
    x_coordinates[column] =
        x0 + (x1 - x0) * (float)column / (float)kSimUnderlayColumns;
  for (int row = 0; row < y_count; row++)
    y_coordinates[row] =
        y0 + (y1 - y0) * (float)row / (float)kSimUnderlayRows;
  if (exclude) {
    x_count = InsertSimGroundCoordinate(
        x_coordinates, x_count, kSimUnderlayMaxColumns + 1, exclude->x);
    x_count = InsertSimGroundCoordinate(
        x_coordinates, x_count, kSimUnderlayMaxColumns + 1,
        exclude->x + exclude->w);
    y_count = InsertSimGroundCoordinate(
        y_coordinates, y_count, kSimUnderlayMaxRows + 1, exclude->y);
    y_count = InsertSimGroundCoordinate(
        y_coordinates, y_count, kSimUnderlayMaxRows + 1,
        exclude->y + exclude->h);
  }

  for (int row = 0; row < y_count; row++) {
    float texture_y = y_coordinates[row];
    for (int column = 0; column < x_count; column++) {
      float texture_x = x_coordinates[column];
      Scene3DPoint projected;
      if (!ProjectSimTexturePoint(matrix, source, viewport, texture_x,
                                  texture_y, 0.0f, &projected))
        return;
      float away = SimCullProximityAt(fade, texture_x, texture_y, source);
      float extent_alpha =
          SimGroundExtentAlphaAt(fade, texture_x, texture_y);
      /* Multiplied into the vertex colour, so it darkens whatever the texture
       * holds rather than mixing it with a colour of its own. That is the
       * difference the fade could not express. */
      float bright = fade ? 1.0f - away * fade->dim : 1.0f;
      SDL_FColor tint = {
        bright, bright, bright,
        base_alpha * (fade ? 1.0f - away * fade->fade : 1.0f) *
            extent_alpha,
      };
      vertices[vertex_count++] = (SDL_Vertex){
        { projected.x, projected.y }, tint,
        { (texture_x - texture_x_at_zero) / span,
          (texture_y - texture_y_at_zero) / span },
      };
    }
  }
  for (int row = 0; row + 1 < y_count; row++) {
    for (int column = 0; column + 1 < x_count; column++) {
      float centre_x =
          (x_coordinates[column] + x_coordinates[column + 1]) * 0.5f;
      float centre_y =
          (y_coordinates[row] + y_coordinates[row + 1]) * 0.5f;
      if (exclude &&
          centre_x >= exclude->x && centre_x < exclude->x + exclude->w &&
          centre_y >= exclude->y && centre_y < exclude->y + exclude->h) {
        float away =
            SimCullProximityAt(fade, centre_x, centre_y, source);
        float cull_alpha = fade ? 1.0f - away * fade->fade : 1.0f;
        float extent_alpha =
            SimGroundExtentAlphaAt(fade, centre_x, centre_y);
        if (cull_alpha < 0.9999f || extent_alpha < 0.9999f) continue;
      }
      int top_left = row * x_count + column;
      int bottom_left = top_left + x_count;
      indices[index_count++] = top_left;
      indices[index_count++] = top_left + 1;
      indices[index_count++] = bottom_left + 1;
      indices[index_count++] = top_left;
      indices[index_count++] = bottom_left + 1;
      indices[index_count++] = bottom_left;
    }
  }
  SDL_RenderGeometry(g_renderer, texture, vertices, vertex_count,
                     indices, index_count);
}

/* Cloud shroud.
 *
 * The ground extension reaches the whole town and beyond, but OAM can only
 * place sprites inside the authentic window plus its live widescreen margins.
 * Everything past that is permanently actor-free, and an empty town reads as a
 * bug rather than as distance. The shroud covers exactly that region: it is
 * drawn last, over the objects, so what it hides is unresolvably distant
 * instead of missing.
 *
 * The field is anchored in town space, sampled through the same mapping the
 * underlay uses, so clouds sit over places rather than sliding with the
 * camera. What moves is the hole: coverage is computed against the
 * sprite-drawable rectangle, which follows the camera, so advancing thins the
 * cover ahead and thickens it behind. That is the "whisking aside" without any
 * animation at all. */
enum {
  kSimCloudTexturePixels = 512,
  kSimCloudOctaves = 5,
  /* Reuse the extension mesh density; the shroud covers the same trapezoid and
   * suffers the same affine-UV error if it is coarser. */
  kSimCloudColumns = kSimUnderlayColumns,
  kSimCloudRows = kSimUnderlayRows,
  kSimCloudVertexCount = (kSimCloudColumns + 1) * (kSimCloudRows + 1),
  kSimCloudIndexCount = kSimCloudColumns * kSimCloudRows * 6,
};

typedef struct SimCloudLayer {
  float scale, offset_x, offset_y, weight, drift_x, drift_y;
} SimCloudLayer;

static const SimCloudLayer kSimCloudLayers[] = {
  { 4.0f, 0.00f, 0.00f, 1.00f, 0.0060f, 0.0011f },
  { 2.7f, 0.37f, 0.61f, 0.85f, 0.0037f, 0.0008f },
  { 6.3f, 0.72f, 0.19f, 0.70f, 0.0094f, 0.0021f },
};

/* Deterministic value noise. A hash rather than rand() so the field is
 * identical every run and a checkpoint image is reproducible. */
static float CloudHash(int x, int y) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}

static float CloudSmooth(float t) { return t * t * (3.0f - 2.0f * t); }

/* Tileable value noise at `period` cells across the texture. */
static float CloudNoise(float x, float y, int period) {
  int x0 = (int)floorf(x), y0 = (int)floorf(y);
  float fx = CloudSmooth(x - (float)x0), fy = CloudSmooth(y - (float)y0);
  int xa = ((x0 % period) + period) % period;
  int ya = ((y0 % period) + period) % period;
  int xb = (xa + 1) % period, yb = (ya + 1) % period;
  float n00 = CloudHash(xa, ya), n10 = CloudHash(xb, ya);
  float n01 = CloudHash(xa, yb), n11 = CloudHash(xb, yb);
  float top = n00 + (n10 - n00) * fx;
  float bottom = n01 + (n11 - n01) * fx;
  return top + (bottom - top) * fy;
}

static uint32_t SimCloudTexel(int x, int y) {
  float amplitude = 0.5f, total = 0.0f, sum = 0.0f;
  int period = 4;
  for (int octave = 0; octave < kSimCloudOctaves; octave++) {
    /* Include both periodic endpoints. Navigation's padded texture contains
     * exact copies of this base field, so the first and last texels must
     * match under linear filtering. */
    const float scale =
        (float)period / (float)(kSimCloudTexturePixels - 1);
    /* Offset every octave off its lattice axes. Sampling every octave at
     * y=0/period made the periodic boundary a coherent straight feature even
     * though its endpoint values matched. */
    const float octave_x = 0.37f + (float)octave * 0.53f;
    const float octave_y = 0.61f + (float)octave * 0.29f;
    total += CloudNoise((float)x * scale + octave_x,
                        (float)y * scale + octave_y, period) *
        amplitude;
    sum += amplitude;
    amplitude *= 0.5f;
    period *= 2;
  }
  float density = (total / sum - 0.42f) / 0.38f;
  if (density < 0.0f) density = 0.0f;
  if (density > 1.0f) density = 1.0f;
  density = CloudSmooth(density);
  unsigned alpha = (unsigned)(density * 255.0f + 0.5f);
  unsigned tint = 236 + (unsigned)(density * 19.0f);
  if (tint > 255) tint = 255;
  return (alpha << 24) | (tint << 16) | (tint << 8) | 255u;
}

static SDL_Texture *EnsureSimCloudTexture(void) {
  if (s_sim_cloud_texture || s_sim_cloud_alloc_failed)
    return s_sim_cloud_texture;
  s_sim_cloud_texture = SDL_CreateTexture(
      g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      kSimCloudTexturePixels, kSimCloudTexturePixels);
  if (!s_sim_cloud_texture) {
    s_sim_cloud_alloc_failed = true;
    fprintf(stderr, "[sim3d-cloud] shroud texture unavailable: %s\n",
            SDL_GetError());
    return NULL;
  }
  SDL_SetTextureBlendMode(s_sim_cloud_texture, SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(s_sim_cloud_texture, SDL_SCALEMODE_LINEAR);

  void *pixels = NULL;
  int pitch = 0;
  if (!SDL_LockTexture(s_sim_cloud_texture, NULL, &pixels, &pitch)) {
    SDL_DestroyTexture(s_sim_cloud_texture);
    s_sim_cloud_texture = NULL;
    s_sim_cloud_alloc_failed = true;
    return NULL;
  }
  for (int y = 0; y < kSimCloudTexturePixels; y++) {
    uint32_t *row = (uint32_t *)((uint8_t *)pixels + (size_t)y * pitch);
    for (int x = 0; x < kSimCloudTexturePixels; x++)
      row[x] = SimCloudTexel(x, y);
  }
  SDL_UnlockTexture(s_sim_cloud_texture);
  return s_sim_cloud_texture;
}

static SDL_Texture *EnsureWorldNavigationCloudTexture(void) {
  enum {
    kPaddedPixels = kSimCloudTexturePixels * 2,
  };
  if (s_world_navigation_cloud_texture ||
      s_world_navigation_cloud_alloc_failed)
    return s_world_navigation_cloud_texture;
  s_world_navigation_cloud_texture = SDL_CreateTexture(
      g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      kPaddedPixels, kPaddedPixels);
  if (!s_world_navigation_cloud_texture) {
    s_world_navigation_cloud_alloc_failed = true;
    fprintf(stderr,
            "[world-navigation] cloud texture unavailable: %s\n",
            SDL_GetError());
    return NULL;
  }
  SDL_SetTextureBlendMode(s_world_navigation_cloud_texture,
                          SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(s_world_navigation_cloud_texture,
                          SDL_SCALEMODE_LINEAR);
  void *pixels = NULL;
  int pitch = 0;
  if (!SDL_LockTexture(
          s_world_navigation_cloud_texture, NULL, &pixels, &pitch)) {
    SDL_DestroyTexture(s_world_navigation_cloud_texture);
    s_world_navigation_cloud_texture = NULL;
    s_world_navigation_cloud_alloc_failed = true;
    return NULL;
  }
  for (int y = 0; y < kPaddedPixels; y++) {
    uint32_t *row = (uint32_t *)((uint8_t *)pixels + (size_t)y * pitch);
    for (int x = 0; x < kPaddedPixels; x++)
      row[x] = SimCloudTexel(
          x % kSimCloudTexturePixels, y % kSimCloudTexturePixels);
  }
  SDL_UnlockTexture(s_world_navigation_cloud_texture);
  return s_world_navigation_cloud_texture;
}

static void DrawSimCloudShroud(const FrameSlot *slot, SDL_Rect source,
                               SDL_Rect viewport, const float matrix[16]) {
  if (!slot->sim.underlay_serial || !slot->sim.cloud_opacity_pct ||
      source.w <= 0 || source.h <= 0)
    return;
  SDL_Texture *texture = EnsureSimCloudTexture();
  if (!texture) return;

  /* Same town-space mapping as the underlay, so a cloud stays over the ground
   * it covers when the camera moves. */
  float origin_x = (float)slot->sim.underlay_origin_tile_x *
      (float)kSimWorldMapTilePixels * (float)kSimWorldMapTownScale;
  float origin_y = (float)slot->sim.underlay_origin_tile_y *
      (float)kSimWorldMapTilePixels * (float)kSimWorldMapTownScale;
  float texture_x_at_zero =
      (float)slot->sim.underlay_screen_x0 - (float)slot->sim.camera_x - origin_x;
  float texture_y_at_zero = -(float)slot->sim.camera_y - origin_y;
  float span = (float)(kSimWorldMapPixels * kSimWorldMapTownScale);

  /* Lifted off the ground plane, using the same pixels-to-world-units scale
   * D3c virtual heights use, so "72 pixels up" means the same thing to a
   * cloud as it does to a flying actor. This must be resolved before the
   * near-plane bound: a constant-height cloud has a different camera-plane
   * intersection than the ground below it. */
  float altitude = SimHeightWorldUnits(source, slot->sim.cloud_altitude_px,
                                       slot->sim.height_scale_x100);

  float x0 = texture_x_at_zero, x1 = texture_x_at_zero + span;
  float y0 = texture_y_at_zero, y1 = texture_y_at_zero + span;
  float margin = (float)kSimUnderlayMarginPixels;
  if (x0 < source.x - margin) x0 = (float)source.x - margin;
  if (x1 > source.x + source.w + margin)
    x1 = (float)(source.x + source.w) + margin;
  if (y0 < source.y - margin) y0 = (float)source.y - margin;
  if (y1 > source.y + source.h + margin)
    y1 = (float)(source.y + source.h) + margin;
  if (x1 - x0 < 1.0f || y1 - y0 < 1.0f) return;

  float aspect = (float)viewport.w / (float)viewport.h;
  float world_y0 = 0.5f - (y0 - source.y) / source.h;
  float world_y1 = 0.5f - (y1 - source.y) / source.h;
  for (int corner = 0; corner < 2; corner++) {
    float texture_x = corner ? x1 : x0;
    float world_x = ((texture_x - source.x) / source.w - 0.5f) * aspect;
    float boundary = 0.0f;
    bool increasing = false;
    if (!Scene3D_DepthBoundaryY(matrix, world_x, altitude,
                                kSimUnderlayMinClipDepth, &boundary,
                                &increasing))
      continue;
    if (increasing) {
      if (world_y1 < boundary) world_y1 = boundary;
    } else if (world_y0 > boundary) {
      world_y0 = boundary;
    }
  }
  if (world_y0 - world_y1 < 1.0f / source.h) return;
  y0 = source.y + (0.5f - world_y0) * source.h;
  y1 = source.y + (0.5f - world_y1) * source.h;

  float clear_x0 = (float)slot->sim.cloud_clear_x0;
  float clear_x1 = (float)slot->sim.cloud_clear_x1;
  float clear_y0 = (float)source.y;
  float clear_y1 = (float)(source.y + source.h);
  float falloff = (float)slot->sim.cloud_falloff_px;
  float inset = (float)slot->sim.cloud_inset_px;
  float opacity = (float)slot->sim.cloud_opacity_pct / 100.0f;
  /* Overlapping banks at different scales and offsets, so each layer's gaps
   * sit over another layer's body and alpha compounds as `1 - prod(1 - a)`.
   *
   * There is deliberately no untextured floor pass. One used to sit at the
   * end, weighted by cover^3, to force the far field opaque where the banks
   * failed to meet -- an SDL_RenderGeometry call with a NULL texture, which
   * is solid white modulated only by vertex alpha. It did what it said and
   * whited out the view, and the premise was wrong anyway: guaranteeing
   * opacity is no longer this pass's job. Per-record cover hides what the
   * sprite window takes away, and the cull haze marks the boundary
   * continuously, so the banks here are free to be thin and gappy. */
  /* Drift, in texture widths per second.
   *
   * Each bank moves at its own rate, and the coarse layer moves slowest: that
   * difference is the whole effect. Three layers sliding together read as one
   * translating image no matter how the noise is shaped, whereas differing
   * rates make the banks pass through each other and the field appears to
   * churn -- gaps opening and closing on their own rather than sweeping by.
   *
   * Wall time rather than the game frame. Weather does not owe the simulation
   * anything, it keeps moving through a pause, and game_frame is a 16-bit
   * counter that would jump the whole sky every eighteen minutes when it
   * wrapped. */
  Uint64 elapsed_ms = SDL_GetTicks();
  float drift = (float)slot->sim.cloud_drift_pct / 100.0f;

  static SDL_Vertex vertices[kSimCloudVertexCount];
  static int indices[kSimCloudIndexCount];
  int index_count = 0;
  for (int row = 0; row < kSimCloudRows; row++) {
    for (int column = 0; column < kSimCloudColumns; column++) {
      int top_left = row * (kSimCloudColumns + 1) + column;
      int bottom_left = top_left + kSimCloudColumns + 1;
      indices[index_count++] = top_left;
      indices[index_count++] = top_left + 1;
      indices[index_count++] = bottom_left + 1;
      indices[index_count++] = top_left;
      indices[index_count++] = bottom_left + 1;
      indices[index_count++] = bottom_left;
    }
  }

  SDL_SetRenderTextureAddressMode(g_renderer, SDL_TEXTURE_ADDRESS_WRAP,
                                  SDL_TEXTURE_ADDRESS_WRAP);
  for (unsigned layer = 0;
       layer < sizeof(kSimCloudLayers) / sizeof(kSimCloudLayers[0]); layer++) {
    float scale = kSimCloudLayers[layer].scale;
    float weight = kSimCloudLayers[layer].weight;
    int vertex_count = 0;
    bool any_cover = false;
    for (int row = 0; row <= kSimCloudRows; row++) {
      float texture_y = y0 + (y1 - y0) * (float)row / (float)kSimCloudRows;
      for (int column = 0; column <= kSimCloudColumns; column++) {
        float texture_x =
            x0 + (x1 - x0) * (float)column / (float)kSimCloudColumns;
        float cover = Sim3D_CloudCoverage(texture_x, texture_y, clear_x0,
                                          clear_x1, clear_y0, clear_y1,
                                          inset, falloff);
        if (cover > 0.0f) any_cover = true;
        Scene3DPoint projected;
        if (!ProjectSimTexturePoint(matrix, source, viewport, texture_x,
                                    texture_y, altitude, &projected)) {
          any_cover = false;
          vertex_count = 0;
          break;
        }
        float u = ((texture_x - texture_x_at_zero) / span) * scale +
            kSimCloudLayers[layer].offset_x +
            Scene3D_WrappedTextureOffset(
                elapsed_ms, kSimCloudLayers[layer].drift_x, drift);
        float v = ((texture_y - texture_y_at_zero) / span) * scale +
            kSimCloudLayers[layer].offset_y +
            Scene3D_WrappedTextureOffset(
                elapsed_ms, kSimCloudLayers[layer].drift_y, drift);
        vertices[vertex_count++] = (SDL_Vertex){
          { projected.x, projected.y },
          { 1.0f, 1.0f, 1.0f, cover * opacity * weight },
          { u, v },
        };
      }
      if (!vertex_count) break;
    }
    if (!any_cover || !vertex_count) continue;
    SDL_RenderGeometry(g_renderer, texture, vertices, vertex_count, indices,
                       index_count);
  }
  SDL_SetRenderTextureAddressMode(g_renderer, SDL_TEXTURE_ADDRESS_AUTO,
                                  SDL_TEXTURE_ADDRESS_AUTO);
}

/* Sim-town dynamic camera.
 *
 * Same construction as the diorama reactive camera above -- a velocity lean
 * eased toward on a wall-clock exponential, plus additive impulses that decay
 * on another -- because the failure modes it was tuned against are the same
 * ones: a fixed per-frame damping factor is twice as stiff at 120Hz as at
 * 60Hz, and an impulse that replaces rather than stacks loses back-to-back
 * events.
 *
 * The magnitudes are smaller. The action stages look at the player from the
 * side, where a lean swings the whole scene across the screen; the town is
 * viewed from near-overhead, where the same angle mostly slides the ground
 * under a camera that is already looking down, and it takes very little
 * before the map appears to swim.
 *
 * Present-thread state, matching the diorama camera's ownership: the game
 * thread hands over a clamped signal and one-shot event flags, and the
 * formula lives here. */
static const float kSimLeanYaw = 0.045f;    /* rad at full lean */
static const float kSimLeanPitch = 0.055f;  /* rad at full lean */
static const float kSimDampTau = 0.22f;     /* s; slower than action mode */
static const float kSimKickPitch = 0.030f;  /* rad */
static const float kSimKickZoom = -0.09f;   /* fraction; slight punch in */
static const float kSimKickTau = 0.18f;     /* s */

typedef struct SimDynamicCameraState {
  float lean_x, lean_y;
  float kick_pitch, kick_zoom;
  uint64_t last_ns;
  uint64_t last_slot_ns;
  bool active;
} SimDynamicCameraState;

static SimDynamicCameraState g_sim_dyncam;

/* Folds the reactive offsets into the camera the projection is built from.
 * Returns with `camera` unchanged when the feature is off, so the pose stays
 * exactly what the pitch/yaw/distance settings describe. */
static void ApplySimDynamicCamera(const FrameSlot *slot,
                                  Scene3DCamera *camera) {
  bool dynamic = slot->sim_camera_mode == kSimCam_Dynamic;
  bool reactive = dynamic && slot->sim_dyncam_strength > 0;

  /* A mode change snaps rather than eases. Easing across it would swing the
   * camera from the free pose to the baseline over a visible fraction of a
   * second, which reads as the camera being knocked rather than as the player
   * having switched modes. Same rule the diorama camera uses. */
  static int previous_mode = -1;
  bool mode_changed = previous_mode != slot->sim_camera_mode;
  previous_mode = slot->sim_camera_mode;

  uint64_t now_ns = SDL_GetTicksNS();
  float dt = 0.0f;
  if (g_sim_dyncam.last_ns != 0) {
    dt = (float)(now_ns - g_sim_dyncam.last_ns) / 1e9f;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 1.0f) dt = 1.0f;   /* resuming from a pause is not a huge step */
  }
  g_sim_dyncam.last_ns = now_ns;

  if (!dynamic) {
    /* Cleared rather than left to decay, so switching the feature off is
     * immediate and switching it back on starts level instead of resuming a
     * lean from whenever it was turned off. */
    g_sim_dyncam = (SimDynamicCameraState){ .last_ns = now_ns };
    return;
  }

  float gain = (float)slot->sim_dyncam_strength / 100.0f;
  float target_x = kSimLeanPitch * gain * slot->sim_dyncam_lean_pitch;
  float target_y = kSimLeanYaw * gain * slot->sim_dyncam_lean_yaw;

  if (!reactive) {
    g_sim_dyncam.lean_x = 0.0f;
    g_sim_dyncam.lean_y = 0.0f;
    g_sim_dyncam.kick_pitch = 0.0f;
    g_sim_dyncam.kick_zoom = 0.0f;
    g_sim_dyncam.active = false;
  } else if (!g_sim_dyncam.active || mode_changed || dt <= 0.0f) {
    g_sim_dyncam.lean_x = target_x;
    g_sim_dyncam.lean_y = target_y;
    g_sim_dyncam.active = true;
  } else {
    float alpha = 1.0f - expf(-dt / kSimDampTau);
    g_sim_dyncam.lean_x += (target_x - g_sim_dyncam.lean_x) * alpha;
    g_sim_dyncam.lean_y += (target_y - g_sim_dyncam.lean_y) * alpha;
  }

  /* Impulses fire only on a genuinely new capture. A present-thread redraw of
   * a slot already processed must not re-trigger, or a paused frame would
   * shake forever. Stacking is additive so a hit taken mid-jolt reads as
   * stronger rather than restarting. */
  if (reactive && slot->timestamp_ns != g_sim_dyncam.last_slot_ns) {
    g_sim_dyncam.last_slot_ns = slot->timestamp_ns;
    if (slot->sim_dyncam_event_hit) {
      g_sim_dyncam.kick_pitch += kSimKickPitch * gain;
      g_sim_dyncam.kick_zoom += kSimKickZoom * gain;
    }
  }
  if (reactive && dt > 0.0f) {
    float decay = expf(-dt / kSimKickTau);
    g_sim_dyncam.kick_pitch *= decay;
    g_sim_dyncam.kick_zoom *= decay;
  }

  camera->tilt_x += g_sim_dyncam.lean_x + g_sim_dyncam.kick_pitch +
      slot->sim_manual_orbit_pitch;
  camera->tilt_y += g_sim_dyncam.lean_y + slot->sim_manual_orbit_yaw;
  camera->distance *= 1.0f + g_sim_dyncam.kick_zoom;
  if (camera->distance < 2.0f) camera->distance = 2.0f;
}

/* Atmospheric backdrop.
 *
 * Replaces the flat clear behind the finite ground with a vertical gradient.
 * Everything opaque still draws over it, so "behind the finite ground" is
 * enforced by draw order rather than by a mask: the only pixels this can reach
 * are the ones nothing else covered.
 *
 * **The ground-plane horizon is never on screen.** Measured across the whole
 * settable pitch range (-700..700 mrad): the vanishing line lands between 544
 * and 5619 destination pixels outside a 224-row viewport, closest at -700, and
 * a pitch of exactly zero has no horizon at all. The plan's D5a-2 wording
 * ("at the tilted map horizon") describes something this camera cannot show.
 * What actually reads as sky in frame is where the ground *data* runs out --
 * past the world map extent or the near-clip bound -- which is a different
 * edge in a different place.
 *
 * So the sky is graded around a **synthetic** horizon placed at a fraction of
 * the viewport height, and the real one is used as the anchor only if it ever
 * becomes visible. That is not dead generality: it is one comparison, and it
 * means widening the pitch range later cannot silently produce sky below the
 * horizon.
 *
 * The synthetic anchor is honest about what it is. The backdrop is only ever
 * seen fully zoomed out, in the corners past the end of the extended map, and
 * there is no horizon line in frame for the eye to check it against -- so its
 * job is to look like sky at those edges, not to agree with a vanishing point
 * that is 1674 pixels off the top of the screen.
 *
 * The two endpoints are authored sky colours, and the scene's own backdrop is
 * what they are mixed *from* rather than what they are derived from.
 *
 * The first version derived both by lifting the backdrop toward white and
 * dropping it toward black, on the reasoning that this keeps whatever hue the
 * game chose. That reasoning only holds if there is a hue: a simulation town's
 * `separated_backdrop_argb` is black, and black lifted toward white is grey,
 * so the sky came out greyscale. Mixing toward an authored blue instead is
 * well-defined for any backdrop, and a town that does pick a coloured one
 * still tints the result rather than being overruled.
 *
 * Strength is the mix, so 0 still reproduces the previous flat fill exactly --
 * the property that makes the D5a-2 checkpoint's "only pixels behind the
 * finite ground change" checkable against A8 rather than against a
 * differently-coloured screen.
 *
 * Sky brightens and desaturates toward the horizon and deepens toward the
 * zenith, which is the one thing about real sky that survives being reduced to
 * two colours. */
/* Authored sky endpoints, mixed with the scene backdrop by strength. Pale and
 * slightly green at the horizon, deeper and bluer overhead -- the same
 * direction ActRaiser's own world-map sky and water use, so the corners past
 * the end of the extended map do not read as a different game's palette. */
static const SDL_FColor kSimSkyHorizon = { 0.60f, 0.74f, 0.90f, 1.0f };
static const SDL_FColor kSimSkyZenith = { 0.16f, 0.33f, 0.66f, 1.0f };

enum {
  /* Percent, at full strength: how far each end is taken toward its sky
   * colour. Asymmetric because the horizon is the readable half -- the zenith
   * mostly needs to not compete with it. */
  kSimBackdropHorizonMixPct = 82,
  kSimBackdropZenithMixPct = 62,
};

/* Gradient position at a screen row: 0 at the anchor and below it, 1 a full
 * span above. Pure so the degenerate anchors are checkable without a camera. */
static float SimBackdropGradientAt(float screen_y, float horizon_y,
                                   float span) {
  if (span <= 0.0f) return 0.0f;
  float t = (horizon_y - screen_y) / span;
  return t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
}

static void DrawSimBackdrop(const FrameSlot *slot, SDL_Rect viewport,
                            const float matrix[16]) {
  uint32_t backdrop = slot->sim.separated_backdrop_argb;
  float base_r = (float)((backdrop >> 16) & 0xFF) / 255.0f;
  float base_g = (float)((backdrop >> 8) & 0xFF) / 255.0f;
  float base_b = (float)(backdrop & 0xFF) / 255.0f;

  float strength = (float)slot->sim.backdrop_strength_pct / 100.0f;
  float horizon_mix = (float)kSimBackdropHorizonMixPct / 100.0f * strength;
  float zenith_mix = (float)kSimBackdropZenithMixPct / 100.0f * strength;

  SDL_FColor horizon = {
    base_r + (kSimSkyHorizon.r - base_r) * horizon_mix,
    base_g + (kSimSkyHorizon.g - base_g) * horizon_mix,
    base_b + (kSimSkyHorizon.b - base_b) * horizon_mix,
    1.0f,
  };
  SDL_FColor zenith = {
    base_r + (kSimSkyZenith.r - base_r) * zenith_mix,
    base_g + (kSimSkyZenith.g - base_g) * zenith_mix,
    base_b + (kSimSkyZenith.b - base_b) * zenith_mix,
    1.0f,
  };

  float top = (float)viewport.y;
  float bottom = (float)(viewport.y + viewport.h);

  /* Anchor the gradient's zero -- its brightest, most distant-looking end --
   * at the real horizon when it is on screen, and at the synthetic one
   * otherwise, which is every pitch the settings allow today. */
  float horizon_y = 0.0f;
  bool horizon_visible = matrix &&
      Scene3D_GroundHorizonScreenY(matrix, viewport.h, &horizon_y) &&
      (horizon_y += (float)viewport.y, horizon_y > top && horizon_y < bottom);
  float anchor = horizon_visible
      ? horizon_y
      : top + (float)viewport.h *
            (float)slot->sim.backdrop_horizon_pct / 100.0f;

  /* A vertex at the anchor when it falls inside, because SDL_RenderGeometry
   * interpolates linearly and the gradient bends there. */
  float rows[3];
  int row_count = 0;
  rows[row_count++] = top;
  if (anchor > top && anchor < bottom) rows[row_count++] = anchor;
  rows[row_count++] = bottom;

  /* The gradient completes exactly at the top of the viewport rather than over
   * a fixed distance, so moving the anchor restretches it instead of leaving
   * a band of flat zenith above wherever it happened to run out. */
  float span = anchor - top;
  if (span < 1.0f) span = 1.0f;
  float left = (float)viewport.x;
  float right = (float)(viewport.x + viewport.w);

  SDL_Vertex vertices[6];
  int indices[12];
  int vertex_count = 0, index_count = 0;
  for (int row = 0; row < row_count; row++) {
    float t = SimBackdropGradientAt(rows[row], anchor, span);
    SDL_FColor color = {
      horizon.r + (zenith.r - horizon.r) * t,
      horizon.g + (zenith.g - horizon.g) * t,
      horizon.b + (zenith.b - horizon.b) * t,
      1.0f,
    };
    vertices[vertex_count++] =
        (SDL_Vertex){ { left, rows[row] }, color, { 0.0f, 0.0f } };
    vertices[vertex_count++] =
        (SDL_Vertex){ { right, rows[row] }, color, { 0.0f, 0.0f } };
  }
  for (int row = 0; row + 1 < row_count; row++) {
    int top_left = row * 2;
    indices[index_count++] = top_left;
    indices[index_count++] = top_left + 1;
    indices[index_count++] = top_left + 3;
    indices[index_count++] = top_left;
    indices[index_count++] = top_left + 3;
    indices[index_count++] = top_left + 2;
  }
  SDL_RenderGeometry(g_renderer, NULL, vertices, vertex_count, indices,
                     index_count);
}

/* D5a cull-event marker overlay.
 *
 * Draws one square per world record that the sprite window is taking away,
 * sized by how much cover Sim3D_SourceCullCover says it has earned. Its whole
 * job is to make the invariant -- every culled record has something over it --
 * checkable before any cloud art exists; the puff renderer replaces the
 * square without changing what selects it. Inert unless AR_SIMCULLMARK is set.
 *
 * Colour is the state, not decoration: green while the record is still being
 * drawn and merely approaching the edge, red once the emitter has actually
 * started clipping its parts. A red marker with no cover under it is exactly
 * the artifact this whole stage exists to remove. */
static bool SimCullMarkersEnabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("AR_SIMCULLMARK");
    enabled = (e && *e && *e != '0') ? 1 : 0;
  }
  return enabled != 0;
}

static void DrawSimCullMarkers(const FrameSlot *slot, SDL_Rect source,
                               SDL_Rect viewport, const float matrix[16],
                               int lift_inset) {
  if (!SimCullMarkersEnabled() || !slot->sim.metadata_valid) return;
  int lead = slot->sim.cull_lead_px ? slot->sim.cull_lead_px
                                    : kSimCullLeadDefaultPx;
  for (unsigned i = 0; i < slot->sim.source_count; i++) {
    const SimSourceRecord *record = &slot->sim.sources[i];
    float cover = Sim3D_SourceCullCover(record, slot->sim.sprite_margin_left,
                                        slot->sim.sprite_margin_right, lead,
                                        slot->sim.cull_corner_px, lift_inset);
    if (cover <= 0.0f) continue;

    /* Biased origin back to a captured-texture point: the emitter stores
     * screen x as `biased - $10` and screen y as `biased - $11`, and
     * underlay_screen_x0 is the column holding SNES x = 0. */
    float texture_x = (float)slot->sim.underlay_screen_x0 +
        (float)record->anchor_x - 16.0f;
    float texture_y = (float)source.y + (float)record->anchor_y - 17.0f;
    /* Placed where the renderer draws the record, not where the record sits.
     * The cover above was timed off the unlifted anchor because that is what
     * the emitter culls on; putting the cover there too would leave it under
     * a flying actor's feet. */
    float lift = SimHeightWorldUnits(
        source, Sim3D_SourceDrawLift(record, slot->sim.height_scale_x100),
        100);
    Scene3DPoint centre;
    if (!ProjectSimTexturePoint(matrix, source, viewport, texture_x,
                                texture_y, lift, &centre))
      continue;

    float half = 3.0f + 5.0f * cover;
    SDL_FRect box = { centre.x - half, centre.y - half, half * 2, half * 2 };
    bool clipping = record->clipped_parts != 0;
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_renderer, clipping ? 255 : 40,
                           clipping ? 48 : 220, 40,
                           (Uint8)(80.0f + 175.0f * cover));
    SDL_RenderFillRect(g_renderer, &box);
  }
}

static void DrawSimWorldUnderlay(const FrameSlot *slot, SDL_Rect source,
                                 SDL_Rect viewport, const float matrix[16],
                                 int lift_inset) {
  if (!slot->sim.underlay_serial || slot->sim.underlay_haze_pct >= 100) return;
  SDL_Texture *texture = EnsureSimUnderlayTexture(slot);
  if (!texture) return;
  /* Two captured pixels per world-map pixel: the world map is the town at
   * half linear resolution. */
  float origin_x = (float)slot->sim.underlay_origin_tile_x *
      (float)kSimWorldMapTilePixels * (float)kSimWorldMapTownScale;
  float origin_y = (float)slot->sim.underlay_origin_tile_y *
      (float)kSimWorldMapTilePixels * (float)kSimWorldMapTownScale;
  float texture_x_at_zero =
      (float)slot->sim.underlay_screen_x0 - (float)slot->sim.camera_x -
      origin_x;
  float texture_y_at_zero = -(float)slot->sim.camera_y - origin_y;
  float span = (float)(kSimWorldMapPixels * kSimWorldMapTownScale);
  uint8_t hazed = (uint8_t)(255 - slot->sim.underlay_haze_pct * 255 / 100);

  /* Focus falloff, in two passes over the same mesh.
   *
   * The blurred copy goes down first at the haze alpha, then the sharp copy
   * over it at `1 - proximity * defocus`. Where the sprite window is live
   * that second alpha is 1 and the result is the sharp map with no haze at
   * all; at the edge it is `1 - defocus`, so the strength setting is exactly
   * how much of the blurred copy is ever allowed to show. In between it is a
   * lerp, so distance haze and defocus arrive together on one ramp instead of
   * as two boundaries the eye has to reconcile.
   *
   * Sharing `cull_haze_lead_px` with the town-ground fade is deliberate: the
   * ground handing over to the world map and the world map going soft are the
   * same event, and giving them separate ramps is what makes a scene look
   * like it has two unrelated edges in it. */
  SimCullFade focus = {
    .lead = slot->sim.cull_haze_lead_px ? slot->sim.cull_haze_lead_px
                                        : kSimCullHazeLeadDefaultPx,
    .corner = slot->sim.cull_corner_px,
    .lift_inset = lift_inset,
    .fade = (float)slot->sim.underlay_defocus_pct / 100.0f,
    .dim = (float)slot->sim.cull_dim_pct / 100.0f,
    .margin_left = slot->sim.sprite_margin_left,
    .margin_right = slot->sim.sprite_margin_right,
    .screen_x0 = slot->sim.underlay_screen_x0,
  };
  /* The blurred copy takes the dim but not the fade: it is the layer being
   * revealed, so fading it would thin the very thing the sharp copy hands over
   * to and the far field would go transparent instead of dark. */
  SimCullFade blurred_dim = focus;
  blurred_dim.fade = 0.0f;
  bool defocus = s_sim_underlay_blur_texture &&
      slot->sim.underlay_defocus_pct != 0 &&
      (slot->sim.effective_features & kSimFeature_CullHaze) != 0;
  if (defocus) {
    DrawSimGroundExtension(s_sim_underlay_blur_texture, texture_x_at_zero,
                           texture_y_at_zero, span, hazed, source, viewport,
                           matrix, &blurred_dim, NULL);
    DrawSimGroundExtension(texture, texture_x_at_zero, texture_y_at_zero,
                           span, 255, source, viewport, matrix, &focus, NULL);
    return;
  }
  /* Sharp-only path (defocus off): still dimmed, never faded, same reason. */
  DrawSimGroundExtension(texture, texture_x_at_zero, texture_y_at_zero, span,
                         hazed, source, viewport, matrix, &blurred_dim, NULL);
}

/* The full-town ground, drawn over the underlay at full resolution. Where an
 * alpha handoff is active, the live captured rectangle is omitted below so
 * this canvas extends BG1 rather than feathering underneath it a second time. */
static void DrawSimTownCanvas(const FrameSlot *slot, SDL_Rect source,
                              SDL_Rect viewport, const float matrix[16],
                              bool cull_fade, int lift_inset,
                              const SDL_FRect *exclude) {
  if (!slot->sim.town_canvas_serial || !s_sim_canvas_texture) return;
  float extent_x0 =
      (float)slot->sim.underlay_screen_x0 - (float)slot->sim.camera_x;
  float extent_y0 = -(float)slot->sim.camera_y;
  SimCullFade fade = {
    .lead = slot->sim.cull_haze_lead_px ? slot->sim.cull_haze_lead_px
                                        : kSimCullHazeLeadDefaultPx,
    .corner = slot->sim.cull_corner_px,
    .lift_inset = lift_inset,
    .fade = cull_fade ? (float)slot->sim.cull_haze_pct / 100.0f : 0.0f,
    .dim = cull_fade ? (float)slot->sim.cull_dim_pct / 100.0f : 0.0f,
    .extent_x0 = extent_x0,
    .extent_y0 = extent_y0,
    .extent_x1 = extent_x0 + (float)kSimTownCanvasPixels,
    .extent_y1 = extent_y0 + (float)kSimTownCanvasPixels,
    .extent_feather = (float)kSimTownExtentFeatherPixels,
    .margin_left = slot->sim.sprite_margin_left,
    .margin_right = slot->sim.sprite_margin_right,
    .screen_x0 = slot->sim.underlay_screen_x0,
  };
  DrawSimGroundExtension(
      s_sim_canvas_texture,
      extent_x0, extent_y0, (float)kSimTownCanvasPixels, 255,
      source, viewport, matrix, &fade, exclude);
}

/* $09 full-world presentation.
 *
 * Navigation uses the same developed texture cache as the town underlay, but
 * none of the underlay geometry: the complete map is the primary plane and
 * its captured Mode-7 matrix is already an affine top-down camera. */
static SDL_FPoint WorldNavigationAuthenticToOutput(
    const FrameSlot *slot, SDL_Rect viewport,
    float authentic_x, float authentic_y) {
  const float authentic_x0 =
      ((float)slot->snes_width -
       (float)kSimWorldNavigationCompositionWidth) * 0.5f;
  const float captured_x = authentic_x0 + authentic_x;
  return (SDL_FPoint){
    (float)viewport.x +
        (captured_x - (float)slot->visible_x0) *
            (float)viewport.w / (float)slot->visible_width,
    (float)viewport.y + authentic_y *
        (float)viewport.h / (float)slot->snes_height,
  };
}

static bool DrawWorldNavigationGround(
    const FrameSlot *slot, SDL_Rect viewport, SDL_Texture *texture) {
  const SimWorldNavigationScene *scene =
      &slot->sim.world_navigation_scene;
  if (!texture || !scene->valid || slot->visible_width <= 0 ||
      slot->snes_height <= 0)
    return false;

  SDL_Vertex vertices[4];
  for (int i = 0; i < 4; i++) {
    float authentic_x = 0.0f, authentic_y = 0.0f;
    const float source_x =
        (float)scene->ground[i].tile_x * kSimWorldMapTilePixels;
    const float source_y =
        (float)scene->ground[i].tile_y * kSimWorldMapTilePixels;
    if (!SimWorldNavigationScene_ProjectSource(
            scene, source_x, source_y, &authentic_x, &authentic_y))
      return false;
    vertices[i] = (SDL_Vertex){
      WorldNavigationAuthenticToOutput(
          slot, viewport, authentic_x, authentic_y),
      {1.0f, 1.0f, 1.0f, 1.0f},
      {scene->ground[i].texture_u, scene->ground[i].texture_v},
    };
  }
  static const int indices[6] = {0, 1, 2, 0, 2, 3};
  SDL_SetTextureColorMod(texture, 255, 255, 255);
  SDL_SetTextureAlphaMod(texture, 255);
  if (!SDL_RenderGeometry(g_renderer, texture, vertices, 4, indices, 6)) {
    fprintf(stderr, "[world-navigation] ground draw failed: %s\n",
            SDL_GetError());
    return false;
  }
  return true;
}

static bool DrawWorldNavigationLightTreatment(
    const FrameSlot *slot, SDL_Rect viewport) {
  if (!slot->sim.world_navigation_lighting) return true;
  const float elevation =
      (float)slot->sim.light_elevation_deg * kPi / 180.0f;
  const float low_sun = 1.0f - sinf(elevation);
  if (low_sun <= 0.001f) return true;

  /* The ground is a flat, top-down texture with one normal, so its diffuse
   * term is necessarily uniform. A restrained warm dusk grade makes that
   * physically honest limitation visible without pretending the painted map
   * has per-pixel normals. Azimuth remains meaningful for cloud shadows. */
  const Uint8 alpha = (Uint8)(low_sun * 72.0f + 0.5f);
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(g_renderer, 42, 24, 12, alpha);
  const SDL_FRect area = ToFRect(viewport);
  const bool ok = SDL_RenderFillRect(g_renderer, &area);
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_NONE);
  return ok;
}

/* The original label selector owns the clear 256x256 region. Outside it, the
 * already-downsampled world texture supplies depth blur and the shared haze
 * setting mixes the terrain toward the live scene backdrop. Both passes use
 * one affine mesh, so the boundary follows the scripted zoom and rotation. */
static bool DrawWorldNavigationActiveRegionHaze(
    const FrameSlot *slot, SDL_Rect viewport) {
  const SimWorldNavigationScene *scene =
      &slot->sim.world_navigation_scene;
  if (!slot->sim.world_navigation_haze ||
      (!slot->sim.underlay_defocus_pct && !slot->sim.underlay_haze_pct))
    return true;

  enum {
    /* Four samples through each fade band preserve the smoothstep's zero
     * slope at the clear edge. Sampling only its endpoints and letting SDL
     * interpolate linearly leaves a visible rectangular crease. */
    kMaxAxis = 12,
    kMaxVertices = kMaxAxis * kMaxAxis,
    kMaxIndices = (kMaxAxis - 1) * (kMaxAxis - 1) * 6,
  };
  /* The town control is in its 2048px high-fidelity source space; navigation
   * uses a 1024px texture, so halve it before applying the same visual width. */
  const float lead = fmaxf(
      1.0f, (float)slot->sim.cull_haze_lead_px * 0.5f);
  const float x0 = scene->active_region_x;
  const float y0 = scene->active_region_y;
  const float x1 = x0 + scene->active_region_width;
  const float y1 = y0 + scene->active_region_height;
  float xs[kMaxAxis] = {0.0f, (float)kSimWorldMapPixels};
  float ys[kMaxAxis] = {0.0f, (float)kSimWorldMapPixels};
  int nx = 2, ny = 2;
  if (scene->active_region_valid) {
    nx = InsertSimGroundCoordinate(xs, nx, kMaxAxis, x0 - lead);
    nx = InsertSimGroundCoordinate(xs, nx, kMaxAxis, x0 - lead * 0.75f);
    nx = InsertSimGroundCoordinate(xs, nx, kMaxAxis, x0 - lead * 0.50f);
    nx = InsertSimGroundCoordinate(xs, nx, kMaxAxis, x0 - lead * 0.25f);
    nx = InsertSimGroundCoordinate(xs, nx, kMaxAxis, x0);
    nx = InsertSimGroundCoordinate(xs, nx, kMaxAxis, x1);
    nx = InsertSimGroundCoordinate(xs, nx, kMaxAxis, x1 + lead * 0.25f);
    nx = InsertSimGroundCoordinate(xs, nx, kMaxAxis, x1 + lead * 0.50f);
    nx = InsertSimGroundCoordinate(xs, nx, kMaxAxis, x1 + lead * 0.75f);
    nx = InsertSimGroundCoordinate(xs, nx, kMaxAxis, x1 + lead);
    ny = InsertSimGroundCoordinate(ys, ny, kMaxAxis, y0 - lead);
    ny = InsertSimGroundCoordinate(ys, ny, kMaxAxis, y0 - lead * 0.75f);
    ny = InsertSimGroundCoordinate(ys, ny, kMaxAxis, y0 - lead * 0.50f);
    ny = InsertSimGroundCoordinate(ys, ny, kMaxAxis, y0 - lead * 0.25f);
    ny = InsertSimGroundCoordinate(ys, ny, kMaxAxis, y0);
    ny = InsertSimGroundCoordinate(ys, ny, kMaxAxis, y1);
    ny = InsertSimGroundCoordinate(ys, ny, kMaxAxis, y1 + lead * 0.25f);
    ny = InsertSimGroundCoordinate(ys, ny, kMaxAxis, y1 + lead * 0.50f);
    ny = InsertSimGroundCoordinate(ys, ny, kMaxAxis, y1 + lead * 0.75f);
    ny = InsertSimGroundCoordinate(ys, ny, kMaxAxis, y1 + lead);
  }

  SDL_Vertex vertices[kMaxVertices];
  int indices[kMaxIndices];
  int vertex_count = 0, index_count = 0;
  for (int row = 0; row < ny; row++) {
    for (int column = 0; column < nx; column++) {
      float authentic_x = 0.0f, authentic_y = 0.0f;
      if (!SimWorldNavigationScene_ProjectSource(
              scene, xs[column], ys[row], &authentic_x, &authentic_y))
        return false;
      const float haze =
          SimWorldNavigationScene_LocationHaze(
              scene, xs[column], ys[row], lead);
      vertices[vertex_count++] = (SDL_Vertex){
        WorldNavigationAuthenticToOutput(
            slot, viewport, authentic_x, authentic_y),
        {1.0f, 1.0f, 1.0f,
         haze * (float)slot->sim.underlay_defocus_pct / 100.0f},
        {xs[column] / (float)kSimWorldMapPixels,
         ys[row] / (float)kSimWorldMapPixels},
      };
    }
  }
  for (int row = 0; row + 1 < ny; row++) {
    for (int column = 0; column + 1 < nx; column++) {
      const int tl = row * nx + column;
      indices[index_count++] = tl;
      indices[index_count++] = tl + 1;
      indices[index_count++] = tl + nx + 1;
      indices[index_count++] = tl;
      indices[index_count++] = tl + nx + 1;
      indices[index_count++] = tl + nx;
    }
  }

  if (slot->sim.underlay_defocus_pct && s_sim_underlay_blur_texture) {
    SDL_SetTextureColorMod(s_sim_underlay_blur_texture, 255, 255, 255);
    SDL_SetTextureAlphaMod(s_sim_underlay_blur_texture, 255);
    if (!SDL_RenderGeometry(
            g_renderer, s_sim_underlay_blur_texture, vertices, vertex_count,
            indices, index_count))
      return false;
  }

  if (slot->sim.underlay_haze_pct) {
    const uint32_t backdrop = slot->sim.separated_backdrop_argb;
    const float r = (float)((backdrop >> 16) & 0xFF) / 255.0f;
    const float g = (float)((backdrop >> 8) & 0xFF) / 255.0f;
    const float b = (float)(backdrop & 0xFF) / 255.0f;
    for (int i = 0; i < vertex_count; i++) {
      const float haze = SimWorldNavigationScene_LocationHaze(
          scene, vertices[i].tex_coord.x * kSimWorldMapPixels,
          vertices[i].tex_coord.y * kSimWorldMapPixels, lead);
      vertices[i].color = (SDL_FColor){
        r, g, b, haze * (float)slot->sim.underlay_haze_pct / 100.0f,
      };
    }
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    const bool ok = SDL_RenderGeometry(
        g_renderer, NULL, vertices, vertex_count, indices, index_count);
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_NONE);
    if (!ok) return false;
  }
  return true;
}

static bool DrawWorldNavigationCloudLayer(
    const FrameSlot *slot, SDL_Rect viewport, SDL_Texture *texture,
    const SimCloudLayer *layer, Uint64 elapsed_ms, float drift,
    float source_offset_x, float source_offset_y, SDL_FColor colour) {
  if (!layer) return false;
  const SimWorldNavigationScene *scene =
      &slot->sim.world_navigation_scene;
  float phase_u = layer->offset_x +
      Scene3D_WrappedTextureOffset(elapsed_ms, layer->drift_x, drift);
  float phase_v = layer->offset_y +
      Scene3D_WrappedTextureOffset(elapsed_ms, layer->drift_y, drift);
  phase_u -= floorf(phase_u);
  phase_v -= floorf(phase_v);
  enum {
    kPaddedPixels = kSimCloudTexturePixels * 2,
  };
  /* The texture contains two exact copies of the proven town cloud field in
   * each axis. Sample one complete copy through a single world quad and slide
   * the window through the padded copy. This retains wraparound drift on
   * backends that cannot wrap SDL_RenderGeometry UVs, without introducing
   * any manually tiled geometry edges. */
  const float u0 =
      (phase_u * kSimCloudTexturePixels + 0.5f) / kPaddedPixels;
  const float v0 =
      (phase_v * kSimCloudTexturePixels + 0.5f) / kPaddedPixels;
  const float u1 =
      (phase_u * kSimCloudTexturePixels +
       kSimCloudTexturePixels - 0.5f) / kPaddedPixels;
  const float v1 =
      (phase_v * kSimCloudTexturePixels +
       kSimCloudTexturePixels - 0.5f) / kPaddedPixels;
  const float u[4] = {u0, u1, u1, u0};
  const float v[4] = {v0, v0, v1, v1};
  SDL_Vertex vertices[4];
  for (int i = 0; i < 4; i++) {
    const float source_x =
        (float)scene->ground[i].tile_x * kSimWorldMapTilePixels;
    const float source_y =
        (float)scene->ground[i].tile_y * kSimWorldMapTilePixels;
    float authentic_x = 0.0f, authentic_y = 0.0f;
    if (!SimWorldNavigationScene_ProjectSource(
            scene, source_x + source_offset_x,
            source_y + source_offset_y, &authentic_x, &authentic_y))
      return false;
    vertices[i] = (SDL_Vertex){
      WorldNavigationAuthenticToOutput(
          slot, viewport, authentic_x, authentic_y),
      colour,
      {u[i], v[i]},
    };
  }
  static const int indices[6] = {0, 1, 2, 0, 2, 3};
  return SDL_RenderGeometry(g_renderer, texture, vertices, 4, indices, 6);
}

static bool DrawWorldNavigationWeather(
    const FrameSlot *slot, SDL_Rect viewport) {
  if (!slot->sim.world_navigation_clouds ||
      !slot->sim.cloud_opacity_pct)
    return true;
  SDL_Texture *texture = EnsureWorldNavigationCloudTexture();
  if (!texture) return false;

  const float opacity = (float)slot->sim.cloud_opacity_pct / 100.0f;
  const float body_visibility = SimWorldNavigationScene_CloudVisibility(
      slot->sim.world_navigation.zoom_current,
      slot->sim.cloud_altitude_px);
  const float drift = (float)slot->sim.cloud_drift_pct / 100.0f;
  const Uint64 elapsed_ms = SDL_GetTicks();
  SDL_SetTextureColorMod(texture, 255, 255, 255);
  SDL_SetTextureAlphaMod(texture, 255);

  /* A cloud's altitude is invisible to an orthographic top-down camera until
   * it casts a displaced shadow. Reuse the town light's world-space shear so
   * the shadow rotates and zooms with the scripted Mode-7 event. The
   * procedural alpha already supplies a soft edge; the softness dial spreads
   * three low-alpha samples across the light-perpendicular axis. */
  if (slot->sim.world_navigation_lighting &&
      slot->sim.shadow_opacity_pct) {
    float light_x = 0.0f, light_y = 0.0f;
    SimShadowLight(slot, &light_x, &light_y);
    const float shadow_x =
        light_x * (float)slot->sim.cloud_altitude_px;
    const float shadow_y =
        light_y * (float)slot->sim.cloud_altitude_px;
    const float blur =
        (float)slot->sim.shadow_softness_pct * 0.08f;
    const int sample_count = blur > 0.01f ? 3 : 1;
    const float perpendicular_x = -sinf(
        (float)slot->sim.light_azimuth_deg * kPi / 180.0f);
    const float perpendicular_y = cosf(
        (float)slot->sim.light_azimuth_deg * kPi / 180.0f);
    for (unsigned layer_index = 0;
         layer_index <
             sizeof(kSimCloudLayers) / sizeof(kSimCloudLayers[0]);
         layer_index++) {
      const SimCloudLayer *layer = &kSimCloudLayers[layer_index];
      for (int sample = 0; sample < sample_count; sample++) {
        const float spread =
            sample_count == 1 ? 0.0f : (float)(sample - 1) * blur;
        const float sample_weight =
            sample_count == 1 ? 1.0f : sample == 1 ? 0.5f : 0.25f;
        const float alpha = opacity * layer->weight *
            ((float)slot->sim.shadow_opacity_pct / 100.0f) *
            0.35f * sample_weight;
        if (!DrawWorldNavigationCloudLayer(
                slot, viewport, texture, layer, elapsed_ms, drift,
                shadow_x + perpendicular_x * spread,
                shadow_y + perpendicular_y * spread,
                (SDL_FColor){0.0f, 0.0f, 0.0f, alpha}))
          return false;
      }
    }
  }

  if (body_visibility > 0.001f) {
    for (unsigned layer_index = 0;
         layer_index < sizeof(kSimCloudLayers) / sizeof(kSimCloudLayers[0]);
         layer_index++) {
      const SimCloudLayer *layer = &kSimCloudLayers[layer_index];
      if (!DrawWorldNavigationCloudLayer(
              slot, viewport, texture, layer, elapsed_ms, drift, 0.0f, 0.0f,
              (SDL_FColor){
                1.0f, 1.0f, 1.0f,
                opacity * layer->weight * body_visibility,
              })) {
        return false;
      }
    }
  }
  return true;
}

static bool DrawWorldNavigationCompositionLayer(
    const FrameSlot *slot, SDL_Rect viewport,
    const SimWorldNavigationCompositionLayer *layer,
    SDL_Texture *texture) {
  if (!layer || !layer->visible) return true;
  if (!texture || !layer->width || !layer->height) return false;
  const SDL_FPoint top_left = WorldNavigationAuthenticToOutput(
      slot, viewport, layer->screen_x, layer->screen_y);
  const SDL_FPoint bottom_right = WorldNavigationAuthenticToOutput(
      slot, viewport, layer->screen_x + layer->width,
      layer->screen_y + layer->height);
  SDL_FRect source = {0.0f, 0.0f, layer->width, layer->height};
  SDL_FRect destination = {
    top_left.x, top_left.y,
    bottom_right.x - top_left.x,
    bottom_right.y - top_left.y,
  };
  return SDL_RenderTexture(g_renderer, texture, &source, &destination);
}

/* BG planes carrying menu furniture rather than world. Deferred past every
 * atmospheric effect so nothing the player is meant to read can be covered by
 * something meant to hide distance.
 *
 * This covers the BG side only. Fixed-tier OBJ -- the menu's icons and the
 * cursors -- are deferred with them by tier rather than by plane, because they
 * share the OBJ ranks with world billboards that must stay under the shroud.
 *
 * BG3 both ranks, plus BG2 **high**. The split is not a guess: §11 of
 * docs/rendering-engine.md records the ownership from a capture -- BG3 carries
 * the text, BG2 carries the visible box frame -- and deferring BG3 alone lifted
 * the text and icons while leaving the panel fill under the cloud shroud,
 * which is exactly how it looked.
 *
 * BG2 **low** is deliberately left at its rank. The presentation order in the
 * plan places it *behind* the projected ground, where it is a background
 * layer rather than UI; promoting it would put whatever a town keeps there on
 * top of everything. If a menu ever appears with part of its frame still under
 * the clouds, that is the plane to look at next -- but it needs evidence
 * first, not a widened predicate.
 *
 * Safe for BG3 because the town HUD's own BG3 pixels are already removed from
 * the profile by the sim3d.c overlay handoff and composited separately after
 * this. */
static bool SimPlaneIsMenu(int plane) {
  return plane == kSim3DPlane_Bg3Low || plane == kSim3DPlane_Bg3High ||
      plane == kSim3DPlane_Bg2High;
}

static void RenderSimProfile(const FrameSlot *slot,
                             SimRenderFeatureMask features,
                             SDL_Rect source, SDL_Rect viewport,
                             const SDL_Rect *clip) {
  SDL_SetRenderClipRect(g_renderer, clip);
  bool separated = (features & kSimFeature_SeparatedComposite) != 0;
  bool ground = (features & kSimFeature_GroundProjection) != 0;
  bool billboards = ground &&
      (features & kSimFeature_ObjectBillboards) != 0;
  bool virtual_height = billboards &&
      (features & kSimFeature_VirtualHeight) != 0;
  bool shadows = billboards && (features & kSimFeature_Shadows) != 0;
  bool soft_shadows = shadows && (features & kSimFeature_SoftShadows) != 0;
  bool rim_light = billboards && (features & kSimFeature_RimLight) != 0;
  bool effect_lighting = ground &&
      (features & kSimFeature_EffectLighting) != 0;
  bool particles = ground && (features & kSimFeature_Particles) != 0;
  bool underlay = ground && (features & kSimFeature_WorldUnderlay) != 0;
  bool clouds = underlay && (features & kSimFeature_CloudShroud) != 0;
  bool cull_haze = underlay && (features & kSimFeature_CullHaze) != 0;
  bool atmospheric_backdrop = (features & kSimFeature_Backdrop) != 0;
  /* The lit region is ground-painted and can only express the height-zero
   * boundary, so its bottom edge is pulled in by the largest lift the
   * classifier hands out. Zero when nothing is being lifted at all -- with
   * VirtualHeight off the ground boundary is already exactly right. */
  int lift_inset = (slot->sim.cull_lift_inset && virtual_height)
      ? Sim3D_MaxDrawLift(slot->sim.height_scale_x100) : 0;
  if (!separated) {
    SDL_FRect src = ToFRect(source), dst = ToFRect(viewport);
    SDL_RenderTexture(g_renderer, g_texture, &src, &dst);
    return;
  }
  if (!ground) {
    SDL_FRect src = ToFRect(source), dst = ToFRect(viewport);
    SDL_RenderTexture(g_renderer, g_sim3d_flat_texture, &src, &dst);
    return;
  }

  uint32_t backdrop = slot->sim.separated_backdrop_argb;
  SDL_SetRenderDrawColor(g_renderer, (backdrop >> 16) & 0xff,
                        (backdrop >> 8) & 0xff, backdrop & 0xff, 255);
  SDL_RenderFillRect(g_renderer, &(SDL_FRect){
      (float)viewport.x, (float)viewport.y,
      (float)viewport.w, (float)viewport.h });

  Scene3DCamera camera = {
    .tilt_x = (float)slot->sim.projection_pitch_mrad / 1000.0f,
    .tilt_y = (float)slot->sim.projection_yaw_mrad / 1000.0f,
    .distance = (float)slot->sim.projection_distance_x100 / 100.0f,
    .fov_y = 0.4f,
  };
  if (camera.distance <= 0.0f)
    camera.distance = Scene3D_AutoFitDistance(camera.fov_y);
  else if (camera.distance < 2.0f)
    camera.distance = 2.0f;

  /* Before the matrix is built, so every stage -- ground, billboards,
   * shadows, the cull boundary, the shroud -- sees one camera. Adjusting the
   * matrix afterwards would leave the object anchors on the old one. */
  ApplySimDynamicCamera(slot, &camera);

  /* Object anchors must use the exact same view/projection transform as the
   * ground mesh. Keeping the matrix at profile scope also prevents camera
   * zoom or pitch from introducing a separate sprite-space approximation. */
  float matrix[16];
  Scene3D_BuildViewProjection(&camera, viewport.w, viewport.h, matrix);

  uint32_t enabled_planes = slot->sim.diagnostic_layer_mask
      ? slot->sim.diagnostic_layer_mask
      : (1u << kSim3DPlane_Count) - 1;
  bool fade_ground_planes = cull_haze &&
      (slot->sim.cull_haze_pct != 0 || slot->sim.cull_dim_pct != 0);

  /* The gradient needs the projected horizon, so it follows the matrix rather
   * than the clear above. The flat fill stays as the base: it costs one
   * rectangle and guarantees no pixel is ever left undefined if the gradient
   * declines to draw. */
  if (atmospheric_backdrop)
    DrawSimBackdrop(slot, viewport, matrix);

  /* Straight after the backdrop clear and before any captured layer: the
   * extension is ground the town is standing on the middle of, so everything
   * the town itself draws belongs on top of it. */
  if (underlay) {
    DrawSimWorldUnderlay(slot, source, viewport, matrix, lift_inset);
    /* Keep the canvas as the opaque backing for transparent BG1 priority
     * pixels, but omit it under BG1 wherever either alpha handoff is active.
     * That applies each feather once instead of twice on one side of the
     * captured-quad seam and once on the other. */
    bool live_ground_enabled =
        ((enabled_planes & (1u << kSim3DPlane_Bg1Low)) &&
         g_sim3d_layer_textures[kSim3DPlane_Bg1Low]) ||
        ((enabled_planes & (1u << kSim3DPlane_Bg1High)) &&
         g_sim3d_layer_textures[kSim3DPlane_Bg1High]);
    SDL_FRect live_ground = ToFRect(source);
    DrawSimTownCanvas(slot, source, viewport, matrix, cull_haze, lift_inset,
                      live_ground_enabled ? &live_ground : NULL);
  }

  SDL_FRect src = ToFRect(source), dst = ToFRect(viewport);
  float town_extent_x0 =
      (float)slot->sim.underlay_screen_x0 - (float)slot->sim.camera_x;
  float town_extent_y0 = -(float)slot->sim.camera_y;
  SimCullFade ground_fade = {
    .lead = slot->sim.cull_haze_lead_px ? slot->sim.cull_haze_lead_px
                                        : kSimCullHazeLeadDefaultPx,
    .corner = slot->sim.cull_corner_px,
    .lift_inset = lift_inset,
    .fade = cull_haze ? (float)slot->sim.cull_haze_pct / 100.0f : 0.0f,
    .dim = cull_haze ? (float)slot->sim.cull_dim_pct / 100.0f : 0.0f,
    .extent_x0 = town_extent_x0,
    .extent_y0 = town_extent_y0,
    .extent_x1 = town_extent_x0 + (float)kSimTownCanvasPixels,
    .extent_y1 = town_extent_y0 + (float)kSimTownCanvasPixels,
    .extent_feather = underlay ? (float)kSimTownExtentFeatherPixels : 0.0f,
    .margin_left = slot->sim.sprite_margin_left,
    .margin_right = slot->sim.sprite_margin_right,
    .screen_x0 = slot->sim.underlay_screen_x0,
  };
  for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
    /* Ground illumination belongs above the complete visible BG1 ground but
     * below the highest world-object rank. Keeping this seam explicit avoids
     * the prototype's unconditional over-paint of every actor while retaining
     * the authentic painter order of lower object ranks and BG layers. */
    if (plane == kSim3DPlane_Obj3)
      DrawSimEffectLocalLighting(slot, effect_lighting, source, viewport,
                                 &camera, matrix);
    if (!(enabled_planes & (1u << plane))) continue;
    if (SimPlaneIsMenu(plane)) continue;
    if (billboards) {
      int object_priority = -1;
      for (int priority = 0; priority < 4; priority++)
        if (plane == Sim3D_ObjPlaneForPriority(priority)) {
          object_priority = priority;
          break;
        }
      if (object_priority >= 0) {
        DrawSimObjectPriority(slot, object_priority, kSimTierFilter_World,
                              true, virtual_height,
                              source, viewport, &camera, matrix, NULL);
        if (rim_light)
          DrawSimRimLight(slot, object_priority, virtual_height, source,
                          viewport, &camera, matrix);
        continue;
      }
    }
    SDL_Texture *texture = g_sim3d_layer_textures[plane];
    if (!texture) continue;
    if (plane == kSim3DPlane_Bg1Low || plane == kSim3DPlane_Bg1High) {
      DrawSimGroundPlane(texture, source, viewport, matrix,
                         (fade_ground_planes || underlay)
                             ? &ground_fade : NULL);
      /* Ground first, mask immediately after, everything else on top: the
       * shadow can only ever darken ground pixels. */
      if (plane == kSim3DPlane_Bg1Low && shadows)
        DrawSimShadowMask(slot, virtual_height, soft_shadows, source,
                          viewport, matrix);
    } else
      SDL_RenderTexture(g_renderer, texture, &src, &dst);
  }

  /* Scene flash intentionally reaches the completed world; sparks are
   * emissive foreground detail. Both remain below atmospheric cover and every
   * fixed-tier menu plane, so neither can tint UI. */
  DrawSimEffectSceneFlash(slot, effect_lighting, viewport);
  DrawSimEffectParticles(slot, particles, source, viewport, &camera, matrix);

  /* Over the objects. The shroud's whole purpose is to cover ground that can
   * never hold an actor, so anything it hides must be hidden completely --
   * including a sprite that strays under its edge. */
  if (clouds)
    DrawSimCloudShroud(slot, source, viewport, matrix);

  /* The menu planes last of all, held back from the painter-order loop above.
   * They are the only thing in the profile that is not part of the world:
   * dialogue, the sim command menus, PAUSE. Leaving them in rank order put
   * them under the shroud, so a cloud bank could drift across a menu the
   * player is reading -- and unlike a sprite that is not an artifact the
   * cover is meant to hide, it is the cover damaging something in front of
   * the camera entirely.
   *
   * Drawn in rank order among themselves, so the box frame still composites
   * under its own text. Promoting them is a change of depth, not of painter
   * order within the group. */
  for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
    if (!(enabled_planes & (1u << plane))) continue;
    /* Full hardware rank is walked, not just the menu planes, because the
     * menu's icons are fixed-tier OBJ drawn through the billboard path and
     * they rank ABOVE the BG2 panel that backs them. Deferring the panel on
     * its own put its opaque fill over them and the menu came out empty.
     *
     * Order within the group is therefore still hardware order; only the
     * group's depth relative to the world has changed. */
    int object_priority = -1;
    for (int priority = 0; priority < 4; priority++)
      if (plane == Sim3D_ObjPlaneForPriority(priority)) {
        object_priority = priority;
        break;
      }
    if (object_priority >= 0) {
      if (billboards)
        DrawSimObjectPriority(slot, object_priority, kSimTierFilter_Fixed,
                              true, virtual_height, source, viewport,
                              &camera, matrix, NULL);
      continue;
    }
    if (!SimPlaneIsMenu(plane)) continue;
    SDL_Texture *texture = g_sim3d_layer_textures[plane];
    if (texture) SDL_RenderTexture(g_renderer, texture, &src, &dst);
  }

  /* Over the shroud deliberately: the question the markers answer is whether
   * cover exists where a record is being taken away, and a marker hidden by
   * the very cover under test cannot answer it. */
  DrawSimCullMarkers(slot, source, viewport, matrix, lift_inset);
}

static void PresentSim3D(const FrameSlot *slot) {
  static bool logged_ground_profile;
  if (!logged_ground_profile &&
      (slot->sim.effective_features & kSimFeature_GroundProjection)) {
    logged_ground_profile = true;
    fprintf(stderr,
            "[sim3d-d3] present features=$%04x camera=%d,%d,%u\n",
            (unsigned)slot->sim.effective_features,
            (int)slot->sim.projection_pitch_mrad,
            (int)slot->sim.projection_yaw_mrad,
            (unsigned)slot->sim.projection_distance_x100);
  }

  SDL_SetRenderLogicalPresentation(g_renderer, 0, 0,
                                   SDL_LOGICAL_PRESENTATION_DISABLED);
  SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
  SDL_RenderClear(g_renderer);
  SDL_Rect viewport = ComputePresentationViewport(
      g_renderer, slot->ws_active, slot->ignore_aspect_ratio,
      slot->pixel_aspect, slot->visible_width, slot->snes_height);
  SDL_Rect source = { slot->visible_x0, 0,
                      slot->visible_width, slot->snes_height };

  RenderSimProfile(slot, slot->sim.effective_features, source, viewport,
                   &viewport);
  SDL_SetRenderClipRect(g_renderer, NULL);

  /* A full SIM capture temporarily supersedes the normal widescreen town-HUD
   * owners. sim3d.c republishes their exact buffers and removes those pixels
   * from the SIM profile, so the established anchored compositor remains the
   * single HUD presentation path for both the flat and projected views. */
  PresentHudOverlayComposited(slot, viewport);
  PresentSceneInspector(slot, viewport);
  PresentCheatBadge(slot, viewport);
  SettingsOverlay_Render(viewport);
  ApplyLogicalPresentation(slot);
}

static bool DrawWorldNavigationMasterFade(
    const FrameSlot *slot, SDL_Rect viewport) {
  const uint8_t alpha = SimWorldNavigationScene_MasterFadeAlpha(
      slot->sim.world_navigation_brightness);
  if (!alpha) return true;
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, alpha);
  const bool ok = SDL_RenderFillRect(g_renderer, &(SDL_FRect){
      (float)viewport.x, (float)viewport.y,
      (float)viewport.w, (float)viewport.h});
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_NONE);
  return ok;
}

static bool PresentWorldNavigation3D(const FrameSlot *slot) {
  const SimWorldNavigationScene *scene =
      &slot->sim.world_navigation_scene;
  const SimWorldNavigationComposition *composition = &scene->composition;
  if (!scene->valid || !composition->valid ||
      !s_world_navigation_composition_upload_valid)
    return false;
  SDL_Texture *world = EnsureSimUnderlayTexture(slot);
  if (!world) return false;
  if (!composition->empty_animation &&
      (!s_world_navigation_palace_texture ||
       !s_world_navigation_ui_texture))
    return false;

  SDL_Rect viewport = ComputePresentationViewport(
      g_renderer, slot->ws_active, slot->ignore_aspect_ratio,
      slot->pixel_aspect, slot->visible_width, slot->snes_height);
  SDL_SetRenderLogicalPresentation(g_renderer, 0, 0,
                                   SDL_LOGICAL_PRESENTATION_DISABLED);
  SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
  SDL_RenderClear(g_renderer);
  SDL_SetRenderClipRect(g_renderer, &viewport);
  if (slot->sim.world_navigation_backdrop)
    DrawSimBackdrop(slot, viewport, NULL);
  if (!DrawWorldNavigationGround(slot, viewport, world)) {
    SDL_SetRenderClipRect(g_renderer, NULL);
    ApplyLogicalPresentation(slot);
    return false;
  }
  if (!DrawWorldNavigationLightTreatment(slot, viewport)) {
    SDL_SetRenderClipRect(g_renderer, NULL);
    ApplyLogicalPresentation(slot);
    return false;
  }
  if (!DrawWorldNavigationActiveRegionHaze(slot, viewport)) {
    SDL_SetRenderClipRect(g_renderer, NULL);
    ApplyLogicalPresentation(slot);
    return false;
  }

  /* Whole-world weather uses the same affine map as the ground. It has no
   * town sprite-window hole or cull boundary: every part of this world is
   * intentional content. The authentic top-down Palace stays over it, and
   * the location label/frame stays screen-space and last. */
  if (!DrawWorldNavigationWeather(slot, viewport)) {
    SDL_SetRenderClipRect(g_renderer, NULL);
    ApplyLogicalPresentation(slot);
    return false;
  }
  /* INIDISP is a master brightness applied after the PPU has composed every
   * layer. Do the same for the host-owned world and all its effects. The
   * Palace/UI captures are drawn afterward because PpuRasterizeObjRange has
   * already applied this frame's brightness to their pixels. */
  if (!DrawWorldNavigationMasterFade(slot, viewport)) {
    SDL_SetRenderClipRect(g_renderer, NULL);
    ApplyLogicalPresentation(slot);
    return false;
  }
  if (!composition->empty_animation &&
      (!DrawWorldNavigationCompositionLayer(
           slot, viewport, &composition->palace,
           s_world_navigation_palace_texture) ||
       !DrawWorldNavigationCompositionLayer(
           slot, viewport, &composition->ui,
           s_world_navigation_ui_texture))) {
    SDL_SetRenderClipRect(g_renderer, NULL);
    ApplyLogicalPresentation(slot);
    return false;
  }
  SDL_SetRenderClipRect(g_renderer, NULL);
  PresentSceneInspector(slot, viewport);
  PresentCheatBadge(slot, viewport);
  SettingsOverlay_Render(viewport);
  ApplyLogicalPresentation(slot);
  return true;
}

void PresentComposite(const FrameSlot *slot,
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
     * scaled by reactive_strength/100 (0 disables sway, exactly reproducing
     * B4-baseline's "snaps to the fixed pose" test). */
    bool dynamic = slot->diorama_camera_mode == kDioramaCam_Dynamic;
    DioramaCameraPose target;
    if (dynamic) {
      float gain = (float)slot->diorama_reactive_strength / 100.0f;
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
     * capture (not a present-thread redraw of one already processed — see
     * the FrameSlot field comment, present.h), and only in Dynamic Cam
     * (event kicks are part of the reactive system, same scoping as
     * vellean/pan). Impulses stack additively (a hit while already mid-jolt
     * gets stronger, not replaced) so back-to-back events still read. Decay
     * runs every present frame regardless, on the same wall-clock exponential
     * basis as the position damping above. */
    bool new_slot = dynamic && slot->timestamp_ns != g_diorama_last_slot_ns;
    g_diorama_last_slot_ns = slot->timestamp_ns;
    if (new_slot) {
      float gain = (float)slot->diorama_reactive_strength / 100.0f;
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
        (float)slot->diorama_reactive_strength / 100.0f,
        slot->diorama_dyncam_lean_yaw, slot->diorama_dyncam_lean_pitch,
        target.tilt_x, target.tilt_y, target.distance,
        g_diorama_render_cam.tilt_x, g_diorama_render_cam.tilt_y,
        g_diorama_render_cam.distance,
        g_diorama_kick_pitch, g_diorama_kick_zoom,
        slot->diorama_dyncam_event_hit, slot->diorama_dyncam_event_land,
        slot->diorama_dyncam_event_boost);
    }

    /* Fix B (SPEC-backdrop-clip.md): resolve BG2's valid captured span from the
     * slot alone (D6 — this file never reads live g_ppu). ws_extra, not
     * extra_left_right, is the offset: the capture pitch and Diorama_Upload's
     * rect are both derived from ws_extra, so it is what texture column 0
     * corresponds to. They are equal today; keeping them distinct is what makes
     * that stay true if either ever moves. */
    int bg2_valid_x0 = 0, bg2_valid_x1 = kFrameSlotLayerTextureWidth;
    /* + obj_apron: the span is in SURFACE columns, and screen x = 0 sits at
     * column obj_apron + ws_extra now that the surfaces carry resolve headroom
     * on both sides. Without it the skybox would crop its sky an apron early. */
    DioramaBg2ValidSpan(slot->ws_extra + slot->obj_apron,
                        slot->extra_left_right,
                        slot->extra_left_cur, slot->extra_right_cur,
                        slot->bg2_margin_source, kFrameSlotLayerTextureWidth,
                        &bg2_valid_x0, &bg2_valid_x1);
    DioramaProjection action_projection;
    if (!Diorama_Composite(g_renderer, slot->snes_width,
                           slot->snes_height + slot->ws_extra_top,
                           slot->obj_apron,
                           slot->pixel_aspect, slot->ignore_aspect_ratio,
                           slot->visible_width, g_diorama_textures, pixels,
                           &scroll_delta, &final_cam, distance_scale,
                           bg2_valid_x0, bg2_valid_x1, &action_projection))
      return;
    int out_w = 0, out_h = 0;
    SDL_GetRenderOutputSize(g_renderer, &out_w, &out_h);
    SDL_Rect viewport = { 0, 0, out_w, out_h };
    DrawActionEffects(slot, viewport, &action_projection);
    /* A7/A5 (followup doc): the diorama branch used to skip the widescreen
     * HUD anchoring entirely — BG3 (ACT/TIME/SCORE, HP, boss health)
     * rendered only as an unanchored, centered 256-wide tilted plane. With
     * diorama_hud_flat on (default), the capture side (actraiser_rtl.c) no
     * longer rebinds BG3 into the diorama layer buffer, leaving the same
     * RemoveFromGame HUD-split capture flat mode uses standing (->
     * g_hud_bg_pixels/g_hud_bg_texture). A straight PresentHudOverlay port
     * produced visible seams (see PresentHudOverlayComposited's comment) —
     * reconstruct into one texture first, then draw it as a plain screen
     * overlay. Diorama_Composite (above) already disabled logical
     * presentation, matching the precondition ApplyLogicalPresentation
     * establishes for the flat branch's PresentHudOverlay call.
     *
     * With diorama_hud_flat off (A5's A/B option), the capture side instead
     * rebinds BG3 into the diorama layer buffer (the pre-A7 behavior) so it
     * renders as the ordinary tilted BG3 plane in Diorama_Composite's own
     * per-layer loop above — skip the anchored overlay entirely here so the
     * two don't both draw a HUD. */
    if (slot->diorama_hud_flat)
      PresentHudOverlayComposited(slot, viewport);
    PresentSceneInspector(slot, viewport);
    PresentCheatBadge(slot, viewport);
    SettingsOverlay_Render(viewport);
    return;
  }

  ApplyLogicalPresentation(slot);
  SDL_RenderClear(g_renderer);
  SDL_Rect src = { slot->visible_x0, 0, slot->visible_width, slot->snes_height };
  SDL_FRect src_f = ToFRect(src);
  SDL_RenderTexture(g_renderer, g_texture, &src_f, NULL);

  SDL_Rect viewport = ComputePresentationViewport(
      g_renderer, slot->ws_active, slot->ignore_aspect_ratio,
      slot->pixel_aspect, slot->visible_width, slot->snes_height);
  SDL_SetRenderLogicalPresentation(g_renderer, 0, 0,
                                   SDL_LOGICAL_PRESENTATION_DISABLED);
  PresentMode7Composite(slot, viewport);
  DrawActionEffects(slot, viewport, NULL);
  PresentHudOverlay(slot, viewport);
  PresentHdReplacements(slot, viewport);
  PresentSceneInspector(slot, viewport);
  PresentCheatBadge(slot, viewport);
  SettingsOverlay_Render(viewport);
  ApplyLogicalPresentation(slot);
}
