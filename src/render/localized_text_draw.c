/* Submission half of the localized text presenter. Everything here works from
 * an already prepared frame and the render device: no font work, no cache, no
 * presenter state. The prepared frame is the whole interface, which is what
 * makes it safe to submit a retained frame while the game thread is building
 * the next one. */
#include "render/localized_text_presenter.h"

#include "render/localized_text_layout.h"

static bool DrawContinueIndicator(
    ArRenderDevice *device, const ArLocalizedPreparedIndicator *indicator, float brightness) {
  const ArRenderRectI bounds = indicator->destination;
  const ArRenderRectF destination = {bounds.x, bounds.y, bounds.w, bounds.h};
  return ArRenderTexture_IsValid(indicator->texture) &&
      ArRenderDevice_DrawTextureTinted(device, indicator->texture, NULL, &destination,
          (ArRenderColorF){brightness, brightness, brightness, 1});
}

static bool DrawTriangle(ArRenderDevice *device,
                         ArRenderPointF first,
                         ArRenderPointF second,
                         ArRenderPointF third,
                         ArRenderColorF color) {
  const ArRenderVertex2D vertices[] = {
      {first, color, {0, 0}}, {second, color, {0, 0}},
      {third, color, {0, 0}},
  };
  const int32_t indices[] = {0, 1, 2};
  return ArRenderDevice_DrawGeometry(
      device, ArRenderTexture_Invalid(), vertices, 3, indices, 3);
}

static bool DrawInlineObject(
    ArRenderDevice *device, const ArLocalizedPreparedInlineObject *object, float brightness) {
  if (!device || !object || object->destination.w <= 0 ||
      object->destination.h <= 0)
    return false;
  const ArRenderRectI bounds = object->destination;
  /* An object that carries native artwork draws it; the shapes below are the
   * fallback for the objects the game has no tile for, and for a keyboard
   * whose glyphs were not on screen to capture. */
  if (ArRenderTexture_IsValid(object->texture)) {
    const ArRenderRectF destination = {
      (float)bounds.x, (float)bounds.y, (float)bounds.w, (float)bounds.h,
    };
    return ArRenderDevice_DrawTextureTinted(
        device, object->texture, NULL, &destination,
        (ArRenderColorF){brightness, brightness, brightness, 1});
  }
  const float x = (float)bounds.x;
  const float y = (float)bounds.y;
  const float w = (float)bounds.w;
  const float h = (float)bounds.h;
  const float thickness = w > 12.0f ? w * 0.12f : 1.0f;
  const ArRenderColorF blue = {0.12f * brightness, 0.29f * brightness, 0.68f * brightness, 1.0f};
  const ArRenderColorF white = {0.96f * brightness, 0.98f * brightness, brightness, 1.0f};
  switch (object->kind) {
    case kArLocalizationInlineObject_SelectionPointer:
      return DrawTriangle(
                 device,
                 (ArRenderPointF){x + w * 0.14f, y + h * 0.12f},
                 (ArRenderPointF){x + w * 0.14f, y + h * 0.88f},
                 (ArRenderPointF){x + w * 0.90f, y + h * 0.50f}, blue) &&
          DrawTriangle(
                 device,
                 (ArRenderPointF){x + w * 0.25f, y + h * 0.28f},
                 (ArRenderPointF){x + w * 0.25f, y + h * 0.72f},
                 (ArRenderPointF){x + w * 0.70f, y + h * 0.50f}, white);
    /* Last resort, and deliberately not an imitation. The game draws these
     * keys as the tiny letter pairs "Bs" and "Ed"; a shape drawn here would be
     * inventing a different affordance, and nothing legible fits one key cell
     * anyway. These marks only appear if the capture failed -- which needs the
     * keyboard to be composed without its own VRAM -- and exist so the key is
     * never blank. */
    case kArLocalizationInlineObject_NameBackspace: {
      const ArRenderRectF stem =
          {x + w * 0.30f, y + h * 0.42f, w * 0.55f, h * 0.18f};
      return ArRenderDevice_DrawSolidRect(
                 device, &stem, white, kArRenderBlendMode_Alpha) &&
          DrawTriangle(
              device, (ArRenderPointF){x + w * 0.12f, y + h * 0.50f},
              (ArRenderPointF){x + w * 0.44f, y + h * 0.18f},
              (ArRenderPointF){x + w * 0.44f, y + h * 0.82f}, white);
    }
    case kArLocalizationInlineObject_NameFinish:
      return ArRenderDevice_DrawLine(
                 device,
                 (ArRenderPointF){x + w * 0.16f, y + h * 0.54f},
                 (ArRenderPointF){x + w * 0.40f, y + h * 0.78f},
                 thickness, blue, kArRenderBlendMode_Alpha) &&
          ArRenderDevice_DrawLine(
                 device,
                 (ArRenderPointF){x + w * 0.40f, y + h * 0.78f},
                 (ArRenderPointF){x + w * 0.86f, y + h * 0.20f},
                 thickness, white, kArRenderBlendMode_Alpha);
    case kArLocalizationInlineObject_NameFieldUnderline: {
      const ArRenderRectF lower = {x, y, w, h};
      const ArRenderRectF upper = {x, y, w, h * 0.5f};
      return ArRenderDevice_DrawSolidRect(
                 device, &lower, blue, kArRenderBlendMode_Alpha) &&
          ArRenderDevice_DrawSolidRect(
                 device, &upper, white, kArRenderBlendMode_Alpha);
    }
    default:
      return false;
  }
}

