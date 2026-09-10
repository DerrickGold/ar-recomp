#include "actraiser/actraiser_localization_schedule.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser/actraiser_localization_runtime.h"
#include "actraiser_game.h"
#include "localization/language_contract.h"
#include "save_system.h"
#include "settings.h"

uint8 g_ram[kActRaiserWramSize];
Settings g_settings;
static const char *s_installed_pack_path;
const char *Settings_LocalizationPackPath(int content) {
  if (content == 2) return s_installed_pack_path;
  return content == 1 ? getenv("AR_LOCALIZATION_PACK") : NULL;
}
static uint8_t s_rom[65536];
static unsigned s_failures, s_frames, s_confirms, s_resets, s_native_entries;
static unsigned s_seen_pages;
static unsigned s_polls;
static unsigned s_native_confirms;
static bool s_first_poll_held;
static bool s_prefix_seen, s_suffix_seen;
static bool s_check_cadence;
static unsigned s_cadence_frames;
static unsigned s_cadence_speed;
static void (*s_frame_hook)(void);
static void (*s_native_byte_hook)(CpuState *cpu, uint8_t code);
static void (*s_native_page_hook)(void);
static ArLocalizationFrame s_frame;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      ++s_failures;                                                            \
    }                                                                          \
  } while (0)

/* A complete USA-shaped source made only from synthetic prose and the public
 * semantic contract. Test runtime activation without requiring a retail dump
 * or weakening production's complete-native-pack validation. */
static void WriteNativeFixture(bool oversized) {
  FILE *file = fopen(AR_TEST_NATIVE_FIXTURE_DIR "/pack.ini", "wb");
  CHECK(file != NULL);
  if (!file)
    exit(1);
  fputs("[pack]\nformat = actraiser-language-pack\nversion = 1\n"
        "id = test.native-switching\nlocale = en-US\nname = Synthetic native\n"
        "autonym = Synthetic native\nauthor = Test suite\nlicense = MIT\n"
        "direction = ltr\ntarget = us-runtime\nsource_profile = us\n"
        "fallback = native-us\ncoverage = complete\n"
        "[fonts]\nprimary = builtin:actraiser-sans\n"
        "fallback = fixture-font.ttf\n"
        "[scripts]\nsource = main.artext\n",
        file);
  CHECK(fclose(file) == 0);
  /* The injected host below checks resolution, not actual font decoding. */
  file = fopen(AR_TEST_NATIVE_FIXTURE_DIR "/fixture-font.ttf", "wb");
  CHECK(file != NULL);
  if (!file) exit(1);
  fputs("synthetic font validated by test presentation host", file);
  CHECK(fclose(file) == 0);
  file = fopen(AR_TEST_NATIVE_FIXTURE_DIR "/main.artext", "wb");
  CHECK(file != NULL);
  if (!file)
    exit(1);
  for (uint32_t i = 0; i < ArLanguageContract_RouteCount(); ++i) {
    const char *id = ArLanguageContract_RouteId(i);
    if (!ArLanguageContract_RouteAvailable(id, kArLanguageSourceProfile_Us))
      continue;
    fprintf(file, ":: %s\n", id);
    const bool speed = !strcmp(id, "system.message_speed.choose");
    if (!speed)
      fputs("@empty\n", file);
    const uint32_t count =
        ArLanguageContract_RequiredAnchorCount(id, kArLanguageSourceProfile_Us);
    for (uint32_t ordinal = 0; ordinal < count; ++ordinal) {
      fprintf(file, "@anchor %s\n",
              ArLanguageContract_RequiredAnchor(id, kArLanguageSourceProfile_Us,
                                                ordinal));
      if (speed && ordinal == 0) {
        fputs("Brief page, {master_name}.\n@wait 7\n", file);
        if (oversized) {
          for (int line = 0; line < 512; ++line)
            fputs("Synthetic words exceeding the dialogue buffer.\n", file);
        }
      }
    }
    fputc('\n', file);
  }
  CHECK(fclose(file) == 0);
}

uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
  (void)cpu;
  return bank == 1 && address >= 0x8000
             ? s_rom[address]
             : g_ram[(bank == 0x7f ? 0x10000u : 0u) + address];
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 address) {
  return cpu_read8(cpu, bank, address) |
         ((uint16_t)cpu_read8(cpu, bank, (uint16_t)(address + 1u)) << 8);
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 address, uint8 value) {
  (void)cpu;
  g_ram[(bank == 0x7f ? 0x10000u : 0u) + address] = value;
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 address, uint16 value) {
  cpu_write8(cpu, bank, address, (uint8_t)value);
  cpu_write8(cpu, bank, (uint16_t)(address + 1u), (uint8_t)(value >> 8));
}
static void Capture(void) {
  ActRaiserLocalizationRuntime_CaptureFrame(&s_frame, 0x7800, 0, NULL, 0, NULL,
                                            0);
  if (strstr(s_frame.text, "Première"))
    s_seen_pages |= 1;
  if (strstr(s_frame.text, "Deuxième"))
    s_seen_pages |= 2;
  if (strstr(s_frame.text, "Dernière"))
    s_seen_pages |= 4;
  if (strstr(s_frame.text, "Avant"))
    s_prefix_seen = true;
  if (strstr(s_frame.text, "Après") && !strstr(s_frame.text, "Avant"))
    s_suffix_seen = true;
}

RecompReturn bank_01_9284_M1X0(CpuState *cpu) {
  CHECK(cpu->m_flag == 1 && cpu->x_flag == 0 && cpu->host_return_valid == 1);
  ++s_frames;
  if (s_frames > 1000) {
    CHECK(
        false); /* Bound regressions in release/press or authored wait loops. */
    exit(1);
  }
  Capture();
  if (s_check_cadence && s_frame.snapshot_count) {
    const ArLocalizationTextSnapshot *snapshot = &s_frame.snapshots[0];
    static const unsigned boundaries[] = {1, 5, 10, 12};
    CHECK(s_cadence_speed && s_cadence_frames < 4 * s_cadence_speed);
    if (s_cadence_speed && s_cadence_frames < 4 * s_cadence_speed)
      CHECK(snapshot->revealed_utf8_bytes == boundaries[s_cadence_frames / s_cadence_speed]);
    ++s_cadence_frames;
  }
  if (s_frame_hook)
    s_frame_hook();
  /* Prove architectural restoration without undoing RAM/animation effects. */
  cpu->A = 0xabcd;
  cpu->X = 0x4567;
  cpu->Y = 0x789a;
  cpu->P = 0x33;
  cpu->_flag_C = 1;
  cpu->_flag_N = 1;
  ++g_ram[0x1000];
  cpu->S += 2;
  return RECOMP_RETURN_NORMAL;
}
RecompReturn bank_01_8C43_M1X0(CpuState *cpu) {
  const RecompReturn result = bank_01_9284_M1X0(cpu);
  ++s_polls;
  if (s_first_poll_held && s_polls == 1) {
    cpu->A = 0x80; /* A held on entry is not a new confirmation. */
  } else {
    cpu->A = (s_polls - (s_first_poll_held ? 1u : 0u)) % 2 ? 0 : 0xc0;
    if (cpu->A)
      ++s_confirms;
  }
  return result;
}

/* Minimal ROM-free token driver. These are synthetic bytes, not a second
 * interpreter implementation. Real decoded-body/input parity is replayed too.
 */
static void ExpandedGlyph(CpuState *cpu) {
  const CpuState saved = *cpu;
  cpu->A = (cpu->A & 0xff00u) | g_ram[0x200];
  cpu_write8(cpu, 0, cpu->S--, 0x90);
  cpu_write8(cpu, 0, cpu->S--, 0x26);
  const uint8_t bank = cpu->PB, m = cpu->m_flag;
  cpu->PB = 2;
  CHECK(!ActRaiser_LocalizationScheduleGlyphDelay(cpu));
  cpu->PB = bank;
  cpu->m_flag = 0;
  CHECK(!ActRaiser_LocalizationScheduleGlyphDelay(cpu));
  cpu->m_flag = m;
  cpu_write8(cpu, 0, cpu->S + 1, 0x27);
  CHECK(!ActRaiser_LocalizationScheduleGlyphDelay(cpu));
  cpu_write8(cpu, 0, cpu->S + 1, 0x26);
  if (ActRaiser_LocalizationScheduleGlyphDelay(cpu)) {
    CHECK(ActRaiser_LocalizationGlyphDelay(cpu) == RECOMP_RETURN_NORMAL);
    CHECK(cpu->S == saved.S && cpu->X == saved.X && cpu->Y == saved.Y);
    CHECK(cpu->A == (saved.A & 0xff00u) && cpu->_flag_C && cpu->_flag_Z && !cpu->_flag_N);
  }
  *cpu = saved;
}

