#include "actraiser/actraiser_localization_schedule.h"
#include "actraiser/actraiser_sim_menu.h"
#include "actraiser/actraiser_miracle_text.h"
#include "actraiser/actraiser_regional_runtime.h"

#include <stdio.h>
#include <stdlib.h>

/* The wrapper re-enters the generated function once, with the original JSR
 * frame. Suppress only its entry hook, not byte/control observations inside it.
 * This is a game-fiber guard, not a process-global renderer toggle. */
static bool s_entering_native;
static bool s_entering_reader;
static bool s_entering_continuation;
static bool s_skipping_menu_text;
static bool s_native_glyph_delay;

extern RecompReturn bank_01_8E29_M0X0(CpuState *cpu);
extern RecompReturn bank_01_8E29_M0X1(CpuState *cpu);
extern RecompReturn bank_01_8E29_M1X0(CpuState *cpu);
extern RecompReturn bank_01_8E29_M1X1(CpuState *cpu);
extern RecompReturn bank_01_8FC5_M1X0(CpuState *cpu);
extern RecompReturn bank_01_9284_M1X0(CpuState *cpu);
extern RecompReturn bank_01_9278_M1X0(CpuState *cpu);
extern RecompReturn bank_01_8C43_M1X0(CpuState *cpu);
extern RecompReturn bank_01_9099_M1X0(CpuState *cpu);
extern RecompReturn bank_01_9099_M1X1(CpuState *cpu);

typedef RecompReturn (*NativeRoutine)(CpuState *cpu);

/* Only architectural registers are restored: native RAM/PPU writes, timing,
 * input and animation work must survive. Do not copy/restore all of CpuState.
 */
typedef struct SavedRegisters {
  uint16_t a, x, y, s, d;
  uint8_t db, pb, p, m, index, emulation, host_return;
  uint8_t n, v, z, c, i, decimal;
} SavedRegisters;

static bool CallPresentationRoutine(CpuState *cpu, NativeRoutine routine,
                                    uint8_t *result_a) {
  const SavedRegisters saved = {
      cpu->A,       cpu->X,       cpu->Y,         cpu->S,
      cpu->D,       cpu->DB,      cpu->PB,        cpu->P,
      cpu->m_flag,  cpu->x_flag,  cpu->emulation, cpu->host_return_valid,
      cpu->_flag_N, cpu->_flag_V, cpu->_flag_Z,   cpu->_flag_C,
      cpu->_flag_I, cpu->_flag_D};
  /* Match a native M=1/X=0 JSR from the reader's continuation. The generated
   * RTS consumes this frame; the host wrapper supplies the continuation. */
  cpu->PB = 1;
  cpu->P = (uint8_t)((cpu->P | 0x20u) & ~0x10u);
  cpu->m_flag = 1;
  cpu->x_flag = 0;
  cpu_write8(cpu, 0, cpu->S--, 0x8e);
  cpu_write8(cpu, 0, cpu->S--, 0x56);
  cpu->host_return_valid = 1;
  const RecompReturn result = routine(cpu);
  const bool balanced = cpu->S == saved.s;
  /* These two audited leaves have only normal RTS exits. A nonlocal return
   * or an unbalanced stack is an execution fault, not a missing translation:
   * restoring registers and continuing would silently swallow native control
   * flow. Stop with evidence instead of pretending native fallback is safe. */
  if (result != RECOMP_RETURN_NORMAL || !balanced) {
    fprintf(stderr,
            "[localization] native presentation call failed: "
            "return=%d stack=$%04X expected=$%04X\n",
            (int)result, (unsigned)cpu->S, (unsigned)saved.s);
    abort();
  }
  if (result_a)
    *result_a = (uint8_t)cpu->A;
  cpu->A = saved.a;
  cpu->X = saved.x;
  cpu->Y = saved.y;
  cpu->S = saved.s;
  cpu->D = saved.d;
  cpu->DB = saved.db;
  cpu->PB = saved.pb;
  cpu->P = saved.p;
  cpu->m_flag = saved.m;
  cpu->x_flag = saved.index;
  cpu->emulation = saved.emulation;
  cpu->host_return_valid = saved.host_return;
  cpu->_flag_N = saved.n;
  cpu->_flag_V = saved.v;
  cpu->_flag_Z = saved.z;
  cpu->_flag_C = saved.c;
  cpu->_flag_I = saved.i;
  cpu->_flag_D = saved.decimal;
  return true;
}

