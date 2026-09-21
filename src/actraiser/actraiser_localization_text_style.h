#ifndef ACTRAISER_LOCALIZATION_TEXT_STYLE_H
#define ACTRAISER_LOCALIZATION_TEXT_STYLE_H

#include "actraiser/actraiser_text_ink_bindings.h"
#include "localization/dialogue_session.h"
#include "localization/localization_frame.h"

enum {
  kActRaiserTextStyleCapacity = 64,
  kActRaiserTextStyleSpanCapacity = kArLocalizationFrameAppearanceCapacity,
};

typedef struct ActRaiserTextPalette {
  uint16_t dialogue[4];
  uint32_t rgb[kActRaiserTextInk_Count];
  uint16_t available;
} ActRaiserTextPalette;

typedef struct ActRaiserTextStyle {
  ArTextRunAppearance appearance;
  uint8_t band_binding, body_binding, shadow_binding;
} ActRaiserTextStyle;

typedef struct ActRaiserTextStyleSpan {
  uint32_t start, end;
  uint8_t style;
} ActRaiserTextStyleSpan;

/* Snapshot only styles actually used by a message. Interning avoids copying
 * font names and palette bindings into every span. Entry zero is the default.
 * Palettes remain symbolic until the frame captures their native inks. */
typedef struct ActRaiserTextStylePlan {
  ArTextSourceOrigin origin;
  bool authored;
  uint8_t style_count;
  uint16_t span_count;
  ActRaiserTextStyle styles[kActRaiserTextStyleCapacity];
  ActRaiserTextStyleSpan spans[kActRaiserTextStyleSpanCapacity];
} ActRaiserTextStylePlan;

/* Dialogue/credits/OBJ bindings name exact CGRAM entries. HUD is populated by
 * its adapter only for an observed, uploaded owner with intact field tiles. */
void ActRaiserTextPalette_Capture(ActRaiserTextPalette *palette,
                                  const uint16_t *cgram, size_t count);
void ActRaiserTextPalette_SetHud(ActRaiserTextPalette *palette,
                                 const uint16_t colors[4]);
/* Location labels reuse wording across a BG menu and the world-map OBJ label.
 * Capture defaults to the menu palette; the world adapter supplies its colors.
 */
void ActRaiserTextPalette_SetLocation(ActRaiserTextPalette *palette,
                                      uint16_t shadow, uint16_t band,
                                      uint16_t body);

/* Append a normalized page slice to a plan. The caller owns/discards the plan
 * on failure. offsets is the normalization map for source_bytes + 1 entries. */
bool ActRaiserTextStyle_AppendPage(
    ActRaiserTextStylePlan *plan, const ArDialoguePageSnapshot *page,
    size_t source_offset, size_t source_bytes, const uint16_t *offsets,
    const char *normalized, size_t normalized_bytes, size_t destination_base,
    char *error, size_t capacity);
void ActRaiserTextStyle_Edit(ActRaiserTextStylePlan *plan, uint32_t offset,
                             uint32_t removed, uint32_t inserted);
/* Applies a full message or slice to the most recently added frame text. A
 * missing native binding releases its claim, retaining native pixels. */
bool ActRaiserTextStyle_Publish(const ActRaiserTextStylePlan *plan,
                                uint32_t source_offset,
                                const ActRaiserTextPalette *palette,
                                ArLocalizationFrame *frame);

#endif
