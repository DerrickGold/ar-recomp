#include "actraiser/actraiser_localization_runtime.h"
#include "actraiser/actraiser_localization_art.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser/actraiser_localization_compose_state.h"
#include "actraiser/actraiser_localization_name_entry.h"
#include "actraiser/actraiser_localization_routes.h"
#include "actraiser/actraiser_localization_schedule.h"
#include "actraiser/actraiser_localization_text.h"
#include "actraiser/actraiser_localization_values.h"
#include "actraiser_game.h"
#include "deterministic_hash.h"
#include "localization/dialogue_session.h"
#include "localization/language_pack.h"
#include "localization/unicode_grapheme.h"
#include "save_system.h"
#include "settings.h"

enum { kManifestPathCapacity = 1024 };

/* Settings serialize these enum ordinals; pin the adapter's direct mapping. */
_Static_assert(kArDialoguePresentation_NativeRetail == 0 &&
               kArDialoguePresentation_Enhanced == 1,
               "update localization presentation settings mapping");
_Static_assert(kArEnhancedTextSampling_Crisp == 0 &&
               kArEnhancedTextSampling_Smooth == 1 &&
               kArEnhancedTextPixelation_None == 0 &&
               kArEnhancedTextPixelation_LowResolution == 1 &&
               kArEnhancedTextPixelation_Mosaic == 2,
               "update localization font settings mapping");

typedef struct DialogueWindow {
  bool valid;
  uint16_t native_first_page;
  uint16_t native_clear_control_count;
  uint32_t current_page;
  uint64_t revision;
  size_t bytes;
  size_t current_page_offset;
  uint32_t current_page_clusters;
  uint32_t current_page_prefix_clusters;
  uint32_t clusters;
  char text[kArLocalizationFrameTextCapacity];
} DialogueWindow;

typedef struct LocalizationRuntime {
  bool configured;
  bool enabled;
  bool refresh_pending;
  bool native_attempted;
  bool selected_attempted;
  int content;
  int presentation;
  ArLanguagePack selected_pack;
  ArLanguagePack native_pack;
  const ArLanguagePack *pack;
  ArDialogueSession session;
  DialogueWindow dialogue_window;
  ActRaiserLocalizationComposeState compose;
  ActRaiserLocalizationValues values;
  ActRaiserLocalizationNameEntryState name_entry;
  ActRaiserLocalizationNameEntryTracker name_tracker;
  uint64_t name_entry_applied_native_revision;
  uint64_t name_entry_completed_observation_serial;
  uint64_t name_entry_completed_compose_serial;
  uint64_t compose_observation_serial;
  uint64_t observation_serial;
  uint64_t failed_observation_serial;
  uint64_t dialogue_ticket;
  bool scheduled_dialogue;
  uint16_t scheduled_window_first_page;
  uint16_t scheduled_window_clear_control;
  bool inherited_native_page_wait;
  uint16_t inherited_native_page;
  bool native_handoff_valid;
  uint16_t native_handoff_game_frame;
  ActRaiserLocalizationTextObservation native_handoff;
  const ActRaiserLocalizationRoute *route;
  char manifest_path[kManifestPathCapacity];
  char native_manifest_path[kManifestPathCapacity];
  char primary_font_path[kArLocalizationFrameFontPathCapacity];
  char fallback_font_paths[kArTextPresentationMaximumFallbackFonts]
                          [kArLocalizationFrameFontPathCapacity];
} LocalizationRuntime;

static LocalizationRuntime s_runtime;
static ArTextPresentationHost s_presentation_host;
/* Never reuse a ticket across game resets: retained old frames may outlive the
 * invocation they describe. This counter carries no emulated game state. */
static uint64_t s_next_dialogue_ticket;

static void ScheduleFailed(const char *reason);

void ActRaiserLocalizationRuntime_SetPresentationHost(
    const ArTextPresentationHost *host) {
  s_presentation_host = (ArTextPresentationHost){0};
  if (!host || host->struct_size <
          offsetof(ArTextPresentationHost, prepare_font) + sizeof(host->prepare_font) ||
      host->abi_version != AR_TEXT_PRESENTATION_ABI_VERSION || !host->prepare_font)
    return;
  s_presentation_host = *host;
}

static bool SynchronizeObservedDialogue(const ArLanguagePack *values_pack,
                                        ArLanguagePackError *error);

static bool CopyPath(char *destination, size_t capacity, const char *source) {
  const size_t length = source ? strlen(source) : 0;
  if (!source || !length || length >= capacity) return false;
  memcpy(destination, source, length + 1u);
  return true;
}

static bool ResolveFontPath(const char *manifest_path, const char *font,
                            char *destination, size_t capacity) {
  if (!manifest_path || !font || !destination || !capacity)
    return false;
  if (!strcmp(font, "builtin:actraiser-sans"))
    return CopyPath(destination, capacity,
                    "game-assets/fonts/noto/"
                    "NotoSans-SemiCondensedExtraBold.ttf");
  if (!strncmp(font, "builtin:", 8))
    return false;
  const char *slash = strrchr(manifest_path, '/');
#ifdef _WIN32
  const char *backslash = strrchr(manifest_path, '\\');
  if (!slash || (backslash && backslash > slash))
    slash = backslash;
#endif
  const size_t directory_bytes =
      slash ? (size_t)(slash - manifest_path + 1) : 0;
  const size_t font_bytes = strlen(font);
  if (directory_bytes + font_bytes >= capacity)
    return false;
  if (directory_bytes)
    memcpy(destination, manifest_path, directory_bytes);
  memcpy(destination + directory_bytes, font, font_bytes + 1u);
  return true;
}

static void MakeSelection(int content, int presentation,
                          ArDialogueContentSelection *selection) {
  *selection = (ArDialogueContentSelection){
      .struct_size = sizeof(*selection),
      .abi_version = AR_DIALOGUE_SESSION_ABI_VERSION,
      .presentation = (ArDialoguePresentation)presentation,
      .selected_pack = presentation && content
          ? &s_runtime.selected_pack : NULL,
      .native_us_enhanced_pack =
          s_runtime.native_pack.content_revision
              ? &s_runtime.native_pack : NULL,
  };
}

static bool LoadRuntimePack(ArLanguagePack *pack, const char *manifest,
                           bool native, ArLanguagePackError *error) {
  ArLanguagePackIo io;
  ArLanguagePackFileIo_Init(&io);
  if (!manifest || !manifest[0]) {
    snprintf(error->message, sizeof(error->message), "no pack configured");
    return false;
  }
  if (!ArLanguagePack_Load(pack, &io, manifest, error)) return false;
  const ArLanguagePackMetadata *metadata = ArLanguagePack_GetMetadata(pack);
  if (!metadata || metadata->target != kArLanguagePackTarget_UsRuntime ||
      metadata->source_profile != kArLanguageSourceProfile_Us ||
      (native && metadata->coverage != kArLanguagePackCoverage_Complete) ||
      !ArLanguageContract_ValidatePack(pack, NULL, error)) {
    if (!error->message[0])
      snprintf(error->message, sizeof(error->message),
               "source is not a compatible%s USA runtime pack",
               native ? " complete" : "");
    ArLanguagePack_Destroy(pack);
    return false;
  }
  return true;
}

