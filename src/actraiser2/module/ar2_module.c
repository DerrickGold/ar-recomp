#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/audio_timing.h"
#include "snesrecomp/game/trace.h"
#include "snesrecomp/game/generated_support.h"
#include "snesrecomp/game_runtime.h"
#include "ar2_capture.h"
#include "ar2_sprites.h"
#include "ar2_world.h"
#include "dev/snes_snapshot.h"
#include "snesrecomp/support/utf8_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ar2_coroutine.h"
#include "snesrecomp/support/digest.h"
#include <setjmp.h>

static jmp_buf escape;
static bool on_game, opened, failed;
static bool returned_to_world;
static char error_text[192];
static uint64_t audio_fraction;
static unsigned checkpoints;
static int16_t pcm[800 * 2];
static uint8_t requested_stage;
static uint32_t block_pc;
static int started, in_interrupt;
static int host_frame;
static unsigned cop_depth;
static uint32_t cop_rtl_target;
static int cop_returned_by_rtl;
static uint16_t cop_rtl_stack_min;
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

static uint16_t ram_word(unsigned address) {
  return g_ram[address] | (uint16_t)g_ram[address + 1] << 8;
}
static void set_ram_word(unsigned address, uint16_t value) {
  g_ram[address] = value;
  g_ram[address + 1] = value >> 8;
}
static bool warp_ready(const SnesRunnerApi *api, SrRunnerHandle *runner) {
  if (g_ram[0x518] || ram_word(0x985)) return false;
  if (g_ram[0x51a] != 4 && g_ram[0x51a] < 0x14) return false;
  /* A fast developer warp can overtake the previous song's workspace
   * initialization. Wait for its command acknowledgement ($F4); the native
   * fade and $098D sequence-position guard then own the transition. */
  static uint8_t aram[65536], dsp[128];
  const SrApuStateQuery query = {.struct_size = SR_APU_STATE_QUERY_V2_SIZE,
                                 .apu_ram = aram,
                                 .apu_ram_capacity = sizeof(aram),
                                 .dsp_registers = dsp,
                                 .dsp_register_capacity = sizeof(dsp)};
  SrApuStateSnapshot apu = {.struct_size = SR_APU_STATE_SNAPSHOT_V2_SIZE};
  return api->query_apu_state(runner, &query, &apu) == SR_RESULT_OK && g_ram[0x5f5] == 1 &&
         apu.apu_to_cpu_ports[0] == 1;
}
static void queue_stage(uint8_t stage) {
  /* Native action-entry initialization at $82:B108..B137. Reset has already
   * derived difficulty-dependent defaults through $80:BB7E. Without this
   * setup a direct scene request enters with zero health and no HUD assets.
   * The scene loader still owns every map, actor, collision and audio task. */
  /* Normal world entry carries bits 1..4, then $82:B108 adds bit 0.
   * Request all five shared resources through $80:BBA4: HUD, Master and
   * effect/text palettes, HUD graphics and common OBJ graphics. A direct
   * entry from the logo otherwise retains its purple palette for START. */
  set_ram_word(0x95f, ram_word(0x95f) | 0x1f);
  set_ram_word(0x91b, ram_word(0x997));
  set_ram_word(0x91d, 0x14);
  set_ram_word(0x91f, 0x14);
  uint16_t magic_charges = ram_word(0x106) ? 3 : 5;
  set_ram_word(0x921, magic_charges);
  set_ram_word(0x923, magic_charges);
  set_ram_word(0x94f, ram_word(0x995));
  set_ram_word(0x94d, 0x99);
  set_ram_word(0x97b, stage);
  set_ram_word(0x95b, stage);
  set_ram_word(0x97d, 0); /* Native controller input, not attract playback. */
  /* $82:B142 arms the loader's wait for the previous song's ready signal
   * on $2141. A quick developer warp can otherwise halt a song while its
   * SPC workspace is still being initialized. */
  set_ram_word(0x98d, 1);
  /* $80:B88C consumes the same pending-scene field written by $82:B168
   * and the cartridge's debug scene selector at $80:8145. */
  g_ram[0x518] = stage;
}

