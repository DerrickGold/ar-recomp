#include "localization/dialogue_session.h"

#include "localization/unicode_grapheme.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AR_MEMBER_END(type, member)                                             \
  (offsetof(type, member) + sizeof(((type *)0)->member))

enum {
  kDialogueSessionMagic = UINT32_C(0x41524453), /* ARDS */
  kMaximumDialoguePages = 64,
  kMaximumDialogueCues = 4096,
  kMaximumDialoguePageBytes = 256 * 1024,
  kMaximumDialogueInlineObjects = 256,
};

typedef enum DialogueCueKind {
  kDialogueCue_Wait = 0,
  kDialogueCue_Control,
  kDialogueCue_Page,
  kDialogueCue_End,
} DialogueCueKind;

typedef struct DialogueCue {
  DialogueCueKind kind;
  size_t utf8_offset;
  uint32_t ordinal;
  uint32_t wait_frames;
  char *control_id;
} DialogueCue;

typedef struct DialoguePage {
  char *utf8;
  size_t utf8_bytes;
  size_t utf8_capacity;
  DialogueCue *cues;
  uint32_t cue_count;
  uint32_t cue_capacity;
  ArDialogueInlineObject *objects;
  uint32_t object_count;
  uint32_t object_capacity;
  uint32_t cluster_count;
} DialoguePage;

typedef struct DialogueProgram {
  DialoguePage *pages;
  uint32_t page_count;
  uint32_t page_capacity;
  uint32_t control_count;
  uint32_t wait_count;
  ArDialogueResolvedSource resolved_source;
  uint64_t source_revision;
  char package_id[kArLanguagePackageIdCapacity];
  char locale[kArLanguageLocaleCapacity];
  ArLanguageDirection direction;
} DialogueProgram;

typedef struct ProgramSource {
  const ArLanguagePack *effective_pack;
  const ArLanguagePack *term_fallback_pack;
  const ArLanguageMessage *message;
  ArDialogueResolvedSource resolved_source;
} ProgramSource;

static void SetError(ArLanguagePackError *error, const char *format, ...) {
  if (!error)
    return;
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(error->message, sizeof(error->message), format, arguments);
  va_end(arguments);
}

static bool IsInitialized(const ArDialogueSession *session) {
  return session && session->private_magic == kDialogueSessionMagic;
}

static char *CopyString(const char *value) {
  const size_t length = value ? strlen(value) : 0;
  char *copy = (char *)malloc(length + 1);
  if (!copy)
    return NULL;
  if (length)
    memcpy(copy, value, length);
  copy[length] = 0;
  return copy;
}

static bool Reserve(void **storage, size_t item_size, uint32_t needed,
                    uint32_t *capacity, ArLanguagePackError *error) {
  if (needed <= *capacity)
    return true;
  uint32_t next = *capacity ? *capacity : 4;
  while (next < needed) {
    if (next > UINT32_MAX / 2) {
      SetError(error, "dialogue allocation exceeds its limit");
      return false;
    }
    next *= 2;
  }
  if ((size_t)next > SIZE_MAX / item_size) {
    SetError(error, "dialogue allocation exceeds addressable memory");
    return false;
  }
  void *grown = realloc(*storage, (size_t)next * item_size);
  if (!grown) {
    SetError(error, "out of memory building dialogue presentation");
    return false;
  }
  *storage = grown;
  *capacity = next;
  return true;
}

static void DestroyPage(DialoguePage *page) {
  if (!page)
    return;
  for (uint32_t i = 0; i < page->cue_count; i++)
    free(page->cues[i].control_id);
  for (uint32_t i = 0; i < page->object_count; i++)
    free((void *)page->objects[i].id);
  free(page->utf8);
  free(page->cues);
  free(page->objects);
  memset(page, 0, sizeof(*page));
}

static void DestroyProgram(DialogueProgram *program) {
  if (!program)
    return;
  for (uint32_t i = 0; i < program->page_count; i++)
    DestroyPage(&program->pages[i]);
  free(program->pages);
  free(program);
}

static bool ValidUtf8(const char *text) {
  if (!text)
    return false;
  const size_t length = strlen(text);
  size_t offset = 0;
  while (offset < length) {
    size_t next = 0;
    if (!ArUnicodeGrapheme_Next(text, length, offset, NULL, &next) ||
        next <= offset)
      return false;
    offset = next;
  }
  return true;
}

static bool FixedStringTerminated(const char *text, size_t capacity) {
  return text && memchr(text, 0, capacity) != NULL;
}

static bool CopyBounded(char *destination, size_t capacity, const char *value,
                        const char *description,
                        ArLanguagePackError *error) {
  const size_t length = value ? strlen(value) : 0;
  if (length >= capacity) {
    SetError(error, "%s is too long", description);
    return false;
  }
  memcpy(destination, value ? value : "", length + 1);
  return true;
}

static bool ValidateResolver(const ArDialogueValueResolver *resolver,
                             bool required, ArLanguagePackError *error) {
  if (!resolver) {
    if (required)
      SetError(error, "dialogue requires a dynamic-value resolver");
    return !required;
  }
  if (resolver->struct_size <
          AR_MEMBER_END(ArDialogueValueResolver, format_number) ||
      resolver->abi_version != AR_DIALOGUE_VALUE_RESOLVER_ABI_VERSION ||
      !resolver->resolve) {
    SetError(error, "dialogue value resolver has an incompatible ABI");
    return false;
  }
  return true;
}

static DialoguePage *AddPage(DialogueProgram *program,
                             ArLanguagePackError *error) {
  if (program->page_count >= kMaximumDialoguePages) {
    SetError(error, "dialogue exceeds %u authored pages",
             kMaximumDialoguePages);
    return NULL;
  }
  if (!Reserve((void **)&program->pages, sizeof(*program->pages),
               program->page_count + 1, &program->page_capacity, error))
    return NULL;
  DialoguePage *page = &program->pages[program->page_count++];
  memset(page, 0, sizeof(*page));
  page->utf8 = (char *)malloc(1);
  if (!page->utf8) {
    SetError(error, "out of memory creating dialogue page");
    return NULL;
  }
  page->utf8[0] = 0;
  page->utf8_capacity = 1;
  return page;
}

