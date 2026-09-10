#include "actraiser/actraiser_localization_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser/actraiser_localization_routes.h"
#include "actraiser/actraiser_localization_text.h"
#include "actraiser_game.h"
#include "localization/dialogue_session.h"
#include "localization/language_pack.h"
#include "localization/unicode_grapheme.h"
#include "save_system.h"

enum { kManifestPathCapacity = 1024 };

typedef struct LocalizationRuntime {
  bool configured;
  bool enabled;
  ArLanguagePack pack;
  ArDialogueSession session;
  uint64_t observation_serial;
  const ActRaiserLocalizationRoute *route;
  char manifest_path[kManifestPathCapacity];
  char primary_font_path[kArLocalizationFrameFontPathCapacity];
} LocalizationRuntime;

static LocalizationRuntime s_runtime;

static bool CopyPath(char *destination, size_t capacity, const char *source) {
  const size_t length = source ? strlen(source) : 0;
  if (!source || !length || length >= capacity) return false;
  memcpy(destination, source, length + 1u);
  return true;
}

static bool ResolveFontPath(const char *manifest_path,
                            const ArLanguagePackMetadata *metadata,
                            char *destination, size_t capacity) {
  if (!manifest_path || !metadata || !destination || !capacity) return false;
  if (!strcmp(metadata->primary_font, "builtin:actraiser-sans"))
    return CopyPath(destination, capacity,
                    "game-assets/fonts/noto/"
                    "NotoSans-SemiCondensedExtraBold.ttf");
  if (!strncmp(metadata->primary_font, "builtin:", 8)) return false;
  const char *slash = strrchr(manifest_path, '/');
#ifdef _WIN32
  const char *backslash = strrchr(manifest_path, '\\');
  if (!slash || (backslash && backslash > slash)) slash = backslash;
#endif
  const size_t directory_bytes = slash ? (size_t)(slash - manifest_path + 1) : 0;
  const size_t font_bytes = strlen(metadata->primary_font);
  if (directory_bytes + font_bytes >= capacity) return false;
  if (directory_bytes) memcpy(destination, manifest_path, directory_bytes);
  memcpy(destination + directory_bytes, metadata->primary_font, font_bytes + 1u);
  return true;
}

static bool EnsureConfigured(void) {
  if (s_runtime.configured) return s_runtime.enabled;
  s_runtime.configured = true;
  ArLanguagePack_Init(&s_runtime.pack);
  ArDialogueSession_Init(&s_runtime.session);
  const char *manifest = getenv("AR_LOCALIZATION_PACK");
  if (!manifest || !manifest[0]) return false;
  if (!CopyPath(s_runtime.manifest_path, sizeof(s_runtime.manifest_path),
                manifest)) {
    fprintf(stderr, "[localization] pack path is too long; native text retained\n");
    return false;
  }
  ArLanguagePackIo io;
  ArLanguagePackFileIo_Init(&io);
  ArLanguagePackError error = {{0}};
  if (!ArLanguagePack_Load(&s_runtime.pack, &io, manifest, &error)) {
    fprintf(stderr, "[localization] pack unavailable (%s); native text retained\n",
            error.message[0] ? error.message : "load failed");
    return false;
  }
  const ArLanguagePackMetadata *metadata =
      ArLanguagePack_GetMetadata(&s_runtime.pack);
  if (!ResolveFontPath(manifest, metadata, s_runtime.primary_font_path,
                       sizeof(s_runtime.primary_font_path))) {
    fprintf(stderr,
            "[localization] primary font cannot be resolved; native text retained\n");
    return false;
  }
  s_runtime.enabled = true;
  fprintf(stderr, "[localization] enhanced pack %s (%s) enabled\n",
          metadata->display_name, metadata->locale);
  return true;
}