static void suspend_game(void *unused) {
  (void)unused;
  Ar2Coroutine_Yield();
}
static void yield_game(void) { cpu_yield_execution(suspend_game, NULL); }
static void checkpoint(CpuState *cpu, uint32_t pc) {
  Ar2Sprites_Checkpoint(pc);
  /* The host owns the imported globe. Stop at the native loader boundary
   * after gameplay requests it, before starting the cartridge's world UI.
   * This terminal coroutine is discarded on close, never resumed. */
  if (pc == 0x80b88c && !requested_stage && ram_word(0x518) == 0x0a && !ram_word(0x881) &&
      Ar2World_SupportsScene(ram_word(0x51a))) {
    returned_to_world = true;
    for (;;)
      yield_game();
  }
  /* Native direct-block and BRR-catalog upload completion. Polling all bytes
   * can advance the APU far past the host's frame target; the next native
   * vblank must still give the newly loaded driver a real tick. */
  if (pc == 0x80d16f || pc == 0x80d0f5) RtlFinishSynchronousApuTransfer();
  block_pc = pc;
  if (++checkpoints > 100000000) {
    snprintf(error_text, sizeof(error_text), "Native frame exceeded its work budget at %06x", pc);
    abort();
  }
}
static int read_rdnmi(const RtlRdnmiReadContext *context) {
  (void)context;
  if (!in_interrupt && block_pc == 0x80bde4) {
    yield_game();
    return 0x82;
  }
  return -1;
}
static void interrupt(uint32_t vector) {
  CpuState saved = g_cpu;
  uint32_t saved_pc = block_pc;
  in_interrupt = 1;
  cpu_push_interrupt_frame(&g_cpu);
  RecompReturn result = cpu_dispatch_pc(&g_cpu, vector, g_cpu.S);
  if (result != RECOMP_RETURN_NORMAL) {
    fprintf(stderr, "interrupt result %d at %06x\n", result, block_pc);
    abort();
  }
  g_cpu = saved;
  block_pc = saved_pc;
  in_interrupt = 0;
}
static void run_game(void) {
  cpu_state_init(&g_cpu, g_ram);
  CpuReturnScope scope;
  cpu_reset_scope_begin(&scope, &g_cpu);
  RecompReturn result = cpu_dispatch_pc(&g_cpu, 0x008000, g_cpu.S);
  while (result == RECOMP_RETURN_TAILCALL ||
         (result == RECOMP_RETURN_OWNED_UNWIND && cpu_finish_reset_tail(&scope, &g_cpu)))
    result = cpu_dispatch_pc_from(&g_cpu, g_tailcall_pc24, g_tailcall_miss_s, g_tailcall_src24);
  fprintf(stderr, "reset returned %d at %06x\n", result, block_pc);
  abort();
}
static void run_frame(void) {
  if (!started) {
    if (!Ar2Coroutine_Create(run_game)) abort();
    started = 1;
  }
  RtlGameFrameBegin();
  on_game = true;
  Ar2Coroutine_Resume();
  on_game = false;
  if (failed || returned_to_world) return;
  int transition = RtlGameFrameComplete(RTL_GAME_FRAME_DISPATCH_NMI_IF_ENABLED);
  if (transition & RTL_GAME_FRAME_NMI_ENTERED) interrupt(0x008007);
}
static const RtlGameIdentity identity = {
    .struct_size = RTL_GAME_IDENTITY_V1_SIZE,
    .game_id = "actraiser2",
    .display_name = "ActRaiser 2",
    .save_name_prefix = "ar2-prototype",
};
static bool dispatch_miss(uint32_t source, uint32_t target) {
  snprintf(error_text, sizeof(error_text), "Uncovered AR2 code %06x -> %06x (M%d X%d)", source,
           target, g_cpu.m_flag, g_cpu.x_flag);
  abort();
}

