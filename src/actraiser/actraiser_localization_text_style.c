#include "actraiser/actraiser_localization_text_style.h"
#include "actraiser/actraiser_localization_style.h"

#include <stdio.h>
#include <string.h>

static uint8_t Binding(const ArTextInk *ink) {
  if (ink->kind != kArTextInk_Binding)
    return kActRaiserTextInk_Literal;
  for (uint8_t i = 1; i < kActRaiserTextInk_Count; ++i)
    if (!strcmp(ink->binding, kArLanguageInkBindings[i].binding))
      return i;
  return kActRaiserTextInk_Count;
}

static void CaptureInk(ActRaiserTextPalette *palette, unsigned binding,
                       uint16_t color) {
  palette->rgb[binding] = ActRaiserLocalizationStyle_Rgb(color);
  palette->available |= (uint16_t)(1u << binding);
}

void ActRaiserTextPalette_Capture(ActRaiserTextPalette *palette,
                                  const uint16_t *cgram, size_t count) {
  *palette = (ActRaiserTextPalette){0};
  if (!cgram)
    return;
  if (count >= 4)
    memcpy(palette->dialogue, cgram, sizeof(palette->dialogue));
  for (unsigned i = 1; i < kActRaiserTextInk_Count; ++i) {
    const ArLanguageInkBindingContract *ink = &kArLanguageInkBindings[i];
    if (!ink->hud && ink->index < count)
      CaptureInk(palette, i, cgram[ink->index]);
  }
}

void ActRaiserTextPalette_SetHud(ActRaiserTextPalette *palette,
                                 const uint16_t colors[4]) {
  for (unsigned i = 1; i < kActRaiserTextInk_Count; ++i) {
    const ArLanguageInkBindingContract *ink = &kArLanguageInkBindings[i];
    if (ink->hud)
      CaptureInk(palette, i, colors[ink->index]);
  }
}

void ActRaiserTextPalette_SetLocation(ActRaiserTextPalette *palette,
                                      uint16_t shadow, uint16_t band,
                                      uint16_t body) {
  CaptureInk(palette, kActRaiserTextInk_LocationShadow, shadow);
  CaptureInk(palette, kActRaiserTextInk_LocationBand, band);
  CaptureInk(palette, kActRaiserTextInk_LocationBody, body);
}

static const ArTextTreatment *FindTreatment(void *context, const char *name) {
  const ArDialoguePageSnapshot *page = context;
  for (size_t i = 0; i < page->treatment_count; ++i)
    if (!strcmp(page->treatments[i].definition.name, name))
      return &page->treatments[i].definition;
  return NULL;
}

/* Validate host binding names during compilation; actual values are captured
 * each frame. Literal RGB and symbolic identity are kept in separate fields. */
static bool ValidateInk(void *context, const char *name, uint32_t *rgb) {
  (void)context;
  for (unsigned i = 1; i < kActRaiserTextInk_Count; ++i) {
    if (strcmp(name, kArLanguageInkBindings[i].binding))
      continue;
    *rgb = 0;
    return true;
  }
  return false;
}

static bool CompileStyle(const ArDialoguePageSnapshot *page,
                         const ArTextTemplateStyle *override,
                         ActRaiserTextStyle *style, char *error,
                         size_t capacity) {
  const ArTextTemplateStyle empty = {0};
  if (!override)
    override = &empty;
  const ArTextAppearanceBindings bindings = {
      .context = (void *)page,
      .find_treatment = FindTreatment,
      .resolve_ink = ValidateInk,
  };
  ArTextTemplateError problem = {0};
  *style = (ActRaiserTextStyle){0};
  if (!ArTextTemplate_ResolveAppearance(&page->default_style, override,
                                        page->numerals == 2, &bindings,
                                        &style->appearance, &problem)) {
    if (error && capacity)
      snprintf(error, capacity, "%s:%u: %s: %s", page->source_path,
               page->source_line, page->resolved_message_id, problem.message);
    return false;
  }
  const char *name = override->treatment[0] ? override->treatment
                                            : page->default_style.treatment;
  const ArTextTreatment *treatment =
      name[0] ? FindTreatment((void *)page, name) : NULL;
  if (treatment) {
    if (!override->has_color && !page->default_style.has_color) {
      style->band_binding = Binding(&treatment->band);
      style->body_binding = Binding(&treatment->body);
    }
    if (style->appearance.shadow_enabled)
      style->shadow_binding = Binding(&treatment->shadow);
  }
  return true;
}

