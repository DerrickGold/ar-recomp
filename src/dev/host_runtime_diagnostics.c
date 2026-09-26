#include "dev/host_runtime_diagnostics.h"

#include <stdio.h>
#include <stdlib.h>

#include "actraiser_game.h"
#include "constants.h"
#include "host/host_clock.h"
#include "host/host_ppu_output.h"
#include "snesrecomp/game/generated_support.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/trace.h"

/* Optional diagnostic windows are independent of the on-screen pipeline HUD. */
enum {
  kUninitializedEnvironmentOption = -2,
  kPerformanceReportIntervalMs = kMillisecondsPerSecond,
};
static int s_tick_perf = -1;
static int s_draw_perf = -1;
static int s_apuprof_ms = kUninitializedEnvironmentOption;

void HostRuntimeDiagnostics_InitTrace(void) {
  cpu_trace_init();

  /* AR_DRIFT_FRAME=N: arm the stack-drift tripwire to fire on the first
   * NORMAL function exit at/after frame N whose exit S != entry S (the
   * unbalanced push/pop leaker). Diagnostic only. */
#if SNESRECOMP_TRACE
  {
    const char *v = getenv("AR_DRIFT_FRAME");
    if (v && v[0]) {
      cpu_trace_arm_stack_drift_tripwire((int32_t)strtol(v, NULL, 0));
      fprintf(stderr, "[AR_DRIFT_FRAME] stack-drift tripwire armed at frame %s\n", v);
    }
  }
#endif
}

/* The SNESRECOMP_ENTRY_MX_CHECK / SNESRECOMP_MX_HISTORY / SNESRECOMP_EXIT_MX_CHECK /
 * SNESRECOMP_CALL_MX_CHECK / SNESRECOMP_TRAP_FUNCTION family: runtime m/x invariant checks and
 * call-stack traps. All diagnostic, all opt-in, and all resolved once here so no hot path pays a
 * getenv. */
void HostRuntimeDiagnostics_ConfigureChecks(void) {
  /* SNESRECOMP_ENTRY_MX_CHECK=1: enable the per-function-entry m/x invariant check
   * (validates the emitter's static m/x analysis on every direct call). */
  {
    const char *e = getenv("SNESRECOMP_ENTRY_MX_CHECK");
    g_sr_entry_mx_check_enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
  }
  /* SNESRECOMP_MX_HISTORY=1: per-PC runtime m/x histogram + live misdecode anomaly trap. */
  {
    const char *e = getenv("SNESRECOMP_MX_HISTORY");
    g_sr_mx_history_enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
    if (g_sr_mx_history_enabled) atexit(sr_mx_history_dump);
  }
  /* SNESRECOMP_EXIT_MX_CHECK=1: per-function EXIT m/x check — fires when a function's runtime
   * exit (m,x) differs from what the emitter told its callers (exit-mx
   * misdecode, e.g. $03:9156). SNESRECOMP_EXIT_STACK_CHECK=1: per-function EXIT stack-balance
   * check — fires when a paired frame's RTS/RTL drifts S (e.g. $01:B8CF).
   * Symmetric twins of SNESRECOMP_ENTRY_MX_CHECK; name the culprit at its own return. */
  {
    const char *e = getenv("SNESRECOMP_EXIT_MX_CHECK");
    g_sr_exit_mx_check_enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
  }
  {
    const char *e = getenv("SNESRECOMP_EXIT_STACK_CHECK");
    g_sr_exit_stack_check_enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
  }
  /* SNESRECOMP_CALL_MX_CHECK=1: per-CALL-SITE m/x invariant check — fires at every JSR/JSL
   * when runtime (m,x) disagrees with what the decoder statically knew at
   * that exact instruction. Catches (m,x) corruption from ANYWHERE upstream
   * of a call (not just decode-time mistakes SNESRECOMP_ENTRY_MX_CHECK/SNESRECOMP_EXIT_MX_CHECK
   * cover), narrowed to the first call site downstream of the corruption. */
  {
    const char *e = getenv("SNESRECOMP_CALL_MX_CHECK");
    g_sr_call_mx_check_enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
  }

  /* SNESRECOMP_TRAP_FUNCTION=<substring>: dump the recomp call stack the first time a matching
   * function is entered (finds the dispatch chain into a misdecode variant). */
  {
    const char *e = getenv("SNESRECOMP_TRAP_FUNCTION");
    g_sr_trap_function = (e && e[0]) ? e : 0;
  }
}

HostRuntimeTickProfile HostRuntimeDiagnostics_BeginTick(void) {
  HostRuntimeTickProfile scope = {0};
  if (s_tick_perf < 0) s_tick_perf = getenv("AR_PERF") ? 1 : 0;
  scope.perf_start_ms = s_tick_perf ? HostClock_Milliseconds() : 0;
  /* SNESRECOMP_APU_PROFILE=<ms>: per-frame APU-stall attribution. Any game frame whose
   * wall time reaches the threshold (default 8 ms; the flag value overrides
   * when >= 2) prints one [apuprof] line splitting the frame into lock-wait
   * vs SPC catch-up vs handshake-spin vs upload vs music-hook time. */
  if (s_apuprof_ms == kUninitializedEnvironmentOption) {
    /* RtlApuProfileIsEnabled caches its own answer in the runner, so it is a
     * separate module's older read of this variable -- it may agree with the
     * environment and still not be a promise about THIS getenv result. Gate
     * the parse on the pointer being parsed. */
    const char *profile = getenv("SNESRECOMP_APU_PROFILE");
    s_apuprof_ms = (RtlApuProfileIsEnabled() && profile && profile[0]) ? atoi(profile) : -1;
    if (s_apuprof_ms >= 0 && s_apuprof_ms < 2) s_apuprof_ms = 8;
  }
  if (s_apuprof_ms > 0) {
    RtlApuProfileReset();
    scope.pushes = g_recomp_push_count;
    scope.loops = g_watchdog_loop_headers;
    scope.apu_start_ns = HostClock_Nanoseconds();
  }

  return scope;
}