static bool EnsureConfigured(void) {
  /* Untouched native mode does not load scripts or initialize the font stack.
   * Once enhanced mode was used, retain semantic observations while hidden so
   * it can be enabled again without waiting for the game to redraw a menu. */
  if (!s_runtime.configured && !g_settings.localization_presentation)
    return false;
  if (!s_runtime.configured) {
    s_runtime.configured = true;
    ArLanguagePack_Init(&s_runtime.selected_pack);
    ArLanguagePack_Init(&s_runtime.native_pack);
    ArDialogueSession_Init(&s_runtime.session);
    ActRaiserLocalizationComposeState_Init(&s_runtime.compose);
    ActRaiserLocalizationNameEntryTracker_Init(&s_runtime.name_tracker);
    s_runtime.content = -1;
    s_runtime.presentation = -1;
    (void)CopyPath(s_runtime.manifest_path, sizeof(s_runtime.manifest_path),
                   getenv("AR_LOCALIZATION_PACK"));
    const char *native = getenv("AR_LOCALIZATION_NATIVE_PACK");
    (void)CopyPath(s_runtime.native_manifest_path,
                   sizeof(s_runtime.native_manifest_path),
                   native && native[0] ? native :
                       "game-assets/languages/native-us/pack.ini");
  }
  const int content = g_settings.localization_content;
  const int presentation = g_settings.localization_presentation;
  if (content == s_runtime.content && presentation == s_runtime.presentation)
    return s_runtime.enabled;

  ArLanguagePackError error = {{0}};
  if (presentation && !s_runtime.native_attempted) {
    s_runtime.native_attempted = true;
    if (!LoadRuntimePack(&s_runtime.native_pack,
                         s_runtime.native_manifest_path, true, &error))
      fprintf(stderr, "[localization] enhanced USA fallback unavailable: %s\n",
              error.message);
  }
  if (presentation && content && !s_runtime.selected_attempted) {
    s_runtime.selected_attempted = true;
    error.message[0] = 0;
    (void)LoadRuntimePack(&s_runtime.selected_pack, s_runtime.manifest_path,
                          false, &error);
  }
  const ArLanguagePack *pack =
      content ? &s_runtime.selected_pack : &s_runtime.native_pack;
  const ArLanguagePackMetadata *metadata =
      pack->content_revision ? ArLanguagePack_GetMetadata(pack) : NULL;
  char font_path[kArLocalizationFrameFontPathCapacity] = {0};
  char fallback_paths[kArTextPresentationMaximumFallbackFonts]
                     [kArLocalizationFrameFontPathCapacity] = {{0}};
  const char *fallbacks[kArTextPresentationMaximumFallbackFonts] = {0};
  if (presentation &&
      (!metadata || !ResolveFontPath(content ? s_runtime.manifest_path
                                             : s_runtime.native_manifest_path,
                                     metadata->primary_font, font_path,
                                     sizeof(font_path)))) {
    if (!error.message[0])
      snprintf(error.message, sizeof(error.message),
               "requested source or font is unavailable");
    goto reject;
  }
  if (presentation) {
    if (metadata->fallback_font_count >
        kArTextPresentationMaximumFallbackFonts) {
      snprintf(error.message, sizeof(error.message), "too many fallback fonts");
      goto reject;
    }
    for (uint32_t i = 0; i < metadata->fallback_font_count; ++i) {
      if (!ResolveFontPath(content ? s_runtime.manifest_path
                                   : s_runtime.native_manifest_path,
                           metadata->fallback_fonts[i], fallback_paths[i],
                           sizeof(fallback_paths[i]))) {
        snprintf(error.message, sizeof(error.message),
                 "fallback font path is unavailable");
        goto reject;
      }
      fallbacks[i] = fallback_paths[i];
    }
    const ArTextPresentationFont font = {
        .struct_size = sizeof(font),
        .abi_version = AR_TEXT_PRESENTATION_ABI_VERSION,
        .stack_id = metadata->primary_font,
        .primary_path = font_path,
        .revision = pack->content_revision,
        .fallback_paths = fallbacks,
        .fallback_count = metadata->fallback_font_count,
    };
    if (!s_presentation_host.prepare_font ||
        !s_presentation_host.prepare_font(s_presentation_host.context, &font,
                                          error.message,
                                          sizeof(error.message))) {
      if (!error.message[0])
        snprintf(error.message, sizeof(error.message),
                 "enhanced text presentation is unavailable");
      goto reject;
    }
  }
  ArDialogueContentSelection selection;
  MakeSelection(content, presentation, &selection);
  const bool was_scheduled = ActRaiserLocalizationRuntime_DialogueScheduled();
  if (!was_scheduled && !SynchronizeObservedDialogue(
                            s_runtime.pack ? s_runtime.pack : pack, &error))
    goto reject;
  if (s_runtime.session.state.message_id[0] &&
      !ArDialogueSession_SwitchBounded(&s_runtime.session, &selection,
                                       kArLocalizationFrameTextCapacity, &error))
    goto reject;
  s_runtime.content = content;
  s_runtime.presentation = presentation;
  s_runtime.scheduled_dialogue = presentation && s_runtime.route &&
                                 s_runtime.session.state.resolved_source !=
                                     kArDialogueResolvedSource_NativeRom;
  if (was_scheduled && !s_runtime.scheduled_dialogue) {
    s_runtime.native_handoff.struct_size = sizeof(s_runtime.native_handoff);
    s_runtime.native_handoff_valid =
        ActRaiserLocalizationText_CopyObservation(&s_runtime.native_handoff);
    s_runtime.native_handoff_game_frame =
        (uint16_t)(g_ram[kActRaiserWram_GameFrame] |
                   ((uint16_t)g_ram[kActRaiserWram_GameFrame + 1u] << 8));
  } else if (s_runtime.scheduled_dialogue) {
    s_runtime.native_handoff_valid = false;
  }
  if (s_runtime.scheduled_window_first_page >
      s_runtime.session.state.authored_page_index)
    s_runtime.scheduled_window_first_page =
        (uint16_t)s_runtime.session.state.authored_page_index;
  s_runtime.dialogue_window.valid = false;
  s_runtime.dialogue_ticket = 0;
  if (presentation) {
    s_runtime.pack = pack;
    memcpy(s_runtime.primary_font_path, font_path, sizeof(font_path));
    memcpy(s_runtime.fallback_font_paths, fallback_paths,
           sizeof(fallback_paths));
    s_runtime.enabled = true;
  }
  s_runtime.refresh_pending = true;
  s_runtime.name_entry_applied_native_revision = 0;
  fprintf(stderr, "[localization] %s: %s\n",
          presentation ? "enhanced" : "native",
          presentation ? metadata->display_name : "untouched USA text/font");
  return s_runtime.enabled;

reject:
  fprintf(stderr,
          "[localization] selection rejected (%s); prior selection retained\n",
          error.message);
  g_settings.localization_content =
      s_runtime.content < 0 ? 0 : s_runtime.content;
  g_settings.localization_presentation =
      s_runtime.presentation < 0 ? 0 : s_runtime.presentation;
  return s_runtime.enabled;
}

void ActRaiserLocalizationRuntime_ApplySettings(void) {
  (void)EnsureConfigured();
}

static bool CaptureValuesForPack(const ArLanguagePack *pack) {
  char master_name[kActRaiserLocalizationMasterNameCapacity];
  char native_name[kActRaiserLocalizationNameLength + 1];
  if (!ActRaiserLocalizationNameEntry_CopyNativeName(
          g_ram, kActRaiserWramSize, native_name, sizeof(native_name)))
    snprintf(master_name, sizeof(master_name), "Master");
  else if (!SaveSystem_CopyLocalizedPlayerName(native_name, master_name,
                                               sizeof(master_name)))
    snprintf(master_name, sizeof(master_name), "%s", native_name);
  const bool captured = ActRaiserLocalizationValues_Capture(
      &s_runtime.values, g_ram, kActRaiserWramSize,
      pack, master_name);
  memset(&s_runtime.name_entry, 0, sizeof(s_runtime.name_entry));
  (void)ActRaiserLocalizationNameEntry_Capture(
      &s_runtime.name_entry, g_ram, kActRaiserWramSize);
  return captured;
}

static bool CaptureValues(void) {
  return CaptureValuesForPack(s_runtime.pack);
}

static uint32_t CountClusters(const char *utf8, size_t bytes) {
  uint32_t count = 0;
  size_t offset = 0;
  while (offset < bytes) {
    size_t next = 0;
    if (!ArUnicodeGrapheme_Next(utf8, bytes, offset, NULL, &next) ||
        next <= offset || count == UINT32_MAX)
      return UINT32_MAX;
    offset = next;
    ++count;
  }
  return count;
}

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