static bool StyleEqual(const ActRaiserTextStyle *a,
                       const ActRaiserTextStyle *b) {
  const ArTextRunAppearance *x = &a->appearance, *y = &b->appearance;
  return !strcmp(x->font_role, y->font_role) &&
         x->scale_basis == y->scale_basis && x->band_rgb == y->band_rgb &&
         x->body_rgb == y->body_rgb && x->shadow_rgb == y->shadow_rgb &&
         x->shadow_enabled == y->shadow_enabled &&
         x->keyline_shadow == y->keyline_shadow && x->italic == y->italic &&
         x->slant_ascii_numerals == y->slant_ascii_numerals &&
         a->band_binding == b->band_binding &&
         a->body_binding == b->body_binding &&
         a->shadow_binding == b->shadow_binding;
}

static bool InternStyle(ActRaiserTextStylePlan *plan,
                        const ActRaiserTextStyle *style, uint8_t *index) {
  for (uint8_t i = 0; i < plan->style_count; ++i)
    if (StyleEqual(&plan->styles[i], style)) {
      *index = i;
      return true;
    }
  if (plan->style_count >= kActRaiserTextStyleCapacity)
    return false;
  *index = plan->style_count++;
  plan->styles[*index] = *style;
  return true;
}

static bool CopyOrigin(ArTextSourceOrigin *origin,
                       const ArDialoguePageSnapshot *page) {
  *origin = (ArTextSourceOrigin){.source_line = page->source_line};
  const struct {
    char *target;
    size_t capacity;
    const char *source;
  } fields[] = {
      {origin->source_path, sizeof(origin->source_path), page->source_path},
      {origin->message_id, sizeof(origin->message_id), page->message_id},
      {origin->resolved_id, sizeof(origin->resolved_id),
       page->resolved_message_id},
      {origin->package_id, sizeof(origin->package_id), page->package_id},
  };
  for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
    const char *source = fields[i].source ? fields[i].source : "";
    const size_t bytes = strlen(source);
    if (bytes >= fields[i].capacity)
      return false;
    memcpy(fields[i].target, source, bytes + 1);
  }
  return true;
}

bool ActRaiserTextStyle_AppendPage(
    ActRaiserTextStylePlan *plan, const ArDialoguePageSnapshot *page,
    size_t source_offset, size_t source_bytes, const uint16_t *offsets,
    const char *normalized, size_t normalized_bytes, size_t destination_base,
    char *error, size_t capacity) {
  if (!plan || !page || !offsets || !normalized ||
      source_offset > page->utf8_bytes ||
      source_bytes > page->utf8_bytes - source_offset ||
      destination_base > UINT32_MAX)
    return false;
  if (!CopyOrigin(&plan->origin, page))
    return false;
  if (page->format_version < 2)
    return !plan->authored;
  ActRaiserTextStyle style;
  uint8_t index;
  if (!CompileStyle(page, NULL, &style, error, capacity))
    return false;
  if (!plan->authored) {
    plan->authored = true;
    plan->styles[0] = style;
    plan->style_count = 1;
  } else if (!StyleEqual(&plan->styles[0], &style))
    return false;
  for (size_t i = 0; i < page->style_span_count; ++i) {
    const ArTextTemplateSpan *span = &page->style_spans[i];
    if (span->end <= source_offset ||
        span->start >= source_offset + source_bytes)
      continue;
    const size_t a =
        span->start > source_offset ? span->start - source_offset : 0;
    const size_t b = span->end < source_offset + source_bytes
                         ? span->end - source_offset
                         : source_bytes;
    size_t first = offsets[a], last = offsets[b];
    if (first > normalized_bytes)
      first = normalized_bytes;
    if (last > normalized_bytes)
      last = normalized_bytes;
    /* The reveal map points before a pending collapsed separator. It belongs
     * to preceding whitespace, not a font/size span starting on this glyph. */
    const char first_byte = page->utf8[source_offset + a];
    if (first < last && normalized[first] == ' ' && first_byte != ' ' &&
        first_byte != '\t' && first_byte != '\r' && first_byte != '\n')
      ++first;
    if (first >= last)
      continue;
    if (last > UINT32_MAX - destination_base ||
        !CompileStyle(page, &span->style, &style, error, capacity))
      return false;
    if (!InternStyle(plan, &style, &index) ||
        plan->span_count >= kActRaiserTextStyleSpanCapacity) {
      if (error && capacity)
        snprintf(error, capacity,
                 "%s:%u: style plan exceeds %u distinct styles or %u spans",
                 page->source_path, page->source_line,
                 kActRaiserTextStyleCapacity, kActRaiserTextStyleSpanCapacity);
      return false;
    }
    plan->spans[plan->span_count++] =
        (ActRaiserTextStyleSpan){(uint32_t)(destination_base + first),
                                 (uint32_t)(destination_base + last), index};
  }
  return true;
}

