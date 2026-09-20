#ifndef ACTRAISER_LOCALIZATION_NAME_COMPOSE_H
#define ACTRAISER_LOCALIZATION_NAME_COMPOSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "actraiser/actraiser_localization_resolved_text.h"

/* Text surgery for the name-entry keyboard: everything that reshapes the
 * authored page into the exact rows, gutters and underline anchors the native
 * screen expects. These are pure buffer operations -- no runtime state, no
 * sessions, no live RAM -- so the runtime adapter is left owning source
 * selection and the dialogue handoff.
 *
 * Every edit relocates inline objects, value isolation and style spans with
 * the text they annotate. */

/* Removes the extraction-era dash glyphs from the entry-field row, keeping the
 * hard line break that positions the keyboard below it. */
bool ActRaiserLocalizationNameCompose_ClearUnderlineRow(
    ActRaiserResolvedText *text);

/* Widens the single blank between keys so a selector can sit beside a key
 * without covering its neighbour. */
bool ActRaiserLocalizationNameCompose_ExpandKeyGutters(
    ActRaiserResolvedText *text);

/* Adds the "< page/total >" row above a multi-page alphabet. */
bool ActRaiserLocalizationNameCompose_InsertPageIndicator(
    ActRaiserResolvedText *text, uint32_t page_index, uint32_t page_count);

/* Attaches an underline object to each of the eight shaped name graphemes and
 * returns the field's layout independently of those decorations. Later gutter
 * and page-indicator insertions occur after this field, so its range is stable. */
bool ActRaiserLocalizationNameCompose_PrepareField(ActRaiserResolvedText *text);

#endif /* ACTRAISER_LOCALIZATION_NAME_COMPOSE_H */