static RecompReturn NativeDialogue(CpuState *cpu) {
  CHECK(!ActRaiser_LocalizationScheduleEntry(cpu)); /* Re-entry guard. */
  ++s_native_entries;
  for (unsigned count = 0; count < 100; ++count) {
    cpu_push_jsr_return_frame(cpu);
    const CpuState before = *cpu;
    CHECK(ActRaiser_LocalizationScheduleByte(cpu));
    CHECK(!memcmp(cpu, &before, sizeof(before)));
    CHECK(ActRaiser_LocalizationReadTextByte(cpu) == RECOMP_RETURN_NORMAL);
    const uint8_t code = (uint8_t)cpu->A;
    if (s_native_byte_hook)
      s_native_byte_hook(cpu, code);
    if (code > 5 && code != ' ') ExpandedGlyph(cpu);
    if (code == 5)
      ++s_resets;
    else if (code == 2) {
      cpu->X = 0x60a;
      cpu_push_jsr_return_frame(cpu);
      if (ActRaiser_LocalizationScheduleContinuation(cpu)) {
        CHECK(ActRaiser_LocalizationContinueDialogue(cpu) ==
              RECOMP_RETURN_NORMAL);
      } else {
        if (s_native_page_hook)
          s_native_page_hook();
        ++s_confirms; /* Original native confirmation completes once. */
        cpu->S += 2;
        cpu->A &= 0xff00u;
        cpu->_flag_Z = 1;
        cpu->_flag_N = 0;
      }
      CHECK(cpu->S == before.S + 2 && cpu->X == 0x60a);
      CHECK(!(cpu->A & 0xff) && cpu->_flag_Z && !cpu->_flag_N);
    } else if (code == 1 || code == 0) {
      cpu->S += 2;
      return RECOMP_RETURN_NORMAL;
    }
  }
  CHECK(false); /* All synthetic scripts must terminate. */
  return RECOMP_RETURN_NORMAL;
}
static RecompReturn NativeContinuation(CpuState *cpu) {
  CHECK(!ActRaiser_LocalizationScheduleContinuation(cpu));
  ++s_native_confirms;
  ++s_confirms;
  cpu->A &= 0xff00u;
  cpu->_flag_Z = 1;
  cpu->_flag_N = 0;
  cpu->S += 2;
  return RECOMP_RETURN_NORMAL;
}
RecompReturn bank_01_9099_M1X0(CpuState *cpu) {
  return NativeContinuation(cpu);
}
RecompReturn bank_01_9099_M1X1(CpuState *cpu) {
  return NativeContinuation(cpu);
}
RecompReturn bank_01_8E29_M0X0(CpuState *cpu) { return NativeDialogue(cpu); }
RecompReturn bank_01_8E29_M0X1(CpuState *cpu) { return NativeDialogue(cpu); }
RecompReturn bank_01_8E29_M1X0(CpuState *cpu) { return NativeDialogue(cpu); }
RecompReturn bank_01_8E29_M1X1(CpuState *cpu) { return NativeDialogue(cpu); }
RecompReturn bank_01_8FC5_M1X0(CpuState *cpu) {
  CHECK(!ActRaiser_LocalizationScheduleByte(cpu));
  while (s_rom[cpu->Y] >= 0x80) {
    for (unsigned n = 0; n < (s_rom[cpu->Y] & 7u) + 2u; ++n)
      ExpandedGlyph(cpu);
    ++cpu->Y; /* Native internal dictionary loop. */
  }
  cpu->A = (cpu->A & 0xff00u) | s_rom[cpu->Y++];
  cpu->S += 2;
  return RECOMP_RETURN_NORMAL;
}

static void Run(uint16_t source, uint16_t caller, const uint8_t *bytes,
                size_t length, uint8_t speed) {
  memset(g_ram, 0, sizeof(g_ram));
  ActRaiserLocalizationText_ResetObservation();
  s_frames = s_confirms = s_resets = s_native_entries = s_seen_pages = 0;
  s_polls = 0;
  s_native_confirms = 0;
  s_prefix_seen = s_suffix_seen = false;
  g_ram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_SkyPalace;
  g_ram[0x200] = speed;
  memcpy(s_rom + source, bytes, length);
  CpuState cpu = {.A = 0x4567,
                  .X = 0x1234,
                  .Y = source,
                  .S = 0x1e0,
                  .PB = 1,
                  .DB = 1,
                  .P = 0x20,
                  .m_flag = 1,
                  .ram = g_ram};
  cpu_write16(&cpu, 0, cpu.S + 1, caller - 1);
  CHECK(ActRaiser_LocalizationScheduleEntry(&cpu));
  CHECK(ActRaiser_LocalizationRunDialogue(&cpu) == RECOMP_RETURN_NORMAL);
  CHECK(cpu.S == 0x1e2 && s_native_entries == 1 && s_resets == 1);
  ActRaiserLocalizationTextObservation observed = {.struct_size =
                                                       sizeof(observed)};
  CHECK(ActRaiserLocalizationText_CopyObservation(&observed));
  CHECK(observed.completed_control_count == 2 && !observed.control_pending);
  CHECK(observed.yielded_to_menu);
  Capture();
}