static bool DrawTextPiece(ArRenderDevice *device,
                          const ArLocalizedPreparedText *text,
                          ArRenderRectI source, ArRenderRectI target, float brightness) {
  if (text->viewport.w > 0 &&
      !ArLocalizedTextLayout_Clip(text->viewport, &source, &target))
    return true;
  const ArRenderRectF source_f = {
      (float)source.x, (float)source.y, (float)source.w, (float)source.h};
  const ArRenderRectF target_f = {
      (float)target.x, (float)target.y, (float)target.w, (float)target.h};
  return ArRenderDevice_DrawTextureTinted(
      device, text->surface.texture, &source_f, &target_f,
      (ArRenderColorF){brightness, brightness, brightness, 1});
}

/* Extends `run` with `next` when the two are the same row of the same surface
 * and touch edge to edge. Merging only exactly abutting rectangles keeps the
 * result pixel-identical: the union adds no pixel that was not already in one
 * of them, nothing is blended twice, and no unrevealed cluster can lie inside
 * it. Overlapping or gapped rectangles are left as separate draws. */
static bool ExtendRun(ArRenderRectI *run, ArRenderRectI next) {
  if (next.y != run->y || next.h != run->h)
    return false;
  if (next.x == run->x + run->w) {
    run->w += next.w;
    return true;
  }
  if (next.x + next.w == run->x) {
    run->x = next.x;
    run->w += next.w;
    return true;
  }
  return false;
}

/* Partially revealed text is drawn as runs of adjacent clusters rather than
 * one textured quad per character, so submission cost scales with lines and
 * runs instead of with the number of visible glyphs. Whole-string shaping,
 * reveal timing and clipping are unchanged. */
static bool DrawRun(ArRenderDevice *device,
                    const ArLocalizedPreparedText *text, ArRenderRectI source,
                    int shift, float brightness) {
  const ArRenderRectI target = {
      text->destination.x + source.x + shift, text->destination.y + source.y,
      source.w, source.h,
  };
  return DrawTextPiece(device, text, source, target, brightness);
}

/* `shifts` carries one horizontal offset per cluster for text laid out on a
 * uniform key pitch, and is NULL for text that draws where it was shaped. */
static bool DrawOne(ArRenderDevice *device,
                    const ArLocalizedPreparedText *text, const int *shifts,
                    float brightness) {
  const ArRenderRectI destination = text->destination;
  size_t revealed = text->revealed_cluster_count;
  if (revealed > text->surface.reveal_cluster_count)
    revealed = text->surface.reveal_cluster_count;
  if (!shifts && revealed >= text->surface.reveal_cluster_count)
    return DrawTextPiece(device, text,
        (ArRenderRectI){0, 0, destination.w, destination.h}, destination, brightness);
  ArRenderRectI run = {0, 0, 0, 0};
  int run_shift = 0;
  bool have_run = false;
  for (size_t index = 0; index < revealed; ++index) {
    const ArTextRevealCluster *cluster =
        &text->surface.reveal_clusters[index];
    if (cluster->width <= 0 || cluster->height <= 0) continue;
    const int shift = shifts ? shifts[index] : 0;
    const ArRenderRectI source = {
        cluster->x, cluster->y, cluster->width, cluster->height,
    };
    if (have_run && shift == run_shift && ExtendRun(&run, source))
      continue;
    if (have_run && !DrawRun(device, text, run, run_shift, brightness))
      return false;
    run = source;
    run_shift = shift;
    have_run = true;
  }
  return !have_run || DrawRun(device, text, run, run_shift, brightness);
}

bool ArLocalizedTextPresenter_Draw(
    ArRenderDevice *device, const ArLocalizedPreparedFrame *prepared) {
  return ArLocalizedTextPresenter_DrawWithBrightness(device, prepared, 1);
}

bool ArLocalizedTextPresenter_DrawWithBrightness(
    ArRenderDevice *device, const ArLocalizedPreparedFrame *prepared, float brightness) {
  if (!device || !prepared || !(brightness >= 0 && brightness <= 1)) return false;
  for (uint8_t index = 0; index < prepared->text_count; ++index) {
    const ArLocalizedPreparedText *text = &prepared->texts[index];
    const int32_t offset = text->cluster_shift_offset;
    const bool shifted = offset >= 0 &&
        (size_t)offset + text->surface.reveal_cluster_count <=
            prepared->cluster_shift_count;
    if (!DrawOne(device, text,
                 shifted ? &prepared->cluster_shifts[offset] : NULL,
                 brightness))
      return false;
  }
  for (uint8_t index = 0; index < prepared->decoration_count; ++index) {
    const ArLocalizedPreparedDecoration *decoration = &prepared->decorations[index];
    const ArRenderRectI r = decoration->destination;
    const ArRenderRectF target = {r.x, r.y, r.w, r.h};
    if (!ArRenderDevice_DrawTextureTinted(device, decoration->texture, NULL, &target,
          (ArRenderColorF){brightness, brightness, brightness, 1})) return false;
  }
  for (uint8_t index = 0; index < prepared->indicator_count; ++index) {
    const ArLocalizedPreparedIndicator *indicator =
        &prepared->indicators[index];
    if (indicator->kind == kArLocalizationIndicator_DialogueContinue &&
        !DrawContinueIndicator(device, indicator, brightness))
      return false;
  }
  for (uint8_t index = 0; index < prepared->inline_object_count; ++index) {
    if (!DrawInlineObject(device, &prepared->inline_objects[index], brightness))
      return false;
  }
  return true;
}
