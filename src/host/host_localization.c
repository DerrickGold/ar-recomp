#include "host/host_localization.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser/actraiser_localization_runtime.h"
#include "actraiser/regional/actraiser_regional_media.h"
#include "app/settings.h"
#include "app/input_map.h"
#include "host/font_resources.h"
#include "host/host_video.h"
#include "host/regional_media_files.h"
#include "localization/language_pack.h"
#include "localization/pack_discovery.h"
#include "platform/sdl/text_rasterizer_sdl.h"
#include "render/localized_text_presenter.h"
#include "settings_overlay/settings_overlay.h"
#include "snesrecomp/support/utf8_fs.h"

static ArHostFontResources s_font_resources;
static ArHostRegionalMediaFiles s_regional_media;
static ArTextBackend s_text_backend;
static bool s_headless;
static bool s_exit_requested;

void HostLocalization_LoadRegionalMedia(void) {
  static const char *const donors[]={"us","jp","eu-en","de","fr"};
  for(unsigned i=0;i<sizeof(donors)/sizeof(donors[0]);++i) {
    char path[128],error[192];
    snprintf(path,sizeof(path),"game-assets/regions/%s.armedia",donors[i]);
    if(!sr_path_exists(path))continue;
    if (!ArHostRegionalMediaFiles_Load(&s_regional_media, path, (ArRegionalMediaRelease)(i + 1),
                                       error, sizeof(error))) {
      fprintf(stderr, "[regional-media] %s: %s; US graphics retained\n", path, error);
      continue;
    }
    const ArRegionalMediaView *view =
        ArHostRegionalMediaFiles_View(&s_regional_media, (ArRegionalMediaRelease)(i + 1));
    if(!ActRaiserRegionalMedia_AddDonor(view)) {
      fprintf(stderr, "[regional-media] %s: donor does not match filename; US graphics retained\n",
              path);
      continue;
    }
    fprintf(stderr,"[regional-media] loaded %s (%zu reviewed resources)\n",donors[i],view->count);
  }
}

static ArFontResourceId RegisterLocalizedFont(
    void *context, const char *manifest, const char *member,
    char *error, size_t capacity) {
  (void)context;
  char path[1024];
  if (member && !strcmp(member, "builtin:actraiser-sans"))
    snprintf(path, sizeof(path),
             "game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf");
  else if (!member || !strncmp(member, "builtin:", 8) ||
           !ArLanguagePack_ResolveMemberPath(manifest, member, path, sizeof(path))) {
    if (error && capacity) snprintf(error, capacity, "font member is unavailable");
    return 0;
  }
  return ArHostFontResources_RegisterFile(&s_font_resources, path, error, capacity);
}

static void RetireLocalizedFont(void *context, ArFontResourceId font) {
  (void)context;
  ArHostFontResources_Retire(&s_font_resources, font);
}

static bool PrepareLocalizedFont(void *context,
                                 const ArTextPresentationFont *font,
                                 char *error, size_t error_capacity) {
  return ArLocalizedTextPresenter_PrepareFont(context, font, error,
                                              error_capacity);
}

static void DiscardPreparedLocalizedFont(void *context) {
  ArLocalizedTextPresenter_DiscardPreparedFont(context);
}