static bool NormalizeText(
    const char *source, size_t source_bytes,
    const ArDialogueInlineObject *source_objects, size_t source_object_count,
    bool preserve_blank_lines,
    char *destination, size_t capacity, size_t *destination_bytes,
    ArLocalizationInlineObjectSnapshot *destination_objects,
    size_t destination_object_capacity, uint8_t *destination_object_count) {
  if (!source || !destination || !capacity || !destination_bytes ||
      !destination_object_count ||
      (source_object_count && (!source_objects || !destination_objects)) ||
      source_object_count > destination_object_capacity ||
      source_object_count > UINT8_MAX)
    return false;
  size_t written = 0;
  size_t object_index = 0;
  bool pending_space = false;
  for (size_t index = 0; index < source_bytes; ++index) {
    const char byte = source[index];
    if (byte == ' ' || byte == '\t' || byte == '\r') {
      pending_space = written && destination[written - 1u] != '\n';
      continue;
    }
    if (byte == '\n') {
      while (written && destination[written - 1u] == ' ') --written;
      if (written &&
          (preserve_blank_lines || destination[written - 1u] != '\n')) {
        if (written + 1u >= capacity) return false;
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
    destination[written++] = byte;
  }
  while (written && (destination[written - 1u] == ' ' ||
                     destination[written - 1u] == '\n'))
    --written;
  destination[written] = 0;
  *destination_bytes = written;
  if (object_index != source_object_count) return false;
  *destination_object_count = (uint8_t)object_index;
  /* A successfully resolved empty message is intentional, not a missing
   * translation. It still owns its native cells while the UI is alive. */
  return true;
}

/* Cache the logical window only when its source/page range changes. Reveal
 * ticks change a byte boundary, not the text, shaping request, or old lines. */
static bool BuildDialogueWindow(
    const ArDialoguePageSnapshot *current,
    const ActRaiserLocalizationTextObservation *observation) {
  DialogueWindow *window = &s_runtime.dialogue_window;
  if (window->valid && window->revision == current->source_revision &&
      window->native_first_page == observation->window_start_page &&
      window->native_clear_control_count == observation->window_start_control_count &&
      window->current_page == current->page_index)
    return true;
  window->valid = false;
  uint32_t first_page = observation->window_start_page;
  size_t first_offset = 0;
  if (observation->window_start_control_count) {
    if (!ArDialogueSession_GetControlPosition(
          &s_runtime.session, observation->window_start_control_count - 1u,
          &first_page, &first_offset) || first_page > current->page_index)
      return false;
  }
  if (first_page > current->page_index) first_page = current->page_index;
  window->bytes = 0;
  for (uint32_t index = first_page; index <= current->page_index; ++index) {
    ArDialoguePageSnapshot page;
    if (!ArDialogueSession_GetAuthoredPage(&s_runtime.session, index, &page))
      return false;
    const size_t source_offset = index == first_page ? first_offset : 0;
    if (source_offset > page.utf8_bytes) return false;
    if (index != first_page) {
      if (window->bytes + 1u >= sizeof(window->text)) return false;
      window->text[window->bytes++] = '\n';
    }
    const size_t offset = window->bytes;
    size_t bytes = 0;
    uint8_t object_count = 0;
    if (!NormalizeText(page.utf8 + source_offset, page.utf8_bytes - source_offset,
                       NULL, 0, false,
                       window->text + offset, sizeof(window->text) - offset,
                       &bytes, NULL, 0, &object_count))
      return false;
    window->bytes += bytes;
    if (index == current->page_index) {
      window->current_page_offset = offset;
      window->current_page_clusters = CountClusters(window->text + offset, bytes);
      window->current_page_prefix_clusters = CountClusters(page.utf8, source_offset);
    }
  }
  window->clusters = CountClusters(window->text, window->bytes);
  if (window->clusters == UINT32_MAX) return false;
  window->native_first_page = observation->window_start_page;
  window->native_clear_control_count = observation->window_start_control_count;
  window->current_page = current->page_index;
  window->revision = current->source_revision;
  window->valid = true;
  s_runtime.dialogue_ticket = s_runtime.scheduled_dialogue
      ? ++s_next_dialogue_ticket : 0;
  return true;
}

/* The logical row immediately before the five keyboard rows is an entry-field
 * underline slot, not translatable wording. Remove the extraction-era dash
 * glyphs while retaining the hard line, then attach semantic underlines to
 * the eight shaped name graphemes independently of keyboard geometry. */
static bool ClearNameEntryUnderline(
    char *utf8, size_t *utf8_bytes,
    ArLocalizationInlineObjectSnapshot *objects, uint8_t object_count) {
  if (!utf8 || !utf8_bytes || !*utf8_bytes) return false;
  size_t starts[64];
  size_t ends[64];
  size_t line_count = 0;
  size_t start = 0;
  for (size_t index = 0; index <= *utf8_bytes; ++index) {
    if (index != *utf8_bytes && utf8[index] != '\n') continue;
    if (line_count >= sizeof(starts) / sizeof(starts[0])) return false;
    starts[line_count] = start;
    ends[line_count] = index;
    ++line_count;
    start = index + 1u;
  }
  if (line_count < kActRaiserLocalizationNameEntryRows + 1u) return false;
  const size_t line =
      line_count - kActRaiserLocalizationNameEntryRows - 1u;
  const size_t remove_start = starts[line];
  const size_t remove_end = ends[line];
  if (remove_end < remove_start) return false;
  const size_t removed = remove_end - remove_start;
  for (uint8_t index = 0; index < object_count; ++index) {
    if (objects[index].end_utf8_byte > remove_start &&
        objects[index].end_utf8_byte <= remove_end)
      return false;
    if (objects[index].end_utf8_byte > remove_end)
      objects[index].end_utf8_byte -= (uint32_t)removed;
  }
  memmove(utf8 + remove_start, utf8 + remove_end,
          *utf8_bytes - remove_end + 1u);
  *utf8_bytes -= removed;
  return true;
}

static void ContentSelection(ArDialogueContentSelection *selection) {
  MakeSelection(s_runtime.content < 0 ? 0 : s_runtime.content,
                s_runtime.presentation < 0 ? 0 : s_runtime.presentation, selection);
}

static void ValueResolver(const ActRaiserLocalizationValues *values,
                          ArDialogueValueResolver *resolver) {
  *resolver = (ArDialogueValueResolver){
      .struct_size = sizeof(*resolver),
      .abi_version = AR_DIALOGUE_VALUE_RESOLVER_ABI_VERSION,
      .context = (void *)values,
      .resolve = ActRaiserLocalizationValues_Resolve,
  };
}

static bool InsertInlineObject(
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

static bool InsertNameEntryText(
    char *utf8, size_t capacity, size_t *utf8_bytes, size_t offset,
    const char *insertion, size_t insertion_bytes,
    ArLocalizationInlineObjectSnapshot *objects, uint8_t object_count) {
  if (!utf8 || !capacity || !utf8_bytes || !insertion || !insertion_bytes ||
      offset > *utf8_bytes || insertion_bytes >= capacity - *utf8_bytes)
    return false;
  memmove(utf8 + offset + insertion_bytes, utf8 + offset,
          *utf8_bytes - offset + 1u);
  memcpy(utf8 + offset, insertion, insertion_bytes);
  *utf8_bytes += insertion_bytes;
  for (uint8_t index = 0; index < object_count; ++index) {
    /* An endpoint equal to the insertion point belongs to the preceding
     * cluster. Only objects attached to following text move. */
    if (objects[index].end_utf8_byte > offset)
      objects[index].end_utf8_byte += (uint32_t)insertion_bytes;
  }
  return true;
}

static bool CollectNameEntryLines(const char *utf8, size_t utf8_bytes,
                                  size_t *starts, size_t *ends,
                                  size_t capacity, size_t *line_count) {
  if (!utf8 || !utf8_bytes || !starts || !ends || !capacity || !line_count)
    return false;
  size_t count = 0;
  size_t start = 0;
  for (size_t index = 0; index <= utf8_bytes; ++index) {
    if (index != utf8_bytes && utf8[index] != '\n') continue;
    if (count >= capacity) return false;
    starts[count] = start;
    ends[count] = index;
    ++count;
    start = index + 1u;
  }
  *line_count = count;
  return true;
}

/* Enhanced glyphs are variable-width, but the retail name grid reserves a
 * blank tile before every key for its arrow. Retain that visual grammar by
 * expanding each normalized inter-key separator to a four-space gutter. The
 * glyphs remain shaped/VWF; only the navigation affordance has fixed room. */
static bool ExpandNameEntryKeyGutters(
    char *utf8, size_t capacity, size_t *utf8_bytes,
    ArLocalizationInlineObjectSnapshot *objects, uint8_t object_count) {
  size_t starts[64];
  size_t ends[64];
  size_t line_count = 0;
  if (!CollectNameEntryLines(
          utf8, *utf8_bytes, starts, ends,
          sizeof(starts) / sizeof(starts[0]), &line_count) ||
      line_count < kActRaiserLocalizationNameEntryRows)
    return false;
  const size_t first = line_count - kActRaiserLocalizationNameEntryRows;
  for (size_t line = line_count; line-- > first;) {
    for (size_t offset = ends[line]; offset-- > starts[line];) {
      if (utf8[offset] != ' ' || offset == starts[line] ||
          offset + 1u >= ends[line] || utf8[offset - 1u] == ' ' ||
          utf8[offset + 1u] == ' ')
        continue;
      if (!InsertNameEntryText(
              utf8, capacity, utf8_bytes, offset, "   ", 3u,
              objects, object_count))
        return false;
    }
  }
  return true;
}

static bool InsertNameEntryPageIndicator(
    char *utf8, size_t capacity, size_t *utf8_bytes,
    uint32_t page_index, uint32_t page_count,
    ArLocalizationInlineObjectSnapshot *objects, uint8_t object_count) {
  if (page_count <= 1u) return true;
  size_t starts[64];
  size_t ends[64];
  size_t line_count = 0;
  if (!CollectNameEntryLines(
          utf8, *utf8_bytes, starts, ends,
          sizeof(starts) / sizeof(starts[0]), &line_count) ||
      line_count < kActRaiserLocalizationNameEntryRows)
    return false;
  char indicator[64];
  const int written = snprintf(
      indicator, sizeof(indicator), "< %u/%u >\n",
      (unsigned)(page_index + 1u), (unsigned)page_count);
  if (written <= 0 || (size_t)written >= sizeof(indicator)) return false;
  return InsertNameEntryText(
      utf8, capacity, utf8_bytes,
      starts[line_count - kActRaiserLocalizationNameEntryRows],
      indicator, (size_t)written, objects, object_count);
}

static bool InsertNameEntryFieldUnderlines(
    const char *utf8, size_t utf8_bytes,
    ArLocalizationInlineObjectSnapshot *objects, size_t capacity,
    uint8_t *object_count) {
  if (!utf8 || !utf8_bytes || !objects || !object_count) return false;
  size_t starts[64];
  size_t ends[64];
  size_t line_count = 0;
  size_t start = 0;
  for (size_t index = 0; index <= utf8_bytes; ++index) {
    if (index != utf8_bytes && utf8[index] != '\n') continue;
    if (line_count >= sizeof(starts) / sizeof(starts[0])) return false;
    starts[line_count] = start;
    ends[line_count] = index;
    ++line_count;
    start = index + 1u;
  }
  if (line_count < kActRaiserLocalizationNameEntryRows + 2u) return false;
  const size_t name_line =
      line_count - kActRaiserLocalizationNameEntryRows - 2u;
  size_t offset = starts[name_line];
  uint8_t graphemes = 0;
  while (offset < ends[name_line]) {
    size_t next = 0;
    if (!ArUnicodeGrapheme_Next(
            utf8, utf8_bytes, offset, NULL, &next) ||
        next <= offset || next > ends[name_line] || next > UINT32_MAX ||
        !InsertInlineObject(
            objects, capacity, object_count,
            (ArLocalizationInlineObjectSnapshot){
                kArLocalizationInlineObject_NameFieldUnderline,
                (uint32_t)next}))
      return false;
    offset = next;
    ++graphemes;
  }
  return graphemes == kActRaiserLocalizationNameLength;
}

static uint64_t NameEntrySourceRevision(uint64_t source_revision) {
  uint64_t revision = DeterministicHash_Fnv1a64(
      DETERMINISTIC_HASH_FNV1A64_OFFSET,
      &source_revision, sizeof(source_revision));
  revision = DeterministicHash_Fnv1a64(
      revision, &s_runtime.name_entry.revision,
      sizeof(s_runtime.name_entry.revision));
  revision = DeterministicHash_Fnv1a64(
      revision, &s_runtime.name_tracker.revision,
      sizeof(s_runtime.name_tracker.revision));
  return revision ? revision : 1u;
}

static bool ResolveNameEntryText(
    ArDialogueContentSelection *selection,
    ActRaiserLocalizationValues *values,
    ArDialogueValueResolver *resolver,
    char *utf8, size_t utf8_capacity, size_t *utf8_bytes,
    ArLocalizationInlineObjectSnapshot *inline_objects,
    size_t inline_object_capacity, uint8_t *inline_object_count,
    uint64_t *page_source_revision, ArLanguagePackError *error) {
  if (!selection || !values || !resolver || !utf8 || !utf8_bytes ||
      !inline_object_count || !page_source_revision || !error ||
      !s_runtime.name_entry.revision)
    return false;

  /* Resolve once to discover the authored page count and map this native
   * transition into the canonical Unicode buffer. The final pass snapshots
   * values again so a newly typed grapheme appears in the same frame. */
  ArDialogueSession probe;
  ArDialogueSession_Init(&probe);
  bool resolved = ArDialogueSession_Begin(
      &probe, selection, "name_entry.prompt_and_alphabet", resolver, error);
  ArDialoguePageSnapshot page;
  if (resolved) resolved = ArDialogueSession_GetPage(&probe, &page);
  if (resolved) {
    resolved = ActRaiserLocalizationNameEntryTracker_SelectPage(
        &s_runtime.name_tracker, &s_runtime.name_entry, page.page_count) &&
        ArDialogueSession_GetAuthoredPage(
            &probe, s_runtime.name_tracker.keyboard_page, &page);
  }
  char normalized[kActRaiserLocalizationComposeTextCapacity];
  size_t normalized_bytes = 0;
  ArLocalizationInlineObjectSnapshot
      normalized_objects[kArLocalizationFrameInlineObjectCapacity];
  uint8_t normalized_object_count = 0;
  if (resolved) {
    resolved = NormalizeText(
        page.utf8, page.utf8_bytes,
        page.inline_objects, page.inline_object_count, false,
        normalized, sizeof(normalized), &normalized_bytes,
        normalized_objects,
        sizeof(normalized_objects) / sizeof(normalized_objects[0]),
        &normalized_object_count);
  }
  if (resolved) {
    resolved = ClearNameEntryUnderline(
        normalized, &normalized_bytes,
        normalized_objects, normalized_object_count);
  }
  uint32_t selected_start = 0;
  uint32_t selected_end = 0;
  if (resolved) {
    resolved = ActRaiserLocalizationNameEntry_SelectedKeyRange(
        &s_runtime.name_entry, normalized, normalized_bytes,
        &selected_start, &selected_end) &&
        ActRaiserLocalizationNameEntryTracker_Synchronize(
            &s_runtime.name_tracker, &s_runtime.name_entry,
            normalized + selected_start, selected_end - selected_start);
  }
  ArDialogueSession_Destroy(&probe);
  if (!resolved) return false;

  char display_name[kActRaiserLocalizationUnicodeNameDisplayCapacity];
  if (!ActRaiserLocalizationNameEntryTracker_CopyDisplayName(
          &s_runtime.name_tracker, display_name, sizeof(display_name)))
    return false;
  snprintf(values->master_name, sizeof(values->master_name), "%s",
           display_name);
  ValueResolver(values, resolver);

  ArDialogueSession session;
  ArDialogueSession_Init(&session);
  resolved = ArDialogueSession_Begin(
      &session, selection, "name_entry.prompt_and_alphabet", resolver, error);
  if (resolved) {
    resolved = ArDialogueSession_GetAuthoredPage(
        &session, s_runtime.name_tracker.keyboard_page, &page);
  }
  if (resolved) {
    resolved = NormalizeText(
        page.utf8, page.utf8_bytes,
        page.inline_objects, page.inline_object_count, false,
        utf8, utf8_capacity, utf8_bytes,
        inline_objects, inline_object_capacity, inline_object_count);
  }
  if (resolved) {
    resolved = ClearNameEntryUnderline(
        utf8, utf8_bytes, inline_objects, *inline_object_count);
  }
  if (resolved) {
    resolved = InsertNameEntryFieldUnderlines(
        utf8, *utf8_bytes, inline_objects,
        inline_object_capacity, inline_object_count);
  }
  if (resolved) {
    resolved = InsertNameEntryPageIndicator(
        utf8, utf8_capacity, utf8_bytes, page.page_index, page.page_count,
        inline_objects, *inline_object_count);
  }
  if (resolved) {
    resolved = ExpandNameEntryKeyGutters(
        utf8, utf8_capacity, utf8_bytes,
        inline_objects, *inline_object_count);
  }
  if (resolved) {
    resolved = ActRaiserLocalizationNameEntry_SelectedKeyRange(
        &s_runtime.name_entry, utf8, *utf8_bytes,
        &selected_start, &selected_end) &&
        InsertInlineObject(
            inline_objects, inline_object_capacity, inline_object_count,
            (ArLocalizationInlineObjectSnapshot){
                kArLocalizationInlineObject_NameCursor, selected_end});
  }
  if (resolved) *page_source_revision = page.source_revision;
  ArDialogueSession_Destroy(&session);
  if (resolved)
    s_runtime.name_entry_applied_native_revision =
        s_runtime.name_entry.revision;
  return resolved;
}

static bool ResolveComposeText(
    void *context, const char *semantic_id,
    char *utf8, size_t utf8_capacity, size_t *utf8_bytes,
    uint32_t *cluster_count, uint64_t *source_revision,
    ArLocalizationInlineObjectSnapshot *inline_objects,
    size_t inline_object_capacity, uint8_t *inline_object_count,
    char *error_text, size_t error_capacity) {
  (void)context;
  if (!s_runtime.presentation) return false;
  if (!semantic_id || !utf8 || !utf8_capacity || !utf8_bytes ||
      !cluster_count || !source_revision || !inline_object_count)
    return false;
  *inline_object_count = 0;
  ArDialogueSession session;
  ArDialogueSession_Init(&session);
  ArDialogueContentSelection selection;
  ArDialogueValueResolver resolver;
  ActRaiserLocalizationValues values = s_runtime.values;
  const bool name_entry =
      !strcmp(semantic_id, "name_entry.prompt_and_alphabet");
  const bool status_table =
      !strncmp(semantic_id, "status.report.", 14) ||
      !strcmp(semantic_id, "system.choice.yes_no") ||
      !strcmp(semantic_id, "system.message_speed.scale_labels") ||
      !strncmp(semantic_id, "sky.menu.", 9) ||
      !strncmp(semantic_id, "sim.menu.", 9);
  ContentSelection(&selection);
  ValueResolver(&values, &resolver);
  ArLanguagePackError error = {{0}};
  ArDialoguePageSnapshot page;
  bool resolved = false;
  if (name_entry) {
    uint64_t name_source_revision = 0;
    resolved = ResolveNameEntryText(
        &selection, &values, &resolver,
        utf8, utf8_capacity, utf8_bytes,
        inline_objects, inline_object_capacity, inline_object_count,
        &name_source_revision, &error);
    page.source_revision = name_source_revision;
  } else {
    resolved = ArDialogueSession_Begin(
        &session, &selection, semantic_id, &resolver, &error);
    if (resolved) resolved = ArDialogueSession_GetPage(&session, &page);
    if (resolved) {
      resolved = NormalizeText(
          page.utf8, page.utf8_bytes,
          page.inline_objects, page.inline_object_count,
          status_table,
          utf8, utf8_capacity, utf8_bytes,
          inline_objects, inline_object_capacity, inline_object_count);
    }
  }
  if (resolved) {
    *cluster_count = CountClusters(utf8, *utf8_bytes);
    *source_revision = !strncmp(semantic_id, "status.report.", 14)
        ? ActRaiserLocalizationValues_ReportRevision(&s_runtime.values)
        : name_entry ? NameEntrySourceRevision(page.source_revision)
                     : page.source_revision;
    resolved = *cluster_count != UINT32_MAX &&
        *source_revision != 0;
  }
  if (!resolved && error_text && error_capacity) {
    snprintf(error_text, error_capacity, "%s",
             error.message[0] ? error.message : "message is unavailable");
  }
  ArDialogueSession_Destroy(&session);
  return resolved;
}

static void RefreshNameEntry(void) {
  const ActRaiserLocalizationComposeSnapshot *snapshot =
      ActRaiserLocalizationComposeState_FindObserved(&s_runtime.compose, 5);
  if (!snapshot || !s_runtime.name_entry.revision ||
      s_runtime.name_entry_applied_native_revision ==
          s_runtime.name_entry.revision)
    return;
  if (!s_runtime.presentation) {
    if (ActRaiserLocalizationNameEntryTracker_SynchronizeNative(
            &s_runtime.name_tracker, &s_runtime.name_entry))
      s_runtime.name_entry_applied_native_revision = s_runtime.name_entry.revision;
    return;
  }
  char error[kArLanguagePackErrorCapacity] = {0};
  if (!ActRaiserLocalizationComposeState_RefreshLatest(
          &s_runtime.compose, 5, ResolveComposeText, NULL,
          error, sizeof(error))) {
    fprintf(stderr, "[localization] name entry refresh unavailable (%s); "
                    "native text retained\n",
            error[0] ? error : "refresh failed");
  }
}

static void
CompleteNameEntry(const ActRaiserLocalizationTextObservation *observation) {
  if (!observation || !observation->serial)
    return;
  const bool completed =
      observation->serial == s_runtime.name_entry_completed_observation_serial;
  const ActRaiserLocalizationComposeSnapshot *surface =
      ActRaiserLocalizationComposeState_FindObserved(&s_runtime.compose, 5);
  if (observation->map_group != g_ram[kActRaiserWram_MapGroup] ||
      observation->map_number != g_ram[kActRaiserWram_CurrentMap] ||
      (surface &&
       observation->entry_compose_serial < surface->generation_serial))
    return;
  if (!surface) {
    if (completed)
      return;
    /* Native-only entry has no enhanced composer state. Its identified return
     * dialogue still retires any prior game's same-spelled Unicode name,
     * without loading a pack/font or changing native input. */
    const ActRaiserLocalizationRoute *route =
        ActRaiserLocalizationRoute_ResolveDialogue(observation);
    if (!route || strcmp(route->semantic_id,
                         "dialogue.event.wrapper_05.call_01.source_00"))
      return;
  }
  /* The next interpreter invocation is the observed keyboard return boundary.
   * Publish before its values are snapshotted, without waiting for SRAM or a
   * rendered frame. Never infer acceptance from an arbitrary matching save. */
  char native_name[kActRaiserLocalizationNameLength + 1];
  if (!completed &&
      ActRaiserLocalizationNameEntry_CopyNativeName(
          g_ram, kActRaiserWramSize, native_name, sizeof(native_name))) {
    const bool tracked =
        surface && s_runtime.name_tracker.initialized &&
        !strcmp(native_name, s_runtime.name_tracker.compatibility_name);
    (void)SaveSystem_SetLocalizedPlayerName(
        tracked ? s_runtime.name_tracker.utf8_name : native_name, native_name);
  }
  s_runtime.name_entry_completed_observation_serial = observation->serial;
  s_runtime.name_entry_completed_compose_serial =
      observation->entry_compose_serial;
  (void)ActRaiserLocalizationComposeState_ReleaseSurface(&s_runtime.compose, 5);
  s_runtime.name_entry_applied_native_revision = 0;
}

static void RefreshReports(void) {
  if (!s_runtime.presentation) return;
  const uint64_t revision =
      ActRaiserLocalizationValues_ReportRevision(&s_runtime.values);
  if (!revision) return;
  for (uint32_t surface = 6; surface <= 7; ++surface) {
    const ActRaiserLocalizationComposeSnapshot *snapshot =
        ActRaiserLocalizationComposeState_Find(&s_runtime.compose, surface);
    if (!snapshot || strncmp(snapshot->semantic_id, "status.report.", 14) ||
        snapshot->source_revision == revision)
      continue;
    char semantic_id[kActRaiserLocalizationComposeSemanticIdCapacity];
    snprintf(semantic_id, sizeof(semantic_id), "%s", snapshot->semantic_id);
    char error[kArLanguagePackErrorCapacity] = {0};
    if (!ActRaiserLocalizationComposeState_Refresh(
            &s_runtime.compose, surface, revision,
            ResolveComposeText, NULL, error, sizeof(error))) {
      fprintf(stderr, "[localization] %s refresh unavailable (%s); "
                      "native text retained\n",
              semantic_id,
              error[0] ? error : "refresh failed");
    }
  }
}

static bool SameNativePosition(const ActRaiserLocalizationTextObservation *a,
                               const ActRaiserLocalizationTextObservation *b) {
  return a->serial == b->serial && a->cursor_pc24 == b->cursor_pc24 &&
         a->page_index == b->page_index &&
         a->page_unit_index == b->page_unit_index &&
         a->completed_control_count == b->completed_control_count &&
         a->control_pending == b->control_pending &&
         a->terminal == b->terminal &&
         a->yielded_to_menu == b->yielded_to_menu &&
         a->awaiting_page_advance == b->awaiting_page_advance;
}

/* Prepare a switch from actual ROM progress, not the previous rendered frame.
 * With the game paused, enhanced -> retail -> enhanced must not overwrite its
 * authored progress with the earlier native byte still suspended underneath. */
static bool SynchronizeObservedDialogue(const ArLanguagePack *values_pack,
                                        ArLanguagePackError *error) {
  ActRaiserLocalizationTextObservation observation = {.struct_size =
                                                          sizeof(observation)};
  if (!ActRaiserLocalizationText_CopyObservation(&observation))
    return true;
  if (observation.serial == s_runtime.failed_observation_serial) {
    s_runtime.route = NULL;
    return true; /* Do not resurrect the failed invocation on the next frame. */
  }
  const ActRaiserLocalizationRoute *route =
      ActRaiserLocalizationRoute_ResolveDialogue(&observation);
  if (!route || observation.map_group != g_ram[kActRaiserWram_MapGroup] ||
      observation.map_number != g_ram[kActRaiserWram_CurrentMap]) {
    s_runtime.route = NULL;
    return true;
  }
  if (s_runtime.native_handoff_valid &&
      s_runtime.native_handoff_game_frame ==
          (uint16_t)(g_ram[kActRaiserWram_GameFrame] |
                     ((uint16_t)g_ram[kActRaiserWram_GameFrame + 1u] << 8)) &&
      SameNativePosition(&observation, &s_runtime.native_handoff))
    return true;
  s_runtime.native_handoff_valid = false;
  if (s_runtime.observation_serial != observation.serial ||
      !s_runtime.session.state.message_id[0]) {
    ArDialogueContentSelection selection;
    ArDialogueValueResolver resolver;
    ContentSelection(&selection);
    if (!CaptureValuesForPack(values_pack)) {
      snprintf(error->message, sizeof(error->message),
               "native dialogue values could not be captured");
      return false;
    }
    ValueResolver(&s_runtime.values, &resolver);
    if (!ArDialogueSession_BeginBounded(&s_runtime.session, &selection,
                                        route->semantic_id, &resolver,
                                        kArLocalizationFrameTextCapacity, error))
      return false;
    s_runtime.observation_serial = observation.serial;
    s_runtime.route = route;
  }
  if (s_runtime.route != route)
    return true;
  const uint16_t units =
      ActRaiserLocalizationRoute_PageUnitCount(route, observation.page_index);
  ArDialogueNativeProgress progress = {
      .struct_size = sizeof(progress),
      .abi_version = AR_DIALOGUE_SESSION_ABI_VERSION,
      .authored_page_index = observation.page_index,
      .revealed_unit_count = observation.yielded_to_menu || observation.terminal
                                 ? units
                                 : observation.page_unit_index,
      .page_unit_count = units,
      .completed_control_count = observation.completed_control_count,
      /* The ROM does not execute authored @wait cues. Keep their ledger
       * monotonic, but retire a cancelled presentation wait after ROM progress.
       */
      .completed_wait_count = s_runtime.session.state.completed_wait_count,
      .control_pending = observation.control_pending,
      .awaiting_input = observation.yielded_to_menu,
      .awaiting_page_advance = observation.awaiting_page_advance,
      .terminal = observation.terminal,
  };
  if (progress.revealed_unit_count > units)
    progress.revealed_unit_count = units;
  const bool native = s_runtime.session.state.resolved_source ==
                      kArDialogueResolvedSource_NativeRom;
  if (!(native ? ArDialogueSession_ObserveNativeProgress(&s_runtime.session,
                                                         &progress, error)
               : ArDialogueSession_SynchronizeNativeProgress(&s_runtime.session,
                                                             &progress, error)))
    return false;
  s_runtime.inherited_native_page_wait = observation.awaiting_page_advance;
  s_runtime.inherited_native_page = observation.page_index;
  s_runtime.scheduled_window_first_page = observation.window_start_page;
  s_runtime.scheduled_window_clear_control =
      observation.window_start_control_count <=
              observation.completed_control_count
          ? observation.window_start_control_count
          : 0;
  s_runtime.dialogue_window.valid = false;
  return true;
}

bool ActRaiserLocalizationRuntime_DialogueScheduled(void) {
  if (ArTextPresentation_Failed(s_runtime.dialogue_ticket))
    ScheduleFailed("enhanced dialogue window was not presented");
  return s_runtime.scheduled_dialogue && s_runtime.route &&
         s_runtime.presentation &&
         s_runtime.session.state.resolved_source !=
             kArDialogueResolvedSource_NativeRom;
}

bool ActRaiserLocalizationRuntime_PageConfirmationPending(void) {
  return ActRaiserLocalizationRuntime_DialogueScheduled() &&
         s_runtime.session.state.awaiting_page_advance;
}

/* Session creation belongs to the interpreter entry, not the first rendered
 * frame: a clear/delay/yield can precede that frame. Values are snapshotted once
 * here and the renderer only reads the resulting program. */
bool ActRaiserLocalizationRuntime_BeginDialogue(
    const ActRaiserLocalizationTextObservation *observation) {
  s_runtime.scheduled_dialogue = false;
  s_runtime.dialogue_ticket = 0;
  s_runtime.failed_observation_serial = 0;
  s_runtime.native_handoff_valid = false;
  s_runtime.inherited_native_page_wait = false;
  CompleteNameEntry(observation);
  if (!observation || !EnsureConfigured() || !CaptureValues()) return false;
  const ActRaiserLocalizationRoute *route =
      ActRaiserLocalizationRoute_ResolveDialogue(observation);
  s_runtime.observation_serial = observation->serial;
  s_runtime.route = NULL;
  s_runtime.dialogue_window.valid = false;
  if (!route) return false;
  ArDialogueContentSelection selection;
  ArDialogueValueResolver resolver;
  ContentSelection(&selection);
  ValueResolver(&s_runtime.values, &resolver);
  ArLanguagePackError error = {{0}};
  if (!ArDialogueSession_BeginBounded(&s_runtime.session, &selection,
                                      route->semantic_id, &resolver,
                                      kArLocalizationFrameTextCapacity, &error)) {
    fprintf(stderr, "[localization] %s unavailable (%s); native text retained\n",
            route->semantic_id, error.message);
    s_runtime.failed_observation_serial = observation->serial;
    return false;
  }
  s_runtime.route = route;
  s_runtime.scheduled_window_first_page = 0;
  s_runtime.scheduled_window_clear_control = 0;
  s_runtime.scheduled_dialogue = s_runtime.presentation &&
      s_runtime.session.state.resolved_source != kArDialogueResolvedSource_NativeRom;
  return s_runtime.scheduled_dialogue;
}

static void ScheduleFailed(const char *reason) {
  fprintf(stderr, "[localization] %s scheduling unavailable (%s); "
                  "native text retained\n",
          s_runtime.route ? s_runtime.route->semantic_id : "dialogue", reason);
  s_runtime.scheduled_dialogue = false;
  s_runtime.failed_observation_serial = s_runtime.observation_serial;
  s_runtime.dialogue_ticket = 0;
  s_runtime.dialogue_window.valid = false;
  s_runtime.route = NULL;
}

static bool AdvanceScheduledPage(bool retain_rows) {
  if (!ArDialogueSession_AdvancePage(&s_runtime.session)) return false;
  if (!retain_rows) {
    s_runtime.scheduled_window_first_page =
        (uint16_t)s_runtime.session.state.authored_page_index;
    s_runtime.scheduled_window_clear_control = 0;
  }
  return true;
}

bool ActRaiserLocalizationRuntime_ContinueDialogue(
    const ActRaiserLocalizationDialogueHost *host, bool retain_rows) {
  if (!host || !host->confirm_page ||
      !ActRaiserLocalizationRuntime_DialogueScheduled()) return false;
  if (!s_runtime.session.state.awaiting_page_advance)
    return true; /* No authored page boundary: no redundant native prompt. */
  if (!host->confirm_page(host->context)) {
    ScheduleFailed("native page confirmation failed");
    return false;
  }
  /* Host UI may have switched source while the native confirmation yielded. */
  if (!ActRaiserLocalizationRuntime_DialogueScheduled()) return true;
  if (!s_runtime.session.state.awaiting_page_advance) return true;
  return AdvanceScheduledPage(retain_rows);
}

/* Native glyph tokens supply the usual reveal ratio. A native page boundary
 * drains only the current authored page; a locked control/end drains added
 * pages too, without ever acknowledging a control itself. All optional waits
 * are consumed through Next/TickWait rather than skipped by ratio mapping. */
static bool PumpDialogue(uint32_t source_revealed, uint32_t source_units,
                         bool cross_pages, uint8_t text_speed, bool pace,
                         const ActRaiserLocalizationDialogueHost *host,
                         ArDialogueToken *last) {
  ArLanguagePackError error = {{0}};
  memset(last, 0, sizeof(*last));
  while (ActRaiserLocalizationRuntime_DialogueScheduled()) {
    if (s_runtime.session.state.awaiting_page_advance) {
      if (!cross_pages)
        return true;
      if (!ActRaiserLocalizationRuntime_ContinueDialogue(host, text_speed != 0))
        return false;
      if (!ActRaiserLocalizationRuntime_DialogueScheduled())
        return false;
      pace = true; /* Added pages have no native glyphs to pace their reveal. */
    }
    while (s_runtime.session.state.wait_frames_remaining) {
      if (!host->wait_frame(host->context)) {
        ScheduleFailed("native animation wait failed");
        return false;
      }
      if (!ActRaiserLocalizationRuntime_DialogueScheduled())
        return false;
      ArDialogueSession_TickWait(&s_runtime.session, 1);
    }
    const uint32_t target_clusters =
        source_units
            ? (uint32_t)(((uint64_t)source_revealed *
                              s_runtime.session.state.page_cluster_count +
                          source_units - 1u) /
                         source_units)
            : UINT32_MAX;
    if (target_clusters != UINT32_MAX &&
        s_runtime.session.state.revealed_cluster_count >= target_clusters)
      return true;
    if (!ArDialogueSession_Next(&s_runtime.session, last, &error)) {
      ScheduleFailed(error.message);
      return false;
    }
    switch (last->kind) {
    case kArDialogueToken_Grapheme:
      if (pace && last->first_scalar != ' ' && last->first_scalar != '\n') {
        /* Match the native $901C delay scale, including instant text. */
        for (uint8_t tick = 0; tick < text_speed; ++tick) {
          if (!host->wait_frame(host->context)) {
            ScheduleFailed("native reveal wait failed");
            return false;
          }
          if (!ActRaiserLocalizationRuntime_DialogueScheduled())
            return false;
        }
      }
      break;
    case kArDialogueToken_WaitStarted:
    case kArDialogueToken_PageComplete:
      break;
    default:
      return true;
    }
  }
  return false;
}

void ActRaiserLocalizationRuntime_ScheduleByte(
    const ActRaiserLocalizationTextObservation *observation, uint8_t code,
    uint8_t text_speed, const ActRaiserLocalizationDialogueHost *host) {
  if (!observation || !host || !host->wait_frame || !host->confirm_page ||
      !ActRaiserLocalizationRuntime_DialogueScheduled() ||
      observation->serial != s_runtime.observation_serial)
    return;
  ArDialogueSession *session = &s_runtime.session;
  if (s_runtime.inherited_native_page_wait &&
      observation->page_index > s_runtime.inherited_native_page) {
    /* The original $9099 was already on the native stack at activation. Its
     * real confirmation pays for this transition; never ask for it twice. */
    s_runtime.inherited_native_page_wait = false;
    if (session->state.awaiting_page_advance &&
        !AdvanceScheduledPage(text_speed != 0)) {
      ScheduleFailed("inherited native continuation could not advance");
      return;
    }
  }
  if (observation->completed_control_count >
      session->state.completed_control_count) {
    const uint32_t ordinal = session->state.completed_control_count;
    const char *id = ArLanguageContract_RequiredAnchor(
        session->state.message_id, kArLanguageSourceProfile_Us, ordinal);
    if (observation->completed_control_count != ordinal + 1u ||
        !ArDialogueSession_CompleteControl(session, ordinal)) {
      ScheduleFailed("native control acknowledgement is out of sequence");
      return;
    }
    if (id && !strncmp(id, "reset_text_cursor.", 18)) {
      s_runtime.scheduled_window_first_page =
          (uint16_t)session->state.authored_page_index;
      s_runtime.scheduled_window_clear_control = (uint16_t)(ordinal + 1u);
    }
  }
  const bool locked = code == 1 || code == 3 || code == 4 || code == 5;
  if (locked) {
    const char *expected = ArLanguageContract_RequiredAnchor(
        session->state.message_id, kArLanguageSourceProfile_Us,
        observation->completed_control_count);
    const char *kind = code == 1   ? "yield."
                       : code == 3 ? "delay."
                       : code == 4 ? "toggle_text_state."
                                   : "reset_text_cursor.";
    if (!expected || strncmp(expected, kind, strlen(kind))) {
      ScheduleFailed("native control kind disagrees with the source contract");
      return;
    }
  }
  const bool boundary = locked || code == 0 || code == 2;
  uint32_t units = 0;
  uint32_t revealed = 0;
  if (!boundary) {
    units = ActRaiserLocalizationRoute_PageUnitCount(s_runtime.route,
                                                     observation->page_index);
    if (!units)
      return;
    revealed = observation->page_unit_index;
    if (revealed > units)
      revealed = units;
  }
  ArDialogueToken token;
  if (!PumpDialogue(revealed, units, locked || code == 0, text_speed, false,
                    host, &token))
    return;
  if (locked && (token.kind != kArDialogueToken_Control ||
                 token.control_ordinal != observation->completed_control_count))
    ScheduleFailed("authored and native control barriers disagree");
  else if (code == 0 && token.kind != kArDialogueToken_End)
    ScheduleFailed("native end precedes an unexecuted control");
}

void ActRaiserLocalizationRuntime_ReturnDialogue(void) {
  if (!ActRaiserLocalizationRuntime_DialogueScheduled()) return;
  ActRaiserLocalizationTextObservation observation = {
      .struct_size = sizeof(observation)};
  if (ActRaiserLocalizationText_CopyObservation(&observation) &&
      observation.serial == s_runtime.observation_serial &&
      observation.yielded_to_menu && s_runtime.session.state.control_pending &&
      !ArDialogueSession_CompleteControl(
          &s_runtime.session, s_runtime.session.state.completed_control_count))
    ScheduleFailed("native menu return could not acknowledge yield");
}

void ActRaiserLocalizationRuntime_CaptureFrame(
    ArLocalizationFrame *frame, uint16_t bg3_tilemap_base_words,
    uint16_t bg3_tile_base_words,
    const uint16_t *vram_words, size_t vram_word_count,
    const uint16_t *cgram_words, size_t cgram_word_count) {
  ArLocalizationFrame_Reset(frame);
  (void)ActRaiserLocalizationRuntime_DialogueScheduled();
  if (!frame || !EnsureConfigured()) return;
  if (!CaptureValues()) return;

  const ArLanguagePackMetadata *metadata =
      ArLanguagePack_GetMetadata(s_runtime.pack);
  ArEnhancedTextSettings settings;
  ArEnhancedTextSettings_Defaults(&settings);
  settings.size_percent = g_settings.localization_font_scale_percent;
  settings.sampling = (ArEnhancedTextSampling)g_settings.localization_font_sampling;
  settings.pixelation =
      (ArEnhancedTextPixelation)g_settings.localization_font_pixelation;
  settings.pixelation_size = settings.pixelation == kArEnhancedTextPixelation_None
      ? 0 : g_settings.localization_font_pixel_size;
  if (!metadata || !ArLocalizationFrame_SetFont(
          frame, metadata->locale, metadata->primary_font,
          s_runtime.primary_font_path, s_runtime.pack->content_revision,
          &settings))
    return;
  const char *fallbacks[kArTextPresentationMaximumFallbackFonts];
  for (uint32_t i = 0; i < metadata->fallback_font_count; ++i)
    fallbacks[i] = s_runtime.fallback_font_paths[i];
  if (!ArLocalizationFrame_SetFallbackFonts(frame, fallbacks,
                                            metadata->fallback_font_count))
    return;
  const ArTextDirection direction =
      metadata->direction == kArLanguageDirection_RightToLeft
          ? kArTextDirection_RightToLeft
          : metadata->direction == kArLanguageDirection_LeftToRight
              ? kArTextDirection_LeftToRight : kArTextDirection_Auto;
  const ArTextCellDestination destination = {
      .background = 3,
      .screen = kArTextCellScreen_Composited,
      .tilemap_base_words = bg3_tilemap_base_words,
  };
  const uint8_t map_group = g_ram[kActRaiserWram_MapGroup];
  const uint8_t map_number = g_ram[kActRaiserWram_CurrentMap];
  ActRaiserLocalizationComposeState_SetScene(
      &s_runtime.compose, map_group, map_number);
  ActRaiserLocalizationComposeObservation compose_observations[256];
  size_t compose_count = 0;
  bool compose_dropped = false;
  if (ActRaiserLocalizationText_CopyComposeObservations(
          s_runtime.compose_observation_serial, compose_observations,
          sizeof(compose_observations) / sizeof(compose_observations[0]),
          &compose_count, &compose_dropped)) {
    if (compose_dropped)
      ActRaiserLocalizationComposeState_Clear(&s_runtime.compose);
    for (size_t index = 0; index < compose_count; ++index) {
      char error[kArLanguagePackErrorCapacity] = {0};
      const ActRaiserLocalizationComposeRoute *compose_route =
          ActRaiserLocalizationRoute_ResolveCompose(
              &compose_observations[index]);
      /* A final keyboard redraw can still be queued when native acceptance
       * enters dialogue. Do not recreate a retired entry generation later. */
      if (compose_route && compose_route->surface_id == 5 &&
          compose_observations[index].serial <=
              s_runtime.name_entry_completed_compose_serial) {
        s_runtime.compose_observation_serial =
            compose_observations[index].serial;
        continue;
      }
      /* Moving the retail name-entry cursor redraws both the keyboard and its
       * native cursor record. That is an update to the active generation, not
       * a new name-entry session: resetting here would discard the authored
       * keyboard page and any Unicode graphemes after every movement. The
       * compose-state lifecycle removes surface 5 when the entry UI closes,
       * so absence is the generation boundary we actually need. */
      if (compose_route && compose_route->surface_id == 5 &&
          !ActRaiserLocalizationComposeState_FindObserved(&s_runtime.compose, 5)) {
        ActRaiserLocalizationNameEntryTracker_Init(&s_runtime.name_tracker);
        s_runtime.name_entry_applied_native_revision = 0;
      }
      (void)ActRaiserLocalizationComposeState_Process(
          &s_runtime.compose, &compose_observations[index],
          ResolveComposeText, NULL, error, sizeof(error));
      s_runtime.compose_observation_serial =
          compose_observations[index].serial;
    }
  }

  ActRaiserLocalizationTextObservation observation = {
      .struct_size = sizeof(observation),
  };
  const bool observation_valid =
      ActRaiserLocalizationText_CopyObservation(&observation);
  /* The name-entry function returns directly into a new dialogue invocation;
   * it does not call another fixed composer to invalidate its old full-screen
   * surface. Order the two observed event streams instead: a newer dialogue
   * whose entry saw this name-entry generation is the exact inverse boundary.
   * Release before appending the frame so no stale keyboard can overlap even
   * the first native fallback frame of the following dialogue. */
  if (observation_valid &&
      ActRaiserLocalizationComposeState_FindObserved(&s_runtime.compose, 5))
    CompleteNameEntry(&observation);
  if (s_runtime.refresh_pending) {
    for (uint32_t surface = kActRaiserLocalizationComposeSurfaceFirst;
         surface <= kActRaiserLocalizationComposeSurfaceLast; ++surface) {
      char error[kArLanguagePackErrorCapacity] = {0};
      (void)ActRaiserLocalizationComposeState_RefreshLatest(
          &s_runtime.compose, surface, ResolveComposeText, NULL,
          error, sizeof(error));
    }
    s_runtime.refresh_pending = false;
  }
  RefreshNameEntry();
  if (ActRaiserLocalizationComposeState_Find(&s_runtime.compose, 5)) {
    _Static_assert(kArLocalizationFrameNameCursorPixels ==
                       kActRaiserLocalizationNameCursorPixels,
                   "name cursor bitmap extent");
    frame->name_cursor_valid = ActRaiserLocalizationNameEntry_CaptureCursor(
        frame->name_cursor_argb, bg3_tile_base_words,
        vram_words, vram_word_count, cgram_words, cgram_word_count);
  }
  RefreshReports();
  if (vram_words && vram_word_count >= 0x8000) {
    /* Fixed US report artwork is not language-bearing. Capture its original
     * pixels/palette at the identified native consumer, not a font surrogate. */
    const uint16_t life = vram_words[
        (bg3_tilemap_base_words + 7 * 32 + 19) & 0x7fff];
    const uint16_t population[] = {0x203a, 0x203b};
    const uint16_t speed[] = {0x203d, 0x203c};
    if (ActRaiserLocalizationComposeState_Find(&s_runtime.compose, 6))
      (void)ActRaiserLocalizationArt_Capture(
        &frame->artwork[kArLocalizationArtwork_Life], bg3_tile_base_words,
        &life, 1, vram_words, vram_word_count, cgram_words, cgram_word_count);
    if (ActRaiserLocalizationComposeState_Find(&s_runtime.compose, 7))
      (void)ActRaiserLocalizationArt_Capture(
        &frame->artwork[kArLocalizationArtwork_Population], bg3_tile_base_words,
        population, 2, vram_words, vram_word_count, cgram_words, cgram_word_count);
    if (ActRaiserLocalizationComposeState_Find(&s_runtime.compose, 9))
      (void)ActRaiserLocalizationArt_Capture(
        &frame->artwork[kArLocalizationArtwork_SpeedDirection], bg3_tile_base_words,
        speed, 2, vram_words, vram_word_count, cgram_words, cgram_word_count);
    if (observation_valid && observation.awaiting_page_advance &&
        observation.continuation_cell_valid) {
      const uint16_t continuation = vram_words[
          (bg3_tilemap_base_words + observation.continuation_cell) & 0x7fff];
      (void)ActRaiserLocalizationArt_Capture(
          &frame->artwork[kArLocalizationArtwork_Continue], bg3_tile_base_words,
          &continuation, 1, vram_words, vram_word_count, cgram_words, cgram_word_count);
    }
    if (ActRaiserLocalizationRuntime_DialogueScheduled()) {
      memset(&frame->artwork[kArLocalizationArtwork_Continue], 0,
             sizeof(frame->artwork[kArLocalizationArtwork_Continue]));
      if (s_runtime.session.state.awaiting_page_advance) {
        /* The exact retail arrow tile and blink clock, independently of a
         * native source-page cursor (added pages have no such cursor). */
        const uint16_t arrow = g_ram[(uint16_t)(observation.direct_page + 0x88)] & 0x10
            ? 0x205f : 0x2000;
        (void)ActRaiserLocalizationArt_Capture(
            &frame->artwork[kArLocalizationArtwork_Continue], bg3_tile_base_words,
            &arrow, 1, vram_words, vram_word_count, cgram_words, cgram_word_count);
      }
    }
  }
  if (s_runtime.presentation)
    (void)ActRaiserLocalizationComposeState_AppendFrame(
        &s_runtime.compose, frame, destination, direction);
  if (!observation_valid) return;
  /* Opcode $00 enters the native input-acknowledgement loop. Keep presenting
   * its completed page until the game changes scene or a later fixed composer
   * begins the replacement UI generation. */
  if (observation.map_group != map_group ||
      observation.map_number != map_number ||
      (observation.terminal &&
       ActRaiserLocalizationComposeState_DialogueWasReplaced(
           &s_runtime.compose, observation.terminal_compose_serial)))
    return;
  const ActRaiserLocalizationRoute *route =
      ActRaiserLocalizationRoute_ResolveDialogue(&observation);
  if (!route) return;

  ArLanguagePackError error = {{0}};
  if (!ActRaiserLocalizationRuntime_DialogueScheduled() &&
      !SynchronizeObservedDialogue(s_runtime.pack, &error)) {
    fprintf(
        stderr,
        "[localization] %s progress unavailable (%s); native text retained\n",
        route->semantic_id, error.message);
    s_runtime.route = NULL; /* One diagnostic/fallback per invocation. */
    return;
  }
  if (s_runtime.route != route || s_runtime.session.state.resolved_source ==
                                      kArDialogueResolvedSource_NativeRom)
    return;
  ArDialoguePageSnapshot page;
  if (!ArDialogueSession_GetPage(&s_runtime.session, &page))
    return;

  if (ActRaiserLocalizationRuntime_DialogueScheduled()) {
    observation.window_start_page = s_runtime.scheduled_window_first_page;
    observation.window_start_control_count = s_runtime.scheduled_window_clear_control;
    observation.awaiting_page_advance = s_runtime.session.state.awaiting_page_advance;
  }
  if (!BuildDialogueWindow(&page, &observation)) {
    ScheduleFailed("dialogue window exceeds its presentation capacity");
    return;
  }
  const DialogueWindow *window = &s_runtime.dialogue_window;
  uint32_t revealed = window->current_page_clusters;
  if (page.cluster_count > window->current_page_prefix_clusters) {
    const uint32_t source_clusters = page.cluster_count - window->current_page_prefix_clusters;
    const uint32_t source_revealed = page.revealed_cluster_count > window->current_page_prefix_clusters
        ? page.revealed_cluster_count - window->current_page_prefix_clusters : 0;
    revealed = (uint32_t)(((uint64_t)source_revealed *
                           window->current_page_clusters + source_clusters - 1u) /
                          source_clusters);
    if (revealed > window->current_page_clusters)
      revealed = window->current_page_clusters;
  }
  size_t reveal_bytes = window->current_page_offset;
  for (uint32_t cluster = 0; cluster < revealed; ++cluster) {
    size_t next = 0;
    if (!ArUnicodeGrapheme_Next(window->text, window->bytes, reveal_bytes,
                                NULL, &next) || next <= reveal_bytes)
      return;
    reveal_bytes = next;
  }
  const bool added = ArLocalizationFrame_AddDialogueWindow(
      frame, route->surface_id, destination, route->region,
      window->text, window->bytes, (uint32_t)reveal_bytes, window->clusters,
      page.source_revision, direction, route->native_font_pixels);
  if (!added) {
    ScheduleFailed("dialogue cannot fit in the current frame");
    return;
  }
  if (ActRaiserLocalizationRuntime_DialogueScheduled()) {
    frame->dialogue_ticket = s_runtime.dialogue_ticket;
    frame->dialogue_surface_id = route->surface_id;
  }
  if (added && revealed == window->current_page_clusters &&
      observation.awaiting_page_advance &&
      frame->artwork[kArLocalizationArtwork_Continue].valid) {
    (void)ArLocalizationFrame_AddIndicator(
        frame, route->surface_id,
        kArLocalizationIndicator_DialogueContinue,
        (ArTextCellRegion){
            route->region.column + route->region.columns / 2u,
            route->region.row + route->region.rows - 1u, 1, 1});
  }
}

void ActRaiserLocalizationRuntime_Shutdown(void) {
  if (s_runtime.configured) {
    ArDialogueSession_Destroy(&s_runtime.session);
    ArLanguagePack_Destroy(&s_runtime.selected_pack);
    ArLanguagePack_Destroy(&s_runtime.native_pack);
  }
  memset(&s_runtime, 0, sizeof(s_runtime));
}