static bool AppendBytes(DialoguePage *page, const char *bytes, size_t size,
                        ArLanguagePackError *error) {
  if (size > kMaximumDialoguePageBytes - page->utf8_bytes) {
    SetError(error, "composed dialogue page exceeds %u UTF-8 bytes",
             kMaximumDialoguePageBytes);
    return false;
  }
  const size_t needed = page->utf8_bytes + size + 1;
  if (needed > page->utf8_capacity) {
    size_t capacity = page->utf8_capacity;
    while (capacity < needed) {
      if (capacity > (kMaximumDialoguePageBytes + 1) / 2) {
        capacity = kMaximumDialoguePageBytes + 1;
        break;
      }
      capacity *= 2;
    }
    char *grown = (char *)realloc(page->utf8, capacity);
    if (!grown) {
      SetError(error, "out of memory composing dialogue page");
      return false;
    }
    page->utf8 = grown;
    page->utf8_capacity = capacity;
  }
  memcpy(page->utf8 + page->utf8_bytes, bytes, size);
  page->utf8_bytes += size;
  page->utf8[page->utf8_bytes] = 0;
  return true;
}

static bool AddCue(DialogueProgram *program, DialoguePage *page,
                   DialogueCue cue, ArLanguagePackError *error) {
  uint32_t total_cues = 0;
  for (uint32_t i = 0; i < program->page_count; i++)
    total_cues += program->pages[i].cue_count;
  if (total_cues >= kMaximumDialogueCues) {
    SetError(error, "dialogue exceeds %u presentation cues",
             kMaximumDialogueCues);
    return false;
  }
  if (!Reserve((void **)&page->cues, sizeof(*page->cues), page->cue_count + 1,
               &page->cue_capacity, error))
    return false;
  page->cues[page->cue_count++] = cue;
  return true;
}

static bool AddInlineObject(DialoguePage *page, const char *id,
                            ArLanguagePackError *error) {
  if (page->object_count >= kMaximumDialogueInlineObjects) {
    SetError(error, "dialogue page has too many inline objects");
    return false;
  }
  if (!Reserve((void **)&page->objects, sizeof(*page->objects),
               page->object_count + 1, &page->object_capacity, error))
    return false;
  char *id_copy = CopyString(id);
  if (!id_copy) {
    SetError(error, "out of memory retaining inline object id");
    return false;
  }
  page->objects[page->object_count++] = (ArDialogueInlineObject){
      .end_utf8_byte = page->utf8_bytes,
      .id = id_copy,
  };
  return true;
}

static const ArLanguageMessage *ResolveAlias(const ArLanguagePack *pack,
                                             const ArLanguageMessage *message) {
  const ArLanguageMessage *cursor = message;
  for (uint32_t depth = 0; cursor && cursor->is_alias &&
                           depth <= ArLanguagePack_MessageCount(pack);
       depth++) {
    cursor = ArLanguagePack_FindMessage(
        pack, ArLanguagePack_GetString(pack, cursor->alias));
  }
  return cursor && !cursor->is_alias ? cursor : NULL;
}

static const ArDialogueValue *FindValue(const ArDialogueStableState *state,
                                        const char *name) {
  for (uint32_t i = 0; i < state->value_count; i++) {
    if (strcmp(state->values[i].name, name) == 0)
      return &state->values[i];
  }
  return NULL;
}

static bool AppendPlainMessage(const ArLanguagePack *pack,
                               const ArLanguageMessage *message,
                               char *output, size_t capacity,
                               ArLanguagePackError *error) {
  const ArLanguageMessage *body = ResolveAlias(pack, message);
  if (!body)
    return false;
  size_t used = 0;
  for (uint32_t i = 0; i < body->operation_count; i++) {
    const ArLanguageOperation *operation =
        ArLanguagePack_GetOperation(pack, body, i);
    const char *value = NULL;
    if (operation->kind == kArLanguageOperation_Text)
      value = ArLanguagePack_GetString(pack, operation->value.text);
    else if (operation->kind == kArLanguageOperation_LineBreak)
      value = "\n";
    else if (operation->kind == kArLanguageOperation_ParagraphBreak)
      value = "\n\n";
    else if (operation->kind == kArLanguageOperation_PageBreak)
      value = " ";
    else if (operation->kind == kArLanguageOperation_End)
      break;
    else if (operation->kind == kArLanguageOperation_Empty)
      continue;
    else {
      SetError(error, "localized term must contain only plain text");
      return false;
    }
    const size_t size = strlen(value);
    if (size >= capacity - used) {
      SetError(error, "localized term exceeds its value limit");
      return false;
    }
    memcpy(output + used, value, size);
    used += size;
  }
  output[used] = 0;
  return true;
}

static bool ResolveLocalizedTerm(const ProgramSource *source, const char *id,
                                 char *output, size_t capacity,
                                 ArLanguagePackError *error) {
  const ArLanguageMessage *message =
      ArLanguagePack_FindMessage(source->effective_pack, id);
  if (message && AppendPlainMessage(source->effective_pack, message, output,
                                    capacity, error))
    return true;
  if (source->term_fallback_pack &&
      source->term_fallback_pack != source->effective_pack) {
    if (error)
      error->message[0] = 0;
    message = ArLanguagePack_FindMessage(source->term_fallback_pack, id);
    if (message && AppendPlainMessage(source->term_fallback_pack, message,
                                      output, capacity, error))
      return true;
  }
  SetError(error, "localized term '%s' is unavailable", id);
  return false;
}

static bool AppendValue(const ProgramSource *source,
                        const ArDialogueStableState *state,
                        const ArDialogueValueResolver *resolver,
                        DialoguePage *page, const char *name,
                        unsigned minimum_digits,
                        ArLanguagePackError *error) {
  const ArDialogueValue *value = FindValue(state, name);
  if (!value) {
    SetError(error, "dialogue value '%s' was not snapshotted", name);
    return false;
  }
  char formatted[kArDialogueValueTextCapacity];
  formatted[0] = 0;
  formatted[sizeof(formatted) - 1] = 0;
  const char *text = value->text;
  if (value->kind == kArLanguagePlaceholder_Number) {
    bool formatted_number = false;
    if (resolver && resolver->format_number) {
      formatted_number = resolver->format_number(
          resolver->context,
          ArLanguagePack_GetMetadata(source->effective_pack)->locale,
          value->number, minimum_digits, formatted, sizeof(formatted));
      formatted[sizeof(formatted) - 1] = 0;
    } else {
      formatted_number =
          snprintf(formatted, sizeof(formatted), "%0*lld",
                   (int)minimum_digits, (long long)value->number) > 0;
    }
    if (!formatted_number || !ValidUtf8(formatted)) {
      SetError(error, "could not format numeric dialogue value '%s'", name);
      return false;
    }
    text = formatted;
  } else if (value->kind == kArLanguagePlaceholder_LocalizedTerm) {
    if (!ResolveLocalizedTerm(source, value->text, formatted,
                              sizeof(formatted), error))
      return false;
    text = formatted;
  } else if (value->kind == kArLanguagePlaceholder_Icon) {
    static const char replacement_object[] = "\xEF\xBF\xBC"; /* U+FFFC */
    if (!AppendBytes(page, replacement_object,
                     sizeof(replacement_object) - 1, error) ||
        !AddInlineObject(page, value->text, error))
      return false;
    return true;
  }
  return AppendBytes(page, text, strlen(text), error);
}