static void ExplainLegacyLanguagePack(void *context, const char *manifest,
                                      const ArLanguagePackMetadata *metadata,
                                      bool native) {
  (void)context;
  const ArUiLocale locale = (ArUiLocale)g_settings.interface_language;
  const char *instructions = ArUiCatalog_Text(
      locale,
      native ? "localization.upgrade.native" : "localization.upgrade.pack",
      NULL);
  char message[4096];
  const ArUiTextArgument arguments[] = {{"name", metadata->display_name},
                                        {"instructions", instructions},
                                        {"path", manifest}};
  if (!ArUiCatalog_Format(
          message, sizeof(message),
          ArUiCatalog_Text(locale, "localization.upgrade.message", NULL),
          arguments, 3))
    snprintf(message, sizeof(message), "%s\n%s\n%s", metadata->display_name,
             instructions, manifest);
  fprintf(stderr, "[localization] %s\n", message);
  if (s_headless)
    return;
  const SDL_MessageBoxButtonData buttons[] = {
      {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1,
       ArUiCatalog_Text(locale, "localization.upgrade.continue", NULL)},
      {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0,
       ArUiCatalog_Text(locale, "localization.upgrade.exit", NULL)}};
  const SDL_MessageBoxData dialog = {
      .flags = SDL_MESSAGEBOX_WARNING,
      .window = g_window,
      .title = ArUiCatalog_Text(locale, "localization.upgrade.title", NULL),
      .message = message,
      .numbuttons = 2,
      .buttons = buttons};
  int answer = -1;
  if (!SDL_ShowMessageBox(&dialog, &answer))
    fprintf(stderr, "[localization] cannot show upgrade window: %s\n",
            SDL_GetError());
  s_exit_requested = answer != 1;
}

void HostLocalization_PublishInstalledPacks(void) {
  /* The launcher has resolved the runtime working directory (utils/ in a
   * bundle). Catalog scanning does not move it to the executable directory. */
  ArLanguagePackCatalog *catalog = calloc(1, sizeof(*catalog));
  SettingsLocalizationPack *choices = calloc(kSettingsLocalizationMaximumPacks, sizeof(*choices));
  if (catalog && choices &&
      ArLanguagePackCatalog_ScanDesktop(catalog, "game-assets/languages/packs")) {
    for (size_t i = 0; i < catalog->count; ++i) {
      const ArLanguagePackCatalogEntry *entry = &catalog->entries[i];
      snprintf(choices[i].id, sizeof(choices[i].id), "%s", entry->metadata.package_id);
      snprintf(choices[i].name, sizeof(choices[i].name), "%s", entry->metadata.display_name);
      snprintf(choices[i].locale, sizeof(choices[i].locale), "%s", entry->metadata.locale);
      snprintf(choices[i].manifest, sizeof(choices[i].manifest), "%s", entry->manifest);
    }
    if (!Settings_SetLocalizationPacks(choices, catalog->count))
      fprintf(stderr, "[localization] installed pack catalog has conflicting identities\n");
  }
  free(choices);
  free(catalog);
}

void HostLocalization_Install(bool headless) {
  s_headless = headless;
  ArSdlTextBackend_Init(&s_text_backend);
  ArLocalizedTextPresenter_SetBackend(&s_text_backend);
  const ArFontResources font_resources = ArHostFontResources_Provider(&s_font_resources);
  ArLocalizedTextPresenter_SetFontResources(&font_resources);
  ArLanguagePackIo pack_io;
  ArLanguagePackFileIo_Init(&pack_io);
  const char *native_manifest = getenv("AR_LOCALIZATION_NATIVE_PACK");
  if (!native_manifest || !native_manifest[0])
    native_manifest = "game-assets/languages/native-us/pack.ini";
  const ActRaiserLocalizationPackHost pack_host = {
      .struct_size = sizeof(pack_host),
      .abi_version = ACTRAISER_LOCALIZATION_PACK_HOST_ABI_VERSION,
      .io = pack_io,
      .native_manifest = native_manifest,
      .require_v2 = true,
      .context = NULL,
      .legacy_pack = ExplainLegacyLanguagePack,
  };
  ActRaiserLocalizationRuntime_SetPackHost(&pack_host);
  ActRaiserLocalizationRuntime_SetButtonPromptHost(InputMap_CaptureButtonPrompts, NULL);
  const ArTextPresentationHost text_host = {
      .struct_size = sizeof(text_host),
      .abi_version = AR_TEXT_PRESENTATION_ABI_VERSION,
      .context = &g_render_device,
      .prepare_font = PrepareLocalizedFont,
      .register_font = RegisterLocalizedFont,
      .retire_font = RetireLocalizedFont,
      .discard_prepared_font = DiscardPreparedLocalizedFont,
  };
  ActRaiserLocalizationRuntime_SetPresentationHost(&text_host);
}

