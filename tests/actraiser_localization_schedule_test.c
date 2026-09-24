#include "actraiser/actraiser_localization_schedule.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "actraiser/actraiser_localization_runtime.h"
#include "actraiser/actraiser_hud.h"
#include "actraiser/actraiser_localization_compose_state.h"
#include "actraiser/actraiser_localization_text_normalize.h"
#include "actraiser_game.h"
#include "localization/language_contract.h"
#include "save_system.h"
#include "settings.h"
#include "actraiser/regional/actraiser_regional_runtime.h"

static ArRegionalSource s_price_source = kArRegionalSource_US;
bool ActRaiserRegional_CopyPrices(ArRegionalCostSnapshot *prices) {
  ArRegionalCostPolicy baseline;
  return ArRegionalCosts_Init(&baseline,s_price_source) &&
      ArRegionalCosts_Resolve(&baseline,prices);
}

uint8 g_ram[kActRaiserWramSize];
Settings g_settings;
/* Supply committed identity; native producer/upload timing has its own suite. */
static int s_credits_page = -1;
int ActRaiserCredits_PresentedPage(void) { return s_credits_page; }
void ActRaiserCredits_ObserveClear(void) {}
static bool s_menu_skip, s_menu_describing, s_menu_aborted;
bool ActRaiserSimMenu_SkipDialogue(const CpuState *cpu) { (void)cpu; return s_menu_skip; }
bool ActRaiserSimMenu_DescriptionAborted(void) { return s_menu_aborted; }
bool ActRaiserSimMenu_Describing(void) { return s_menu_describing; }
bool ActRaiserSimMenu_FastReveal(void) { return false; }
void ActRaiserSimMenu_DescriptionWait(void) {}
void ActRaiserSimMenu_BeginDialogue(const CpuState *cpu) { (void)cpu; }
void ActRaiserSimMenu_ClearDialogue(void) {}
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
static ArLanguagePackIo s_file_pack_io;
static unsigned s_pack_reads, s_pack_releases;

static bool ReadPack(void *context, const char *path, size_t maximum_bytes,
                     ArLanguagePackBlob *blob, char *error,
                     size_t error_capacity) {
  (void)context;
  const bool loaded = s_file_pack_io.read_file(
      s_file_pack_io.context, path, maximum_bytes, blob, error, error_capacity);
  if (loaded) ++s_pack_reads;
  return loaded;
}

