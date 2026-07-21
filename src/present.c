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
#include <string.h>
#include "present.h"
#include "types.h"
#include "diorama.h"
#include "diorama_planes.h"
#include "settings_overlay.h"
#include "scene_inspector.h"

/* kPixelAspect_Crt43 and kDioramaCam_Free/kDioramaCam_Dynamic are plain enum
 * constants (not live state) — fine to pull in just for those. */
#include "settings.h"

extern SDL_Renderer *g_renderer;
extern SDL_Texture *g_texture;
extern SDL_Texture *g_hud_bg_texture;
extern SDL_Texture *g_hud_obj_texture;
extern SDL_Texture *g_m7_texture;
extern uint8_t *g_m7_overlay_pixels;
extern uint8_t g_pixels[];
extern uint8_t g_hud_bg_pixels[];
extern uint8_t g_hud_obj_pixels[];
extern SDL_Texture *g_diorama_textures[kDioramaPlane_Count];
extern uint8_t *g_diorama_layer_pixels[kDioramaPlane_Count];

enum { kHdMode7Scale = 4 };  /* mirrors main.c's private enum */

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
 * main.c's InspectWindowPoint feeds it from live state. Ported verbatim from
 * the pre-M5 main.c version, just reading `inputs` instead of g_ppu/g_settings/
 * g_snes_width/g_snes_height directly. */
