#include "actraiser/actraiser_localization_text_normalize.h"

#include <string.h>

/* Retail extraction retains doubled spaces at former tile-row boundaries.
 * Enhanced layout owns wrapping, so collapse horizontal ASCII whitespace but
 * preserve explicit authored line breaks. */
static ArLocalizationInlineObjectKind InlineObjectKind(const char *id) {
  if (!id) return kArLocalizationInlineObject_None;
  if (!strcmp(id, "icon.status.life"))
    return kArLocalizationInlineObject_StatusLife;
  if (!strcmp(id, "icon.status.population"))
    return kArLocalizationInlineObject_StatusPopulation;
  if (!strcmp(id, "icon.ui.speed_direction"))
    return kArLocalizationInlineObject_SpeedDirection;
  if (!strcmp(id, "icon.ui.selection_pointer"))
    return kArLocalizationInlineObject_SelectionPointer;
  if (!strcmp(id, "icon.name_entry.backspace"))
    return kArLocalizationInlineObject_NameBackspace;
  if (!strcmp(id, "icon.name_entry.finish"))
    return kArLocalizationInlineObject_NameFinish;
  return kArLocalizationInlineObject_None;
}

bool ActRaiserLocalizationText_NormalizeStructured(
    const char *source, size_t source_bytes,
    const ArDialogueInlineObject *source_objects, size_t source_object_count,
    bool preserve_blank_lines,
    char *destination, size_t capacity, size_t *destination_bytes,
    ArLocalizationInlineObjectSnapshot *destination_objects,
    size_t destination_object_capacity, uint8_t *destination_object_count,
    uint16_t *reveal_offsets,
    const uint8_t *source_boundaries, uint8_t *destination_boundaries) {
  if (!source || !destination || !capacity || !destination_bytes ||
      !destination_object_count ||
      (source_object_count && (!source_objects || !destination_objects)) ||
      source_object_count > destination_object_capacity ||
      source_object_count > UINT8_MAX ||
      (destination_boundaries && !source_boundaries) ||
      (reveal_offsets && (source_bytes > kArLocalizationFrameTextCapacity ||
                          capacity > UINT16_MAX)))
    return false;
  if (destination_boundaries)
    memset(destination_boundaries, 0, AR_TEXT_BOUNDARY_BYTES(capacity));
  size_t written = 0;
  size_t object_index = 0;
  bool pending_space = false;
  for (size_t index = 0; index < source_bytes; ++index) {
    if (reveal_offsets) reveal_offsets[index] = (uint16_t)written;
    const char byte = source[index];
    const bool boundary = ArTextBoundary_Get(source_boundaries, index);
    if (byte == ' ' || byte == '\t' || byte == '\r' ||
        (destination_boundaries && byte == '\n' && !boundary)) {
      pending_space = written && destination[written - 1u] != '\n';
      continue;
    }
    if (byte == '\n') {
      while (written && destination[written - 1u] == ' ') --written;
      if (written &&
          (preserve_blank_lines || destination[written - 1u] != '\n')) {
        if (written + 1u >= capacity) return false;
        if (destination_boundaries)
          ArTextBoundary_Set(destination_boundaries, written, boundary);
        destination[written++] = '\n';
      }
      pending_space = false;
      continue;
    }
    if (object_index < source_object_count &&
        source_objects[object_index].end_utf8_byte == index + 3u) {
      static const uint8_t kObjectMarker[] = {0xEF, 0xBF, 0xBC};
      static const uint8_t kFigureSpace[] = {0xE2, 0x80, 0x87};
      static const uint8_t kEmSpace[] = {0xE2, 0x80, 0x83};
      const ArLocalizationInlineObjectKind kind =
          InlineObjectKind(source_objects[object_index].id);
      if (kind == kArLocalizationInlineObject_None ||
          index + sizeof(kObjectMarker) > source_bytes ||
          memcmp(source + index, kObjectMarker, sizeof(kObjectMarker)) ||
          written + (pending_space ? 1u : 0u) + sizeof(kFigureSpace) >=
              capacity)
        return false;
      if (pending_space) destination[written++] = ' ';
      pending_space = false;
      memcpy(destination + written,
             kind == kArLocalizationInlineObject_StatusLife ? kEmSpace : kFigureSpace,
             sizeof(kFigureSpace));
      written += sizeof(kFigureSpace);
      destination_objects[object_index] =
          (ArLocalizationInlineObjectSnapshot){kind, (uint32_t)written};
      ++object_index;
      index += sizeof(kObjectMarker) - 1u;
      continue;
    }
    if (pending_space) {
      if (written + 1u >= capacity) return false;
      destination[written++] = ' ';
      pending_space = false;
    }
    if (written + 1u >= capacity) return false;
    if (destination_boundaries)
      ArTextBoundary_Set(destination_boundaries, written, boundary);
    destination[written++] = byte;
  }
  while (written && (destination[written - 1u] == ' ' ||
                     destination[written - 1u] == '\n'))
    --written;
  destination[written] = 0;
  *destination_bytes = written;
  if (reveal_offsets) reveal_offsets[source_bytes] = (uint16_t)written;
  if (object_index != source_object_count) return false;
  *destination_object_count = (uint8_t)object_index;
  /* A successfully resolved empty message is intentional, not a missing
   * translation. It still owns its native cells while the UI is alive. */
  return true;
}

bool ActRaiserLocalizationText_Normalize(
    const char *source, size_t source_bytes,
    const ArDialogueInlineObject *source_objects, size_t source_object_count,
    bool preserve_blank_lines,
    char *destination, size_t capacity, size_t *destination_bytes,
    ArLocalizationInlineObjectSnapshot *destination_objects,
    size_t destination_object_capacity, uint8_t *destination_object_count,
    uint16_t *reveal_offsets) {
  return ActRaiserLocalizationText_NormalizeStructured(
      source, source_bytes, source_objects, source_object_count,
      preserve_blank_lines, destination, capacity, destination_bytes,
      destination_objects, destination_object_capacity, destination_object_count,
      reveal_offsets, NULL, NULL);
}

bool ActRaiserLocalizationText_InsertInlineObject(
    ArLocalizationInlineObjectSnapshot *objects, size_t capacity,
    uint8_t *count, ArLocalizationInlineObjectSnapshot object) {
  if (!objects || !count || *count >= capacity ||
      object.kind == kArLocalizationInlineObject_None ||
      !object.end_utf8_byte)
    return false;
  size_t position = 0;
  while (position < *count &&
         objects[position].end_utf8_byte <= object.end_utf8_byte)
    ++position;
  if (position < *count) {
    memmove(&objects[position + 1u], &objects[position],
            ((size_t)*count - position) * sizeof(objects[0]));
  }
  objects[position] = object;
  ++*count;
  return true;
}