static bool WaitFrame(void *context) {
  return CallPresentationRoutine(context, bank_01_9284_M1X0, NULL);
}

static bool ConfirmPage(void *context) {
  ActRaiserSimMenu_DescriptionWait();
  /* $9261 is exactly two $8C43 / BIT #$C0 loops: release, then press.
   * Keep its native per-frame work, but return between polls if the authored
   * page no longer exists. Never cancel a native gameplay/menu input loop. */
  bool released = false;
  while (ActRaiserLocalizationRuntime_PageConfirmationPending()) {
    uint8_t buttons = 0;
    if (!CallPresentationRoutine(context, bank_01_8C43_M1X0, &buttons))
      return false;
    if (!ActRaiserLocalizationRuntime_PageConfirmationPending())
      return true;
    if (released && (buttons & 0xc0u)) {
      ActRaiserSimMenu_DescriptionWait();
      return true;
    }
    if (!(buttons & 0xc0u))
      released = true;
  }
  return true;
}

static ActRaiserLocalizationDialogueHost Host(CpuState *cpu) {
  return (ActRaiserLocalizationDialogueHost){cpu, WaitFrame, ConfirmPage};
}

static bool EnhancedGlyphDelay(void) {
  return s_skipping_menu_text || ActRaiserSimMenu_DescriptionAborted() ||
      ActRaiserSimMenu_FastReveal() || ActRaiserLocalizationRuntime_DialogueScheduled();
}

bool ActRaiser_LocalizationScheduleGlyphDelay(CpuState *cpu) {
  if (s_native_glyph_delay) return false;
  return cpu && cpu->PB == 1 && cpu->m_flag == 1 &&
      cpu_read16(cpu, 0, (uint16_t)(cpu->S + 1u)) == 0x9026 &&
      (EnhancedGlyphDelay() ||
       (cpu->DB == 1 && cpu->Y >= 0xfcd6 && cpu->Y <= 0xff15));
}

RecompReturn ActRaiser_LocalizationGlyphDelay(CpuState *cpu) {
  /* The predicate is read-only. Native text takes its original delay body
   * after adapting the verified price digit; enhanced text keeps its own clock. */
  ArRegionalCostSnapshot prices;
  if (ActRaiserRegional_CopyPrices(&prices)) ActRaiserMiracle_UpdateNativeDigit(cpu, &prices);
  if (!EnhancedGlyphDelay()) {
    s_native_glyph_delay = true;
    const RecompReturn result = bank_01_9278_M1X0(cpu);
    s_native_glyph_delay = false;
    return result;
  }
  const ActRaiserLocalizationDialogueHost host = Host(cpu);
  if (!s_skipping_menu_text && !ActRaiserSimMenu_DescriptionAborted())
    ActRaiserLocalizationRuntime_RevealGlyph((uint8_t)cpu->A, &host);
  /* $9278 CMP #0 / JSR $9284 / DEC A loop: A.low=0, C=Z=1,
   * N=0, high accumulator and all other registers preserved; consume RTS.
   * Each authored glyph uses $9284 itself, so neither menu animation nor
   * native word-expansion callbacks can introduce a second reveal clock. */
  cpu->A &= 0xff00u;
  cpu->_flag_C = cpu->_flag_Z = 1;
  cpu->_flag_N = 0;
  cpu->P = (uint8_t)((cpu->P & ~0x83u) | 0x03u);
  cpu->S = (uint16_t)(cpu->S + 2u);
  return RECOMP_RETURN_NORMAL;
}