static void SelectPresentation(int presentation) {
  g_settings.localization_presentation = presentation;
  ActRaiserLocalizationRuntime_ApplySettings();
  CHECK(g_settings.localization_presentation == presentation);
  Capture();
}

static void DisableOnce(void) {
  s_frame_hook = NULL;
  SelectPresentation(0);
  CHECK(!ActRaiserLocalizationRuntime_DialogueScheduled());
}

static void RoundTripOnce(void) {
  s_frame_hook = NULL;
  const uint32_t count = s_frame.snapshot_count;
  SelectPresentation(0);
  CHECK(!s_frame.snapshot_count);
  SelectPresentation(1);
  CHECK(ActRaiserLocalizationRuntime_DialogueScheduled());
  CHECK(s_frame.snapshot_count == count);
}

static void EnableAfterGlyph(CpuState *cpu, uint8_t code) {
  (void)cpu;
  if (code <= 5)
    return;
  s_native_byte_hook = NULL;
  SelectPresentation(1);
  CHECK(ActRaiserLocalizationRuntime_DialogueScheduled());
}

static void EnableDuringNativePage(void) {
  s_native_page_hook = NULL;
  SelectPresentation(1);
  CHECK(ActRaiserLocalizationRuntime_DialogueScheduled());
}

static void DisableAtPage(void) {
  if (ActRaiserLocalizationRuntime_PageConfirmationPending())
    DisableOnce();
}

static void SelectContent(int content) {
  g_settings.localization_content = content;
  ActRaiserLocalizationRuntime_ApplySettings();
  CHECK(g_settings.localization_content == content);
  CHECK(ActRaiserLocalizationRuntime_DialogueScheduled());
  Capture();
}

static void ShortenAtPage(void) {
  if (!ActRaiserLocalizationRuntime_PageConfirmationPending())
    return;
  s_frame_hook = NULL;
  SelectContent(0);
  CHECK(!ActRaiserLocalizationRuntime_PageConfirmationPending());
  CHECK(strstr(s_frame.text, "Brief page,"));
}

static void ExtendOnce(void) {
  s_frame_hook = NULL;
  SelectContent(1);
}

static void StyleOnce(void) {
  s_frame_hook = NULL;
  const uint32_t before = s_frame.snapshot_count;
  g_settings.localization_font_scale_percent = 100;
  g_settings.localization_font_pixel_size = 0;
  ActRaiserLocalizationRuntime_ApplySettings();
  Capture();
  CHECK(s_frame.snapshot_count == before);
}

static bool s_font_available = true;
static unsigned s_font_preflights;
static bool PrepareFont(void *context, const ArTextPresentationFont *font,
                        char *error, size_t error_capacity) {
  CHECK(context == &s_font_available);
  CHECK(font && font->abi_version == AR_TEXT_PRESENTATION_ABI_VERSION &&
        font->struct_size == sizeof(*font) && font->stack_id[0] &&
        font->primary_path[0] && font->revision);
  ++s_font_preflights;
  if (font->fallback_count) {
    CHECK(font->fallback_count == 1);
    CHECK(!strcmp(font->fallback_paths[0],
                  AR_TEST_NATIVE_FIXTURE_DIR "/fixture-font.ttf"));
  }
  if (!s_font_available)
    snprintf(error, error_capacity, "injected unavailable font");
  return s_font_available;
}

static void RejectFontSwitchOnce(void) {
  s_frame_hook = NULL;
  s_font_available = false;
  g_settings.localization_content = 0;
  ActRaiserLocalizationRuntime_ApplySettings();
  CHECK(g_settings.localization_content == 1 &&
        g_settings.localization_presentation == 1 &&
        ActRaiserLocalizationRuntime_DialogueScheduled());
  s_font_available = true;
}

static void FailPresentationOnce(void) {
  s_frame_hook = NULL;
  const uint64_t ticket = s_frame.dialogue_ticket;
  CHECK(ticket && s_frame.dialogue_surface_id);
  ArTextPresentation_BeginFrame(ticket);
  ArTextPresentation_EndFrame(); /* Font was ready; actual frame failed. */
  ArTextPresentation_BeginFrame(ticket);
  ArTextPresentation_MarkReady(ticket);
  ArTextPresentation_EndFrame(); /* Re-present cannot erase a latched failure.
                                  */
  CHECK(!ActRaiserLocalizationRuntime_DialogueScheduled());
  CHECK(g_settings.localization_content == 1 &&
        g_settings.localization_presentation == 1);
  Capture();
  Capture();
  CHECK(!s_frame.snapshot_count && !s_frame.dialogue_ticket);
}

