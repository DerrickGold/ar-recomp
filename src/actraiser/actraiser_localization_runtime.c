#include "actraiser/actraiser_localization_runtime.h"
#include "actraiser/actraiser_localization_hud.h"
#include "actraiser/actraiser_localization_credits.h"
#include "actraiser/actraiser_localization_art.h"
#include "actraiser/actraiser_localization_style.h"

#include <stdio.h>
#include "actraiser/actraiser_sim_menu.h"
#include "actraiser/actraiser_regional_runtime.h"
#include "actraiser/actraiser_miracle_translation.h"
#include "localization/unicode_grapheme.h"
#include "actraiser/actraiser_localization_style.h"
#include <stdlib.h>
#include <string.h>

#include "actraiser/actraiser_dialogue_adapter.h"
#include "actraiser/actraiser_dialogue_window.h"
#include "actraiser/actraiser_localization_compose_state.h"
#include "actraiser/actraiser_localization_fixed_text.h"
#include "actraiser/actraiser_localization_name_compose.h"
#include "actraiser/actraiser_localization_name_entry.h"
#include "actraiser/actraiser_localization_routes.h"
#include "actraiser/actraiser_localization_schedule.h"
#include "actraiser/actraiser_localization_text.h"
#include "actraiser/actraiser_localization_text_normalize.h"
#include "actraiser/actraiser_localization_values.h"
#include "actraiser/actraiser_localization_world_navigation.h"
#include "actraiser_game.h"
#include "deterministic_hash.h"
#include "localization/language_contract.h"
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

/* The first composition pass only locates the selected Unicode key. Keep its
 * normalized page while the source, page and captured name are unchanged.
 * The final pass still resolves live values after applying the native edit. */
typedef struct NameEntryKeyMap {
  bool valid;
  uint32_t page_index;
  uint32_t page_count;
  char source_name[kActRaiserLocalizationMasterNameCapacity];
  size_t text_bytes;
  char text[kActRaiserLocalizationComposeTextCapacity];
} NameEntryKeyMap;

typedef struct LocalizationRuntime {
  ActRaiserLocalizationHud hud;
  ActRaiserLocalizationCredits credits;
  bool configured;
  bool enabled;
  bool refresh_pending;
  bool native_attempted;
  int selected_content;
  int content;
  int presentation;
  bool compatibility_checked, legacy_blocked;
  int compatibility_content, compatibility_presentation;
  ArLanguagePack selected_pack;
  ArLanguagePack native_pack;
  const ArLanguagePack *pack;
  ArDialogueSession session;
  ActRaiserDialogueWindow dialogue_window;
  ActRaiserLocalizationComposeState compose;
  ActRaiserLocalizationValues values;
  ActRaiserLocalizationWorldNavigation world_navigation;
  ActRaiserLocalizationNameEntryState name_entry;
  ActRaiserLocalizationNameEntryTracker name_tracker;
  NameEntryKeyMap name_key_map;
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
  ArFontResourceId primary_font;
  ArFontResourceId fallback_fonts[kArTextPresentationMaximumFallbackFonts];
  ArTextFontRole font_roles[kArTextFontMaximumRoles];
  size_t font_role_count;
} LocalizationRuntime;

static LocalizationRuntime s_runtime;
static ArTextPresentationHost s_presentation_host;
static ArLanguagePackIo s_pack_io;
static char s_native_manifest[kManifestPathCapacity];
static ActRaiserLocalizationPackHost s_pack_host;
/* Never reuse a ticket across game resets: retained old frames may outlive the
 * invocation they describe. This counter carries no emulated game state. */
static uint64_t s_next_dialogue_ticket;
static ActRaiserTextStylePlan s_menu_help_style;
static ArTextBidiSpans s_menu_help_bidi;

static void ScheduleFailed(const char *reason);
static bool CopyPath(char *destination, size_t capacity, const char *source);

void ActRaiserLocalizationRuntime_SetPresentationHost(
    const ArTextPresentationHost *host) {
  s_presentation_host = (ArTextPresentationHost){0};
  if (!host || host->struct_size <
          offsetof(ArTextPresentationHost, discard_prepared_font) +
              sizeof(host->discard_prepared_font) ||
      host->abi_version != AR_TEXT_PRESENTATION_ABI_VERSION || !host->prepare_font ||
      !host->register_font || !host->retire_font || !host->discard_prepared_font)
    return;
  s_presentation_host = *host;
}

void ActRaiserLocalizationRuntime_SetPackHost(
    const ActRaiserLocalizationPackHost *host) {
  if (s_runtime.configured) return;
  s_pack_io = (ArLanguagePackIo){0};
  s_pack_host = (ActRaiserLocalizationPackHost){0};
  s_native_manifest[0] = 0;
  if (!host) return;
  if (host->struct_size < offsetof(ActRaiserLocalizationPackHost, legacy_pack) +
                              sizeof(host->legacy_pack) ||
      host->abi_version != ACTRAISER_LOCALIZATION_PACK_HOST_ABI_VERSION ||
      host->io.struct_size < offsetof(ArLanguagePackIo, release_file) +
                                 sizeof(host->io.release_file) ||
      host->io.abi_version != AR_LANGUAGE_PACK_IO_ABI_VERSION ||
      !host->io.read_file || !host->io.release_file ||
      !CopyPath(s_native_manifest, sizeof(s_native_manifest),
                host->native_manifest))
    return;
  s_pack_io = host->io;
  s_pack_host = *host;
}

static bool SynchronizeObservedDialogue(const ArLanguagePack *values_pack,
                                        ArLanguagePackError *error);

static bool CopyPath(char *destination, size_t capacity, const char *source) {
  const size_t length = source ? strlen(source) : 0;
  if (!source || !length || length >= capacity) return false;
  memcpy(destination, source, length + 1u);
  return true;
}

static void RetireFonts(ArFontResourceId primary,
                        const ArFontResourceId *fallbacks,
                        const ArTextFontRole *roles, size_t role_count) {
  if (!s_presentation_host.retire_font) return;
  if (primary)
    s_presentation_host.retire_font(s_presentation_host.context, primary);
  for (size_t i = 0; i < kArTextPresentationMaximumFallbackFonts; ++i)
    if (fallbacks[i])
      s_presentation_host.retire_font(s_presentation_host.context, fallbacks[i]);
  for (size_t i = 0; i < role_count; ++i) {
    if (roles[i].primary)
      s_presentation_host.retire_font(s_presentation_host.context,
                                      roles[i].primary);
    for (size_t j = 0; j < roles[i].fallback_count; ++j)
      if (roles[i].fallbacks[j])
        s_presentation_host.retire_font(s_presentation_host.context,
                                        roles[i].fallbacks[j]);
  }
}