static bool defer_wide_activation(const CpuState *cpu) {
  /* $0951 starts at $82:8073, when the Master enters its input loop, and
   * resets on scene load/death. $095B is too early: it clears before the
   * stone-to-Master animation has finished. Keep the extra view rendered,
   * but don't let its actors wake before the player can defend themselves. */
  if (cpu->emulation || cpu->m_flag || cpu->x_flag ||
      !Ar2World_SupportsScene(ram_word(0x51a)) || ram_word(0x951)) return false;
  const unsigned actor = cpu->Y; /* COP dispatcher holds the actor in Y. */
  if (actor > 0xffd0 || actor == ram_word(0x649)) return false;
  /* Preserve COP $1E's existing wait and its out-of-world bypass. Only
   * visibility added by the wider capture needs this extra condition. */
  const uint16_t x = ram_word(actor), y = ram_word(actor + 2);
  if ((ram_word(actor + 4) & 0x8000) || x >= ram_word(0x550) || y >= ram_word(0x554))
    return false;
  const int left = (int16_t)(x - ram_word(actor + 0x10) - ram_word(0x540));
  const int right = (int16_t)(x + ram_word(actor + 0x14) - ram_word(0x540));
  return !((left >= 0 && left < 256) || (left < 0 && right >= 0 && right < 256));
}

RecompReturn Ar2_DispatchCopHandler(CpuState *cpu) {
  uint16_t target = cpu_read16(cpu, 0x80, (uint16_t)(0x82db + cpu->X));
  if (cpu->X >= 0x1a2 || target < 0x847d || target == 0xffff) {
    fprintf(stderr, "unsupported COP signature %02x\n", cpu->X / 2);
    abort();
  }
  if (target == 0x87cc && defer_wide_activation(cpu)) {
    /* Enter COP $1E's native sleep path after TYX and its in-world bounds
     * checks (carry clear). Native code owns the retry delay, saved task PC,
     * operand consumption and scheduler return. No actor flags are changed. */
    cpu->X = cpu->Y;
    cpu->_flag_C = 0;
    cpu_mirrors_to_p(cpu);
    target = 0x87e5;
  }
  return cpu_dispatch_pc_from(cpu, 0x800000 | target, cpu->S, 0x8082d8);
}

RecompReturn Ar2_MoveBlock(CpuState *cpu) {
  if (cpu->emulation || cpu->m_flag || cpu->x_flag || g_ram[0x502] != 0x8b ||
      g_ram[0x503] != 0x54 || g_ram[0x506] != 0xab || g_ram[0x507] != 0x6b)
    abort();
  uint8_t source = g_ram[0x505], destination = g_ram[0x504];
  cpu_write8(cpu, 0, cpu->S--, cpu->DB);
  unsigned count = (unsigned)cpu->A + 1u;
  for (unsigned i = 0; i < count; ++i) {
    cpu_write8(cpu, destination, cpu->Y++, cpu_read8(cpu, source, cpu->X++));
  }
  cpu->A = 0xffff;
  cpu->DB = cpu_read8(cpu, 0, ++cpu->S);
  cpu->_flag_Z = cpu->DB == 0;
  cpu->_flag_N = (cpu->DB & 0x80) != 0;
  cpu_mirrors_to_p(cpu);
  cpu->S += 3;
  cpu->PB = cpu_read8(cpu, 0, cpu->S);
  return RECOMP_RETURN_NORMAL;
}

RecompReturn Ar2_CopRtl(CpuState *cpu) {
  if (!cop_depth) abort();
  uint16_t pc = cpu_read16(cpu, 0, (uint16_t)(cpu->S + 1));
  cpu->S += 3;
  cpu->PB = cpu_read8(cpu, 0, cpu->S);
  cop_rtl_target = ((uint32_t)cpu->PB << 16) | (uint16_t)(pc + 1);
  cop_returned_by_rtl = 1;
  return RECOMP_RETURN_NORMAL;
}

bool Ar2_CopReturnBoundary(CpuState *cpu) { return cop_depth && cpu->S >= cop_rtl_stack_min; }