static void FailPresentationAtPage(void) {
  if (ActRaiserLocalizationRuntime_PageConfirmationPending())
    FailPresentationOnce();
}

static void StaleFailureOnce(void) {
  s_frame_hook = NULL;
  const uint64_t old_ticket = s_frame.dialogue_ticket;
  SelectContent(0);
  SelectContent(1);
  CHECK(s_frame.dialogue_ticket > old_ticket);
  ArTextPresentation_BeginFrame(old_ticket);
  ArTextPresentation_EndFrame();
  CHECK(ActRaiserLocalizationRuntime_DialogueScheduled());
}

static bool BeginNameDialogue(void) {
  CpuState cpu = {.S = 0x1e0, .PB = 1, .DB = 1, .Y = 0xfa7b, .ram = g_ram};
  cpu_write16(&cpu, 0, cpu.S + 1, 0x8afa);
  CHECK(!ActRaiser_LocalizationObserveTextEntry(&cpu));
  ActRaiserLocalizationTextObservation observation = {.struct_size =
                                                          sizeof(observation)};
  CHECK(ActRaiserLocalizationText_CopyObservation(&observation));
  return ActRaiserLocalizationRuntime_BeginDialogue(&observation);
}

static void TestUnicodeNameHandoff(void) {
  const char *native_path = AR_TEST_NATIVE_FIXTURE_DIR "/name.srm";
  const char *sidecar_path = AR_TEST_NATIVE_FIXTURE_DIR "/name.srm.arname";
  const char *ini_path = AR_TEST_NATIVE_FIXTURE_DIR "/name.ini";
  for (int native_at_finish = 0; native_at_finish < 2; ++native_at_finish) {
    remove(native_path);
    remove(sidecar_path);
    ActRaiserLocalizationRuntime_Shutdown();
    ActRaiserLocalizationText_ResetObservation();
    memset(g_ram, 0, sizeof(g_ram));
    g_ram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_SkyPalace;
    uint8_t live[kActRaiserSramSize] = {0};
    memcpy(live + 0x1439, "OLD", 3);
    Save_RecomputeChecksum(live);
    SaveError error = {{0}};
    CHECK(SaveSystem_Attach(live, sizeof(live), kSaveBackend_NativeSrm,
                            native_path, ini_path, &error));
    CHECK(SaveSystem_WriteActive(&error));
    g_settings.localization_content = 1;
    g_settings.localization_presentation = 1;
    CpuState keyboard = {
        .S = 0x1e0, .DB = 1, .Y = 0xef3b, .A = 0x0703, .ram = g_ram};
    CHECK(!ActRaiser_LocalizationObserveTextCompose(&keyboard));
    Capture();
    CHECK(strstr(s_frame.text, "Test keyboard"));
    g_ram[0x288] = 'A'; /* The native key at the enhanced É position. */
    g_ram[0x34d] = 1;
    Capture();
    CHECK(strstr(s_frame.text, "Test keyboard\nÉ"));
    char name[64];
    CHECK(!SaveSystem_CopyLocalizedPlayerName("A", name, sizeof(name)));
    if (native_at_finish)
      SelectPresentation(0);
    /* A redraw observed just before return may not yet have been captured. */
    CHECK(!ActRaiser_LocalizationObserveTextCompose(&keyboard));
    /* Acceptance must publish before the first post-keyboard frame, even
     * when the user has selected retail rendering before pressing finish. */
    CHECK(BeginNameDialogue() == !native_at_finish);
    CHECK(SaveSystem_CopyLocalizedPlayerName("A", name, sizeof(name)));
    CHECK(!strcmp(name, "É"));
    CHECK(!strcmp((char *)live + 0x1439, "OLD"));
    CHECK(SaveSystem_AutoPersistIfChanged(&error));
    g_settings.localization_content = 0;
    g_settings.localization_presentation = 1;
    CHECK(BeginNameDialogue());
    Capture();
    CHECK(strstr(s_frame.text, "Brief page, É."));
    CHECK(!strstr(s_frame.text, "Test keyboard"));
    /* Saving is independent of enhanced frame capture or font availability. */
    SelectPresentation(0);
    memcpy(live + 0x1439, "A\0\0\0", 4);
    Save_RecomputeChecksum(live);
    uint8_t saved[kActRaiserSramSize];
    memcpy(saved, live, sizeof(saved));
    CHECK(SaveSystem_AutoPersistIfChanged(&error));
    CHECK(!memcmp(saved, live, sizeof(saved)));
    ActRaiserLocalizationRuntime_Shutdown();
    ActRaiserLocalizationText_ResetObservation();
    memset(live, 0, sizeof(live));
    CHECK(SaveSystem_Attach(live, sizeof(live), kSaveBackend_NativeSrm,
                            native_path, ini_path, &error));
    CHECK(SaveSystem_LoadActive(&error));
    CHECK(!memcmp(saved, live, sizeof(saved)) && Save_ChecksumValid(live));
    g_settings.localization_presentation = 1;
    CHECK(BeginNameDialogue());
    Capture();
    CHECK(strstr(s_frame.text, "Brief page, É."));
    /* A different live game never inherits the last saved Unicode spelling. */
    memcpy(g_ram + 0x288, "NEW", 4);
    CHECK(BeginNameDialogue());
    Capture();
    CHECK(strstr(s_frame.text, "Brief page, NEW."));
    CHECK(!strstr(s_frame.text, "Brief page, É."));
    /* A new, entirely native-only name-entry run can choose the same fallback
     * letters as an old Unicode name. Acceptance replaces that old metadata. */
    ActRaiserLocalizationRuntime_Shutdown();
    ActRaiserLocalizationText_ResetObservation();
    g_settings.localization_presentation = 0;
    memcpy(g_ram + 0x288, "A\0\0", 4);
    const unsigned preflights = s_font_preflights;
    ActRaiserLocalizationTextObservation accepted = {
        .struct_size = sizeof(accepted),
        .abi_version = ACTRAISER_LOCALIZATION_TEXT_OBSERVATION_ABI_VERSION,
        .serial = 1,
        .source_pc24 = 0x048efc,
        .caller_pc24 = 0x0193b2,
        .context_pc24 = 0x0185ca,
        .map_number = kActRaiserNonActionMap_SkyPalace,
    };
    CHECK(!ActRaiserLocalizationRuntime_BeginDialogue(&accepted));
    CHECK(s_font_preflights == preflights);
    CHECK(SaveSystem_CopyLocalizedPlayerName("A", name, sizeof(name)));
    CHECK(!strcmp(name, "A"));
    CHECK(SaveSystem_AutoPersistIfChanged(&error));
    CHECK(SaveSystem_LoadActive(&error));
    CHECK(SaveSystem_CopyLocalizedPlayerName("A", name, sizeof(name)));
    CHECK(!strcmp(name, "A"));
  }
  remove(native_path);
  remove(sidecar_path);
  ActRaiserLocalizationRuntime_Shutdown();
}

