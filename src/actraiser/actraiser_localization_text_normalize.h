#ifndef ACTRAISER_LOCALIZATION_TEXT_NORMALIZE_H
#define ACTRAISER_LOCALIZATION_TEXT_NORMALIZE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "localization/dialogue_session.h"
#include "localization/localization_frame.h"

/* Turns one resolved dialogue page into the bytes and inline objects a frame
 * snapshot carries. This is presentation-neutral text handling shared by every
 * surface; it holds no runtime state and reads no live game memory.
 *
 * `reveal_offsets`, when supplied, receives the destination byte offset of
 * each source byte so a caller can map a native reveal position onto the
 * normalized text. */
bool ActRaiserLocalizationText_Normalize(
    const char *source, size_t source_bytes,
    const ArDialogueInlineObject *source_objects, size_t source_object_count,
    bool preserve_blank_lines,
    char *destination, size_t capacity, size_t *destination_bytes,
    ArLocalizationInlineObjectSnapshot *destination_objects,
    size_t destination_object_capacity, uint8_t *destination_object_count,
    uint16_t *reveal_offsets);

/* Fixed-table variant: remaps the compiler's authored boundaries while
 * normalizing. Inserted values stay inline (including any value newlines).
 * Both bitmaps cover their respective UTF-8 byte capacities. */
bool ActRaiserLocalizationText_NormalizeStructured(
    const char *source, size_t source_bytes,
    const ArDialogueInlineObject *source_objects, size_t source_object_count,
    bool preserve_blank_lines,
    char *destination, size_t capacity, size_t *destination_bytes,
    ArLocalizationInlineObjectSnapshot *destination_objects,
    size_t destination_object_capacity, uint8_t *destination_object_count,
    uint16_t *reveal_offsets,
    const uint8_t *source_boundaries, uint8_t *destination_boundaries);

/* Inserts one object in ascending byte order, keeping the snapshot list sorted
 * the way the frame contract requires. */
bool ActRaiserLocalizationText_InsertInlineObject(
    ArLocalizationInlineObjectSnapshot *objects, size_t capacity,
    uint8_t *count, ArLocalizationInlineObjectSnapshot object);

/* Map immutable value spans through normalization's existing byte map. The
 * source may be a retained page slice; mapped spans append at destination_base.
 * Edge whitespace belongs to layout, not to the isolated semantic value. */
bool ActRaiserLocalizationText_MapBidiSpans(
    const ArTextBidiSpan *spans, size_t count, size_t source_offset,
    size_t source_bytes, const uint16_t *offsets,
    const char *normalized, size_t normalized_bytes, size_t destination_base,
    ArTextBidiSpans *destination);

#endif /* ACTRAISER_LOCALIZATION_TEXT_NORMALIZE_H */
