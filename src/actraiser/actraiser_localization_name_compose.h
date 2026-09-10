#ifndef ACTRAISER_LOCALIZATION_NAME_COMPOSE_H
#define ACTRAISER_LOCALIZATION_NAME_COMPOSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "localization/localization_frame.h"

/* Text surgery for the name-entry keyboard: everything that reshapes the
 * authored page into the exact rows, gutters and underline anchors the native
 * screen expects. These are pure buffer operations -- no runtime state, no
 * sessions, no live RAM -- so the runtime adapter is left owning source
 * selection and the dialogue handoff.
 *
 * Every entry point keeps the caller's inline objects attached to the same
 * text as bytes move. */

/* Removes the extraction-era dash glyphs from the entry-field row, keeping the
 * hard line break that positions the keyboard below it. */
bool ActRaiserLocalizationNameCompose_ClearUnderlineRow(
    char *utf8, size_t *utf8_bytes,
    ArLocalizationInlineObjectSnapshot *objects, uint8_t object_count);

/* Widens the single blank between keys so a selector can sit beside a key
 * without covering its neighbour. */
bool ActRaiserLocalizationNameCompose_ExpandKeyGutters(
    char *utf8, size_t capacity, size_t *utf8_bytes,
    ArLocalizationInlineObjectSnapshot *objects, uint8_t object_count);

/* Adds the "< page/total >" row above a multi-page alphabet. */
bool ActRaiserLocalizationNameCompose_InsertPageIndicator(
    char *utf8, size_t capacity, size_t *utf8_bytes,
    uint32_t page_index, uint32_t page_count,
    ArLocalizationInlineObjectSnapshot *objects, uint8_t object_count);

/* Attaches an underline object to each of the eight shaped name graphemes,
 * independently of keyboard geometry. */
bool ActRaiserLocalizationNameCompose_InsertFieldUnderlines(
    const char *utf8, size_t utf8_bytes,
    ArLocalizationInlineObjectSnapshot *objects, size_t capacity,
    uint8_t *count);

#endif /* ACTRAISER_LOCALIZATION_NAME_COMPOSE_H */