static uint32_t CountClusters(const char *text, size_t size,
                              size_t maximum_offset) {
  size_t offset = 0;
  uint32_t count = 0;
  if (maximum_offset > size)
    maximum_offset = size;
  while (offset < maximum_offset) {
    size_t next = 0;
    if (!ArUnicodeGrapheme_Next(text, size, offset, NULL, &next) ||
        next > maximum_offset)
      return UINT32_MAX;
    offset = next;
    count++;
  }
  return offset == maximum_offset ? count : UINT32_MAX;
}

static size_t ByteOffsetForClusters(const DialoguePage *page,
                                    uint32_t cluster_count) {
  size_t offset = 0;
  uint32_t count = 0;
  while (offset < page->utf8_bytes && count < cluster_count) {
    size_t next = 0;
    if (!ArUnicodeGrapheme_Next(page->utf8, page->utf8_bytes, offset, NULL,
                                &next))
      return 0;
    offset = next;
    count++;
  }
  return offset;
}

static bool BuildProgram(const ProgramSource *source,
                         const ArDialogueStableState *state,
                         const ArDialogueValueResolver *resolver,
                         DialogueProgram **out_program,
                         ArLanguagePackError *error) {
  DialogueProgram *program = (DialogueProgram *)calloc(1, sizeof(*program));
  if (!program) {
    SetError(error, "out of memory building dialogue program");
    return false;
  }
  program->resolved_source = source->resolved_source;
  const ArLanguagePackMetadata *metadata =
      ArLanguagePack_GetMetadata(source->effective_pack);
  program->source_revision = source->effective_pack->content_revision;
  program->direction = metadata->direction;
  if (!CopyBounded(program->package_id, sizeof(program->package_id),
                   metadata->package_id, "package id", error) ||
      !CopyBounded(program->locale, sizeof(program->locale), metadata->locale,
                   "locale", error))
    goto failed;

  DialoguePage *page = AddPage(program, error);
  if (!page)
    goto failed;
  const ArLanguageMessage *body =
      ResolveAlias(source->effective_pack, source->message);
  if (!body) {
    SetError(error, "%s: could not resolve message alias", state->message_id);
    goto failed;
  }
  for (uint32_t i = 0; i < body->operation_count; i++) {
    const ArLanguageOperation *operation =
        ArLanguagePack_GetOperation(source->effective_pack, body, i);
    if (!operation) {
      SetError(error, "%s: invalid message operation", state->message_id);
      goto failed;
    }
    switch (operation->kind) {
    case kArLanguageOperation_Text: {
      const char *text = ArLanguagePack_GetString(source->effective_pack,
                                                  operation->value.text);
      if (!AppendBytes(page, text, operation->value.text.length, error))
        goto failed;
      break;
    }
    case kArLanguageOperation_Placeholder: {
      const char *name = ArLanguagePack_GetString(
          source->effective_pack, operation->value.placeholder);
      if (!AppendValue(source, state, resolver, page, name,
                        operation->minimum_digits, error))
        goto failed;
      break;
    }
    case kArLanguageOperation_LineBreak:
      if (!AppendBytes(page, "\n", 1, error))
        goto failed;
      break;
    case kArLanguageOperation_ParagraphBreak:
      if (!AppendBytes(page, "\n\n", 2, error))
        goto failed;
      break;
    case kArLanguageOperation_PageBreak:
      if (!AddCue(program, page,
                  (DialogueCue){.kind = kDialogueCue_Page,
                                .utf8_offset = page->utf8_bytes},
                  error))
        goto failed;
      page = AddPage(program, error);
      if (!page)
        goto failed;
      break;
    case kArLanguageOperation_WaitFrames:
      if (!AddCue(program, page,
                  (DialogueCue){.kind = kDialogueCue_Wait,
                                .utf8_offset = page->utf8_bytes,
                                .ordinal = program->wait_count++,
                                .wait_frames = operation->value.wait_frames},
                  error))
        goto failed;
      break;
    case kArLanguageOperation_Anchor: {
      const char *id = ArLanguagePack_GetString(source->effective_pack,
                                                operation->value.anchor);
      char *copy = CopyString(id);
      if (!copy ||
          !AddCue(program, page,
                  (DialogueCue){.kind = kDialogueCue_Control,
                                .utf8_offset = page->utf8_bytes,
                                .ordinal = program->control_count++,
                                .control_id = copy},
                  error)) {
        free(copy);
        goto failed;
      }
      break;
    }
    case kArLanguageOperation_Empty:
      break;
    case kArLanguageOperation_End:
      if (!AddCue(program, page,
                  (DialogueCue){.kind = kDialogueCue_End,
                                .utf8_offset = page->utf8_bytes},
                  error))
        goto failed;
      i = body->operation_count;
      break;
    case kArLanguageOperation_Event:
      SetError(error, "%s: author event reached dialogue compiler",
               state->message_id);
      goto failed;
    }
  }
  for (uint32_t i = 0; i < program->page_count; i++) {
    program->pages[i].cluster_count = CountClusters(
        program->pages[i].utf8, program->pages[i].utf8_bytes,
        program->pages[i].utf8_bytes);
    if (program->pages[i].cluster_count == UINT32_MAX) {
      SetError(error, "%s: composed page is not valid UTF-8",
               state->message_id);
      goto failed;
    }
  }
  *out_program = program;
  return true;

failed:
  DestroyProgram(program);
  return false;
}