RecompReturn Ar2_Cop(CpuState *cpu, uint32_t source, uint32_t *resume) {
  if (cpu->emulation) abort();
  if (cop_depth++) abort();
  cop_returned_by_rtl = 0;
  cop_rtl_stack_min = cpu->S;
  uint8_t hrv = cpu->host_return_valid;
  cpu_push_interrupt_frame(cpu);
  cpu_write16(cpu, 0, (uint16_t)(cpu->S + 2), (uint16_t)(source + 2));
  cpu->_flag_I = 1;
  cpu->_flag_D = 0;
  cpu_mirrors_to_p(cpu);
  cpu->PB = 0x80;
  cpu->host_return_valid = 0;
  RecompReturn result = cpu_dispatch_pc_from(cpu, 0x8082c3, cpu->S, source);
  --cop_depth;
  if (result != RECOMP_RETURN_NORMAL) {
    cpu->host_return_valid = hrv;
    return result;
  }
  /* RTI has consumed its real frame; those bytes still identify the live
   * continuation, including handlers that rewrote/unwound that frame. */
  uint32_t target =
      cpu_read16(cpu, 0, (uint16_t)(cpu->S - 2)) | ((uint32_t)cpu_read8(cpu, 0, cpu->S) << 16);
  if (cop_returned_by_rtl) target = cop_rtl_target;
  cpu->PB = target >> 16;
  cpu->host_return_valid = hrv;
  *resume = target;
  return RECOMP_RETURN_NORMAL;
}
static const RtlGameExecutionApi execution = {
    .struct_size = RTL_GAME_EXECUTION_API_V3_SIZE,
    .run_frame = run_frame,
    .read_rdnmi = read_rdnmi,
    .execution_checkpoint = checkpoint,
    .recover_dispatch_miss = dispatch_miss,
};
static const RtlGameModule module = {
    .abi_version = RTL_GAME_MODULE_ABI_VERSION,
    .struct_size = RTL_GAME_MODULE_V2_SIZE,
    .capabilities = RTL_GAME_MODULE_CAP_IDENTITY | RTL_GAME_MODULE_CAP_EXECUTION,
    .identity = &identity,
    .execution = &execution,
};
static void irq(void *unused, uint32_t line) {
  (void)unused;
  (void)line;
  interrupt(0x00800b);
}
static void require_ok(SrResult r) {
  if (r != SR_RESULT_OK) {
    fprintf(stderr, "runner error %d\n", r);
    abort();
  }
}

/* Every abort in this private module is redirected here at compile time.
 * Failed native execution is discarded, never resumed; the host's AR1
 * runtime and suspended coroutine remain untouched. */
_Noreturn void sr_native_module_abort(void) {
  failed = true;
  if (!error_text[0])
    snprintf(error_text, sizeof(error_text), "AR2 stopped at uncovered code %06x", block_pc);
  if (on_game) {
    for (;;)
      yield_game();
  }
  longjmp(escape, 1);
}

static void close_game(void) {
  if (opened) SnesShutdown();
  Ar2Coroutine_Destroy();
  opened = false;
  started = 0;
  on_game = false;
}

static bool open_game(const uint8_t *rom, size_t size, uint8_t stage) {
  close_game();
  error_text[0] = 0;
  failed = false;
  returned_to_world = false;
  if (size != 0x180000 || (stage != 0x14 && stage != 0x16)) {
    snprintf(error_text, sizeof(error_text), "Unsupported AR2 ROM or entrance");
    return false;
  }
  static const uint8_t expected[32] = {0x71, 0xbd, 0xd4, 0x48, 0xa3, 0x0b, 0x88, 0x72,
                                       0x58, 0x64, 0xe5, 0x55, 0x94, 0xeb, 0xb6, 0x7a,
                                       0x11, 0x8b, 0x1f, 0x19, 0x7a, 0x3f, 0x9e, 0x5d,
                                       0xd3, 0x9d, 0xbf, 0x23, 0x39, 0x9e, 0xfa, 0x15};
  uint8_t digest[32];
  if (!rom || !sr_support_sha256(rom, size, digest) || memcmp(digest, expected, 32)) {
    snprintf(error_text, sizeof(error_text), "Unsupported AR2 ROM digest");
    return false;
  }
  if (setjmp(escape)) return false;
  block_pc = 0;
  in_interrupt = 0;
  cop_depth = 0;
  cop_returned_by_rtl = 0;
  audio_fraction = 0;
  host_frame = 0;
  Ar2Sprites_Reset();
  requested_stage = stage;
  require_ok(RtlRegisterGame(&module));
  if (!SnesInit(rom, size)) {
    snprintf(error_text, sizeof(error_text), "AR2 runtime initialization failed");
    return false;
  }
  opened = true;
  RtlReset(0);
  RtlSetAudioOutputRate(48000);
  return true;
}