static bool ResolveValue(void *context, const char *name,
                         ArLanguagePlaceholderKind expected_kind,
                         ArDialogueValue *value, char *error,
                         size_t error_capacity) {
  (void)context;
  if (!strcmp(name, "master_name") &&
      expected_kind == kArLanguagePlaceholder_LocalizedText) {
    value->kind = expected_kind;
    if (!SaveSystem_CopyPlayerName(value->text, sizeof(value->text)))
      snprintf(value->text, sizeof(value->text), "Master");
    return true;
  }
  if (error && error_capacity)
    snprintf(error, error_capacity, "unsupported live value: %s", name);
  return false;
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
static bool NormalizeText(const char *source, size_t source_bytes,
                          char *destination, size_t capacity,
                          size_t *destination_bytes) {
  if (!source || !destination || !capacity || !destination_bytes) return false;
  size_t written = 0;
  bool pending_space = false;
  for (size_t index = 0; index < source_bytes; ++index) {
    const char byte = source[index];
    if (byte == ' ' || byte == '\t' || byte == '\r') {
      pending_space = written && destination[written - 1u] != '\n';
      continue;
    }
    if (byte == '\n') {
      while (written && destination[written - 1u] == ' ') --written;
      if (written && destination[written - 1u] != '\n') {
        if (written + 1u >= capacity) return false;
        destination[written++] = '\n';
      }
      pending_space = false;
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
  return written != 0;
}

void ActRaiserLocalizationRuntime_CaptureFrame(
    ArLocalizationFrame *frame, uint16_t bg3_tilemap_base_words,
    unsigned bg3_tilemap_width_tiles,
    unsigned bg3_tilemap_height_tiles,
    const uint16_t *vram_words,
    size_t vram_word_count) {
  ArLocalizationFrame_Reset(frame);
  if (!frame || !EnsureConfigured()) return;
  ActRaiserLocalizationTextObservation observation = {
      .struct_size = sizeof(observation),
  };
  if (!ActRaiserLocalizationText_CopyObservation(&observation))
    return;
  /* Opcode $00 enters the native input-acknowledgement loop. Keep presenting
   * its completed page until the game changes scene or a later fixed composer
   * begins the replacement UI generation. */
  if (observation.map_group != g_ram[kActRaiserWram_MapGroup] ||
      observation.map_number != g_ram[kActRaiserWram_CurrentMap] ||
      ActRaiserLocalizationText_TerminalWasReplaced(&observation))
    return;
  const ActRaiserLocalizationRoute *route =
      ActRaiserLocalizationRoute_ResolveDialogue(&observation);
  if (!route) return;

  ArLanguagePackError error = {{0}};
  if (s_runtime.observation_serial != observation.serial) {
    const ArDialogueContentSelection selection = {
        .struct_size = sizeof(selection),
        .abi_version = AR_DIALOGUE_SESSION_ABI_VERSION,
        .presentation = kArDialoguePresentation_Enhanced,
        .selected_pack = &s_runtime.pack,
        .native_us_enhanced_pack = &s_runtime.pack,
    };
    const ArDialogueValueResolver resolver = {
        .struct_size = sizeof(resolver),
        .abi_version = AR_DIALOGUE_VALUE_RESOLVER_ABI_VERSION,
        .resolve = ResolveValue,
    };
    s_runtime.observation_serial = observation.serial;
    s_runtime.route = NULL;
    if (!ArDialogueSession_Begin(&s_runtime.session, &selection,
                                 route->semantic_id, &resolver, &error)) {
      fprintf(stderr,
              "[localization] %s unavailable (%s); native text retained\n",
              route->semantic_id,
              error.message[0] ? error.message : "session failed");
      return;
    }
    s_runtime.route = route;
  }
  if (s_runtime.route != route) return;

  const uint16_t page_units = ActRaiserLocalizationRoute_PageUnitCount(
      route, observation.page_index);
  ArDialogueNativeProgress progress = {
      .struct_size = sizeof(progress),
      .abi_version = AR_DIALOGUE_SESSION_ABI_VERSION,
      .authored_page_index = observation.page_index,
      .revealed_unit_count = observation.page_unit_index,
      .page_unit_count = page_units,
  };
  if (progress.revealed_unit_count > progress.page_unit_count)
    progress.revealed_unit_count = progress.page_unit_count;
  if (!ArDialogueSession_SynchronizeNativeProgress(
          &s_runtime.session, &progress, &error))
    return;
  ArDialoguePageSnapshot page;
  if (!ArDialogueSession_GetPage(&s_runtime.session, &page)) return;

  char normalized[kArLocalizationFrameTextCapacity];
  size_t normalized_bytes = 0;
  if (!NormalizeText(page.utf8, page.utf8_bytes, normalized,
                     sizeof(normalized), &normalized_bytes))
    return;
  const uint32_t normalized_clusters =
      CountClusters(normalized, normalized_bytes);
  if (normalized_clusters == UINT32_MAX) return;
  uint32_t revealed = normalized_clusters;
  if (page.cluster_count) {
    revealed = (uint32_t)(((uint64_t)page.revealed_cluster_count *
                           normalized_clusters + page.cluster_count - 1u) /
                          page.cluster_count);
    if (revealed > normalized_clusters) revealed = normalized_clusters;
  }

  const ArLanguagePackMetadata *metadata =
      ArLanguagePack_GetMetadata(&s_runtime.pack);
  ArEnhancedTextSettings settings;
  ArEnhancedTextSettings_Defaults(&settings);
  if (!metadata || !ArLocalizationFrame_SetFont(
          frame, metadata->locale, metadata->primary_font,
          s_runtime.primary_font_path, s_runtime.pack.content_revision,
          &settings))
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
  ArTextCellRegion native_preserves[
      kActRaiserLocalizationMaximumNativePreserves];
  const size_t native_preserve_count =
      ActRaiserLocalizationRoute_FindNativePreserves(
          route, bg3_tilemap_base_words,
          bg3_tilemap_width_tiles, bg3_tilemap_height_tiles,
          vram_words, vram_word_count, native_preserves,
          kActRaiserLocalizationMaximumNativePreserves);
  if (native_preserve_count == SIZE_MAX) return;
  (void)ArLocalizationFrame_AddText(
      frame, route->surface_id, destination, route->region,
      normalized, normalized_bytes, revealed, normalized_clusters,
      page.source_revision, direction, route->native_font_pixels,
      native_preserves, (uint8_t)native_preserve_count);
}

void ActRaiserLocalizationRuntime_Shutdown(void) {
  if (s_runtime.configured) {
    ArDialogueSession_Destroy(&s_runtime.session);
    ArLanguagePack_Destroy(&s_runtime.pack);
  }
  memset(&s_runtime, 0, sizeof(s_runtime));
}
