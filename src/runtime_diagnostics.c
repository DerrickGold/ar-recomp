#include "runtime_diagnostics.h"

#include "actraiser_game.h"   /* kActRaiserWram_MapGroup/_CurrentMap */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "present_cadence_metrics.h"
#include "snesrecomp/runner.h"
#include "run_dir.h"

enum {
  kDiagnosticPathCapacity = 320,
  kDispatchHistoryCapacity = 1024,
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
typedef struct DiagnosticDispatchEvent {
  uint32_t pc24;
  uint32_t source_pc24;
  const char *function_name;
  uint8_t mx;
  uint8_t found;
  uint8_t mirrored;
  uint32_t frame;
} DiagnosticDispatchEvent;

static DiagnosticDispatchEvent
    s_dispatch_history[kDispatchHistoryCapacity];
static unsigned s_dispatch_history_count;
static const SnesRunnerApi *s_diagnostic_api;
static SrRunnerHandle *s_diagnostic_runner;
static uint64_t s_diagnostic_subscription;

static void RuntimeDiagnostics_OnEvent(void *user_data,
                                       SrRunnerHandle *runner,
                                       const SrRunnerEvent *event) {
  (void)user_data;
  if (runner != s_diagnostic_runner || !event) return;
  if (event->type == SR_EVENT_DYNAMIC_DISPATCH) {
    DiagnosticDispatchEvent *record =
        &s_dispatch_history[
            s_dispatch_history_count++ % kDispatchHistoryCapacity];
    record->pc24 = event->pc24;
    record->source_pc24 = event->source_pc24;
    record->function_name = event->label;
    record->mx =
        (uint8_t)(((event->cpu_flags & SR_CPU_STATE_M_FLAG) != 0u ? 2u : 0u) |
                  ((event->cpu_flags & SR_CPU_STATE_X_FLAG) != 0u ? 1u : 0u));
    record->found =
        (event->flags & SR_EVENT_DISPATCH_FOUND) != 0u;
    record->mirrored =
        (event->flags & SR_EVENT_DISPATCH_MIRRORED) != 0u;
    record->frame = (uint32_t)event->frame_counter;
  }
}

void RuntimeDiagnostics_Unbind(void) {
  if (s_diagnostic_api && s_diagnostic_runner &&
      s_diagnostic_subscription != 0u) {
    s_diagnostic_api->unsubscribe_events(s_diagnostic_runner,
                                         s_diagnostic_subscription);
  }
  s_diagnostic_subscription = 0u;
  s_diagnostic_runner = NULL;
  s_diagnostic_api = NULL;
}

bool RuntimeDiagnostics_Bind(SrRunnerHandle *runner) {
  SrEventSubscription subscription;
  const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  RuntimeDiagnostics_Unbind();
  memset(s_dispatch_history, 0, sizeof(s_dispatch_history));
  s_dispatch_history_count = 0u;
  if (!api || !runner ||
      api->struct_size < SNES_RUNNER_API_EVENT_OBSERVER_SIZE ||
      (api->capabilities &
       (SR_RUNNER_CAP_EXECUTION_STATE | SR_RUNNER_CAP_EVENT_OBSERVERS)) !=
          (SR_RUNNER_CAP_EXECUTION_STATE | SR_RUNNER_CAP_EVENT_OBSERVERS))
    return false;

  memset(&subscription, 0, sizeof(subscription));
  subscription.struct_size = sizeof(subscription);
  subscription.event_mask = SR_EVENT_MASK_DYNAMIC_DISPATCH;
  subscription.callback = RuntimeDiagnostics_OnEvent;
  s_diagnostic_api = api;
  s_diagnostic_runner = runner;
  if (api->subscribe_events(runner, &subscription,
                            &s_diagnostic_subscription) != SR_RESULT_OK) {
    RuntimeDiagnostics_Unbind();
    return false;
  }
  return true;
}

typedef struct DiagnosticRunnerView {
  const SnesRunnerApi *api;
  SrRunnerHandle *runner;
  SrCpuStateSnapshot cpu;
  SrExecutionSnapshot execution;
  SrBorrowedSpan wram;
  SrBorrowedSpan sram;
  bool has_sram;
} DiagnosticRunnerView;

static bool DiagnosticRunnerView_Borrow(DiagnosticRunnerView *view) {
  memset(view, 0, sizeof(*view));
  view->api = s_diagnostic_api;
  view->runner = s_diagnostic_runner;
  if (!view->api || !view->runner ||
      view->api->struct_size < SNES_RUNNER_API_EXECUTION_STATE_SIZE ||
      (view->api->capabilities &
       (SR_RUNNER_CAP_BORROWED_BYTE_SPANS | SR_RUNNER_CAP_CPU_STATE |
        SR_RUNNER_CAP_EXECUTION_STATE)) !=
          (SR_RUNNER_CAP_BORROWED_BYTE_SPANS | SR_RUNNER_CAP_CPU_STATE |
           SR_RUNNER_CAP_EXECUTION_STATE))
    return false;
  view->cpu.struct_size = sizeof(view->cpu);
  view->execution.struct_size = sizeof(view->execution);
  view->wram.struct_size = sizeof(view->wram);
  view->sram.struct_size = sizeof(view->sram);
  if (view->api->query_cpu_state(view->runner, &view->cpu) != SR_RESULT_OK ||
      view->api->query_execution_state(view->runner, &view->execution) !=
          SR_RESULT_OK ||
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

static void Diagnostic_JsonString(FILE *file, const char *text) {
  const unsigned char *cursor =
      (const unsigned char *)(text ? text : "");
  fputc('"', file);
  while (*cursor != '\0') {
    if (*cursor == '"' || *cursor == '\\') fputc('\\', file);
    if (*cursor >= 0x20u) fputc(*cursor, file);
    ++cursor;
  }
  fputc('"', file);
}

static void Diagnostic_WriteDispatchLog(const char *path) {
  FILE *file;
  unsigned shown;
  unsigned start;
  unsigned index;
  if (!path || (file = fopen(path, "wb")) == NULL) return;
  shown = s_dispatch_history_count < kDispatchHistoryCapacity
              ? s_dispatch_history_count
              : kDispatchHistoryCapacity;
  start = s_dispatch_history_count - shown;
  fprintf(file,
          "{\n  \"dispatch_log\": {\"total\": %u, \"shown\": %u, \"events\": [",
          s_dispatch_history_count, shown);
  for (index = 0u; index < shown; ++index) {
    const DiagnosticDispatchEvent *event =
        &s_dispatch_history[(start + index) % kDispatchHistoryCapacity];
    fprintf(file,
            "%s\n    {\"i\":%u,\"pc24\":\"%06X\",\"source_pc24\":\"%06X\",\"func\":",
            index == 0u ? "" : ",", start + index, event->pc24,
            event->source_pc24);
    Diagnostic_JsonString(file, event->function_name);
    fprintf(file,
            ",\"mx\":%u,\"found\":%u,\"mirror\":%u,\"frame\":%u}",
            event->mx, event->found, event->mirrored, event->frame);
  }
  fputs("\n  ]}\n}\n", file);
  fclose(file);
}

void DumpDiagState(const char *tag) {
#ifndef _WIN32
  mkdir("saves", kDiagnosticDirectoryMode);
#endif

  /* The watchdog can enter here at the deepest generated call chain. Keep the
   * bounded execution history off that coroutine stack. */
  static DiagnosticRunnerView view;
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
    const unsigned stack_count =
        view.execution.stack_depth < SR_EXECUTION_STACK_CAPACITY
            ? view.execution.stack_depth
            : SR_EXECUTION_STACK_CAPACITY;
    fprintf(state_file, "=== ActRaiser recomp state dump (%s) ===\n",
            tag ? tag : "");
    fprintf(state_file, "frame=%d  last_func=%s\n", frame_number,
            view.execution.current_function
                ? view.execution.current_function
                : "(none)");
    fprintf(state_file,
            "A=%04x X=%04x Y=%04x S=%04x D=%04x DB=%02x PB=%02x P=%02x"
            " m=%d x=%d\n",
            view.cpu.a, view.cpu.x, view.cpu.y, view.cpu.s, view.cpu.d,
            view.cpu.db, view.cpu.pb, view.cpu.p,
            (view.cpu.flags & SR_CPU_STATE_M_FLAG) != 0u,
            (view.cpu.flags & SR_CPU_STATE_X_FLAG) != 0u);
    fprintf(state_file,
            "recomp call stack (innermost first), depth=%d:\n",
            (int)view.execution.stack_depth);
    for (int stack_index = (int)stack_count - 1;
         stack_index >= 0; stack_index--) {
      fprintf(state_file, "  [%d] %s\n",
              (int)stack_count - 1 - stack_index,
              view.execution.stack[stack_index].function_name
                  ? view.execution.stack[stack_index].function_name
                  : "?");
    }
    fprintf(state_file,
            "game-state: $18=%02x $19=%02x  town-level $0291=%04x\n",
            view.wram.data[kActRaiserWram_MapGroup],
            view.wram.data[kActRaiserWram_CurrentMap],
            Diagnostic_ReadWram16(&view, kActRaiserWram_TownLevel));

    const unsigned block_count =
        view.execution.history_count < SR_EXECUTION_HISTORY_CAPACITY
            ? view.execution.history_count
            : SR_EXECUTION_HISTORY_CAPACITY;
    fprintf(state_file,
            "block history (last %d, oldest-first) pc m x S X  "
            "(watch S drift across a call to find the unbalanced subroutine):\n",
            (int)block_count);
    for (unsigned block_index = 0u; block_index < block_count;
         block_index++) {
      const SrExecutionBlock *block =
          &view.execution.history[block_index];
      fprintf(state_file, "  %06X m=%u x=%u S=%04X X=%04X\n",
              block->pc24,
              (block->cpu_flags & SR_CPU_STATE_M_FLAG) != 0u,
              (block->cpu_flags & SR_CPU_STATE_X_FLAG) != 0u,
              block->stack_pointer, block->register_x);
    }
    fclose(state_file);
  }

  Diagnostic_WriteDispatchLog(s_dispatch_path);

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
