#include "runtime_diagnostics.h"

#include "actraiser_game.h"   /* kActRaiserWram_MapGroup/_CurrentMap */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "common_cpu_infra.h"
#include "cpu_state.h"
#include "diagnostic.h"
#include "present_cadence_metrics.h"
#include "runner_next.h"
#include "runner_next_internal.h"
#include "run_dir.h"

enum {
  kDiagnosticPathCapacity = 320,
  kBlockHistoryCapacity = 256,
  kBlockHistoryMFlagBit = 16,
  kBlockHistoryXFlagBit = 17,
  kRegisterValueMask = 0xffff,
};

#ifndef _WIN32
static const mode_t kDiagnosticDirectoryMode = 0755;
#endif

/* These buffers remain static because the watchdog can call DumpDiagState from
 * the game coroutine at the deepest point in its recompiled dispatch chain. */
static char s_wram_path[kDiagnosticPathCapacity];
static char s_sram_path[kDiagnosticPathCapacity];
static char s_state_path[kDiagnosticPathCapacity];
static char s_dispatch_path[kDiagnosticPathCapacity];
static uint32_t s_block_history_pc[kBlockHistoryCapacity];
static uint32_t s_block_history_aux[kBlockHistoryCapacity];
static uint16_t s_block_history_stack[kBlockHistoryCapacity];

typedef struct DiagnosticRunnerView {
  const SnesRunnerApi *api;
  SrRunnerHandle *runner;
  SrCpuStateSnapshot cpu;
  SrBorrowedSpan wram;
  SrBorrowedSpan sram;
  bool has_sram;
} DiagnosticRunnerView;

static bool DiagnosticRunnerView_Borrow(DiagnosticRunnerView *view) {
  memset(view, 0, sizeof(*view));
  view->api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  view->runner = sr_runner_handle(g_snes);
  if (!view->api || !view->runner ||
      view->api->struct_size < SNES_RUNNER_API_CPU_STATE_SIZE ||
      (view->api->capabilities &
       (SR_RUNNER_CAP_BORROWED_BYTE_SPANS | SR_RUNNER_CAP_CPU_STATE)) !=
          (SR_RUNNER_CAP_BORROWED_BYTE_SPANS | SR_RUNNER_CAP_CPU_STATE))
    return false;
  view->cpu.struct_size = sizeof(view->cpu);
  view->wram.struct_size = sizeof(view->wram);
  view->sram.struct_size = sizeof(view->sram);
  if (view->api->query_cpu_state(view->runner, &view->cpu) != SR_RESULT_OK ||
      view->api->borrow_memory(view->runner, SR_MEMORY_WRAM, &view->wram) !=
          SR_RESULT_OK ||
      view->wram.byte_size < kActRaiserWramSize)
    return false;
  view->has_sram =
      view->api->borrow_memory(view->runner, SR_MEMORY_SRAM, &view->sram) ==
          SR_RESULT_OK &&
      view->sram.data && view->sram.byte_size > 0u;
  return true;
}

static uint16_t Diagnostic_ReadWram16(const DiagnosticRunnerView *view,
                                      uint32_t address) {
  return (uint16_t)(view->wram.data[address] |
                    ((uint16_t)view->wram.data[address + 1u] << 8));
}