int main(void) {
  WriteNativeFixture(false);
  g_settings.localization_presentation = 1;
  g_settings.localization_content = 1;
  g_settings.localization_font_scale_percent = 140;
  const uint8_t one_page[] = {5, 'a', 'b', 1};
  /* No presentation host: activation cannot introduce invisible waits/pages. */
  Run(0xfa7b, 0x8afb, one_page, sizeof(one_page), 0);
  CHECK(!s_confirms && !s_frames && !s_frame.snapshot_count);
  CHECK(g_settings.localization_presentation == 0 && !s_font_preflights);
  ArTextPresentationHost text_host = {
      .struct_size = sizeof(text_host),
      .abi_version = AR_TEXT_PRESENTATION_ABI_VERSION,
      .context = &s_font_available,
      .prepare_font = PrepareFont,
  };
  ++text_host.abi_version;
  ActRaiserLocalizationRuntime_SetPresentationHost(&text_host);
  g_settings.localization_presentation = 1;
  Run(0xfa7b, 0x8afb, one_page, sizeof(one_page), 0);
  CHECK(!s_confirms && !s_frames && !s_font_preflights);
  --text_host.abi_version;
  ActRaiserLocalizationRuntime_SetPresentationHost(&text_host);
  s_font_available = false;
  g_settings.localization_content = 1;
  g_settings.localization_presentation = 1;
  Run(0xfa7b, 0x8afb, one_page, sizeof(one_page), 0);
  CHECK(!s_confirms && !s_frames && !s_frame.snapshot_count);
  CHECK(g_settings.localization_presentation == 0 && s_font_preflights == 1);
  s_font_available = true;
  g_settings.localization_content = 1;
  g_settings.localization_presentation = 1;
  Run(0xfa7b, 0x8afb, one_page, sizeof(one_page), 0);
  CHECK(s_confirms == 2 && s_frames == 12 && s_seen_pages == 7);
  CHECK(strstr(s_frame.text, "Dernière") && !strstr(s_frame.text, "Première"));
  CHECK(s_font_preflights == 2); /* No probing on glyph/reveal/frame ticks. */
  s_frame_hook = RejectFontSwitchOnce;
  Run(0xfa7b, 0x8afb, one_page, sizeof(one_page), 0);
  CHECK(!s_frame_hook && s_confirms == 2 && s_frames == 12 &&
        s_seen_pages == 7);
  CHECK(s_font_preflights == 3);
  /* A newly selected installed pack is staged, not loaded over the active
   * working pack. Both malformed sources and font rejection retain it. */
  s_installed_pack_path = "/missing/community-pack/pack.ini";
  g_settings.localization_content = 2;
  ActRaiserLocalizationRuntime_ApplySettings();
  CHECK(g_settings.localization_content == 1);
  s_installed_pack_path = getenv("AR_LOCALIZATION_PACK");
  s_font_available = false;
  g_settings.localization_content = 2;
  ActRaiserLocalizationRuntime_ApplySettings();
  CHECK(g_settings.localization_content == 1);
  s_font_available = true;
  g_settings.localization_content = 2;
  ActRaiserLocalizationRuntime_ApplySettings();
  CHECK(g_settings.localization_content == 2);
  Run(0xfa7b, 0x8afb, one_page, sizeof(one_page), 0);
  CHECK(s_confirms == 2 && s_frames == 12 && s_seen_pages == 7);
  g_settings.localization_content = 1;
  ActRaiserLocalizationRuntime_ApplySettings();

  /* One visible grapheme per speed interval regardless of source compression,
   * source length, removed whitespace, combining accents or astral UTF-8. */
  const uint8_t cadence_sources[][12] = {
      {5, 'a', 'b', 'c', 'd', 'e', 'f', 1},
      {5, 0x80, 0x87, 1},
      {5, 'a', 1},
  };
  for (unsigned i = 0; i < 3; ++i) {
    const uint8_t speeds[] = {0, 1, 2, 4, 9};
    for (unsigned j = 0; j < sizeof(speeds) / sizeof(speeds[0]); ++j) {
      s_cadence_frames = 0;
      s_cadence_speed = speeds[j];
      s_check_cadence = true;
      Run(0xf854, 0x8794, cadence_sources[i], sizeof(cadence_sources[i]), speeds[j]);
      s_check_cadence = false;
      CHECK(s_cadence_frames == 4 * speeds[j] && s_frames == 4 * speeds[j] && s_confirms == 0);
    }
  }

  s_first_poll_held = true;
  Run(0xfa7b, 0x8afb, one_page, sizeof(one_page), 0);
  CHECK(s_confirms == 2 && s_frames == 13 && s_seen_pages == 7);
  s_first_poll_held = false;

  Run(0xfa7b, 0x8afb, one_page, sizeof(one_page), 1);
  CHECK(s_confirms == 2 && s_frames > 10 && s_seen_pages == 7);
  CHECK(strstr(s_frame.text, "Première") && strstr(s_frame.text, "Dernière"));

  const uint8_t dictionary_pages[] = {5, 0x80, 0x81, 2, 0x82, 1};
  Run(0xfa7b, 0x8afb, dictionary_pages, sizeof(dictionary_pages), 0);
  CHECK(s_confirms == 2 && s_frames == 12 && s_seen_pages == 7);

  s_frame_hook = FailPresentationOnce;
  Run(0xfa7b, 0x8afb, one_page, sizeof(one_page), 0);
  CHECK(!s_frame_hook && !s_confirms && s_frames == 1);
  CHECK(!s_frame.snapshot_count);
  s_frame_hook = FailPresentationAtPage;
  Run(0xfa7b, 0x8afb, one_page, sizeof(one_page), 0);
  CHECK(!s_frame_hook && !s_confirms && s_frames == 4);
  s_frame_hook = FailPresentationAtPage;
  Run(0xfa7b, 0x8afb, dictionary_pages, sizeof(dictionary_pages), 0);
  CHECK(!s_frame_hook && s_native_confirms == 1 && s_confirms == 1);
  s_frame_hook = StaleFailureOnce;
  Run(0xfa7b, 0x8afb, one_page, sizeof(one_page), 0);
  CHECK(!s_frame_hook && s_confirms == 2 && s_frames == 12 && s_seen_pages == 7);

  const uint8_t three_pages[] = {5, 'a', 2, 'b', 2, 'c', 1};
  Run(0xf99b, 0x8aa0, three_pages, sizeof(three_pages), 1);
  CHECK(s_confirms == 0 && s_frames == 0);
  CHECK(s_frame.snapshot_count == 1 && s_frame.snapshots[0].utf8_bytes == 0);

  Run(0xf812, 0x8740, one_page, sizeof(one_page), 0);
  CHECK(s_confirms == 0 && s_frames == 6);
  CHECK(s_prefix_seen && s_suffix_seen);
  CHECK(strstr(s_frame.text, "Après") && !strstr(s_frame.text, "Avant"));

  /* A same-pause round trip cannot replay waits or lose authored progress. */
  s_frame_hook = RoundTripOnce;
  Run(0xfa7b, 0x8afb, one_page, sizeof(one_page), 0);
  CHECK(!s_frame_hook && s_confirms == 2 && s_frames == 12 &&
        s_seen_pages == 7);

  s_frame_hook = DisableOnce;
  Run(0xfa7b, 0x8afb, one_page, sizeof(one_page), 0);
  CHECK(!s_frame_hook && !s_confirms && s_frames == 1);
  CHECK(!s_frame.snapshot_count);
  SelectPresentation(1);

  /* Cancel an added-only confirmation without waiting for a fake button. */
  s_frame_hook = DisableAtPage;
  Run(0xfa7b, 0x8afb, one_page, sizeof(one_page), 0);
  CHECK(!s_frame_hook && !s_confirms && s_frames == 4);
  CHECK(!s_frame.snapshot_count);
  SelectPresentation(1);
  CHECK(s_frame.snapshot_count == 1);

  /* Turning retail on inside a real native $02 must delegate the original
   * wait, not treat the setting change as its confirmation. */
  s_frame_hook = DisableAtPage;
  Run(0xfa7b, 0x8afb, dictionary_pages, sizeof(dictionary_pages), 0);
  CHECK(!s_frame_hook && s_native_confirms == 1 && s_confirms == 1);
  SelectPresentation(1);

  s_frame_hook = ShortenAtPage;
  Run(0xfa7b, 0x8afb, one_page, sizeof(one_page), 0);
  CHECK(!s_frame_hook && !s_confirms && s_frames == 4);
  CHECK(strstr(s_frame.text, "Brief page,"));
  CHECK(s_frame.fallback_font_count == 1 &&
        !strcmp(s_frame.fallback_font_paths[0],
                AR_TEST_NATIVE_FIXTURE_DIR "/fixture-font.ttf"));

  s_frame_hook = ExtendOnce;
  Run(0xfa7b, 0x8afb, one_page, sizeof(one_page), 0);
  CHECK(!s_frame_hook && s_confirms == 2 && s_seen_pages == 7);
  CHECK(s_frames == 16); /* Keep the old 7-frame wait; do not restart at 3. */

  s_frame_hook = StyleOnce;
  Run(0xfa7b, 0x8afb, one_page, sizeof(one_page), 0);
  CHECK(!s_frame_hook && s_confirms == 2 && s_frames == 12);

  /* Activation before any rendered enhanced frame adopts current ROM state. */
  ActRaiserLocalizationRuntime_Shutdown();
  g_settings.localization_presentation = 0;
  s_native_byte_hook = EnableAfterGlyph;
  Run(0xfa7b, 0x8afb, one_page, sizeof(one_page), 0);
  CHECK(!s_native_byte_hook && s_confirms == 2 && s_seen_pages == 7);

  ActRaiserLocalizationRuntime_Shutdown();
  g_settings.localization_presentation = 0;
  s_native_page_hook = EnableDuringNativePage;
  Run(0xfa7b, 0x8afb, dictionary_pages, sizeof(dictionary_pages), 0);
  CHECK(!s_native_page_hook && s_confirms == 2);
  CHECK(strstr(s_frame.text, "Dernière"));

  ActRaiserLocalizationRuntime_Shutdown();
  g_settings.localization_presentation = 0;
  CpuState native = {.Y = 0xf812, .S = 0x1e0, .DB = 1, .PB = 1, .m_flag = 1};
  const CpuState before = native;
  CHECK(ActRaiser_LocalizationScheduleEntry(&native));
  CHECK(!ActRaiser_LocalizationScheduleContinuation(&native));
  CHECK(!memcmp(&native, &before, sizeof(native)));
  const unsigned prior_frames = s_frames, prior_confirms = s_confirms;
  CHECK(ActRaiser_LocalizationRunDialogue(&native) == RECOMP_RETURN_NORMAL);
  CHECK(native.S == before.S + 2 && native.Y == before.Y + sizeof(one_page));
  CHECK(s_frames == prior_frames && s_confirms == prior_confirms);
  ActRaiserLocalizationRuntime_Shutdown();
  WriteNativeFixture(true);
  g_settings.localization_content = 0;
  g_settings.localization_presentation = 1;
  Run(0xfa7b, 0x8afb, one_page, sizeof(one_page), 0);
  CHECK(!s_confirms && !s_frames && !s_frame.snapshot_count);
  CHECK(!ActRaiserLocalizationRuntime_DialogueScheduled());
  Capture();
  CHECK(!s_frame.snapshot_count); /* No retry/resurrection on capture. */
  ActRaiserLocalizationRuntime_Shutdown();
  WriteNativeFixture(false);
  TestUnicodeNameHandoff();
  return s_failures ? 1 : 0;
}