bool ActRaiser_LocalizationScheduleEntry(CpuState *cpu) {
  if (s_entering_native) {
    s_entering_native = false;
    return false;
  }
  ActRaiserSimMenu_BeginDialogue(cpu);
  (void)ActRaiser_LocalizationObserveTextEntry(cpu);
  ActRaiserLocalizationTextObservation observation = {.struct_size =
                                                          sizeof(observation)};
  if (cpu && !ActRaiserSimMenu_SkipDialogue(cpu) &&
      ActRaiserLocalizationText_CopyObservation(&observation))
    (void)ActRaiserLocalizationRuntime_BeginDialogue(&observation);
  /* The read-only return acknowledgement also matters in native mode. */
  return cpu != NULL;
}

bool ActRaiser_LocalizationScheduleMenuClear(CpuState *cpu) {
  if (cpu) ActRaiserSimMenu_ClearDialogue();
  return ActRaiser_LocalizationObserveMenuClear(cpu);
}

RecompReturn ActRaiser_LocalizationRunDialogue(CpuState *cpu) {
  static NativeRoutine const routines[] = {bank_01_8E29_M0X0, bank_01_8E29_M0X1,
                                           bank_01_8E29_M1X0,
                                           bank_01_8E29_M1X1};
  s_skipping_menu_text = ActRaiserSimMenu_SkipDialogue(cpu);
  if (s_skipping_menu_text) ActRaiserLocalizationRuntime_ReturnDialogue();
  s_entering_native = true;
  const RecompReturn result =
      routines[((cpu->m_flag & 1u) << 1) | (cpu->x_flag & 1u)](cpu);
  s_entering_native = false;
  s_skipping_menu_text = false;
  if (result == RECOMP_RETURN_NORMAL) {
    ActRaiserLocalizationText_ObserveReturn();
    ActRaiserLocalizationRuntime_ReturnDialogue();
  }
  return result;
}

bool ActRaiser_LocalizationSkipMenuAcknowledgement(CpuState *cpu) {
  ActRaiserSimMenu_DescriptionWait();
  return cpu && (s_skipping_menu_text || ActRaiserSimMenu_DescriptionAborted()) &&
      cpu->m_flag && !cpu->x_flag;
}

RecompReturn ActRaiser_LocalizationMenuAcknowledgement(CpuState *cpu) {
  /* An explicit successful acknowledgement of an audited pure-text prompt.
   * No pad state is changed; the next native confirmation owns a fresh edge. */
  cpu->A = (cpu->A & 0xff00u) | 0x80u;
  cpu->_flag_Z = 0; cpu->_flag_N = 1;
  cpu->P = (cpu->P & ~0x82u) | 0x80u;
  cpu->S += 2;
  return RECOMP_RETURN_NORMAL;
}

bool ActRaiser_LocalizationScheduleByte(CpuState *cpu) {
  if (s_entering_reader) {
    s_entering_reader = false;
    return false;
  }
  (void)ActRaiser_LocalizationObserveTextByte(cpu);
  if (!cpu)
    return false;
  if (!ActRaiserLocalizationRuntime_DialogueScheduled())
    return cpu->m_flag == 1 && cpu->x_flag == 0;
  ActRaiserLocalizationTextObservation observation = {.struct_size =
                                                          sizeof(observation)};
  if (ActRaiserLocalizationText_CopyObservation(&observation)) {
    const ActRaiserLocalizationDialogueHost host = Host(cpu);
    ActRaiserLocalizationRuntime_ScheduleByte(&observation,
                                              cpu_read8(cpu, cpu->DB, cpu->Y),
                                              cpu_read8(cpu, 0, 0x0200), &host);
  }
  return cpu->m_flag == 1 && cpu->x_flag == 0;
}