void DumpDiagState(const char *tag) {
#ifndef _WIN32
  mkdir("saves", kDiagnosticDirectoryMode);
#endif

  DiagnosticRunnerView view;
  if (!DiagnosticRunnerView_Borrow(&view)) {
    fprintf(stderr, "[dump] runner ABI state is unavailable (%s)\n",
            tag ? tag : "");
    return;
  }
  const int frame_number = (int)view.cpu.frame_counter;

  const bool is_hotkey_dump = tag && strcmp(tag, "hotkey") == 0;
  if (is_hotkey_dump) {
    RunDirFile(s_wram_path, sizeof(s_wram_path), "dump_f%d_wram.bin",
               frame_number);
    RunDirFile(s_sram_path, sizeof(s_sram_path), "dump_f%d_sram.bin",
               frame_number);
    RunDirFile(s_state_path, sizeof(s_state_path), "dump_f%d_state.txt",
               frame_number);
    RunDirFile(s_dispatch_path, sizeof(s_dispatch_path),
               "dump_f%d_dispatch_log.json", frame_number);
  } else {
    RunDirFile(s_wram_path, sizeof(s_wram_path), "dump_wram.bin");
    RunDirFile(s_sram_path, sizeof(s_sram_path), "dump_sram.bin");
    RunDirFile(s_state_path, sizeof(s_state_path), "dump_state.txt");
    RunDirFile(s_dispatch_path, sizeof(s_dispatch_path),
               "dump_dispatch_log.json");
  }

  {
    FILE *wram_file = fopen(s_wram_path, "wb");
    if (wram_file) {
      fwrite(view.wram.data, 1, kActRaiserWramSize, wram_file);
      fclose(wram_file);
    }
  }
  if (view.has_sram) {
    FILE *sram_file = fopen(s_sram_path, "wb");
    if (sram_file) {
      fwrite(view.sram.data, 1, (size_t)view.sram.byte_size, sram_file);
      fclose(sram_file);
    }
  }

  FILE *state_file = fopen(s_state_path, "w");
  if (state_file) {
    fprintf(state_file, "=== ActRaiser recomp state dump (%s) ===\n",
            tag ? tag : "");
    fprintf(state_file, "frame=%d  last_func=%s\n", frame_number,
            g_last_recomp_func ? g_last_recomp_func : "(none)");
    fprintf(state_file,
            "A=%04x X=%04x Y=%04x S=%04x D=%04x DB=%02x PB=%02x P=%02x"
            " m=%d x=%d\n",
            view.cpu.a, view.cpu.x, view.cpu.y, view.cpu.s, view.cpu.d,
            view.cpu.db, view.cpu.pb, view.cpu.p,
            (view.cpu.flags & SR_CPU_STATE_M_FLAG) != 0u,
            (view.cpu.flags & SR_CPU_STATE_X_FLAG) != 0u);
    fprintf(state_file,
            "recomp call stack (innermost first), depth=%d:\n",
            g_recomp_stack_top);
    for (int stack_index = g_recomp_stack_top - 1;
         stack_index >= 0; stack_index--) {
      fprintf(state_file, "  [%d] %s\n",
              g_recomp_stack_top - 1 - stack_index,
              g_recomp_stack[stack_index]
                  ? g_recomp_stack[stack_index]
                  : "?");
    }
    fprintf(state_file,
            "game-state: $18=%02x $19=%02x  town-level $0291=%04x\n",
            view.wram.data[kActRaiserWram_MapGroup],
            view.wram.data[kActRaiserWram_CurrentMap],
            Diagnostic_ReadWram16(&view, kActRaiserWram_TownLevel));

    const int block_count = ar_block_history3(
        s_block_history_pc, s_block_history_aux, s_block_history_stack,
        kBlockHistoryCapacity);
    fprintf(state_file,
            "block history (last %d, oldest-first) pc m x S X  "
            "(watch S drift across a call to find the unbalanced subroutine):\n",
            block_count);
    for (int block_index = 0; block_index < block_count; block_index++) {
      fprintf(state_file, "  %06X m=%u x=%u S=%04X X=%04X\n",
              s_block_history_pc[block_index],
              (s_block_history_aux[block_index] >>
               kBlockHistoryMFlagBit) & 1,
              (s_block_history_aux[block_index] >>
               kBlockHistoryXFlagBit) & 1,
              s_block_history_stack[block_index],
              s_block_history_aux[block_index] & kRegisterValueMask);
    }
    fclose(state_file);
  }

  CpuDispatchLogWriteFile(s_dispatch_path);

  const PresentCadenceMetrics cadence = PresentCadence_GetMetrics();
  fprintf(stderr,
          "[present-cadence] tick-presents=%lu re-presents=%lu "
          "max-represent-alpha=%.3f no-present-no-sleep=%lu\n",
          cadence.tick_present_count, cadence.represent_count,
          (double)cadence.maximum_represent_alpha,
          cadence.no_present_no_sleep_iteration_count);
  fprintf(stderr, "[dump] wrote %s + wram/sram/dispatch_log (%s)\n",
          s_state_path, tag ? tag : "");
}