static bool RegisterFontRoles(const ArLanguagePackMetadata *metadata,
                              const char *manifest, ArTextFontRole *roles,
                              ArLanguagePackError *error) {
  for (size_t i = 0; i < metadata->font_role_count; ++i) {
    const ArLanguageFontRole *source = &metadata->font_roles[i];
    ArTextFontRole *role = &roles[i];
    snprintf(role->name, sizeof(role->name), "%s", source->name);
    role->fallback_count = source->fallback_font_count;
    role->primary = s_presentation_host.register_font(
        s_presentation_host.context, manifest, source->primary_font,
        error->message, sizeof(error->message));
    if (!role->primary)
      return false;
    for (size_t j = 0; j < role->fallback_count; ++j) {
      role->fallbacks[j] = s_presentation_host.register_font(
          s_presentation_host.context, manifest, source->fallback_fonts[j],
          error->message, sizeof(error->message));
      if (!role->fallbacks[j])
        return false;
    }
  }
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
  if (!s_pack_io.read_file || !s_pack_io.release_file ||
      !manifest || !manifest[0]) {
    snprintf(error->message, sizeof(error->message), "no pack configured");
    return false;
  }
  if (!ArLanguagePack_Load(pack, &s_pack_io, manifest, error)) return false;
  const ArLanguagePackMetadata *metadata = ArLanguagePack_GetMetadata(pack);
  if (!metadata || (s_pack_host.require_v2 && metadata->format_version != 2) ||
      metadata->target != kArLanguagePackTarget_UsRuntime ||
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

static bool LegacySelection(int content, int presentation) {
  if (!s_pack_host.require_v2)
    return false;
  if (s_runtime.compatibility_checked &&
      s_runtime.compatibility_content == content &&
      s_runtime.compatibility_presentation == presentation)
    return s_runtime.legacy_blocked;
  s_runtime.compatibility_checked = true;
  s_runtime.compatibility_content = content;
  s_runtime.compatibility_presentation = presentation;
  s_runtime.legacy_blocked = false;
  if (!presentation)
    return false;
  const char *paths[] = {content ? Settings_LocalizationPackPath(content)
                                 : NULL,
                         s_runtime.native_manifest_path};
  for (unsigned i = 0; i < 2; ++i) {
    if (!paths[i] || !paths[i][0])
      continue;
    ArLanguagePackMetadata metadata;
    ArLanguagePackError error = {{0}};
    if (!ArLanguagePack_ReadMetadata(&s_pack_io, paths[i], &metadata, NULL,
                                     &error) ||
        metadata.format_version != 1)
      continue;
    s_runtime.legacy_blocked = true;
    fprintf(stderr,
            "[localization] v1 pack '%s' requires a Builder upgrade; "
            "using native ROM text for this session selection: %s\n",
            metadata.package_id, paths[i]);
    if (s_pack_host.legacy_pack)
      s_pack_host.legacy_pack(s_pack_host.context, paths[i], &metadata, i == 1);
    return true;
  }
  return false;
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
    ActRaiserLocalizationWorldNavigation_Init(&s_runtime.world_navigation);
    s_runtime.content = -1;
    s_runtime.selected_content = -1;
    s_runtime.presentation = -1;
    (void)CopyPath(s_runtime.native_manifest_path,
                   sizeof(s_runtime.native_manifest_path),
                   s_native_manifest);
  }
  const int content = g_settings.localization_content;
  const int presentation =
      LegacySelection(content, g_settings.localization_presentation)
          ? 0
          : g_settings.localization_presentation;
  if (content == s_runtime.content && presentation == s_runtime.presentation)
    return s_runtime.enabled;

  ArLanguagePackError error = {{0}};
  ArLanguagePack candidate;
  ArLanguagePack_Init(&candidate);
  bool candidate_ready = false;
  ArFontResourceId primary_font = 0;
  ArFontResourceId fallbacks[kArTextPresentationMaximumFallbackFonts] = {0};
  ArTextFontRole roles[kArTextFontMaximumRoles] = {0};
  size_t role_count = 0;
  const char *selected_manifest = s_runtime.manifest_path;
  if (presentation && !s_runtime.native_attempted) {
    s_runtime.native_attempted = true;
    if (!LoadRuntimePack(&s_runtime.native_pack,
                         s_runtime.native_manifest_path, true, &error))
      fprintf(stderr, "[localization] enhanced USA fallback unavailable: %s\n",
              error.message);
  }
  if (presentation && content && content != s_runtime.selected_content) {
    selected_manifest = Settings_LocalizationPackPath(content);
    error.message[0] = 0;
    if (!selected_manifest || strlen(selected_manifest) >= sizeof(s_runtime.manifest_path) ||
        !LoadRuntimePack(&candidate, selected_manifest, false, &error)) goto reject;
    candidate_ready = true;
  }
  const ArLanguagePack *pack =
      content ? (candidate_ready ? &candidate : &s_runtime.selected_pack) : &s_runtime.native_pack;
  const ArLanguagePackMetadata *metadata =
      pack->content_revision ? ArLanguagePack_GetMetadata(pack) : NULL;
  if (presentation &&
      (!metadata || !s_presentation_host.register_font)) {
    if (!error.message[0])
      snprintf(error.message, sizeof(error.message),
               "requested source or font is unavailable");
    goto reject;
  }
  if (presentation) {
    if (metadata->fallback_font_count >
            kArTextPresentationMaximumFallbackFonts ||
        metadata->font_role_count > kArTextFontMaximumRoles) {
      snprintf(error.message, sizeof(error.message), "too many fallback fonts");
      goto reject;
    }
    const char *manifest = content ? selected_manifest : s_runtime.native_manifest_path;
    primary_font = s_presentation_host.register_font(
        s_presentation_host.context, manifest, metadata->primary_font,
        error.message, sizeof(error.message));
    if (!primary_font) goto reject;
    for (uint32_t i = 0; i < metadata->fallback_font_count; ++i) {
      fallbacks[i] = s_presentation_host.register_font(
          s_presentation_host.context, manifest, metadata->fallback_fonts[i],
          error.message, sizeof(error.message));
      if (!fallbacks[i]) goto reject;
    }
    role_count = metadata->font_role_count;
    if (!RegisterFontRoles(metadata, manifest, roles, &error))
      goto reject;
    const ArTextPresentationFont font = {
        .struct_size = sizeof(font),
        .abi_version = AR_TEXT_PRESENTATION_ABI_VERSION,
        .stack_id = metadata->primary_font,
        .primary = primary_font,
        .revision = pack->content_revision,
        .fallbacks = fallbacks,
        .fallback_count = metadata->fallback_font_count,
        .roles = roles,
        .role_count = role_count,
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
  if (presentation && content) selection.selected_pack = pack;
  const bool was_scheduled = ActRaiserLocalizationRuntime_DialogueScheduled();
  if (!was_scheduled && !SynchronizeObservedDialogue(
                            s_runtime.pack ? s_runtime.pack : pack, &error))
    goto reject;
  ActRaiserMiracle_ConstrainText(&selection,s_runtime.session.state.message_id,
                                &s_runtime.values.prices);
  if (s_runtime.session.state.message_id[0] &&
      !ArDialogueSession_SwitchBounded(&s_runtime.session, &selection,
                                       kArLocalizationFrameTextCapacity, &error))
    goto reject;
  /* Sessions own their composed program. Only after validation, font readiness
   * and bounded live-switch succeed can we retire the previous pack storage. */
  if (candidate_ready) {
    ArLanguagePack_Destroy(&s_runtime.selected_pack);
    s_runtime.selected_pack = candidate;
    ArLanguagePack_Init(&candidate);
    s_runtime.selected_content = content;
    (void)CopyPath(s_runtime.manifest_path, sizeof(s_runtime.manifest_path), selected_manifest);
    pack = &s_runtime.selected_pack;
    metadata = ArLanguagePack_GetMetadata(pack);
  }
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
    RetireFonts(s_runtime.primary_font, s_runtime.fallback_fonts,
                s_runtime.font_roles, s_runtime.font_role_count);
    s_runtime.primary_font = primary_font;
    memcpy(s_runtime.fallback_fonts, fallbacks, sizeof(fallbacks));
    memcpy(s_runtime.font_roles, roles, sizeof(roles));
    s_runtime.font_role_count = role_count;
    s_runtime.enabled = true;
  }
  s_runtime.refresh_pending = true;
  s_runtime.hud.resolved = false;
  s_runtime.credits.resolved = false;
  ActRaiserLocalizationWorldNavigation_Invalidate(
      &s_runtime.world_navigation);
  s_runtime.name_entry_applied_native_revision = 0;
  s_runtime.name_key_map.valid = false;
  fprintf(stderr, "[localization] %s: %s\n",
          presentation ? "enhanced" : "native",
          presentation ? metadata->display_name : "untouched USA text/font");
  return s_runtime.enabled;

reject:
  if (s_presentation_host.discard_prepared_font)
    s_presentation_host.discard_prepared_font(s_presentation_host.context);
  RetireFonts(primary_font, fallbacks, roles, role_count);
  ArLanguagePack_Destroy(&candidate);
  if (s_runtime.legacy_blocked) {
    s_runtime.content = content;
    s_runtime.presentation = 0;
    ScheduleFailed(error.message);
    return false;
  }
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
      pack, s_runtime.native_pack.content_revision
          ? &s_runtime.native_pack : NULL, master_name);
  if (captured && !ActRaiserRegional_CopyPrices(&s_runtime.values.prices)) return false;
  memset(&s_runtime.name_entry, 0, sizeof(s_runtime.name_entry));
  (void)ActRaiserLocalizationNameEntry_Capture(
      &s_runtime.name_entry, g_ram, kActRaiserWramSize);
  return captured;
}

static bool CaptureValues(void) {
  return CaptureValuesForPack(s_runtime.pack);
}

static bool BuildDialogueWindow(
    const ArDialoguePageSnapshot *current,
    const ActRaiserLocalizationTextObservation *observation) {
  ActRaiserDialogueWindow *window = &s_runtime.dialogue_window;
  const bool unchanged =
      window->valid && window->revision == current->source_revision &&
      window->native_first_page == observation->window_start_page &&
      window->native_clear_control_count ==
          observation->window_start_control_count &&
      window->current_page == current->page_index;
  if (!ActRaiserDialogueWindow_Build(window, &s_runtime.session, current,
                                     observation->window_start_page,
                                     observation->window_start_control_count))
    return false;
  if (!unchanged)
    s_runtime.dialogue_ticket =
        s_runtime.scheduled_dialogue ? ++s_next_dialogue_ticket : 0;
  return true;
}

static void ContentSelection(ArDialogueContentSelection *selection) {
  MakeSelection(s_runtime.content < 0 ? 0 : s_runtime.content,
                s_runtime.presentation < 0 ? 0 : s_runtime.presentation,
                selection);
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

/* The typed name is deliberately left out. The composed bytes already carry
 * it wherever it is drawn, and a revision that changed with every key press
 * would give the unchanged keyboard around the name a new identity too, so it
 * could never be reused while only the name line is rebuilt. */
static uint64_t NameEntrySourceRevision(uint64_t source_revision) {
  uint64_t revision = DeterministicHash_Fnv1a64(
      DETERMINISTIC_HASH_FNV1A64_OFFSET,
      &source_revision, sizeof(source_revision));
  revision = DeterministicHash_Fnv1a64(
      revision, &s_runtime.name_tracker.keyboard_page,
      sizeof(s_runtime.name_tracker.keyboard_page));
  return revision ? revision : 1u;
}

static bool PageLanguage(const ArDialoguePageSnapshot *page,
                         ArLocalizationTextLanguage *language) {
  if (!page || !page->locale || !language) return false;
  const size_t bytes = strlen(page->locale);
  if (!bytes || bytes >= sizeof(language->locale)) return false;
  *language = (ArLocalizationTextLanguage){
      .direction = page->direction == kArLanguageDirection_RightToLeft
          ? kArTextDirection_RightToLeft
          : page->direction == kArLanguageDirection_LeftToRight
              ? kArTextDirection_LeftToRight : kArTextDirection_Auto};
  memcpy(language->locale, page->locale, bytes + 1u);
  return true;
}

static bool PrepareNameEntryKeyMap(
    ArDialogueContentSelection *selection, ArDialogueValueResolver *resolver,
    const char *source_name, ArLanguagePackError *error) {
  NameEntryKeyMap *keys = &s_runtime.name_key_map;
  /* SelectPage applies an edge wrap, so call it exactly once per update. */
  const bool page_selected = keys->valid;
  if (page_selected && !ActRaiserLocalizationNameEntryTracker_SelectPage(
          &s_runtime.name_tracker, &s_runtime.name_entry, keys->page_count))
    return false;
  if (keys->valid && keys->page_index == s_runtime.name_tracker.keyboard_page &&
      !strcmp(keys->source_name, source_name))
    return true;
  keys->valid = false;

  ArDialogueSession probe;
  ArDialogueSession_Init(&probe);
  ArDialoguePageSnapshot page;
  bool resolved = ArDialogueSession_Begin(
      &probe, selection, "name_entry.prompt_and_alphabet", resolver, error) &&
      ArDialogueSession_GetPage(&probe, &page);
  if (resolved && !page_selected) {
    resolved = ActRaiserLocalizationNameEntryTracker_SelectPage(
        &s_runtime.name_tracker, &s_runtime.name_entry, page.page_count);
  }
  if (resolved) {
    resolved = ArDialogueSession_GetAuthoredPage(
        &probe, s_runtime.name_tracker.keyboard_page, &page);
  }
  ActRaiserResolvedText normalized = {0};
  if (resolved) {
    resolved =
        ActRaiserLocalizationText_Normalize(
            page.utf8, page.utf8_bytes, page.inline_objects,
            page.inline_object_count, false, normalized.utf8,
            sizeof(normalized.utf8), &normalized.utf8_bytes,
            normalized.inline_objects, kArLocalizationFrameInlineObjectCapacity,
            &normalized.inline_object_count, NULL) &&
        ActRaiserLocalizationNameCompose_ClearUnderlineRow(&normalized);
    if (resolved) {
      memcpy(keys->text, normalized.utf8, normalized.utf8_bytes + 1);
      keys->text_bytes = normalized.utf8_bytes;
    }
  }
  if (resolved) {
    keys->page_index = page.page_index;
    keys->page_count = page.page_count;
    keys->valid = strlen(source_name) < sizeof(keys->source_name);
    if (keys->valid)
      strcpy(keys->source_name, source_name);
  }
  ArDialogueSession_Destroy(&probe);
  return resolved && keys->valid;
}

static bool ResolveNameEntryText(ArDialogueContentSelection *selection,
                                 ActRaiserLocalizationValues *values,
                                 ArDialogueValueResolver *resolver,
                                 ActRaiserResolvedText *text,
                                 ArLanguagePackError *error) {
  if (!selection || !values || !resolver || !text || !error ||
      !s_runtime.name_entry.revision)
    return false;

  if (!PrepareNameEntryKeyMap(selection, resolver, values->master_name, error))
    return false;
  const NameEntryKeyMap *keys = &s_runtime.name_key_map;
  uint32_t selected_start = 0, selected_end = 0;
  if (!ActRaiserLocalizationNameEntry_SelectedKeyRange(
          &s_runtime.name_entry, keys->text, keys->text_bytes, &selected_start,
          &selected_end) ||
      !ActRaiserLocalizationNameEntryTracker_Synchronize(
          &s_runtime.name_tracker, &s_runtime.name_entry,
          keys->text + selected_start, selected_end - selected_start))
    return false;

  char display_name[kActRaiserLocalizationUnicodeNameDisplayCapacity];
  if (!ActRaiserLocalizationNameEntryTracker_CopyDisplayName(
          &s_runtime.name_tracker, display_name, sizeof(display_name)))
    return false;
  snprintf(values->master_name, sizeof(values->master_name), "%s",
           display_name);
  ValueResolver(values, resolver);

  ArDialogueSession session;
  ArDialogueSession_Init(&session);
  ArDialoguePageSnapshot page;
  bool resolved = ArDialogueSession_Begin(
      &session, selection, "name_entry.prompt_and_alphabet", resolver, error);
  if (resolved) {
    resolved = ArDialogueSession_GetAuthoredPage(
        &session, s_runtime.name_tracker.keyboard_page, &page);
  }
  if (resolved)
    resolved = ActRaiserLocalizationFixedText_FromPage(
        &page, text, error->message, sizeof(error->message));
  if (resolved) {
    resolved =
        ActRaiserLocalizationNameEntry_SelectedKeyRange(
            &s_runtime.name_entry, text->utf8, text->utf8_bytes,
            &selected_start, &selected_end) &&
        ActRaiserLocalizationText_InsertInlineObject(
            text->inline_objects, kArLocalizationFrameInlineObjectCapacity,
            &text->inline_object_count,
            (ArLocalizationInlineObjectSnapshot){
                kArLocalizationInlineObject_NameCursor, selected_end});
  }
  if (resolved) {
    text->source_revision = page.source_revision;
    resolved = PageLanguage(&page, &text->language);
  }
  ArDialogueSession_Destroy(&session);
  if (resolved)
    s_runtime.name_entry_applied_native_revision =
        s_runtime.name_entry.revision;
  return resolved;
}

typedef struct CapturedTextField {
  const char *hud_value;
  const char *location_name;
} CapturedTextField;

static bool ResolveComposeText(void *context, const char *semantic_id,
                               ActRaiserResolvedText *text, char *error_text,
                               size_t error_capacity) {
  if (!s_runtime.presentation)
    return false;
  if (!semantic_id || !text)
    return false;
  memset(text, 0, sizeof(*text));
  ArDialogueSession session;
  ArDialogueSession_Init(&session);
  ArDialogueContentSelection selection;
  ArDialogueValueResolver resolver;
  ActRaiserLocalizationValues values = s_runtime.values;
  const CapturedTextField *field = context;
  values.hud_value = field ? field->hud_value : NULL;
  values.location_name = field ? field->location_name : NULL;
  const bool name_entry =
      !strcmp(semantic_id, "name_entry.prompt_and_alphabet");
  ContentSelection(&selection);
  ValueResolver(&values, &resolver);
  ArLanguagePackError error = {{0}};
  ArDialoguePageSnapshot page;
  bool resolved = false;
  if (name_entry) {
    resolved =
        ResolveNameEntryText(&selection, &values, &resolver, text, &error);
    page.source_revision = text->source_revision;
  } else {
    resolved = ArDialogueSession_Begin(&session, &selection, semantic_id,
                                       &resolver, &error);
    if (resolved)
      resolved = ArDialogueSession_GetPage(&session, &page) &&
                 PageLanguage(&page, &text->language);
    if (resolved)
      resolved = ActRaiserLocalizationFixedText_FromPage(
          &page, text, error.message, sizeof(error.message));
  }
  if (resolved) {
    text->source_revision =
        !strncmp(semantic_id, "status.report.", 14)
            ? ActRaiserLocalizationValues_ReportRevision(&s_runtime.values)
        : name_entry ? NameEntrySourceRevision(page.source_revision)
                     : page.source_revision;
    resolved = text->cluster_count != UINT32_MAX && text->source_revision != 0;
  }
  if (!resolved && error_text && error_capacity) {
    snprintf(error_text, error_capacity, "%s",
             error.message[0] ? error.message : "message is unavailable");
  }
  ArDialogueSession_Destroy(&session);
  return resolved;
}

static bool ResolveHudValue(void *context, const char *id,
                            const char *native_value,
                            ActRaiserResolvedText *text, char *error,
                            size_t capacity) {
  (void)context;
  CapturedTextField field = {.hud_value = native_value};
  return ResolveComposeText(&field, id, text, error, capacity);
}

static bool ResolveWorldLabel(void *context, const char *id, const char *name,
                              ActRaiserResolvedText *text, char *error,
                              size_t capacity) {
  (void)context;
  CapturedTextField field = {.location_name = name};
  return ResolveComposeText(&field, id, text, error, capacity);
}

static void RefreshNameEntry(void) {
  const ActRaiserLocalizationComposeSnapshot *snapshot =
      ActRaiserLocalizationComposeState_FindObserved(
          &s_runtime.compose, kActRaiserLocalizationNameEntrySurface);
  if (!snapshot || !s_runtime.name_entry.revision ||
      s_runtime.name_entry_applied_native_revision ==
          s_runtime.name_entry.revision)
    return;
  if (!s_runtime.presentation) {
    if (ActRaiserLocalizationNameEntryTracker_SynchronizeNative(
            &s_runtime.name_tracker, &s_runtime.name_entry))
      s_runtime.name_entry_applied_native_revision =
          s_runtime.name_entry.revision;
    return;
  }
  char error[kArLanguagePackErrorCapacity] = {0};
  if (!ActRaiserLocalizationComposeState_RefreshLatest(
          &s_runtime.compose, kActRaiserLocalizationNameEntrySurface,
          ResolveComposeText, NULL,
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
      ActRaiserLocalizationComposeState_FindObserved(
          &s_runtime.compose, kActRaiserLocalizationNameEntrySurface);
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
  (void)ActRaiserLocalizationComposeState_ReleaseSurface(
      &s_runtime.compose, kActRaiserLocalizationNameEntrySurface);
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
        snapshot->text.source_revision == revision)
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
    ActRaiserMiracle_ConstrainText(&selection,route->semantic_id,&s_runtime.values.prices);
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
  ActRaiserMiracle_ConstrainText(&selection,route->semantic_id,&s_runtime.values.prices);
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

/* Expanded native glyphs supply reveal opportunities, never source-byte ratios.
 * A native page boundary drains only the current authored page;
 * a locked control/end drains added
 * pages too, without ever acknowledging a control itself. All optional waits
 * are consumed through Next/TickWait rather than skipped by ratio mapping. */
static bool PumpDialogue(bool one_glyph, bool cross_pages, uint8_t text_speed,
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
    if (!ArDialogueSession_Next(&s_runtime.session, last, &error)) {
      ScheduleFailed(error.message);
      return false;
    }
    switch (last->kind) {
    case kArDialogueToken_Grapheme:
      if (last->first_scalar != ' ' && last->first_scalar != '\n' &&
          last->first_scalar != '\t' && last->first_scalar != '\r') {
        /* Match the native $901C delay scale, including instant text. */
        for (uint8_t tick = 0; tick < text_speed && !ActRaiserSimMenu_FastReveal(); ++tick) {
          if (!host->wait_frame(host->context)) {
            ScheduleFailed("native reveal wait failed");
            return false;
          }
          if (!ActRaiserLocalizationRuntime_DialogueScheduled())
            return false;
        }
        if (one_glyph) return true;
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

void ActRaiserLocalizationRuntime_RevealGlyph(
    uint8_t text_speed, const ActRaiserLocalizationDialogueHost *host) {
  if (!host || !host->wait_frame || !host->confirm_page) return;
  ArDialogueToken token;
  (void)PumpDialogue(true, false, text_speed, host, &token);
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
    if (ActRaiserDialogueWindow_ControlClears(id)) {
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
  if (!boundary) return;
  ArDialogueToken token;
  if (!PumpDialogue(false, locked || code == 0, text_speed,
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
    const uint16_t *cgram_words, size_t cgram_word_count,
    bool mode7_transformed) {
  ArLocalizationFrame_Reset(frame);
  (void)ActRaiserLocalizationRuntime_DialogueScheduled();
  if (!frame || !cgram_words || cgram_word_count < 4 || !EnsureConfigured()) return;
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
          s_runtime.primary_font, s_runtime.pack->content_revision,
          &settings))
    return;
  if (!ArLocalizationFrame_SetFallbackFonts(frame, s_runtime.fallback_fonts,
                                            metadata->fallback_font_count))
    return;
  if (!ArLocalizationFrame_SetFontRoles(frame, s_runtime.font_roles,
                                        s_runtime.font_role_count))
    return;
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
      if (compose_route &&
          compose_route->surface_id == kActRaiserLocalizationNameEntrySurface &&
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
       * compose-state lifecycle removes the surface when the entry UI closes,
       * so absence is the generation boundary we actually need. */
      if (compose_route &&
          compose_route->surface_id == kActRaiserLocalizationNameEntrySurface &&
          !ActRaiserLocalizationComposeState_FindObserved(
              &s_runtime.compose, kActRaiserLocalizationNameEntrySurface)) {
        ActRaiserLocalizationNameEntryTracker_Init(&s_runtime.name_tracker);
        s_runtime.name_key_map.valid = false;
        s_runtime.name_entry_applied_native_revision = 0;
      }
      (void)ActRaiserLocalizationComposeState_Process(
          &s_runtime.compose, &compose_observations[index],
          ResolveComposeText, NULL, error, sizeof(error));
      s_runtime.compose_observation_serial =
          compose_observations[index].serial;
    }
  }

  /* A title exit spins/shrinks the native layer rather than clearing its text
   * tiles. Retire flat replacements after consuming queued native composes,
   * or a redraw observed on the first transformed frame can resurrect them.
   * No other surface is retired: city text stays flat over Mode 7 scenery. */
  if (mode7_transformed) {
    (void)ActRaiserLocalizationComposeState_ReleaseSurface(
        &s_runtime.compose, kActRaiserLocalizationTitleTextSurface);
    (void)ActRaiserLocalizationComposeState_ReleaseSurface(
        &s_runtime.compose, kActRaiserLocalizationTitleSelectorSurface);
    (void)ActRaiserLocalizationComposeState_ReleaseSurface(
        &s_runtime.compose, kActRaiserLocalizationTitleCopyrightSurface);
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
      ActRaiserLocalizationComposeState_FindObserved(
          &s_runtime.compose, kActRaiserLocalizationNameEntrySurface))
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
  if (ActRaiserLocalizationComposeState_Find(
          &s_runtime.compose, kActRaiserLocalizationNameEntrySurface)) {
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
    /* The keyboard's two action keys. They are font characters $7E and $7F --
     * nominal ASCII slots the game font fills with symbols, the same way the
     * selector is $3E and the population icon $3A. Palette 0, like every other
     * BG3 dialogue glyph.
     *
     * Read off the real screen, they are not pictograms: each is a pair of
     * tiny letters packed into one tile, "Ed" for End and "Bs" for BackSpace,
     * the second letter subscripted. Nothing renders them legibly at that size
     * except the game's own art, which is why they are captured rather than
     * drawn. */
    const uint16_t name_finish[] = {0x007e};
    const uint16_t name_backspace[] = {0x007f};
    if (ActRaiserLocalizationComposeState_Find(
          &s_runtime.compose, kActRaiserLocalizationNameEntrySurface)) {
      (void)ActRaiserLocalizationArt_Capture(
          &frame->artwork[kArLocalizationArtwork_NameFinish], bg3_tile_base_words,
          name_finish, 1, vram_words, vram_word_count, cgram_words, cgram_word_count);
      (void)ActRaiserLocalizationArt_Capture(
          &frame->artwork[kArLocalizationArtwork_NameBackspace], bg3_tile_base_words,
          name_backspace, 1, vram_words, vram_word_count, cgram_words, cgram_word_count);
    }
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
  ActRaiserTextPalette inks;
  ActRaiserTextPalette_Capture(&inks, cgram_words, cgram_word_count);
  const ActRaiserHudOwner hud_owner = ActRaiserHud_Presented(map_group, map_number);
  ActRaiserLocalizationHud_CapturePalette(&inks, hud_owner, bg3_tilemap_base_words,
                                          vram_words, vram_word_count,
                                          cgram_words, cgram_word_count);
  if (s_runtime.presentation)
    (void)ActRaiserLocalizationComposeState_AppendFrame(
        &s_runtime.compose, frame, destination, &inks);
  /* Native producers/upload own identity; the adapter only validates and
   * presents their fields, including the SIM bar inside the temple. */
  if (s_runtime.presentation)
    ActRaiserLocalizationHud_Append(
        &s_runtime.hud, hud_owner, frame, destination, bg3_tile_base_words, vram_words,
        vram_word_count, cgram_words, cgram_word_count, ResolveComposeText,
        ResolveHudValue, NULL);
  if (s_runtime.presentation)
    ActRaiserLocalizationCredits_Append(&s_runtime.credits, frame, destination,
        ActRaiserCredits_PresentedPage(), map_group, map_number, bg3_tile_base_words,
        g_ram, kActRaiserWramSize, vram_words, vram_word_count,
        cgram_words, cgram_word_count, ResolveComposeText, NULL);
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
  ArLocalizationTextLanguage language;
  if (!ArDialogueSession_GetPage(&s_runtime.session, &page) ||
      !PageLanguage(&page, &language))
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
  const ActRaiserDialogueWindow *window = &s_runtime.dialogue_window;
  if (window->bidi.count > kArTextMaximumBidiSpans - frame->bidi.count) {
    ScheduleFailed("dialogue value spans exceed frame capacity");
    return;
  }
  /* Whitespace normalization changes byte/cluster counts. Map the actual
   * grapheme boundary through the cached normalization, not another ratio.
   * This is O(1) per frame and never reveals half a UTF-8 grapheme. */
  const size_t reveal_bytes =
      ActRaiserDialogueWindow_RevealedBytes(window, page.revealed_utf8_bytes);
  static int reveal_trace = -1;
  static uint64_t trace_ticket;
  static size_t trace_bytes;
  if (reveal_trace < 0) {
    const char *value = getenv("AR_LOCALIZATION_REVEAL_TRACE");
    reveal_trace = value && value[0] && value[0] != '0';
  }
  if (reveal_trace && (trace_ticket != s_runtime.dialogue_ticket ||
                       trace_bytes != reveal_bytes)) {
    const unsigned game_frame = g_ram[kActRaiserWram_GameFrame] |
        ((unsigned)g_ram[kActRaiserWram_GameFrame + 1u] << 8);
    fprintf(stderr,
            "[localization-reveal] gf=%u serial=%llu page=%u bytes=%zu total=%zu\n",
            game_frame, (unsigned long long)observation.serial,
            page.page_index, reveal_bytes, window->bytes);
    trace_ticket = s_runtime.dialogue_ticket;
    trace_bytes = reveal_bytes;
  }
  const bool added =
      ArLocalizationFrame_AddStructuredDialogueWindow(
          frame, route->surface_id, destination, route->region, window->text,
          window->bytes, (uint32_t)reveal_bytes, window->clusters,
          page.source_revision, language.direction, route->native_font_pixels,
          window->structural_boundaries) &&
      ArLocalizationFrame_SetTextLanguage(frame, &language) &&
      ArLocalizationFrame_SetTextBidiSpans(frame, &window->bidi) &&
      ActRaiserTextStyle_Publish(&window->styles, 0, &inks, frame);
  if (!added) {
    ScheduleFailed("dialogue cannot fit in the current frame");
    return;
  }
  ActRaiserLocalizationStyle_Ordinary(
      &frame->snapshots[frame->snapshot_count - 1u], cgram_words);
  if (ActRaiserLocalizationRuntime_DialogueScheduled()) {
    frame->dialogue_ticket = s_runtime.dialogue_ticket;
    frame->dialogue_surface_id = route->surface_id;
  }
  if (added && reveal_bytes == window->bytes &&
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

void ActRaiserLocalizationRuntime_AppendWorldNavigationLabel(
    ArLocalizationFrame *frame, uint16_t active_location,
    bool native_label_visible,
    const uint16_t *cgram_words, size_t cgram_word_count) {
  if (!s_runtime.presentation || !frame) return;
  char error[kArLanguagePackErrorCapacity] = {0};
  if (!ActRaiserLocalizationWorldNavigation_Append(
          &s_runtime.world_navigation, frame, active_location,
          native_label_visible, cgram_words, cgram_word_count,
          ResolveComposeText, ResolveWorldLabel, NULL, error, sizeof(error)) &&
      error[0])
    fprintf(stderr,
            "[localization] world-navigation label unavailable (%s); "
            "native text retained\n",
            error);
}

static void MenuLabelSingleLine(ActRaiserResolvedText *text) {
  /* Modern labels have one wide row. Keep style/bidi byte offsets intact
   * while discarding line breaks authored for the original narrow boxes. */
  for (size_t byte=0;byte<text->utf8_bytes;++byte)
    if (text->utf8[byte]=='\n' || text->utf8[byte]=='\r') text->utf8[byte]=' ';
}

static void CaptureMenuDockLabels(
    ArLocalizationFrame *dock, const ArLocalizationFrame *source,
    const uint16_t *cgram, size_t cgram_count) {
  if (!dock) return;
  ArLocalizationFrame_Reset(dock);
  if (!source || !s_runtime.presentation ||
      !ArLocalizationFrame_SetFont(dock, source->locale, source->font_stack_id,
          source->primary_font, source->font_revision, &source->settings)) return;
  ArLocalizationFrame_SetFallbackFonts(dock, source->fallback_fonts,
                                       source->fallback_font_count);
  ArLocalizationFrame_SetFontRoles(dock, source->font_roles,
                                   source->font_role_count);
  static const char *const categories[6]={
    "sim.menu.choice.movement", "sim.menu.choice.direct_people",
    "sim.menu.choice.miracles", "sim.menu.choice.offerings",
    "sim.menu.choice.status", "sim.menu.choice.other"};
  ActRaiserTextPalette palette;
  ActRaiserTextPalette_Capture(&palette,cgram,cgram_count);
  for (unsigned i=0;i<6;++i) {
    ActRaiserResolvedText text={0};
    char error[256]={0};
    if (!ResolveComposeText(NULL,categories[i],&text,error,sizeof(error))) continue;
    MenuLabelSingleLine(&text);
    if (!ArLocalizationFrame_AddScreenText(dock,610+i,0,0,192,18,
          text.utf8,text.utf8_bytes,text.cluster_count,text.cluster_count,
          text.source_revision,text.language.direction,8,
          kArLocalizationTextLayout_CenteredLabel)) continue;
    ArLocalizationFrame_SetTextLanguage(dock,&text.language);
    ArLocalizationFrame_SetTextBidiSpans(dock,&text.bidi);
    ActRaiserTextStyle_Publish(&text.styles,0,&palette,dock);
  }
}

static void AppendMenuLabels(
    ArLocalizationFrame *frame, const SimMenuModel *menu,
    const uint16_t *cgram, size_t cgram_count) {
  if (!frame || !menu || !s_runtime.presentation ||
      menu->phase == kSimMenu_Native || menu->phase == kSimMenu_Closed) return;
  const bool confirmation=menu->phase==kSimMenu_Confirm ||
      (menu->phase==kSimMenu_Dialogue && menu->dialogue_has_selector &&
       SimMenuModel_Action(menu)!=15);
  SimMenuModel origin=*menu;
  if(origin.phase==kSimMenu_Describe || origin.phase==kSimMenu_Dialogue)
    origin.phase=origin.return_phase;
  if(origin.phase==kSimMenu_Confirm) origin.phase=kSimMenu_Browse;
  if(origin.phase==kSimMenu_MessageSpeed) origin.phase=kSimMenu_Browse;
  menu=&origin;
  static const char *const categories[6]={
    "sim.menu.choice.movement", "sim.menu.choice.direct_people",
    "sim.menu.choice.miracles", "sim.menu.choice.offerings",
    "sim.menu.choice.status", "sim.menu.choice.other"};
  for (unsigned index=0;index<9;++index) {
    const char *id=NULL;
    char item[64];
    if (!index) id=categories[menu->category];
    else if (menu->phase == kSimMenu_Inventory && index<=menu->item_count) {
      snprintf(item,sizeof(item),"sim.menu.possession.slot_%02u",menu->items[index-1]-1);
      id=item;
    } else if (menu->phase == kSimMenu_Browse && menu->submenu) {
      for (unsigned a=0;a<15;++a)
        if (kSimMenuActions[a].category==menu->category &&
            kSimMenuActions[a].row==index-1) id=kSimMenuActions[a].semantic_id;
    }
    if (!id) continue;
    ActRaiserResolvedText text={0};
    char error[256]={0};
    if (!ResolveComposeText(NULL,id,&text,error,sizeof(error))) continue;
    MenuLabelSingleLine(&text);
    /* Keep the existing 10px requested font, with enough vertical room for
     * its shadow and descenders at the largest enhanced-text setting. */
    if (!ArLocalizationFrame_AddScreenText(frame,600+index,0,0,132,18,
          text.utf8,text.utf8_bytes,text.cluster_count,text.cluster_count,
          text.source_revision,text.language.direction,10,
          kArLocalizationTextLayout_SingleLineLabel)) continue;
    ArLocalizationFrame_SetTextLanguage(frame,&text.language);
    ArLocalizationFrame_SetTextBidiSpans(frame,&text.bidi);
    ActRaiserTextPalette palette;
    ActRaiserTextPalette_Capture(&palette,cgram,cgram_count);
    ActRaiserTextStyle_Publish(&text.styles,0,&palette,frame);
  }
  if (confirmation) {
    /* Reserve translated choice ink before the native selector composes it.
     * Same source and 6x5 native region/font as system.choice.yes_no. This
     * record is measured only; the native selector still draws its labels. */
    ActRaiserResolvedText text={0};
    char error[256]={0};
    if (ResolveComposeText(NULL,"system.choice.yes_no",&text,error,sizeof(error)) &&
        ArLocalizationFrame_AddScreenText(frame,609,0,0,48,40,text.utf8,
          text.utf8_bytes,text.cluster_count,text.cluster_count,text.source_revision,
          text.language.direction,7,kArLocalizationTextLayout_SingleLineLabel)) {
      ArLocalizationFrame_SetTextLanguage(frame,&text.language);
      ArLocalizationFrame_SetTextBidiSpans(frame,&text.bidi);
      ActRaiserTextPalette palette;
      ActRaiserTextPalette_Capture(&palette,cgram,cgram_count);
      ActRaiserTextStyle_Publish(&text.styles,0,&palette,frame);
    }
  }
}

void ActRaiserLocalizationRuntime_CaptureMenuLabels(
    ArLocalizationFrame *labels, const ArLocalizationFrame *source,
    const SimMenuModel *menu,
    const uint16_t *cgram, size_t cgram_count) {
  /* Native inventory/HUD/dialogue claims can fill their entire snapshot
   * budget. Copy only font configuration; keep every modern label separate. */
  CaptureMenuDockLabels(labels,source,cgram,cgram_count);
  AppendMenuLabels(labels,menu,cgram,cgram_count);
}

bool ActRaiserLocalizationRuntime_BeginMenuHelp(
    ArDialogueSession *session, const char *id, const char *fallback) {
  if (!session || !id || !fallback) return false;
  ArLanguagePackError error={{0}};
  ArDialogueContract contract={0};
  /* Optional neutral help has no gameplay controls or dynamic values. A pack
   * can supply it without changing the contracts of any native action. */
  if (EnsureConfigured() && s_runtime.presentation) {
    const ArLanguageMessage *message=ArLanguagePack_FindMessage(&s_runtime.selected_pack,id);
    if (message) {
      const ArDialogueSource source={.effective_pack=&s_runtime.selected_pack,
        .message=message,.resolved_source=kArDialogueResolvedSource_SelectedPack,
        .presentation=kArDialoguePresentation_Enhanced};
      if (ArDialogueSession_BeginSource(session,&source,&contract,id,NULL,
          kArLocalizationFrameTextCapacity,&error)) return true;
    }
  }
  char script[4096];
  const int length=snprintf(script,sizeof(script),
      "@define-style native_help band=native:dialogue.band "
      "body=native:dialogue.body shadow=native:dialogue.shadow\n"
      ":: %s\n@style native_help\n%s\n@end\n",id,fallback);
  if (length<0 || (size_t)length>=sizeof(script)) return false;
  const ArTextDocumentSource text={"sim-menu-help.artext",script,(size_t)length};
  const ArTextDocumentConfig document={.id="sim-menu-help",.locale="en-US",
    .format_version=2,.sources=&text,.source_count=1};
  ArLanguagePack pack; ArLanguagePack_Init(&pack);
  bool valid=ArLanguagePack_ParseDocument(&pack,&document,&error);
  if (valid) {
    const ArDialogueSource source={.effective_pack=&pack,
      .message=ArLanguagePack_FindMessage(&pack,id),
      .resolved_source=kArDialogueResolvedSource_NativeEnhanced,
      .presentation=kArDialoguePresentation_Enhanced};
    valid=ArDialogueSession_BeginSource(session,&source,&contract,id,NULL,
                                        kArLocalizationFrameTextCapacity,&error);
  }
  ArLanguagePack_Destroy(&pack);
  return valid;
}

bool ActRaiserLocalizationRuntime_PrepareMenuHelpStyle(
    const ArDialoguePageSnapshot *source, const SimMenuHelpPage *help) {
  memset(&s_menu_help_style, 0, sizeof(s_menu_help_style));
  memset(&s_menu_help_bidi, 0, sizeof(s_menu_help_bidi));
  if (!source || !help || help->source_end > source->utf8_bytes) return false;
  const size_t bytes = help->source_end - help->source_start;
  uint16_t *offsets = calloc(bytes + 1, sizeof(*offsets));
  if (!offsets) return false;
  /* Help retains the authored source slice exactly. Native-only soft wraps
   * never enter this text, so style and bidi offsets remain byte-for-byte. */
  for (size_t at = 0; at <= bytes; ++at) offsets[at]=(uint16_t)at;
  char error[256];
  bool valid = ActRaiserTextStyle_AppendPage(&s_menu_help_style, source,
      help->source_start, bytes, offsets, help->text, help->bytes, 0,
      error, sizeof(error));
  for (size_t i = 0; valid && i < source->bidi_span_count; ++i) {
    ArTextBidiSpan span = source->bidi_spans[i];
    if (span.end <= help->source_start || span.start >= help->source_end) continue;
    const size_t a = span.start > help->source_start ? span.start - help->source_start : 0;
    const size_t b = span.end < help->source_end ? span.end - help->source_start : bytes;
    span.start = offsets[a]; span.end = offsets[b];
    if (span.start < span.end) {
      if (s_menu_help_bidi.count >= kArTextMaximumBidiSpans) valid = false;
      else s_menu_help_bidi.spans[s_menu_help_bidi.count++] = span;
    }
  }
  free(offsets);
  return valid;
}

void ActRaiserLocalizationRuntime_AppendMenuHelp(
    ArLocalizationFrame *frame, const SimMenuHelpPage *help,
    const uint16_t *cgram, size_t cgram_count) {
  if(!frame || !help || !help->active || !s_runtime.presentation ||
      !cgram || cgram_count<4) return;
  const size_t revealed=help->revealed_glyphs==help->glyph_count?help->bytes:
      help->revealed_glyphs?help->text_ends[help->revealed_glyphs-1]:0;
  size_t at=0,next; uint32_t total=0,visible=0;
  while(at<help->bytes && ArUnicodeGrapheme_Next(help->text,help->bytes,at,NULL,&next)) {
    ++total; if(next<=revealed) ++visible; at=next;
  }
  if(!ArLocalizationFrame_AddScreenText(frame,700,40,156,176,56,
    help->text,help->bytes,visible,total,
    /* Zero is an invalid snapshot revision; page zero at byte zero is the
     * most common Help page and must be published too. */
    (((uint64_t)help->authored_page<<32) | help->source_start)+1,
    help->direction,8,kArLocalizationTextLayout_DialogueWindow)) return;
  ArLocalizationTextLanguage language={.direction=help->direction};
  snprintf(language.locale,sizeof(language.locale),"%s",help->locale);
  ArLocalizationFrame_SetTextLanguage(frame,&language);
  ArLocalizationTextSnapshot *text=&frame->snapshots[frame->snapshot_count-1];
  text->revealed_utf8_bytes=(uint32_t)revealed;
  text->style_id=kArTextStyle_RetailPaletteBands;
  text->shadow_enabled=true; text->shadow_shape=kArTextShadow_Diagonal;
  text->shadow_rgb=ActRaiserLocalizationStyle_Rgb(cgram[1]);
  text->band_rgb=ActRaiserLocalizationStyle_Rgb(cgram[2]);
  text->body_rgb=ActRaiserLocalizationStyle_Rgb(cgram[3]);
  ActRaiserTextPalette palette;
  ActRaiserTextPalette_Capture(&palette, cgram, cgram_count);
  ActRaiserTextStyle_Publish(&s_menu_help_style, 0, &palette, frame);
  ArLocalizationFrame_SetTextBidiSpans(frame, &s_menu_help_bidi);
}

void ActRaiserLocalizationRuntime_Shutdown(void) {
  RetireFonts(s_runtime.primary_font, s_runtime.fallback_fonts,
              s_runtime.font_roles, s_runtime.font_role_count);
  if (s_runtime.configured) {
    ArDialogueSession_Destroy(&s_runtime.session);
    ArLanguagePack_Destroy(&s_runtime.selected_pack);
    ArLanguagePack_Destroy(&s_runtime.native_pack);
  }
  memset(&s_runtime, 0, sizeof(s_runtime));
}