RecompReturn ActRaiser_LocalizationReadTextByte(CpuState *cpu) {
  const uint16_t first_cursor = cpu->Y;
  const bool dictionary = cpu_read8(cpu, cpu->DB, cpu->Y) >= 0x80;
  s_entering_reader = true;
  const RecompReturn result = bank_01_8FC5_M1X0(cpu);
  s_entering_reader = false;
  if (result == RECOMP_RETURN_NORMAL && dictionary) {
    ActRaiserLocalizationText_ObserveDecodedByte(cpu, first_cursor);
    ActRaiserLocalizationTextObservation observation = {
        .struct_size = sizeof(observation)};
    if (ActRaiserLocalizationText_CopyObservation(&observation)) {
      const ActRaiserLocalizationDialogueHost host = Host(cpu);
      ActRaiserLocalizationRuntime_ScheduleByte(
          &observation, (uint8_t)cpu->A, cpu_read8(cpu, 0, 0x0200), &host);
    }
  }
  return result;
}

bool ActRaiser_LocalizationScheduleContinuation(CpuState *cpu) {
  if (s_entering_continuation) {
    s_entering_continuation = false;
    return false;
  }
  (void)ActRaiser_LocalizationObserveContinuation(cpu);
  return cpu && cpu->m_flag == 1 &&
      (s_skipping_menu_text || ActRaiserSimMenu_Describing() ||
       ActRaiserLocalizationRuntime_DialogueScheduled());
}

RecompReturn ActRaiser_LocalizationContinueDialogue(CpuState *cpu) {
  const ActRaiserLocalizationDialogueHost host = Host(cpu);
  if (ActRaiserSimMenu_Describing() &&
      !ActRaiserLocalizationRuntime_DialogueScheduled()) {
    /* Retail Help needs the same fresh acknowledgement as authored Help,
     * including Back during the wait. Keep the native blinking arrow and
     * frame service, but never let a cancelled read-only page block again. */
    ActRaiserSimMenu_DescriptionWait();
    bool released = false;
    while (!ActRaiserSimMenu_DescriptionAborted()) {
      cpu_write8(cpu, 0x7f, (uint16_t)(0xb000u + cpu->X),
          cpu_read8(cpu, 0, 0x88) & 0x10 ? 0x5f : 0);
      cpu_write8(cpu, 0, 0xf1, cpu_read8(cpu, 0, 0xf1) + 1);
      uint8_t buttons = 0;
      (void)CallPresentationRoutine(cpu, bank_01_8C43_M1X0, &buttons);
      if (released && (buttons & 0xc0)) break;
      if (!(buttons & 0xc0)) released = true;
    }
    ActRaiserSimMenu_DescriptionWait();
  } else if (!s_skipping_menu_text && !ActRaiserSimMenu_DescriptionAborted()) {
    (void)ActRaiserLocalizationRuntime_ContinueDialogue(
        &host, cpu_read8(cpu, 0, 0x0200) != 0);
  }
  if (!s_skipping_menu_text && !ActRaiserSimMenu_Describing() &&
      !ActRaiserLocalizationRuntime_DialogueScheduled()) {
    /* Unlike an added-only prompt, this is a real ROM $02 continuation.
     * Switching to retail must not pay its confirmation or scroll the native
     * page automatically. Resume the original routine on the same JSR frame. */
    s_entering_continuation = true;
    const RecompReturn result = cpu->x_flag
        ? bank_01_9099_M1X1(cpu) : bank_01_9099_M1X0(cpu);
    s_entering_continuation = false;
    return result;
  }
  /* $9099's postcondition: erase its temporary arrow cell, A.low=0, Z=1,
   * N=0; preserve X/Y and the high accumulator. The caller still executes
   * $8F97's authentic clear/retained-scroll operation. */
  cpu_write8(cpu, 0x7f, (uint16_t)(0xb000u + cpu->X), 0);
  cpu->A &= 0xff00u;
  cpu->_flag_Z = 1;
  cpu->_flag_N = 0;
  cpu->P = (uint8_t)((cpu->P & ~0x82u) | 0x02u);
  cpu->S = (uint16_t)(cpu->S + 2u);
  return RECOMP_RETURN_NORMAL;
}