static bool ValidatePackMetadataForRuntime(const ArLanguagePack *pack,
                                           bool required,
                                           ArLanguagePackError *error) {
  if (!pack)
    return !required;
  const ArLanguagePackMetadata *metadata = ArLanguagePack_GetMetadata(pack);
  if (!metadata || metadata->target != kArLanguagePackTarget_UsRuntime ||
      metadata->source_profile != kArLanguageSourceProfile_Us) {
    if (required)
      SetError(error, "runtime language pack must target the U.S. contract");
    return false;
  }
  return true;
}

static bool ValidateMessageForRuntime(const ArLanguagePack *pack,
                                      const char *semantic_id, bool required,
                                      ArLanguagePackError *error) {
  if (!ValidatePackMetadataForRuntime(pack, required, error))
    return false;
  ArLanguagePackError detail;
  if (!ArLanguageContract_ValidateMessage(pack, semantic_id, &detail)) {
    if (required)
      SetError(error, "%s", detail.message);
    return false;
  }
  return true;
}

static bool SelectProgramSource(const ArDialogueContentSelection *selection,
                                const char *semantic_id,
                                ProgramSource *source,
                                ArLanguagePackError *error) {
  memset(source, 0, sizeof(*source));
  if (!selection ||
      selection->struct_size <
          AR_MEMBER_END(ArDialogueContentSelection,
                        native_us_enhanced_pack) ||
      selection->abi_version != AR_DIALOGUE_SESSION_ABI_VERSION ||
      selection->presentation < kArDialoguePresentation_NativeRetail ||
      selection->presentation > kArDialoguePresentation_Enhanced) {
    SetError(error, "invalid dialogue content selection");
    return false;
  }
  if (selection->selected_pack &&
      selection->presentation != kArDialoguePresentation_Enhanced) {
    SetError(error, "community language packs require enhanced presentation");
    return false;
  }
  if (selection->selected_pack) {
    source->message =
        ArLanguagePack_FindMessage(selection->selected_pack, semantic_id);
    if (source->message) {
      if (!ValidateMessageForRuntime(selection->selected_pack, semantic_id,
                                     true, error))
        return false;
      source->effective_pack = selection->selected_pack;
      source->term_fallback_pack =
          ValidatePackMetadataForRuntime(selection->native_us_enhanced_pack,
                                         false, NULL)
              ? selection->native_us_enhanced_pack
              : NULL;
      source->resolved_source = kArDialogueResolvedSource_SelectedPack;
      return true;
    }
  }
  if ((!selection->selected_pack &&
       selection->presentation == kArDialoguePresentation_Enhanced) ||
      selection->selected_pack) {
    if (ValidateMessageForRuntime(selection->native_us_enhanced_pack,
                                  semantic_id, false, NULL)) {
      source->message = ArLanguagePack_FindMessage(
          selection->native_us_enhanced_pack, semantic_id);
      if (source->message) {
        source->effective_pack = selection->native_us_enhanced_pack;
        source->term_fallback_pack = selection->native_us_enhanced_pack;
        source->resolved_source = kArDialogueResolvedSource_NativeEnhanced;
        return true;
      }
    }
  }
  source->resolved_source = kArDialogueResolvedSource_NativeRom;
  return true;
}

static bool SnapshotValues(ArDialogueStableState *state,
                           const ArDialogueValueResolver *resolver,
                           ArLanguagePackError *error) {
  const uint32_t count =
      ArLanguageContract_AllowedPlaceholderCount(state->message_id);
  if (count > kArDialogueMaximumValues) {
    SetError(error, "%s requires too many dynamic values", state->message_id);
    return false;
  }
  if (count &&
      !ValidateResolver(resolver, true, error)) {
    SetError(error, "%s requires a dialogue value resolver", state->message_id);
    return false;
  }
  state->value_count = count;
  for (uint32_t i = 0; i < count; i++) {
    const char *name =
        ArLanguageContract_AllowedPlaceholder(state->message_id, i);
    ArDialogueValue value = {0};
    const ArLanguagePlaceholderKind kind =
        ArLanguageContract_PlaceholderKind(name);
    if (!resolver->resolve(resolver->context, name, kind, &value,
                           error ? error->message : NULL,
                           error ? sizeof(error->message) : 0)) {
      if (error && !error->message[0])
        SetError(error, "%s: could not snapshot value '%s'", state->message_id,
                 name);
      return false;
    }
    if (value.kind != kind) {
      SetError(error, "%s: value '%s' has the wrong type", state->message_id,
               name);
      return false;
    }
    if (!CopyBounded(value.name, sizeof(value.name), name, "value name",
                     error))
      return false;
    if (!FixedStringTerminated(value.name, sizeof(value.name)) ||
        !FixedStringTerminated(value.text, sizeof(value.text))) {
      SetError(error, "%s: value resolver exceeded its fixed output",
               state->message_id);
      return false;
    }
    if (kind != kArLanguagePlaceholder_Number &&
        (!value.text[0] || !ValidUtf8(value.text))) {
      SetError(error, "%s: value '%s' is empty or invalid UTF-8",
               state->message_id, name);
      return false;
    }
    state->values[i] = value;
  }
  return true;
}

static bool ValidateStableValues(const ArDialogueStableState *state,
                                 ArLanguagePackError *error) {
  const uint32_t expected =
      ArLanguageContract_AllowedPlaceholderCount(state->message_id);
  if (state->value_count != expected ||
      state->value_count > kArDialogueMaximumValues) {
    SetError(error, "%s: saved dynamic-value set is incompatible",
             state->message_id);
    return false;
  }
  for (uint32_t i = 0; i < state->value_count; i++) {
    if (!FixedStringTerminated(state->values[i].name,
                               sizeof(state->values[i].name)) ||
        !FixedStringTerminated(state->values[i].text,
                               sizeof(state->values[i].text))) {
      SetError(error, "%s: saved dynamic-value strings are invalid",
               state->message_id);
      return false;
    }
    for (uint32_t j = i + 1; j < state->value_count; j++) {
      if (strcmp(state->values[i].name, state->values[j].name) == 0) {
        SetError(error, "%s: saved dynamic-value names are duplicated",
                 state->message_id);
        return false;
      }
    }
  }
  for (uint32_t i = 0; i < expected; i++) {
    const char *name =
        ArLanguageContract_AllowedPlaceholder(state->message_id, i);
    const ArDialogueValue *value = FindValue(state, name);
    const ArLanguagePlaceholderKind kind =
        ArLanguageContract_PlaceholderKind(name);
    if (!value || value->kind != kind ||
        (kind != kArLanguagePlaceholder_Number &&
         (!value->text[0] || !ValidUtf8(value->text)))) {
      SetError(error, "%s: saved value '%s' is invalid", state->message_id,
               name);
      return false;
    }
  }
  return true;
}