void ActRaiserTextStyle_Edit(ActRaiserTextStylePlan *plan, uint32_t offset,
                             uint32_t removed, uint32_t inserted) {
  if (!plan)
    return;
  uint16_t kept = 0;
  for (uint16_t i = 0; i < plan->span_count; ++i) {
    ActRaiserTextStyleSpan span = plan->spans[i];
    if (span.start >= offset + removed)
      span.start = span.start - removed + inserted;
    else if (span.start > offset)
      span.start = offset + inserted;
    if (span.end > offset)
      span.end =
          span.end >= offset + removed ? span.end - removed + inserted : offset;
    if (span.start < span.end)
      plan->spans[kept++] = span;
  }
  plan->span_count = kept;
}

/* Pack loading validates each template's declared roles. A native fallback
 * message can name a role absent from the selected translation; that role
 * inherits the selected body stack, keeping fallback wording readable. */
static void UseAvailableFont(const ArLocalizationFrame *frame,
                             ArTextRunAppearance *appearance) {
  if (!strcmp(appearance->font_role, "body"))
    return;
  for (uint8_t i = 0; i < frame->font_role_count; ++i)
    if (!strcmp(appearance->font_role, frame->font_roles[i].name))
      return;
  snprintf(appearance->font_role, sizeof(appearance->font_role), "body");
}

static bool ResolveStyle(const ActRaiserTextStyle *style,
                         const ActRaiserTextPalette *palette,
                         ArTextRunAppearance *appearance) {
  *appearance = style->appearance;
  const uint8_t bindings[] = {style->band_binding, style->body_binding,
                              style->shadow_binding};
  uint32_t *inks[] = {&appearance->band_rgb, &appearance->body_rgb,
                      &appearance->shadow_rgb};
  for (unsigned i = 0; i < 3; ++i) {
    const unsigned binding = bindings[i];
    if (!binding)
      continue;
    if (!palette || binding >= kActRaiserTextInk_Count ||
        !(palette->available & (1u << binding)))
      return false;
    *inks[i] = palette->rgb[binding];
  }
  return true;
}

bool ActRaiserTextStyle_Publish(const ActRaiserTextStylePlan *plan,
                                uint32_t source_offset,
                                const ActRaiserTextPalette *palette,
                                ArLocalizationFrame *frame) {
  if (!plan || !frame || !frame->snapshot_count)
    return false;
  if (!ArLocalizationFrame_SetTextOrigin(frame, &plan->origin))
    return false;
  if (!plan->authored)
    return true;
  const ArLocalizationTextSnapshot *snapshot =
      &frame->snapshots[frame->snapshot_count - 1];
  const uint32_t bytes = snapshot->utf8_bytes;
  ArTextRunAppearance appearance;
  ArTextAppearanceSpan spans[kActRaiserTextStyleSpanCapacity];
  size_t count = 0;
  bool valid = plan->style_count &&
               plan->style_count <= kActRaiserTextStyleCapacity &&
               plan->span_count <= kActRaiserTextStyleSpanCapacity &&
               bytes <= UINT32_MAX - source_offset &&
               ResolveStyle(&plan->styles[0], palette, &appearance);
  for (uint16_t i = 0; valid && i < plan->span_count; ++i) {
    const ActRaiserTextStyleSpan *span = &plan->spans[i];
    if (span->end <= source_offset || span->start >= source_offset + bytes)
      continue;
    ArTextAppearanceSpan *mapped = &spans[count++];
    mapped->start =
        span->start > source_offset ? span->start - source_offset : 0;
    mapped->end = (span->end < source_offset + bytes ? span->end
                                                     : source_offset + bytes) -
                  source_offset;
    valid =
        span->style < plan->style_count &&
        ResolveStyle(&plan->styles[span->style], palette, &mapped->appearance);
  }
  if (valid) {
    UseAvailableFont(frame, &appearance);
    for (size_t i = 0; i < count; ++i)
      UseAvailableFont(frame, &spans[i].appearance);
  }
  if (valid &&
      ArLocalizationFrame_SetTextAppearance(frame, &appearance, spans, count))
    return true;
  ArLocalizationFrame_ReleaseText(frame, snapshot->surface_id);
  return false;
}
