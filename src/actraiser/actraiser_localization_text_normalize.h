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

/* Inserts one object in ascending byte order, keeping the snapshot list sorted
 * the way the frame contract requires. */
bool ActRaiserLocalizationText_InsertInlineObject(
    ArLocalizationInlineObjectSnapshot *objects, size_t capacity,
    uint8_t *count, ArLocalizationInlineObjectSnapshot object);

#endif /* ACTRAISER_LOCALIZATION_TEXT_NORMALIZE_H */