static void InstallNativeSource(ArDialogueSession *session,
                                ArDialoguePresentation presentation) {
  session->state.presentation = presentation;
  session->state.resolved_source = kArDialogueResolvedSource_NativeRom;
  session->state.package_id[0] = 0;
  session->state.source_revision = 0;
  session->private_native_observation_seen = false;
}

static bool FindControlCue(DialogueProgram *program, uint32_t ordinal,
                           uint32_t *page_index, uint32_t *cue_index) {
  for (uint32_t page = 0; page < program->page_count; page++) {
    for (uint32_t cue = 0; cue < program->pages[page].cue_count; cue++) {
      const DialogueCue *candidate = &program->pages[page].cues[cue];
      if (candidate->kind == kDialogueCue_Control &&
          candidate->ordinal == ordinal) {
        *page_index = page;
        *cue_index = cue;
        return true;
      }
    }
  }
  return false;
}

static void MapProgramProgress(ArDialogueSession *session,
                               DialogueProgram *program,
                               uint32_t old_page_index,
                               uint32_t old_revealed,
                               uint32_t old_total,
                               bool clamped_from_later_page) {
  uint32_t page_index = old_page_index;
  if (page_index >= program->page_count)
    page_index = program->page_count - 1;
  /* A delivered but unacknowledged control is the strongest possible
   * semantic cursor. Its exact anchor wins over page/reveal heuristics. */
  uint32_t anchored_page = 0;
  uint32_t anchored_cue = 0;
  if (session->state.control_pending &&
      FindControlCue(program, session->state.completed_control_count,
                     &anchored_page, &anchored_cue)) {
    DialoguePage *page = &program->pages[anchored_page];
    const size_t offset = page->cues[anchored_cue].utf8_offset;
    session->state.authored_page_index = anchored_page;
    session->state.revealed_cluster_count =
        CountClusters(page->utf8, page->utf8_bytes, offset);
    session->state.page_cluster_count = page->cluster_count;
    session->private_revealed_utf8_bytes = offset;
    session->private_cue_index = anchored_cue;
    return;
  }
  /* Authored pages may move around locked controls, but switching can never
   * jump over a control that has not been applied. */
  for (uint32_t candidate = 0; candidate < page_index; candidate++) {
    DialoguePage *candidate_page = &program->pages[candidate];
    bool has_uncompleted_control = false;
    for (uint32_t cue = 0; cue < candidate_page->cue_count; cue++) {
      if (candidate_page->cues[cue].kind == kDialogueCue_Control &&
          candidate_page->cues[cue].ordinal >=
              session->state.completed_control_count) {
        has_uncompleted_control = true;
        break;
      }
    }
    if (has_uncompleted_control) {
      page_index = candidate;
      clamped_from_later_page = false;
      break;
    }
  }
  DialoguePage *page = &program->pages[page_index];
  uint32_t revealed = 0;
  if (clamped_from_later_page) {
    revealed = page->cluster_count;
  } else if (old_total) {
    const uint64_t numerator =
        (uint64_t)old_revealed * page->cluster_count + old_total / 2;
    revealed = (uint32_t)(numerator / old_total);
  } else if (old_revealed) {
    revealed = old_revealed < page->cluster_count ? old_revealed
                                                  : page->cluster_count;
  }
  if (revealed > page->cluster_count)
    revealed = page->cluster_count;
  size_t revealed_bytes = ByteOffsetForClusters(page, revealed);

  /* Nor may page heuristics move backwards across the most recently applied
   * semantic anchor when a translation relocates an authored page break. */
  if (session->state.completed_control_count &&
      FindControlCue(program, session->state.completed_control_count - 1,
                     &anchored_page, &anchored_cue)) {
    DialoguePage *anchor_page = &program->pages[anchored_page];
    const size_t anchor_offset =
        anchor_page->cues[anchored_cue].utf8_offset;
    if (page_index < anchored_page ||
        (page_index == anchored_page && revealed_bytes < anchor_offset)) {
      page_index = anchored_page;
      page = anchor_page;
      revealed_bytes = anchor_offset;
      revealed =
          CountClusters(page->utf8, page->utf8_bytes, revealed_bytes);
    }
  }

  uint32_t cue_index = 0;
  while (cue_index < page->cue_count) {
    DialogueCue *cue = &page->cues[cue_index];
    if (cue->kind == kDialogueCue_Control) {
      if (cue->ordinal < session->state.completed_control_count) {
        cue_index++;
        continue;
      }
      if (cue->utf8_offset < revealed_bytes) {
        revealed_bytes = cue->utf8_offset;
        revealed = CountClusters(page->utf8, page->utf8_bytes, revealed_bytes);
      }
      break;
    }
    if (cue->kind == kDialogueCue_Wait &&
        cue->ordinal < session->state.completed_wait_count) {
      cue_index++;
      continue;
    }
    if (cue->utf8_offset < revealed_bytes)
      cue_index++;
    else
      break;
  }
  session->state.authored_page_index = page_index;
  session->state.revealed_cluster_count = revealed;
  session->state.page_cluster_count = page->cluster_count;
  session->private_revealed_utf8_bytes = revealed_bytes;
  session->private_cue_index = cue_index;
}

static bool ActivateSelection(ArDialogueSession *session,
                              const ArDialogueContentSelection *selection,
                              const ArDialogueStableState *stable,
                              ArLanguagePackError *error) {
  ProgramSource source;
  if (!SelectProgramSource(selection, stable->message_id, &source, error))
    return false;
  DialogueProgram *program = NULL;
  if (source.resolved_source != kArDialogueResolvedSource_NativeRom &&
      !BuildProgram(&source, stable,
                    session->private_has_resolver
                        ? &session->private_resolver
                        : NULL,
                    &program, error))
    return false;

  DialogueProgram *old_program = (DialogueProgram *)session->private_program;
  const uint32_t old_page = stable->authored_page_index;
  const uint32_t old_revealed = stable->revealed_cluster_count;
  const uint32_t old_total = stable->page_cluster_count;
  const bool clamped = program && old_page >= program->page_count;
  session->state = *stable;
  session->state.struct_size = sizeof(session->state);
  session->state.abi_version = AR_DIALOGUE_SESSION_ABI_VERSION;
  session->private_program = program;
  session->private_cue_index = 0;
  session->private_revealed_utf8_bytes = 0;
  if (program) {
    session->state.presentation = selection->presentation;
    session->state.resolved_source = program->resolved_source;
    session->state.source_revision = program->source_revision;
    snprintf(session->state.package_id, sizeof(session->state.package_id),
             "%s", program->package_id);
    MapProgramProgress(session, program, old_page, old_revealed, old_total,
                       clamped);
    if (session->state.awaiting_page_advance &&
        session->state.authored_page_index + 1 >= program->page_count) {
      session->state.awaiting_page_advance = false;
      DialoguePage *last =
          &program->pages[session->state.authored_page_index];
      session->state.revealed_cluster_count = last->cluster_count;
      session->state.page_cluster_count = last->cluster_count;
      session->private_revealed_utf8_bytes = last->utf8_bytes;
    }
  } else {
    InstallNativeSource(session, selection->presentation);
  }
  DestroyProgram(old_program);
  return true;
}