static bool step_game(uint32_t input, int extra, bool diorama, ArImportedGameFrame *out) {
  if (!opened || failed || !out || extra < 0 || extra > 128) return false;
  if (setjmp(escape)) return false;
  checkpoints = 0;
  const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  SrRunnerHandle *runner = RtlGameRunner();
  if (requested_stage && warp_ready(api, runner)) {
    queue_stage(requested_stage);
    requested_stage = 0;
  }
  if (requested_stage && host_frame > 600) {
    snprintf(error_text, sizeof(error_text), "AR2 scene loader did not become ready");
    failed = true;
    return false;
  }
  if (!returned_to_world) {
    /* A finite-edge capture can move one margin inward. Keep all actors in
     * that whole view eligible without moving the native gameplay camera. */
    Ar2Sprites_SetMargin(diorama ? 128 : extra);
    RtlRunFrame(requested_stage ? 0 : input);
  }
  if (failed) return false;
  if (returned_to_world) {
    *out = (ArImportedGameFrame){.frame = host_frame,
                                 .scene = ram_word(0x51a),
                                 .health = ram_word(0x91d),
                                 .lives = ram_word(0x91b),
                                 .event = kArImportedGameEvent_ReturnToWorld};
    return true;
  }
  int camera_shift = 0;
  if (Ar2World_SupportsScene(ram_word(0x51a)))
    camera_shift = Ar2World_CaptureShift(ram_word(0x538), ram_word(0x55c) * 16,
                                         diorama ? 128 : extra);
  if (!Ar2Sprites_Publish(runner, api, camera_shift) ||
      !Ar2Capture_Draw(runner, api, g_ram, extra, diorama, camera_shift, irq)) {
    snprintf(error_text, sizeof(error_text), "AR2 frame capture failed");
    failed = true;
    return false;
  }
  audio_fraction += UINT64_C(48000) * RTL_NTSC_FRAME_INTERVAL_NS;
  unsigned samples = audio_fraction / UINT64_C(1000000000);
  audio_fraction %= UINT64_C(1000000000);
  RtlRenderAudio(pcm, samples, 2);
  const unsigned player = ram_word(0x649);
  *out = (ArImportedGameFrame){.video = *Ar2Capture_Frame(),
                               .pcm = pcm,
                               .pcm_frames = samples,
                               .frame = host_frame++,
                               .scene = ram_word(0x51a),
                               .player_x = ram_word(player),
                               .player_y = ram_word(player + 2),
                               .health = ram_word(0x91d),
                               .lives = ram_word(0x91b)};
  return !failed;
}

static const char *last_error(void) { return error_text; }
static bool snapshot(const char *prefix) {
  if (!opened || !prefix) return false;
  bool ok = SnesSnapshot_Write(prefix, RtlGameRunner(), sr_runner_get_api(SR_RUNNER_ABI_VERSION));
  char path[4096];
  if (snprintf(path, sizeof(path), "%s.cpu.json", prefix) >= (int)sizeof(path)) return false;
  FILE *file = sr_fopen(path, "w");
  if (!file) return false;
  fprintf(file, "{\"game\":\"actraiser2\",\"frame\":%d,\"scene\":%u,\"block_pc\":%u,"
      "\"a\":%u,\"x\":%u,\"y\":%u,\"s\":%u,\"d\":%u,\"db\":%u,\"pb\":%u,"
      "\"m\":%u,\"x_flag\":%u,\"failed\":%s}\n",
      host_frame, ram_word(0x51a), block_pc, g_cpu.A, g_cpu.X, g_cpu.Y, g_cpu.S,
      g_cpu.D, g_cpu.DB, g_cpu.PB, g_cpu.m_flag, g_cpu.x_flag, failed ? "true" : "false");
  ok &= !ferror(file);
  return fclose(file) == 0 && ok;
}
static const ArImportedGameApi imported_api = {
    .version = AR_IMPORTED_GAME_ABI,
    .size = sizeof(ArImportedGameApi),
    .open = open_game,
    .step = step_game,
    .close = close_game,
    .error = last_error,
    .snapshot = snapshot,
};
AR_IMPORTED_GAME_EXPORT const ArImportedGameApi *ArImportedGame_GetApi(uint32_t version) {
  return version == AR_IMPORTED_GAME_ABI ? &imported_api : NULL;
}