void HostLocalization_InstallInterfaceFonts(void) {
  const ArFontResources font_resources = ArHostFontResources_Provider(&s_font_resources);
  /* Interface text has its own font/cache lifetime, independent of whichever
   * game language pack is selected. Resources are resolved by this host. */
  char ui_font_error[kArTextRasterErrorCapacity] = {0};
  const ArFontResourceId ui_primary = ArTextBackend_IsReady(&s_text_backend)
      ? ArHostFontResources_RegisterFile(
            &s_font_resources,
            "game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf",
            ui_font_error, sizeof(ui_font_error)) : 0;
  const ArFontResourceId ui_fallbacks[] = {
      ui_primary ? ArHostFontResources_RegisterFile(
          &s_font_resources, "game-assets/fonts/noto/NotoSansJP-Bold.otf",
          ui_font_error, sizeof(ui_font_error)) : 0,
      ui_primary ? ArHostFontResources_RegisterFile(
          &s_font_resources, "game-assets/fonts/noto/NotoSansArabic-Bold.ttf",
          ui_font_error, sizeof(ui_font_error)) : 0,
      ui_primary ? ArHostFontResources_RegisterFile(
          &s_font_resources, "game-assets/fonts/noto/NotoSansHebrew-Bold.ttf",
          ui_font_error, sizeof(ui_font_error)) : 0};
  const size_t ui_fallback_count = sizeof(ui_fallbacks) / sizeof(ui_fallbacks[0]);
  bool ui_fonts_ready = ui_primary != 0;
  for (size_t i = 0; i < ui_fallback_count; ++i)
    ui_fonts_ready = ui_fonts_ready && ui_fallbacks[i] != 0;
  const ArTextBackendConfig ui_fonts = {
      .struct_size = sizeof(ui_fonts),
      .abi_version = AR_TEXT_BACKEND_CONFIG_ABI_VERSION,
      .font_stack_id = "system-interface",
      .resources = font_resources, .primary_font = ui_primary,
      .fallback_fonts = ui_fallbacks, .fallback_font_count = ui_fallback_count,
      .font_revision = 2, .cached_size_capacity = 16,
  };
  if (ArTextBackend_IsReady(&s_text_backend) &&
      ArRenderDevice_IsReady(&g_render_device) &&
      (!ui_fonts_ready ||
       !SettingsOverlay_SetTextBackend(&s_text_backend, &ui_fonts,
                                        ui_font_error, sizeof(ui_font_error))))
    fprintf(stderr, "[settings-menu] Unicode font unavailable; keeping native interface: %s\n",
            ui_font_error);
  ArHostFontResources_Retire(&s_font_resources, ui_primary);
  for (size_t i = 0; i < ui_fallback_count; ++i)
    ArHostFontResources_Retire(&s_font_resources, ui_fallbacks[i]);
}

bool HostLocalization_ExitRequested(void) {
  return s_exit_requested;
}

void HostLocalization_Shutdown(void) {
  ActRaiserRegionalMedia_ClearDonors();
  ArHostRegionalMediaFiles_Destroy(&s_regional_media);
  ActRaiserLocalizationRuntime_Shutdown();
  ActRaiserLocalizationRuntime_SetPresentationHost(NULL);
  ActRaiserLocalizationRuntime_SetPackHost(NULL);
  ActRaiserLocalizationRuntime_SetButtonPromptHost(NULL, NULL);
}

void HostLocalization_ReleaseFonts(void) {
  ArLocalizedTextPresenter_SetFontResources(NULL);
  if (!ArHostFontResources_Destroy(&s_font_resources))
    fprintf(stderr, "[localized-text] font resources still leased at shutdown\n");
}

void HostLocalization_ApplySetting(const SettingDesc *desc) {
  if (desc->category == kSettingCat_Localization)
    ActRaiserLocalizationRuntime_ApplySettings();
}