void ArDialogueSession_Init(ArDialogueSession *session) {
  if (!session)
    return;
  memset(session, 0, sizeof(*session));
  session->state.struct_size = sizeof(session->state);
  session->state.abi_version = AR_DIALOGUE_SESSION_ABI_VERSION;
  session->private_magic = kDialogueSessionMagic;
}

void ArDialogueSession_Destroy(ArDialogueSession *session) {
  if (!IsInitialized(session))
    return;
  DestroyProgram((DialogueProgram *)session->private_program);
  ArDialogueSession_Init(session);
}

bool ArDialogueSession_Begin(ArDialogueSession *session,
                             const ArDialogueContentSelection *selection,
                             const char *semantic_id,
                             const ArDialogueValueResolver *resolver,
                             ArLanguagePackError *error) {
  if (error)
    error->message[0] = 0;
  if (!IsInitialized(session) || !semantic_id ||
      !ArLanguageContract_RouteAvailable(semantic_id,
                                         kArLanguageSourceProfile_Us)) {
    SetError(error, "unknown or unavailable U.S. dialogue route");
    return false;
  }
  if (resolver && !ValidateResolver(resolver, false, error))
    return false;
  ArDialogueStableState stable = {
      .struct_size = sizeof(stable),
      .abi_version = AR_DIALOGUE_SESSION_ABI_VERSION,
  };
  if (!CopyBounded(stable.message_id, sizeof(stable.message_id), semantic_id,
                   "semantic message id", error) ||
      !SnapshotValues(&stable, resolver, error))
    return false;

  ArDialogueSession scratch;
  ArDialogueSession_Init(&scratch);
  if (resolver) {
    scratch.private_resolver = *resolver;
    scratch.private_has_resolver = true;
  }
  if (!ActivateSelection(&scratch, selection, &stable, error)) {
    ArDialogueSession_Destroy(&scratch);
    return false;
  }
  ArDialogueSession_Destroy(session);
  *session = scratch;
  return true;
}

bool ArDialogueSession_Switch(ArDialogueSession *session,
                              const ArDialogueContentSelection *selection,
                              ArLanguagePackError *error) {
  if (error)
    error->message[0] = 0;
  if (!IsInitialized(session) || !session->state.message_id[0]) {
    SetError(error, "cannot switch an inactive dialogue session");
    return false;
  }
  return ActivateSelection(session, selection, &session->state, error);
}

bool ArDialogueSession_Next(ArDialogueSession *session,
                            ArDialogueToken *token,
                            ArLanguagePackError *error) {
  if (error)
    error->message[0] = 0;
  if (!IsInitialized(session) || !token || !session->state.message_id[0]) {
    SetError(error, "invalid dialogue session step");
    return false;
  }
  memset(token, 0, sizeof(*token));
  if (session->state.resolved_source == kArDialogueResolvedSource_NativeRom) {
    token->kind = kArDialogueToken_NativeAdapter;
    return true;
  }
  if (session->state.terminal) {
    token->kind = kArDialogueToken_End;
    return true;
  }
  if (session->state.wait_frames_remaining || session->state.awaiting_input ||
      session->state.awaiting_page_advance) {
    token->kind = kArDialogueToken_Blocked;
    return true;
  }
  DialogueProgram *program = (DialogueProgram *)session->private_program;
  if (!program || session->state.authored_page_index >= program->page_count) {
    SetError(error, "dialogue presentation state is invalid");
    return false;
  }
  DialoguePage *page = &program->pages[session->state.authored_page_index];
  while (session->private_cue_index < page->cue_count) {
    DialogueCue *cue = &page->cues[session->private_cue_index];
    if (cue->utf8_offset > session->private_revealed_utf8_bytes)
      break;
    if (cue->kind == kDialogueCue_Control) {
      if (cue->ordinal < session->state.completed_control_count) {
        session->private_cue_index++;
        continue;
      }
      if (cue->ordinal != session->state.completed_control_count) {
        SetError(error, "dialogue locked-control sequence is inconsistent");
        return false;
      }
      session->state.control_pending = true;
      token->kind = kArDialogueToken_Control;
      token->control_id = cue->control_id;
      token->control_ordinal = cue->ordinal;
      return true;
    }
    if (cue->kind == kDialogueCue_Wait) {
      session->private_cue_index++;
      if (cue->ordinal < session->state.completed_wait_count)
        continue;
      session->state.completed_wait_count = cue->ordinal + 1;
      session->state.wait_frames_total = cue->wait_frames;
      session->state.wait_frames_remaining = cue->wait_frames;
      token->kind = kArDialogueToken_WaitStarted;
      token->wait_frames = cue->wait_frames;
      return true;
    }
    if (cue->kind == kDialogueCue_Page) {
      session->private_cue_index++;
      session->state.awaiting_page_advance = true;
      token->kind = kArDialogueToken_PageComplete;
      return true;
    }
    session->private_cue_index++;
    session->state.terminal = true;
    token->kind = kArDialogueToken_End;
    return true;
  }
  if (session->private_revealed_utf8_bytes >= page->utf8_bytes) {
    SetError(error, "dialogue page has no terminal cue");
    return false;
  }
  size_t next = 0;
  uint32_t scalar = 0;
  if (!ArUnicodeGrapheme_Next(page->utf8, page->utf8_bytes,
                              session->private_revealed_utf8_bytes, &scalar,
                              &next)) {
    SetError(error, "dialogue page contains invalid UTF-8");
    return false;
  }
  session->private_revealed_utf8_bytes = next;
  session->state.revealed_cluster_count++;
  token->kind = kArDialogueToken_Grapheme;
  token->first_scalar = scalar;
  token->end_utf8_byte = next;
  return true;
}