void HostRuntimeDiagnostics_EndTick(HostRuntimeTickProfile scope) {
  if (scope.apu_start_ns) {
    RtlApuProfile profile = {.struct_size = RTL_APU_PROFILE_V2_SIZE};
    uint64_t dt_ns = HostClock_Nanoseconds() - scope.apu_start_ns;
    RtlApuProfileRead(&profile);
    if (dt_ns >= (uint64_t)s_apuprof_ms * kNanosecondsPerMillisecond) {
      const unsigned gf = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
      double audiowait_ms = RtlApuProfileTakeAudioWaitMax() / (double)kNanosecondsPerMillisecond;
      fprintf(stderr,
              "[apuprof] gf=%u dt=%.1fms lockwait=%.2fms "
              "portsync=%.2fms/%llucyc/%uc apu=%llu "
              "audio=%llu uploadctl=%llu timeline=%llu other=%llu "
              "reads=%u writes=%u "
              "hook=%.2fms upload=%.2fms schedlat=%llusmp pushes=%lu "
              "loops=%llu audiowait-max=%.2fms last=%s\n",
              gf, dt_ns / (double)kNanosecondsPerMillisecond,
              profile.lock_wait_ns / (double)kNanosecondsPerMillisecond,
              profile.port_sync_ns / (double)kNanosecondsPerMillisecond,
              (unsigned long long)profile.apu_cycles_port_sync, profile.port_sync_calls,
              (unsigned long long)profile.apu_cycles_total,
              (unsigned long long)profile.apu_cycles_audio_demand,
              (unsigned long long)profile.apu_cycles_upload_control,
              (unsigned long long)profile.apu_cycles_timeline,
              (unsigned long long)profile.apu_cycles_unattributed, profile.port_reads,
              profile.port_writes, profile.hook_ns / (double)kNanosecondsPerMillisecond,
              profile.upload_ns / (double)kNanosecondsPerMillisecond,
              (unsigned long long)profile.scheduled_latency_max, g_recomp_push_count - scope.pushes,
              (unsigned long long)(g_watchdog_loop_headers - scope.loops), audiowait_ms,
              profile.last_port_function ? profile.last_port_function : "-");
    }
  }
  if (s_tick_perf) {
    static uint64_t win_start, run_ms_sum, run_ms_max;
    static int win_frames;
    static uint64_t last_cu_calls, last_cu_cycles;
    static unsigned last_gf;
    uint64_t t1 = HostClock_Milliseconds();
    uint64_t dt = t1 - scope.perf_start_ms;
    run_ms_sum += dt;
    if (dt > run_ms_max) run_ms_max = dt;
    win_frames++;
    if (!win_start) win_start = t1;
    if (t1 - win_start >= kPerformanceReportIntervalMs) {
      uint64_t cc, cy;
      RtlApuProfileReadCatchupStats(&cc, &cy);
      const unsigned gf = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
      fprintf(stderr,
              "[perf] fps=%d run-ms avg=%.1f max=%llu gf+=%u "
              "apu-catchup calls=%llu cyc=%llu $18=%02x\n",
              win_frames, (double)run_ms_sum / win_frames, (unsigned long long)run_ms_max,
              (unsigned)(uint16)(gf - last_gf), (unsigned long long)(cc - last_cu_calls),
              (unsigned long long)(cy - last_cu_cycles), g_ram[kActRaiserWram_MapGroup]);
      last_cu_calls = cc;
      last_cu_cycles = cy;
      last_gf = gf;
      win_start = t1;
      run_ms_sum = 0;
      run_ms_max = 0;
      win_frames = 0;
    }
  }
}

uint64_t HostRuntimeDiagnostics_BeginDraw(void) {
  if (s_draw_perf < 0) s_draw_perf = getenv("AR_PERF") ? 1 : 0;

  return s_draw_perf ? HostClock_Milliseconds() : 0;
}

void HostRuntimeDiagnostics_EndDraw(uint64_t started_ms) {
  if (s_draw_perf) {
    static uint64_t draw_win_start, draw_ms_sum, draw_ms_max;
    static int draw_win_frames;
    uint64_t now = HostClock_Milliseconds();
    uint64_t dt = now - started_ms;
    draw_ms_sum += dt;
    if (dt > draw_ms_max) draw_ms_max = dt;
    draw_win_frames++;
    if (!draw_win_start) draw_win_start = now;
    if (now - draw_win_start >= kPerformanceReportIntervalMs) {
      fprintf(stderr,
              "[draw-perf] frames=%d draw-ms avg=%.1f max=%llu "
              "$18=%02x $19=%02x authentic-capture=%s\n",
              draw_win_frames, (double)draw_ms_sum / draw_win_frames,
              (unsigned long long)draw_ms_max, g_ram[kActRaiserWram_MapGroup],
              g_ram[kActRaiserWram_CurrentMap], HostPpuOutput_AuthenticEnabled() ? "on" : "off");
      draw_win_start = now;
      draw_ms_sum = 0;
      draw_ms_max = 0;
      draw_win_frames = 0;
    }
  }
}

bool HostRuntimeDiagnostics_PipelineLoggingEnabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *value = getenv("AR_PIPELINE_PERF");
    enabled = value && value[0] && value[0] != '0';
  }
  return enabled != 0;
}