int BuildHudPresentationChunks(SDL_Rect viewport,
                               const HudProjectionInputs *in,
                               HudPresentationChunk *chunks) {
  if (!in->hud_bg_texture || !in->hud_split_height) return 0;

  int count = 0;
  double scale_y, scale_x;
  if (in->hud_scale_percent == 0) {
    scale_y = (double)viewport.h / in->snes_height;
    scale_x = (double)viewport.w / in->visible_width;
  } else {
    scale_y = in->hud_scale_percent / 100.0;
    scale_x = scale_y * (in->pixel_aspect == kPixelAspect_Crt43 ? 7.0 / 6.0 : 1.0);
  }

  int tex_extra = (in->snes_width - 256) / 2;
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

  int right_source_w = 256 - in->hud_right_start;
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
    src.w = 256 - in->hud_right_start;
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
    src.w = 256;
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

  /* Action's selected-magic icon (4 OAM, 16x16), simulation's hourglass
   * (4 OAM, 16x16), and Sky Palace's magic icon (2 OAM, 16x8) are separately
   * validated OAM signatures; the caller resolves the icon x/y (from live
   * oam/highOam or the FrameSlot snapshot) and passes it in already
   * resolved, so this function stays free of oam[]/highOam[] entirely. */
  if (in->hud_obj_texture && in->obj_icon_valid && in->obj_icon_x < 256) {
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

  const FrameSlotOverlayCapture *obj_capture =
      &slot->overlay_captures[kFrameSlotOverlay_Obj];
  if (slot->oam_valid && obj_capture->oamCount == 4) {
    int first = obj_capture->oamFirst;
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
  SDL_SetRenderTarget(g_renderer, NULL);

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
  int extra = (slot->snes_width - 256) / 2;
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
  SDL_RenderLine(g_renderer, (float)(px - 7), (float)py, (float)(px + 7), (float)py);
  SDL_RenderLine(g_renderer, (float)px, (float)(py - 7), (float)px, (float)(py + 7));

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
    Diorama_Upload(g_diorama_textures, pixels, slot->snes_width,
                   slot->snes_height);
  } else {
    SDL_Rect upload = { 0, 0, slot->snes_width, slot->snes_height };
    SDL_UpdateTexture(g_texture, &upload, g_pixels, slot->snes_width * 4);
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
    const FrameSlot *curr, const DioramaScrollSnapshot *prev) {
  DioramaScrollDelta d = {0};
  if (!prev || !curr) return d;
  if (!prev->diorama_active) return d;
  if (curr->turbo_active || prev->turbo_active) return d;      /* §6.4 */
  if (curr->timestamp_ns == 0 || prev->timestamp_ns == 0) return d;
  if (curr->bg_mode != prev->bg_mode) return d;                /* §6.4 mode change */
  if (curr->timestamp_ns <= prev->timestamp_ns) return d;      /* not a real pair yet */
  uint64_t span = curr->timestamp_ns - prev->timestamp_ns;
  if (span >= 50000000ULL) return d;                            /* §6.2 sanity: <50ms */
  uint64_t now = SDL_GetTicksNS();
  if (now < curr->timestamp_ns) return d;
  float t = (float)(now - curr->timestamp_ns) / (float)span;
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;                                       /* §6.4 turbo/frame-skip clamp */

  d.active = true;
  /* B1b (followup doc) source fix: the WRAM camera is a real world-pixel
   * position, not a 10-bit modular PPU register, so a plain signed
   * difference is exact — no wrap correction needed (the old ±512/1024
   * hScroll/vScroll fixup this replaced would itself corrupt a large
   * legitimate camera delta if kept). BG3/BG4 (index 2/3) have no WRAM
   * camera and stay at their zero-initialized d.bg_du/bg_dv — BG3 is the
   * HUD (composes with diorama_hud_flat's default, where BG3 isn't even a
   * tilted plane), and BG4 is unused in this game's Mode 1. */
  int dh1 = curr->bg1_camera_x - prev->bg1_camera_x;
  int dv1 = curr->bg1_camera_y - prev->bg1_camera_y;
  d.bg_du[0] = (t * (float)dh1) / (float)curr->snes_width;
  d.bg_dv[0] = (t * (float)dv1) / (float)curr->snes_height;

  int dh2 = curr->bg2_camera_x - prev->bg2_camera_x;
  int dv2 = curr->bg2_camera_y - prev->bg2_camera_y;
  d.bg_du[1] = (t * (float)dh2) / (float)curr->snes_width;
  d.bg_dv[1] = (t * (float)dv2) / (float)curr->snes_height;

  /* AR_INTERP_LOG=1: log BG1's interpolated offset every present, so the M7
   * acceptance test (ar-recomp-threading-impl.md milestone M7) can assert
   * "monotonic sub-steps ~half the per-tick delta" mechanically instead of
   * by eye. */
  static int log_on = -1;
  if (log_on < 0) {
    const char *e = getenv("AR_INTERP_LOG");
    log_on = (e && e[0] && e[0] != '0') ? 1 : 0;
  }
  if (log_on) {
    fprintf(stderr, "[interp] t=%.3f bg1_du=%.5f bg1_dv=%.5f span_ns=%llu\n",
            t, d.bg_du[0], d.bg_dv[0], (unsigned long long)span);
  }
  return d;
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

void PresentComposite(const FrameSlot *slot,
                      const DioramaScrollSnapshot *prev_scroll) {
  if (!g_renderer || !g_texture) return;

  if (slot->diorama_active) {
    uint8_t *pixels[kDioramaPlane_Count];
    memcpy(pixels, g_diorama_layer_pixels, sizeof(pixels));
    pixels[kDioramaPlane_Backdrop] = g_pixels;
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
        ? ComputeDioramaScrollDelta(slot, prev_scroll)
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
      final_cam.tilt_x += g_diorama_kick_pitch;
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

    if (!Diorama_Composite(g_renderer, slot->snes_width, slot->snes_height,
                           slot->pixel_aspect, slot->ignore_aspect_ratio,
                           slot->visible_width, g_diorama_textures, pixels,
                           &scroll_delta, &final_cam, distance_scale))
      return;
    int out_w = 0, out_h = 0;
    SDL_GetRenderOutputSize(g_renderer, &out_w, &out_h);
    SDL_Rect viewport = { 0, 0, out_w, out_h };
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
  PresentHudOverlay(slot, viewport);
  PresentHdReplacements(slot, viewport);
  PresentSceneInspector(slot, viewport);
  SettingsOverlay_Render(viewport);
  ApplyLogicalPresentation(slot);
}