bool ArDialogueSession_CompleteControl(ArDialogueSession *session,
                                       uint32_t control_ordinal) {
  if (!IsInitialized(session) || !session->state.control_pending ||
      session->state.resolved_source == kArDialogueResolvedSource_NativeRom)
    return false;
  DialogueProgram *program = (DialogueProgram *)session->private_program;
  if (!program || session->state.authored_page_index >= program->page_count)
    return false;
  DialoguePage *page = &program->pages[session->state.authored_page_index];
  if (session->private_cue_index >= page->cue_count)
    return false;
  DialogueCue *cue = &page->cues[session->private_cue_index];
  if (cue->kind != kDialogueCue_Control ||
      cue->ordinal != control_ordinal ||
      control_ordinal != session->state.completed_control_count)
    return false;
  session->private_cue_index++;
  session->state.completed_control_count++;
  session->state.control_pending = false;
  if (strncmp(cue->control_id, "yield.", 6) == 0)
    session->state.awaiting_input = true;
  return true;
}

void ArDialogueSession_TickWait(ArDialogueSession *session, uint32_t frames) {
  if (!IsInitialized(session) || !session->state.wait_frames_remaining)
    return;
  if (frames >= session->state.wait_frames_remaining)
    session->state.wait_frames_remaining = 0;
  else
    session->state.wait_frames_remaining -= frames;
}

bool ArDialogueSession_ResumeInput(ArDialogueSession *session) {
  if (!IsInitialized(session) || !session->state.awaiting_input)
    return false;
  session->state.awaiting_input = false;
  return true;
}

bool ArDialogueSession_AdvancePage(ArDialogueSession *session) {
  if (!IsInitialized(session) || !session->state.awaiting_page_advance ||
      session->state.resolved_source == kArDialogueResolvedSource_NativeRom)
    return false;
  DialogueProgram *program = (DialogueProgram *)session->private_program;
  if (!program || session->state.authored_page_index + 1 >= program->page_count)
    return false;
  session->state.awaiting_page_advance = false;
  session->state.authored_page_index++;
  DialoguePage *page = &program->pages[session->state.authored_page_index];
  session->state.revealed_cluster_count = 0;
  session->state.page_cluster_count = page->cluster_count;
  session->private_revealed_utf8_bytes = 0;
  session->private_cue_index = 0;
  return true;
}

bool ArDialogueSession_GetPage(const ArDialogueSession *session,
                               ArDialoguePageSnapshot *page) {
  if (!IsInitialized(session) || !page ||
      session->state.resolved_source == kArDialogueResolvedSource_NativeRom)
    return false;
  DialogueProgram *program = (DialogueProgram *)session->private_program;
  if (!program || session->state.authored_page_index >= program->page_count)
    return false;
  const DialoguePage *source =
      &program->pages[session->state.authored_page_index];
  *page = (ArDialoguePageSnapshot){
      .utf8 = source->utf8,
      .utf8_bytes = source->utf8_bytes,
      .revealed_utf8_bytes = session->private_revealed_utf8_bytes,
      .page_index = session->state.authored_page_index,
      .page_count = program->page_count,
      .revealed_cluster_count = session->state.revealed_cluster_count,
      .cluster_count = source->cluster_count,
      .inline_objects = source->objects,
      .inline_object_count = source->object_count,
      .source_revision = program->source_revision,
      .package_id = program->package_id,
      .locale = program->locale,
      .direction = program->direction,
  };
  return true;
}

bool ArDialogueSession_GetAuthoredPage(const ArDialogueSession *session,
                                       uint32_t page_index,
                                       ArDialoguePageSnapshot *page) {
  if (!IsInitialized(session) || !page ||
      session->state.resolved_source == kArDialogueResolvedSource_NativeRom)
    return false;
  DialogueProgram *program = (DialogueProgram *)session->private_program;
  if (!program || page_index >= program->page_count) return false;
  const DialoguePage *source = &program->pages[page_index];
  *page = (ArDialoguePageSnapshot){
      .utf8 = source->utf8,
      .utf8_bytes = source->utf8_bytes,
      .revealed_utf8_bytes = source->utf8_bytes,
      .page_index = page_index,
      .page_count = program->page_count,
      .revealed_cluster_count = source->cluster_count,
      .cluster_count = source->cluster_count,
      .inline_objects = source->objects,
      .inline_object_count = source->object_count,
      .source_revision = program->source_revision,
      .package_id = program->package_id,
      .locale = program->locale,
      .direction = program->direction,
  };
  return true;
}

bool ArDialogueSession_ObserveNativeProgress(
    ArDialogueSession *session, const ArDialogueNativeProgress *progress,
    ArLanguagePackError *error) {
  if (error)
    error->message[0] = 0;
  const uint32_t anchor_count =
      IsInitialized(session) && session->state.message_id[0]
          ? ArLanguageContract_RequiredAnchorCount(
                session->state.message_id, kArLanguageSourceProfile_Us)
          : 0;
  if (!IsInitialized(session) || !progress ||
      progress->struct_size <
          AR_MEMBER_END(ArDialogueNativeProgress, terminal) ||
      progress->abi_version != AR_DIALOGUE_SESSION_ABI_VERSION ||
      session->state.resolved_source != kArDialogueResolvedSource_NativeRom ||
      progress->authored_page_index >= kMaximumDialoguePages ||
      progress->revealed_unit_count > progress->page_unit_count ||
      (session->private_native_observation_seen &&
       (progress->authored_page_index < session->state.authored_page_index ||
        (progress->authored_page_index == session->state.authored_page_index &&
         progress->revealed_unit_count <
             session->state.revealed_cluster_count))) ||
      progress->completed_control_count > anchor_count ||
      progress->completed_control_count <
          session->state.completed_control_count ||
      progress->completed_wait_count > kMaximumDialogueCues ||
      progress->completed_wait_count < session->state.completed_wait_count ||
      progress->wait_frames_total > 3600 ||
      progress->wait_frames_remaining > progress->wait_frames_total ||
      (session->state.terminal && !progress->terminal) ||
      (progress->terminal &&
       (progress->control_pending || progress->awaiting_input ||
        progress->awaiting_page_advance || progress->wait_frames_remaining)) ||
      (progress->control_pending &&
       progress->completed_control_count >= anchor_count)) {
    SetError(error, "invalid or regressive native dialogue progress");
    return false;
  }
  session->state.authored_page_index = progress->authored_page_index;
  session->state.revealed_cluster_count = progress->revealed_unit_count;
  session->state.page_cluster_count = progress->page_unit_count;
  session->state.completed_control_count =
      progress->completed_control_count;
  session->state.completed_wait_count = progress->completed_wait_count;
  session->state.wait_frames_remaining = progress->wait_frames_remaining;
  session->state.wait_frames_total = progress->wait_frames_total;
  session->state.control_pending = progress->control_pending;
  session->state.awaiting_input = progress->awaiting_input;
  session->state.awaiting_page_advance = progress->awaiting_page_advance;
  session->state.terminal = progress->terminal;
  session->private_native_observation_seen = true;
  return true;
}