static void ReleasePack(void *context, ArLanguagePackBlob *blob) {
  (void)context;
  ++s_pack_releases;
  s_file_pack_io.release_file(s_file_pack_io.context, blob);
}

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
    if (!strcmp(id, "title.save_choice.labels"))
      fputs("Continue fixture\n@line\nNew fixture\n", file);
    else if (!strcmp(id, "title.copyright"))
      fputs("Footer first\n@line\nFooter second\n@line\nFooter third\n", file);
    else if (!strcmp(id, "city.fillmore.name"))
      fputs("Town fixture\n", file);
    else if (!strcmp(id, "action.hud.pause"))
      fputs("Pause fixture\n", file);
    else if (!strcmp(id, "credits.page_01"))
      fputs("- Credits fixture -\n@line\nA contributor\n", file);
    else if (!strncmp(id, "sim.menu.", 9))
      fputs("Menu label fixture\n", file);
    else if (!strcmp(id, "system.choice.yes_no"))
      fputs("Yes\n@line\nNo\n", file);
    else if (!speed)
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
static void CaptureWithTransform(bool mode7_transformed) {
  const uint16_t palette[] = {0, 0, 0x7f33, 0x7fff};
  ActRaiserLocalizationRuntime_CaptureFrame(&s_frame, 0x7800, 0, NULL, 0, palette,
                                            4, mode7_transformed);
  for (uint8_t i = 0; i < s_frame.snapshot_count; ++i) {
    const ArLocalizationTextSnapshot *s = &s_frame.snapshots[i];
    if (s_frame.dialogue_ticket && s->surface_id==s_frame.dialogue_surface_id)
      CHECK(s->layout==kArLocalizationTextLayout_DialogueWindow);
    CHECK(s->bidi_span_offset + s->bidi_span_count <= s_frame.bidi.count);
    CHECK(ArTextBidiSpans_Valid(s_frame.bidi.spans + s->bidi_span_offset,
        s->bidi_span_count, s_frame.text + s->utf8_offset, s->utf8_bytes, 0));
    CHECK(s_frame.snapshots[i].shadow_enabled);
    CHECK(s_frame.snapshots[i].shadow_rgb == 0);
    CHECK(s_frame.snapshots[i].band_rgb == 0x9cceff);
    CHECK(s_frame.snapshots[i].body_rgb == 0xffffff);
  }
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

static void Capture(void) { CaptureWithTransform(false); }

RecompReturn bank_01_9278_M1X0(CpuState *cpu) {
  CHECK(!ActRaiser_LocalizationScheduleGlyphDelay(cpu));
  cpu->A &= 0xff00u;
  cpu->_flag_C = cpu->_flag_Z = 1;
  cpu->_flag_N = 0;
  cpu->P = (cpu->P & ~0x83u) | 3u;
  cpu->S += 2;
  return RECOMP_RETURN_NORMAL;
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
static unsigned s_last_font, s_live_fonts;
static unsigned s_discarded_preflights;
static void DiscardPreparedFont(void *context) {
  CHECK(context == &s_font_available);
  ++s_discarded_preflights;
}
static bool s_registered_fonts[4096], s_fallback_fonts[4096];
static ArFontResourceId RegisterFont(void *context, const char *manifest,
                                     const char *member, char *error, size_t capacity) {
  (void)error; (void)capacity;
  CHECK(context == &s_font_available && manifest && manifest[0] && member && member[0]);
  CHECK(s_last_font + 1 < 4096);
  if (s_last_font + 1 >= 4096) return 0;
  const unsigned id = ++s_last_font;
  s_registered_fonts[id] = true;
  s_fallback_fonts[id] = !strcmp(member, "fixture-font.ttf");
  ++s_live_fonts;
  return id;
}
static void RetireFont(void *context, ArFontResourceId id) {
  CHECK(context == &s_font_available && id < 4096);
  if (id >= 4096) return;
  CHECK(s_registered_fonts[id]);
  if (s_registered_fonts[id]) --s_live_fonts;
  s_registered_fonts[id] = false;
}
static bool PrepareFont(void *context, const ArTextPresentationFont *font,
                        char *error, size_t error_capacity) {
  CHECK(context == &s_font_available);
  CHECK(font && font->abi_version == AR_TEXT_PRESENTATION_ABI_VERSION &&
        font->struct_size == sizeof(*font) && font->stack_id[0] &&
        font->primary && font->revision);
  ++s_font_preflights;
  if (font->fallback_count) {
    CHECK(font->fallback_count == 1);
    CHECK(font->fallbacks[0] < 4096 && s_fallback_fonts[font->fallbacks[0]]);
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

static bool FrameHasText(const char *text) {
  for (uint8_t i = 0; i < s_frame.snapshot_count; ++i) {
    const char *actual = ArLocalizationFrame_GetText(&s_frame, i, NULL);
    if (actual && !strcmp(actual, text)) return true;
  }
  return false;
}

static void TestTitleTransformHandoff(void) {
  ActRaiserLocalizationRuntime_Shutdown();
  ActRaiserLocalizationText_ResetObservation();
  memset(g_ram, 0, sizeof(g_ram));
  g_settings.localization_content = 0;
  g_settings.localization_presentation = 1;
  CpuState title = {.S = 0x1e0, .DB = 2, .Y = 0xa9a7, .A = 0x1100,
                    .ram = g_ram};
  CHECK(!ActRaiser_LocalizationObserveTextCompose(&title));
  Capture();
  CHECK(FrameHasText("Continue fixture"));
  CHECK(FrameHasText("New fixture"));
  CHECK(s_frame.cells.count == 2); /* Separate fixed cells for the two choices. */
  CpuState copyright = title;
  copyright.Y = 0xa9de;
  copyright.A = 0x1700;
  CHECK(!ActRaiser_LocalizationObserveTextCompose(&copyright));
  Capture();
  CHECK(FrameHasText("Footer first\nFooter second\nFooter third"));
  const ArTextCellRecord *footer = ArTextCellRecordSet_Find(
      &s_frame.cells, kActRaiserLocalizationTitleCopyrightSurface);
  CHECK(footer && footer->region.row == 23 && footer->region.rows == 5);
  CHECK(footer && s_frame.snapshots[footer->snapshot_slot].layout ==
                      kArLocalizationTextLayout_CenteredBlock);
  CHECK(FrameHasText("Continue fixture") && FrameHasText("New fixture"));
  CpuState professional = title;
  professional.Y = 0xaa60;
  professional.A = 0x110c;
  CHECK(!ActRaiser_LocalizationObserveTextCompose(&professional));
  Capture();
  CHECK(ArTextCellRecordSet_Find(&s_frame.cells,
      kActRaiserLocalizationTitleSelectorSurface));
  CaptureWithTransform(true);
  CHECK(s_frame.cells.count == 0 && s_frame.snapshot_count == 0);
  Capture();
  CHECK(s_frame.cells.count == 0); /* Identity alone cannot resurrect old text. */

  /* A native redraw queued on the same frame as the spin must also retire,
   * rather than recreating a replacement after the transform check. */
  CHECK(!ActRaiser_LocalizationObserveTextCompose(&title));
  CHECK(!ActRaiser_LocalizationObserveTextCompose(&copyright));
  CaptureWithTransform(true);
  CHECK(s_frame.cells.count == 0 && s_frame.snapshot_count == 0);

  /* A transformed world-map background does not transform its flat city HUD. */
  g_ram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_SkyPalace;
  CpuState city = {.S = 0x1e0, .DB = 1, .Y = 0xf1cb, .A = 0x0106,
                   .ram = g_ram};
  CHECK(!ActRaiser_LocalizationObserveTextCompose(&city));
  CaptureWithTransform(true);
  CHECK(FrameHasText("Town fixture"));
  CHECK(ArTextCellRecordSet_Find(&s_frame.cells, 4));
  CHECK(!ArTextCellRecordSet_Find(&s_frame.cells,
      kActRaiserLocalizationTitleTextSurface));
  CaptureWithTransform(true);
  CHECK(FrameHasText("Town fixture"));

  /* Reentering the title with a new native compose restores both choices. */
  g_ram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Title;
  CHECK(!ActRaiser_LocalizationObserveTextCompose(&title));
  Capture();
  CHECK(FrameHasText("Continue fixture"));
  CHECK(FrameHasText("New fixture"));
  CHECK(!ArTextCellRecordSet_Find(&s_frame.cells, 4));
  ActRaiserLocalizationRuntime_Shutdown();
  ActRaiserLocalizationText_ResetObservation();
}

static void TestSimulationPause(void) {
  ActRaiserLocalizationRuntime_Shutdown();
  ActRaiserLocalizationText_ResetObservation();
  memset(g_ram, 0, sizeof(g_ram));
  /* A synthetic five-cell source lets the real erase observer calculate the
   * resume footprint. The native string is never the translated font input. */
  memcpy(g_ram + 0xa8ef, "ABCDE", 6);
  CpuState pause = {.S = 0x1e0, .DB = 0, .Y = 0xa8ef, .A = 0x0b0d, .ram = g_ram};
  for (uint8_t town = kActRaiserSimulationTown_First;
       town <= kActRaiserSimulationTown_Last; ++town) {
    g_ram[kActRaiserWram_CurrentMap] = town;
    g_settings.localization_content = 1;
    g_settings.localization_presentation = 1;
    CHECK(!ActRaiser_LocalizationObserveTextCompose(&pause));
    Capture();
    CHECK(FrameHasText("En pause"));
    const ArTextCellRecord *owner = ArTextCellRecordSet_Find(&s_frame.cells, 13);
    CHECK(owner && owner->destination.background == 3 && owner->region.row == 11);
    CHECK(!ActRaiserLocalizationRuntime_DialogueScheduled());
    /* Existing packs supply the same entry for both modes; changing content
     * or fonts during a native pause must refresh without a new compose. */
    g_settings.localization_content = 0;
    ActRaiserLocalizationRuntime_ApplySettings();
    Capture();
    CHECK(FrameHasText("Pause fixture"));
    g_settings.localization_presentation = 0;
    ActRaiserLocalizationRuntime_ApplySettings();
    Capture();
    CHECK(!ArTextCellRecordSet_Find(&s_frame.cells, 13));
    g_settings.localization_content = 1;
    g_settings.localization_presentation = 1;
    ActRaiserLocalizationRuntime_ApplySettings();
    Capture();
    CHECK(FrameHasText("En pause"));
    CHECK(!ActRaiser_LocalizationObserveTextErase(&pause));
    Capture();
    CHECK(!ArTextCellRecordSet_Find(&s_frame.cells, 13));
    CHECK(!FrameHasText("En pause"));
    g_settings.localization_content = 0;
    ActRaiserLocalizationRuntime_ApplySettings();
    Capture();
    CHECK(!FrameHasText("Pause fixture"));
  }
  ActRaiserLocalizationRuntime_Shutdown();
  ActRaiserLocalizationText_ResetObservation();
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
    CHECK(s_frame.snapshot_count > 0);
    const uint64_t empty_keyboard_revision = s_frame.snapshots[0].source_revision;
    CHECK(s_frame.snapshots[0].live_line_cells == 8);
    /* Optional CPU benchmark of the real native-compose/capture path. */
    if (!native_at_finish && getenv("AR_TEST_NAME_ENTRY_BENCH")) {
      const clock_t started = clock();
      for (unsigned move = 0; move < 2000; ++move) {
        g_ram[0x34b] = (uint8_t)(1 + move % 11);
        CHECK(!ActRaiser_LocalizationObserveTextCompose(&keyboard));
        Capture();
      }
      fprintf(stderr, "name-entry: 2000 cursor updates took %.2f ms CPU\n",
              1000.0 * (clock() - started) / CLOCKS_PER_SEC);
      g_ram[0x34b] = 0;
      CHECK(!ActRaiser_LocalizationObserveTextCompose(&keyboard));
      Capture();
    }
    /* Page transitions invalidate the lookup, and returning to page one
     * restores its Unicode key rather than retaining the other alphabet. */
    g_ram[0x34b] = 12;
    CHECK(!ActRaiser_LocalizationObserveTextCompose(&keyboard));
    Capture();
    CHECK(strstr(s_frame.text, "Second keyboard") && strstr(s_frame.text, "< 2/2 >"));
    CHECK(s_frame.snapshots[0].live_line_cells == 8);
    g_ram[0x288] = 'M';
    g_ram[0x34d] = 1;
    Capture();
    CHECK(strstr(s_frame.text, "Second keyboard\nΨ"));
    g_ram[0x288] = 0;
    g_ram[0x34d] = 0;
    Capture();
    g_ram[0x34b] = 0;
    CHECK(!ActRaiser_LocalizationObserveTextCompose(&keyboard));
    Capture();
    CHECK(strstr(s_frame.text, "Test keyboard") && strstr(s_frame.text, "< 1/2 >"));
    CHECK(s_frame.snapshots[0].source_revision == empty_keyboard_revision);
    g_ram[0x288] = 'A'; /* The native key at the enhanced É position. */
    g_ram[0x34d] = 1;
    Capture();
    CHECK(strstr(s_frame.text, "Test keyboard\nÉ"));
    CHECK(s_frame.snapshots[0].source_revision == empty_keyboard_revision);
    CHECK(s_frame.snapshots[0].live_line_cells == 8);

    /* Moving the native selector must not reraster the keyboard: the arrow is
     * a separately drawn native object, so the text keeps its identity and
     * only the cursor object moves. */
    CHECK(s_frame.snapshot_count > 0);
    const uint64_t keyboard_revision = s_frame.snapshots[0].source_revision;
    uint32_t cursor_before = 0;
    for (uint8_t i = 0; i < s_frame.inline_object_count; ++i)
      if (s_frame.inline_objects[i].kind ==
          kArLocalizationInlineObject_NameCursor)
        cursor_before = s_frame.inline_objects[i].end_utf8_byte;
    CHECK(cursor_before != 0);
    g_ram[0x34b] = (uint8_t)(g_ram[0x34b] + 1u);
    CHECK(!ActRaiser_LocalizationObserveTextCompose(&keyboard));
    Capture();
    CHECK(s_frame.snapshot_count > 0 &&
          s_frame.snapshots[0].source_revision == keyboard_revision);
    uint32_t cursor_after = cursor_before;
    for (uint8_t i = 0; i < s_frame.inline_object_count; ++i)
      if (s_frame.inline_objects[i].kind ==
          kArLocalizationInlineObject_NameCursor)
        cursor_after = s_frame.inline_objects[i].end_utf8_byte;
    CHECK(cursor_after != cursor_before);
    g_ram[0x34b] = (uint8_t)(g_ram[0x34b] - 1u);
    CHECK(!ActRaiser_LocalizationObserveTextCompose(&keyboard));
    Capture();
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

static void TestStructuredNormalization(void) {
  static const char source[] = "\n  É|li\nse  |  left \n \n B|C  \n";
  uint8_t source_bits[AR_TEXT_BOUNDARY_BYTES(sizeof(source))] = {0};
  for (size_t i = 0; i < sizeof(source) - 1; ++i)
    if (source[i] == '\n' || source[i] == '|')
      ArTextBoundary_Set(source_bits, i, true);
  ArTextBoundary_Set(source_bits, strstr(source, "É|li") - source + 2, false);
  ArTextBoundary_Set(source_bits, strstr(source, "li\nse") - source + 2, false);
  ArTextBoundary_Set(source_bits, strstr(source, "B|C") - source + 1, false);
  const size_t preferred_source = (size_t)(strstr(source, "  left") - source);
  ArTextBoundary_Set(source_bits, preferred_source, true);
  char output[80];
  uint8_t output_bits[AR_TEXT_BOUNDARY_BYTES(sizeof(output))];
  memset(output_bits, 0xff, sizeof(output_bits));
  size_t bytes = 0;
  uint8_t objects = 0;
  uint16_t offsets[sizeof(source)];
  CHECK(ActRaiserLocalizationText_NormalizeStructured(
      source, sizeof(source) - 1, NULL, 0, true, output, sizeof(output), &bytes,
      NULL, 0, &objects, offsets, source_bits, output_bits));
  CHECK(!strcmp(output, "É|li se | left\n\nB|C"));
  const uint32_t name_start = (uint32_t)(strstr(source,"É|li")-source);
  const uint32_t tail_start = (uint32_t)(strstr(source,"B|C")-source);
  const ArTextBidiSpan values[] = {{name_start,name_start+(uint32_t)strlen("É|li\nse"),kArTextDirection_Auto},
      {tail_start,tail_start+3,kArTextDirection_LeftToRight}};
  ArTextBidiSpans mapped = {0};
  CHECK(ActRaiserLocalizationText_MapBidiSpans(values,2,0,sizeof(source)-1,
      offsets,output,bytes,20,&mapped));
  CHECK(mapped.count == 2 && mapped.spans[0].start == 20);
  CHECK(mapped.spans[0].end == 20+strlen("É|li se"));
  CHECK(mapped.spans[1].end == 20+bytes);
  ArTextBidiSpans_Edit(&mapped,20,0,3);
  CHECK(mapped.spans[0].start == 23 && mapped.spans[1].end == 23+bytes);
  ArTextBidiSpans_Edit(&mapped,23,3,0);
  CHECK(mapped.spans[0].start == 23 && mapped.spans[0].end == 20+strlen("É|li se"));
  const size_t delimiter = strstr(output, " | ") - output + 1;
  const size_t preferred = delimiter + 1u;
  for (size_t i = 0; i < bytes; ++i)
    CHECK(ArTextBoundary_Get(output_bits, i) ==
          (i == delimiter || i == preferred || output[i] == '\n'));
  CHECK(!objects);
  CHECK(ActRaiserLocalizationText_NormalizeStructured(
      "", 0, NULL, 0, true, output, sizeof(output), &bytes,
      NULL, 0, &objects, NULL, source_bits, output_bits));
  CHECK(!bytes && !output[0]);
  for (size_t i = 0; i < sizeof(output_bits); ++i) CHECK(!output_bits[i]);
}

static void TestCreditsWithoutDialogueObservation(void) {
  ActRaiserLocalizationRuntime_Shutdown();
  ActRaiserLocalizationText_ResetObservation();
  memset(g_ram,0,sizeof(g_ram));
  g_settings.localization_content=0;
  g_settings.localization_presentation=1;
  g_ram[kActRaiserWram_MapGroup]=8;
  g_ram[kActRaiserWram_CurrentMap]=1;
  static uint16_t vram[0x8000],cgram[16]={0,0x7fff,0,0,0,0x25f};
  for (unsigned page=0;page<20;++page) for (unsigned i=0;i<1024;++i) {
    const uint16_t word=i==13*32+10 ? 0x420+page : 0x10;
    g_ram[0x4000+page*0x800+i*2]=word;
    g_ram[0x4000+page*0x800+i*2+1]=word>>8;
    if (page==1) vram[0x3800+i]=word;
  }
  uint8_t before[kActRaiserWramSize];memcpy(before,g_ram,sizeof(before));
  s_credits_page=-1;
  ActRaiserLocalizationRuntime_CaptureFrame(&s_frame,0x3800,0x5000,vram,0x8000,cgram,16,false);
  CHECK(!s_frame.snapshot_count); // Matching tiles alone cannot identify credits.
  s_credits_page=1;
  ActRaiserLocalizationRuntime_CaptureFrame(&s_frame,0x3800,0x5000,vram,0x8000,cgram,16,false);
  CHECK(s_frame.snapshot_count==1 && strstr(s_frame.text,"Credits fixture"));
  CHECK(!memcmp(before,g_ram,sizeof(before)));
  g_settings.localization_presentation=0;
  ActRaiserLocalizationRuntime_CaptureFrame(&s_frame,0x3800,0x5000,vram,0x8000,cgram,16,false);
  CHECK(!s_frame.snapshot_count);
  g_settings.localization_presentation=1;
  ActRaiserLocalizationRuntime_CaptureFrame(&s_frame,0x3800,0x5000,vram,0x8000,cgram,16,false);
  CHECK(s_frame.snapshot_count==1 && strstr(s_frame.text,"Credits fixture"));
  g_ram[kActRaiserWram_MapGroup]=0;
  ActRaiserLocalizationRuntime_CaptureFrame(&s_frame,0x3800,0x5000,vram,0x8000,cgram,16,false);
  CHECK(!s_frame.snapshot_count);
  s_credits_page=-1;
}

static void TestHudSceneParity(void) {
  /* A real V2 partial translation, including native HUD inks, an alternate
   * font role, scaling and italic values. Every SIM scene shares these IDs. */
  FILE *file = fopen(AR_TEST_NATIVE_FIXTURE_DIR "/hud.ini", "wb");
  CHECK(file);
  if (!file) return;
  fputs("[pack]\nformat = actraiser-language-pack\nversion = 2\n"
        "id = test.hud-parity\nlocale = fr-CA\nname = HUD fixture\n"
        "autonym = HUD fixture\nauthor = Test suite\nlicense = MIT\n"
        "direction = ltr\ntarget = us-runtime\nsource_profile = us\n"
        "fallback = native-us\ncoverage = partial\n"
        "[fonts]\nprimary = builtin:actraiser-sans\n"
        "[font.hud]\nprimary = builtin:actraiser-sans\n"
        "[scripts]\nsource = hud.artext\n", file);
  CHECK(fclose(file) == 0);
  file = fopen(AR_TEST_NATIVE_FIXTURE_DIR "/hud.artext", "wb");
  CHECK(file);
  if (!file) return;
  fputs("@define-style hud band=native:hud.band body=native:hud.body "
        "shadow=native:hud.shadow\n"
        ":: sim_sky.hud.context_label\n@style hud\n@font hud\nRégion\n"
        ":: sim_sky.hud.angel_label\n@style hud\nAnge\n"
        ":: sim_sky.hud.sp_label\n@style hud\nPS\n"
        ":: sim_sky.hud.population_value\n@style hud\n@scale 120%\n"
        "<i>{hud_value}</i>\n"
        ":: sim_sky.hud.sp_value\n@style hud\n<i>{hud_value}</i>\n", file);
  CHECK(fclose(file) == 0);
  ActRaiserLocalizationRuntime_Shutdown();
  ActRaiserLocalizationText_ResetObservation();
  ActRaiserHud_Reset();
  memset(g_ram, 0, sizeof(g_ram));
  s_installed_pack_path = AR_TEST_NATIVE_FIXTURE_DIR "/hud.ini";
  g_settings.localization_content = 2;
  g_settings.localization_presentation = 1;
  static uint16_t vram[0x8000], cgram[32];
  memset(vram, 0, sizeof(vram));
  cgram[5] = 1; cgram[6] = 0x001f; cgram[7] = 0x03e0;
  const uint16_t context[] = {34, 91, 92, 93, 94, 39};
  for (unsigned i = 0; i < 6; ++i) {
    vram[0x5800 + 32 + i] = 0x2400 | context[i];
    vram[0x5800 + 64 + i] = 0x2400 | (21 + i);
    vram[0x5800 + 96 + i] = 0x2400 | (15 + i); /* Stale ENEMY is unowned. */
  }
  vram[0x5800 + 64 + 21] = 0x2453;
  vram[0x5800 + 64 + 22] = 0x2450;
  for (unsigned i = 0; i < 8; ++i) {
    vram[0x5800 + 32 + 23 + i] = 0x2400 | "123/0456"[i];
    vram[0x5800 + 64 + 23 + i] = 0x2400 | " 012/099"[i];
  }
  g_ram[0x19] = 8;
  ActRaiserLocalizationRuntime_CaptureFrame(
      &s_frame, 0x5800, 0, vram, 0x8000, cgram, 32, false);
  CHECK(!s_frame.snapshot_count); /* Tiles do not establish ownership. */
  for (unsigned map = 1; map <= 8; ++map) {
    g_ram[0x19] = map;
    CpuState cpu = {.PB = 2, .m_flag = 1, .S = 0x1e0};
    CHECK(!ActRaiser_LocalizationObserveHudTemplate(&cpu));
    ActRaiserHud_ObserveUpload();
    ActRaiserLocalizationRuntime_CaptureFrame(
        &s_frame, 0x5800, 0, vram, 0x8000, cgram, 32, false);
    CHECK(s_frame.snapshot_count == 5);
    CHECK(FrameHasText("Région") && FrameHasText("Ange") && FrameHasText("PS"));
    CHECK(FrameHasText("123/0456") && FrameHasText("012/099"));
    CHECK(!strcmp(s_frame.snapshots[0].appearance.font_role, "hud"));
    CHECK(s_frame.snapshots[3].appearance.scale_basis == 12000);
    CHECK(s_frame.appearance_span_count == 2);
    for (unsigned i = 0; i < s_frame.appearance_span_count; ++i)
      CHECK(s_frame.appearance_spans[i].appearance.italic);
    for (unsigned i = 0; i < s_frame.snapshot_count; ++i) {
      CHECK(s_frame.snapshots[i].has_appearance);
      CHECK(s_frame.snapshots[i].appearance.body_rgb == 0x00ff00);
      CHECK(s_frame.snapshots[i].appearance.band_rgb == 0xff0000);
      CHECK(!strcmp(s_frame.snapshots[i].language.locale, "fr-CA"));
    }
  }
  /* Temple remains owned through menu clears and native/enhanced toggles. */
  CpuState clear = {.PB = 1, .S = 0x1e0};
  CHECK(!ActRaiser_LocalizationObserveMenuClear(&clear));
  for (unsigned enabled = 0; enabled <= 1; ++enabled) {
    g_settings.localization_presentation = enabled;
    ActRaiserLocalizationRuntime_CaptureFrame(
        &s_frame, 0x5800, 0, vram, 0x8000, cgram, 32, false);
    CHECK(s_frame.snapshot_count == (enabled ? 5 : 0));
    CHECK(ActRaiserHud_Presented(0, 8).kind == kActRaiserHud_Simulation);
  }
  vram[0x5800 + 64 + 21] = 0; /* Unexpected overwrite loses only SP claims. */
  ActRaiserLocalizationRuntime_CaptureFrame(
      &s_frame, 0x5800, 0, vram, 0x8000, cgram, 32, false);
  CHECK(s_frame.snapshot_count == 3 && FrameHasText("123/0456"));
  vram[0x5800 + 64 + 21] = 0x2453;
  for (unsigned map = 9; map <= 10; ++map) {
    g_ram[0x19] = map == 9 ? 9 : 0; /* World and title keep no stale claims. */
    ActRaiserLocalizationRuntime_CaptureFrame(
        &s_frame, 0x5800, 0, vram, 0x8000, cgram, 32, false);
    CHECK(!s_frame.snapshot_count);
  }
  ActRaiserHud_Reset();
  ActRaiserLocalizationRuntime_Shutdown();
  ActRaiserLocalizationText_ResetObservation();
  s_installed_pack_path = NULL;
}

static void TestPartialRtlSources(void) {
  const char *path = AR_TEST_NATIVE_FIXTURE_DIR "/partial.ini";
  FILE *file = fopen(path, "wb");
  CHECK(file);
  if (!file) return;
  fputs("[pack]\nformat = actraiser-language-pack\nversion = 1\n"
        "id = test.partial-rtl\nlocale = ar\nname = Synthetic RTL\n"
        "autonym = اختبار\nauthor = Test suite\nlicense = MIT\n"
        "direction = rtl\ntarget = us-runtime\nsource_profile = us\n"
        "fallback = native-us\ncoverage = partial\n"
        "[fonts]\nprimary = builtin:actraiser-sans\n"
        "[scripts]\nsource = partial.artext\n", file);
  CHECK(fclose(file) == 0);
  file = fopen(AR_TEST_NATIVE_FIXTURE_DIR "/partial.artext", "wb");
  CHECK(file);
  if (!file) return;
  fputs(":: city.fillmore.name\nمدينة\n", file);
  CHECK(fclose(file) == 0);

  ActRaiserLocalizationRuntime_Shutdown();
  ActRaiserLocalizationText_ResetObservation();
  memset(g_ram, 0, sizeof(g_ram));
  s_installed_pack_path = path;
  g_settings.localization_content = 2;
  g_settings.localization_presentation = 1;
  CpuState title = {.S = 0x1e0, .DB = 2, .Y = 0xa9a7, .A = 0x1100, .ram = g_ram};
  CHECK(!ActRaiser_LocalizationObserveTextCompose(&title));
  Capture();
  CHECK(FrameHasText("Continue fixture") && FrameHasText("New fixture"));
  CHECK(!strcmp(s_frame.locale, "ar"));
  CHECK(s_frame.snapshot_count == 2);
  for (uint8_t i = 0; i < s_frame.snapshot_count; ++i) {
    CHECK(!strcmp(s_frame.snapshots[i].language.locale, "en-US"));
    CHECK(s_frame.snapshots[i].language.direction == kArTextDirection_LeftToRight);
  }
  g_ram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_SkyPalace;
  CpuState city = {.S = 0x1e0, .DB = 1, .Y = 0xf1cb, .A = 0x0106, .ram = g_ram};
  CHECK(!ActRaiser_LocalizationObserveTextCompose(&city));
  Capture();
  CHECK(FrameHasText("مدينة") && s_frame.snapshot_count == 1);
  CHECK(!strcmp(s_frame.snapshots[0].language.locale, "ar"));
  CHECK(s_frame.snapshots[0].language.direction == kArTextDirection_RightToLeft);
  /* A translated and fallback surface coexist; font selection is shared but
   * their paragraph bases and shaping locales are not. */
  const uint8_t one_page[] = {5, 'a', 'b', 1};
  Run(0xfa7b, 0x8afb, one_page, sizeof(one_page), 0);
  bool dialogue = false;
  CHECK(!strcmp(s_frame.locale, "ar"));
  for (uint8_t i = 0; i < s_frame.snapshot_count; ++i) {
    if (s_frame.snapshots[i].layout != kArLocalizationTextLayout_DialogueWindow) continue;
    dialogue = true;
    CHECK(!strcmp(s_frame.snapshots[i].language.locale, "en-US"));
    CHECK(s_frame.snapshots[i].language.direction == kArTextDirection_LeftToRight);
  }
  CHECK(dialogue);
  ActRaiserLocalizationRuntime_Shutdown();
  ActRaiserLocalizationText_ResetObservation();
  s_installed_pack_path = NULL;
}

static unsigned s_legacy_notices;
static bool s_legacy_native;
static void LegacyNotice(void *context, const char *manifest,
                         const ArLanguagePackMetadata *metadata, bool native) {
  CHECK(context == &s_legacy_notices && manifest &&
        metadata->format_version == 1);
  ++s_legacy_notices;
  s_legacy_native = native;
}

static void TestLegacySelectionGate(ActRaiserLocalizationPackHost host) {
  ActRaiserLocalizationRuntime_Shutdown();
  ActRaiserLocalizationText_ResetObservation();
  host.require_v2 = true;
  host.context = &s_legacy_notices;
  host.legacy_pack = LegacyNotice;
  ActRaiserLocalizationRuntime_SetPackHost(&host);
  const unsigned reads = s_pack_reads, fonts = s_font_preflights;
  g_settings.localization_content = 1;
  g_settings.localization_presentation = 0;
  ActRaiserLocalizationRuntime_ApplySettings();
  CHECK(s_pack_reads == reads && !s_legacy_notices);
  g_settings.localization_presentation = 1;
  ActRaiserLocalizationRuntime_ApplySettings();
  CHECK(s_legacy_notices == 1 && !s_legacy_native);
  CHECK(s_pack_reads == reads + 1); /* Manifest only, no v1 script or font. */
  CHECK(g_settings.localization_content == 1 &&
        g_settings.localization_presentation == 1);
  for (unsigned i = 0; i < 3; ++i) {
    Capture();
    CHECK(!s_frame.snapshot_count &&
          !ActRaiserLocalizationRuntime_DialogueScheduled());
  }
  CHECK(s_pack_reads == reads + 1 && s_legacy_notices == 1 &&
        s_font_preflights == fonts);
  g_settings.localization_content = 0;
  ActRaiserLocalizationRuntime_ApplySettings();
  CHECK(s_legacy_notices == 2 && s_legacy_native);
  CHECK(g_settings.localization_content == 0 &&
        g_settings.localization_presentation == 1);
  /* Choosing Native and then attempting enhanced again is a new attempt. */
  g_settings.localization_presentation = 0;
  ActRaiserLocalizationRuntime_ApplySettings();
  g_settings.localization_presentation = 1;
  ActRaiserLocalizationRuntime_ApplySettings();
  CHECK(s_legacy_notices == 3 && s_legacy_native);
}

static void TestMenuLabelCapacity(void) {
  static ArLocalizationFrame source, before, labels;
  Capture();
  source=s_frame;
  CHECK(source.font_revision);
  /* Reproduce a native frame with no remaining snapshots: modern inventory
   * rows must not lose their chosen font, nor displace dialogue or HUD text. */
  while (source.snapshot_count<kArTextCellRecordCapacity) {
    if (!ArLocalizationFrame_AddScreenText(&source,900+source.snapshot_count,
          0,0,8,8,"x",1,1,1,1,kArTextDirection_LeftToRight,8,
          kArLocalizationTextLayout_SingleLineLabel)) {
      CHECK(false); return;
    }
  }
  before=source;
  SimMenuModel menu={.phase=kSimMenu_Inventory,.category=3,.submenu=true,
      .item_count=8,.items={19,18,13,11,10,9,8,7},.item_slot=7};
  const uint16_t palette[]={0,0,0x7f33,0x7fff};
  for (unsigned describe=0;describe<2;++describe) {
    if (describe) {
      menu.phase=kSimMenu_Describe; menu.return_phase=kSimMenu_Inventory;
    }
    ActRaiserLocalizationRuntime_CaptureMenuLabels(
        &labels,&source,&menu,palette,4);
    CHECK(labels.snapshot_count==15);
    for (unsigned id=600;id<=615;++id) {
      if (id==609) continue;
      const ArLocalizationScreenTextRecord *record=
          ArLocalizationFrame_FindScreenText(&labels,id);
      CHECK(record && labels.snapshots[record->snapshot_slot].utf8_bytes);
    }
    CHECK(labels.font_revision==source.font_revision &&
          labels.primary_font==source.primary_font &&
          labels.fallback_font_count==source.fallback_font_count);
    CHECK(!memcmp(&labels.settings,&source.settings,sizeof(labels.settings)));
    CHECK(!memcmp(&source,&before,sizeof(source)));
  }
  menu.phase=kSimMenu_Confirm; menu.category=2; menu.row[2]=2;
  ActRaiserLocalizationRuntime_CaptureMenuLabels(&labels,&source,&menu,palette,4);
  CHECK(ArLocalizationFrame_FindScreenText(&labels,603)); /* Sun header */
  CHECK(ArLocalizationFrame_FindScreenText(&labels,609)); /* Yes/No measurement */
  CHECK(!ArLocalizationFrame_FindScreenText(&labels,608)); /* no stale inventory */
  CHECK(!memcmp(&source,&before,sizeof(source)));
}

static void TestMenuHelp(void) {
  for (unsigned item = 7; item <= 8; ++item) {
    ArDialogueSession session;
    ArDialogueSession_Init(&session);
    const unsigned prior_frames = s_frames, prior_confirms = s_confirms;
    CHECK(ActRaiserLocalizationRuntime_BeginMenuHelp(&session,
        item == 7 ? "sim.help.item.07" : "sim.help.item.08", SimMenuHelp_Item(item)));
    ArDialoguePageSnapshot page;
    CHECK(ArDialogueSession_GetPage(&session, &page));
    CHECK(page.page_count == (item == 7 ? 1 : 2));
    unsigned windows = 0;
    for (;;) {
      size_t start = 0;
      do {
        SimMenuHelpPage help;
        CHECK(SimMenuHelp_Build(&help, page.utf8, page.utf8_bytes, start,
                               page.page_index + 1 < page.page_count));
        CHECK(help.source_end > start);
        CHECK(ActRaiserLocalizationRuntime_PrepareMenuHelpStyle(&page, &help));
        help.authored_page=page.page_index;
        help.revealed_glyphs=help.glyph_count;
        snprintf(help.locale,sizeof(help.locale),"%s",page.locale);
        help.direction=kArTextDirection_LeftToRight;
        /* A zero-based first page must survive publication as screen text.
         * The former zero revision / Flow layout silently discarded it. */
        Capture();
        const uint16_t palette[]={0,0,0x7f33,0x7fff};
        ActRaiserLocalizationRuntime_AppendMenuHelp(&s_frame,&help,palette,4);
        const ArLocalizationScreenTextRecord *record=
            ArLocalizationFrame_FindScreenText(&s_frame,700);
        CHECK(record != NULL);
        if (record) {
          const ArLocalizationTextSnapshot *snapshot=&s_frame.snapshots[record->snapshot_slot];
          CHECK(snapshot->source_revision != 0);
          CHECK(snapshot->layout == kArLocalizationTextLayout_DialogueWindow);
          CHECK(snapshot->utf8_bytes == help.bytes);
          CHECK(snapshot->revealed_cluster_count == snapshot->cluster_count);
          CHECK(snapshot->revealed_utf8_bytes == snapshot->utf8_bytes);
          CHECK(!memcmp(s_frame.text+snapshot->utf8_offset,help.text,help.bytes));
        }
        start = help.source_end; ++windows;
      } while (start < page.utf8_bytes);
      while (!session.state.awaiting_page_advance && !session.state.terminal) {
        ArDialogueToken token; ArLanguagePackError error;
        CHECK(ArDialogueSession_Next(&session, &token, &error));
        CHECK(token.kind != kArDialogueToken_Control);
        ArDialogueSession_TickWait(&session, session.state.wait_frames_remaining);
      }
      if (!ArDialogueSession_AdvancePage(&session)) break;
      CHECK(ArDialogueSession_GetPage(&session, &page));
    }
    CHECK(windows > 1);
    CHECK(s_frames == prior_frames && s_confirms == prior_confirms);
    ArDialogueSession_Destroy(&session);
  }
}

static void TestMenuDialogueKinds(void) {
  /* Menu placement must not turn a scheduled question or acknowledgement into
   * a fitted label. Every kind supports authored pages/waits before returning
   * to its original selector or caller, including a miracle's read-only Help. */
  static const struct {uint16_t source,caller; bool selector,describe;} cases[]={
    {0xfedc,0x836c,false,true},  /* Sun description. */
    {0xff57,0x8385,true,false},  /* Sun Yes/No question. */
    {0xf9ba,0x8abe,true,false},  /* Progress Log continue question. */
    {0xf9e6,0x8af1,false,false}, /* Progress Log acknowledgement. */
    {0xfaee,0x8254,false,false}, /* Direct the People follow-up. */
    {0xf98f,0x84f8,false,false}, /* Offering inventory cancellation. */
  };
  ActRaiserLocalizationRuntime_Shutdown();
  g_settings.localization_presentation=1;
  g_settings.localization_content=1;
  for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
    memset(g_ram,0,sizeof(g_ram));
    g_ram[kActRaiserWram_CurrentMap]=1; /* Fillmore simulation, not title. */
    ActRaiserLocalizationText_ResetObservation();
    s_frames=s_confirms=s_polls=s_native_entries=s_resets=0;
    s_menu_describing=cases[i].describe;
    const uint8_t script[]={5,'a',cases[i].selector?1:0};
    memcpy(s_rom+cases[i].source,script,sizeof(script));
    CpuState cpu={.Y=cases[i].source,.S=0x1e0,.PB=1,.DB=1,
                  .P=0x20,.m_flag=1,.ram=g_ram};
    cpu_write16(&cpu,0,cpu.S+1,cases[i].caller-1);
    CHECK(ActRaiser_LocalizationScheduleEntry(&cpu));
    CHECK(ActRaiser_LocalizationRunDialogue(&cpu)==RECOMP_RETURN_NORMAL);
    CHECK(cpu.S==0x1e2 && s_native_entries==1 && s_resets==1);
    CHECK(s_confirms==1); /* The added page needs its own acknowledgement. */
    Capture();
    CHECK(s_frame.dialogue_ticket && s_frame.dialogue_surface_id);
    bool found=false;
    for (uint8_t n=0;n<s_frame.snapshot_count;++n) {
      const ArLocalizationTextSnapshot *snapshot=&s_frame.snapshots[n];
      if (snapshot->surface_id!=s_frame.dialogue_surface_id) continue;
      found=true;
      CHECK(snapshot->layout==kArLocalizationTextLayout_DialogueWindow);
      CHECK(snapshot->revealed_utf8_bytes==snapshot->utf8_bytes);
      CHECK(strstr(s_frame.text+snapshot->utf8_offset,"Menu dialogue final page."));
    }
    CHECK(found);
  }
  s_menu_describing=false;
  ActRaiserLocalizationRuntime_Shutdown();
}

static void TestNativeMenuContinuation(void) {
  g_settings.localization_presentation = 0;
  s_frames = s_polls = s_confirms = s_native_entries = 0;
  s_menu_skip = true;
  const uint8_t text[] = {5, 'a', 2, 'b', 2, 'c', 1};
  memcpy(s_rom + 0xfa7b, text, sizeof(text));
  CpuState c = {.A = 0xab00, .Y = 0xfa7b, .X = 0x60a, .S = 0x1e0,
               .PB = 1, .DB = 1, .P = 0x20, .m_flag = 1, .ram = g_ram};
  cpu_write16(&c, 0, c.S + 1, 0x8afa);
  CHECK(ActRaiser_LocalizationScheduleEntry(&c));
  CHECK(ActRaiser_LocalizationRunDialogue(&c) == RECOMP_RETURN_NORMAL);
  CHECK(c.S == 0x1e2 && s_native_entries == 1 && !s_polls && !s_confirms);
  s_menu_skip = false;
  s_menu_describing = true;
  s_first_poll_held = true;
  c.S = 0x1e0; c.X = 0x60a; c.Y = 0x9876; c.A = 0xab5f;
  CHECK(ActRaiser_LocalizationScheduleContinuation(&c));
  CHECK(ActRaiser_LocalizationContinueDialogue(&c) == RECOMP_RETURN_NORMAL);
  CHECK(s_polls == 3 && s_confirms == 1); /* held, release, fresh press */
  CHECK(c.S == 0x1e2 && c.X == 0x60a && c.Y == 0x9876 && c.A == 0xab00);
  CHECK(c._flag_Z && !c._flag_N && !cpu_read8(&c, 0x7f, 0xb60a));
  s_menu_aborted = true; c.S = 0x1e0;
  CHECK(ActRaiser_LocalizationContinueDialogue(&c) == RECOMP_RETURN_NORMAL);
  CHECK(s_polls == 3 && c.S == 0x1e2); /* cancellation never waits again */
  s_menu_aborted = s_menu_describing = s_first_poll_held = false;
}

static void TestNativePriceDelay(void) {
  CpuState cpu = {.PB=1, .DB=1, .S=0x1e0, .m_flag=1, .X=2, .Y=0xfcd7, .A=3};
  cpu_write16(&cpu, 0, cpu.S+1, 0x9026);
  s_rom[0xfcd6]='0';
  cpu_write8(&cpu, 0x7f, 0xb000, '0');
  s_price_source=kArRegionalSource_Japan;
  const CpuState before=cpu;
  CHECK(ActRaiser_LocalizationScheduleGlyphDelay(&cpu));
  CHECK(!memcmp(&cpu,&before,sizeof(cpu)));
  CHECK(cpu_read8(&cpu,0x7f,0xb000)=='0'); /* predicate doesn't write */
  CHECK(ActRaiser_LocalizationGlyphDelay(&cpu)==RECOMP_RETURN_NORMAL);
  CHECK(cpu_read8(&cpu,0x7f,0xb000)=='2');
  CHECK(cpu.S==0x1e2 && cpu.A==0 && cpu._flag_C && cpu._flag_Z);
  s_price_source=kArRegionalSource_US;
}

int main(void) {
  TestNativePriceDelay();
  TestStructuredNormalization();
  WriteNativeFixture(false);
  ArLanguagePackFileIo_Init(&s_file_pack_io);
  const ArLanguagePackIo pack_io = {
      .struct_size = sizeof(pack_io),
      .abi_version = AR_LANGUAGE_PACK_IO_ABI_VERSION,
      .read_file = ReadPack,
      .release_file = ReleasePack,
  };
  const ActRaiserLocalizationPackHost pack_host = {
      .struct_size = sizeof(pack_host),
      .abi_version = ACTRAISER_LOCALIZATION_PACK_HOST_ABI_VERSION,
      .io = pack_io,
      .native_manifest = getenv("AR_LOCALIZATION_NATIVE_PACK"),
  };
  ActRaiserLocalizationRuntime_SetPackHost(&pack_host);
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
      .register_font = RegisterFont,
      .retire_font = RetireFont,
      .discard_prepared_font = DiscardPreparedFont,
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
  TestMenuHelp();
  TestMenuLabelCapacity();
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
        s_frame.fallback_fonts[0] < 4096 && s_fallback_fonts[s_frame.fallback_fonts[0]]);

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
  TestTitleTransformHandoff();
  TestSimulationPause();
  TestUnicodeNameHandoff();
  TestCreditsWithoutDialogueObservation();
  TestHudSceneParity();
  TestPartialRtlSources();
  TestMenuDialogueKinds();
  TestLegacySelectionGate(pack_host);
  ActRaiserLocalizationRuntime_Shutdown();
  TestNativeMenuContinuation();
  ActRaiserLocalizationRuntime_SetPackHost(NULL);
  CHECK(s_pack_reads > 0 && s_pack_reads == s_pack_releases);
  CHECK(!s_live_fonts);
  CHECK(s_discarded_preflights > 0);
  return s_failures ? 1 : 0;
}