bool ArDialogueSession_SynchronizeNativeProgress(
    ArDialogueSession *session, const ArDialogueNativeProgress *progress,
    ArLanguagePackError *error) {
  if (error) error->message[0] = 0;
  if (!IsInitialized(session) || !progress ||
      progress->struct_size <
          AR_MEMBER_END(ArDialogueNativeProgress, terminal) ||
      progress->abi_version != AR_DIALOGUE_SESSION_ABI_VERSION ||
      session->state.resolved_source == kArDialogueResolvedSource_NativeRom ||
      progress->authored_page_index >= kMaximumDialoguePages ||
      progress->revealed_unit_count > progress->page_unit_count) {
    SetError(error, "invalid enhanced/native dialogue synchronization");
    return false;
  }
  DialogueProgram *program = (DialogueProgram *)session->private_program;
  if (!program || !program->page_count) {
    SetError(error, "enhanced dialogue program has no pages");
    return false;
  }

  uint32_t page_index = progress->authored_page_index;
  if (page_index >= program->page_count) page_index = program->page_count - 1u;
  DialoguePage *page = &program->pages[page_index];
  uint32_t revealed = page->cluster_count;
  /* A native continuation/end is authoritative completion. Decoder-unit
   * ratios are only an approximation while typing (dictionary/inline values
   * need not expand to the extraction's number of top-level observations). */
  if (progress->page_unit_count && !progress->awaiting_page_advance &&
      !progress->terminal) {
    const uint64_t numerator =
        (uint64_t)progress->revealed_unit_count * page->cluster_count;
    revealed = (uint32_t)((numerator + progress->page_unit_count - 1u) /
                          progress->page_unit_count);
    if (revealed > page->cluster_count) revealed = page->cluster_count;
  }

  session->state.authored_page_index = page_index;
  session->state.revealed_cluster_count = revealed;
  session->state.page_cluster_count = page->cluster_count;
  session->private_revealed_utf8_bytes = ByteOffsetForClusters(page, revealed);
  session->state.completed_control_count = progress->completed_control_count;
  session->state.completed_wait_count = progress->completed_wait_count;
  session->state.wait_frames_remaining = progress->wait_frames_remaining;
  session->state.wait_frames_total = progress->wait_frames_total;
  session->state.control_pending = progress->control_pending;
  session->state.awaiting_input = progress->awaiting_input;
  session->state.awaiting_page_advance = progress->awaiting_page_advance;
  session->state.terminal = progress->terminal;
  session->private_native_observation_seen = true;
  return true;
}

bool ArDialogueSession_ExportState(const ArDialogueSession *session,
                                   ArDialogueStableState *state) {
  if (!IsInitialized(session) || !state || !session->state.message_id[0])
    return false;
  *state = session->state;
  state->struct_size = sizeof(*state);
  state->abi_version = AR_DIALOGUE_SESSION_ABI_VERSION;
  return true;
}

bool ArDialogueSession_Restore(ArDialogueSession *session,
                               const ArDialogueContentSelection *selection,
                               const ArDialogueStableState *state,
                               const ArDialogueValueResolver *resolver,
                               ArLanguagePackError *error) {
  if (error)
    error->message[0] = 0;
  if (!IsInitialized(session) || !state ||
      state->struct_size < AR_MEMBER_END(ArDialogueStableState, values) ||
      state->abi_version != AR_DIALOGUE_SESSION_ABI_VERSION ||
      !FixedStringTerminated(state->message_id, sizeof(state->message_id)) ||
      !FixedStringTerminated(state->package_id, sizeof(state->package_id)) ||
      !ArLanguageContract_RouteAvailable(state->message_id,
                                         kArLanguageSourceProfile_Us) ||
      state->presentation < kArDialoguePresentation_NativeRetail ||
      state->presentation > kArDialoguePresentation_Enhanced ||
      state->resolved_source < kArDialogueResolvedSource_NativeRom ||
      state->resolved_source > kArDialogueResolvedSource_SelectedPack ||
      state->authored_page_index >= kMaximumDialoguePages ||
      state->revealed_cluster_count > state->page_cluster_count ||
      state->completed_wait_count > kMaximumDialogueCues ||
      state->wait_frames_total > 3600 ||
      state->completed_control_count >
          ArLanguageContract_RequiredAnchorCount(
              state->message_id, kArLanguageSourceProfile_Us) ||
      state->wait_frames_remaining > state->wait_frames_total ||
      (state->terminal &&
       (state->control_pending || state->awaiting_input ||
        state->awaiting_page_advance || state->wait_frames_remaining)) ||
      (state->control_pending &&
       (state->awaiting_input || state->awaiting_page_advance ||
        state->wait_frames_remaining)) ||
      (state->control_pending &&
       state->completed_control_count >=
           ArLanguageContract_RequiredAnchorCount(
               state->message_id, kArLanguageSourceProfile_Us)) ||
      !ValidateStableValues(state, error)) {
    if (error && !error->message[0])
      SetError(error, "saved dialogue state is invalid");
    return false;
  }
  if (resolver && !ValidateResolver(resolver, false, error))
    return false;
  ArDialogueSession scratch;
  ArDialogueSession_Init(&scratch);
  if (resolver) {
    scratch.private_resolver = *resolver;
    scratch.private_has_resolver = true;
  }
  if (!ActivateSelection(&scratch, selection, state, error)) {
    ArDialogueSession_Destroy(&scratch);
    return false;
  }
  ArDialogueSession_Destroy(session);
  *session = scratch;
  return true;
}
