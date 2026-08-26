#include "actraiser_rtl.h"

#ifdef SNESRECOMP_NEXT_COMMON_CPU_INFRA_H

#include "audio_trace.h"
#include "cpu_state.h"
#include "spc_upload.h"
#include "snes/apu.h"
#include "snes/snes.h"
#include "snes/spc.h"

#include <stddef.h>
#include <string.h>

enum {
  kUploadDpPointer = 0xa5,
  kBootstrapEntry = 0x0400,
  kBootstrapIdle0 = 0x0460,
  kBootstrapIdle1 = 0x0462,
  kBootstrapMaxCycles = 131072,
  kSampleDestinationPointer = 0x0358,
  kSamplePoolAddress = 0x088000,
  kResidentWaitLow = 0x0f0e,
  kResidentWaitHigh = 0x0f18,
  kResidentResume = 0x0f48,
};

static bool s_resident_completion_pending;

static size_t rom_size(void) {
  if (g_snes != NULL && g_snes->cart != NULL &&
      g_snes->cart->romSize != 0u)
    return g_snes->cart->romSize;
  return 0x80000u;
}

static bool bootstrap_present(const Apu *apu) {
  static const uint8 entry[] = {
      0x20, 0xcd, 0xcf, 0xbd, 0xe8, 0x00, 0x5d, 0xaf,
      0xc8, 0xf0, 0xd0, 0xfb, 0xc5, 0xff, 0x11,
  };
  static const uint8 idle[] = {0xeb, 0xfd, 0xf0, 0xfc};
  return apu != NULL &&
      memcmp(apu->ram + kBootstrapEntry, entry, sizeof(entry)) == 0 &&
      memcmp(apu->ram + kBootstrapIdle0, idle, sizeof(idle)) == 0;
}

static void finish_bootstrap(Apu *apu) {
  unsigned cycles = 0u;
  if (!bootstrap_present(apu)) return;
  audio_trace_set_producer(AUDIO_TRACE_PRODUCER_CPU);
  while (cycles++ < kBootstrapMaxCycles &&
         apu->spc->pc != kBootstrapIdle0 &&
         apu->spc->pc != kBootstrapIdle1 && !apu->spc->stopped)
    apu_cycle(apu);
  audio_trace_set_producer(AUDIO_TRACE_PRODUCER_UNKNOWN);
}

static bool resident_uploader_present(const Apu *apu) {
  const uint8 *ram;
  if (apu == NULL) return false;
  ram = apu->ram;
  return ram[kResidentResume] == 0xcdu &&
      ram[kResidentResume + 1] == 0x31u &&
      ram[kResidentResume + 2] == 0xd8u &&
      ram[kResidentResume + 3] == 0xf1u &&
      ram[kResidentResume + 4] == 0x6fu;
}

static bool resident_waiting(const Apu *apu) {
  return resident_uploader_present(apu) && apu->spc->pc >= kResidentWaitLow &&
      apu->spc->pc <= kResidentWaitHigh;
}

bool ActRaiser_SpcUploadSource(CpuState *cpu, uint32 *source24) {
  uint16 dp;
  uint16 address;
  if (cpu == NULL || source24 == NULL) return false;
  dp = (uint16)(cpu->D + kUploadDpPointer);
  address = (uint16)g_ram[dp] |
      ((uint16)g_ram[(uint16)(dp + 1u)] << 8);
  *source24 = ((uint32)g_ram[(uint16)(dp + 2u)] << 16) | address;
  return true;
}

bool ActRaiser_SpcUploadCustomize(CpuState *cpu,
                                  const SrSpcUploadResult *upload,
                                  uint32 source24) {
  uint16 destination;
  uint16 last_destination;
  uint16 last_length = 0u;
  size_t pool_offset;
  bool success;
  (void)source24;
  if (cpu == NULL || upload == NULL || g_snes == NULL || g_snes->apu == NULL)
    return false;
  if (g_last_recomp_func == NULL ||
      strstr(g_last_recomp_func, "9964") == NULL ||
      (upload->entry_point & 0xffu) == 0u)
    return true;
  destination = (uint16)g_ram[kSampleDestinationPointer] |
      ((uint16)g_ram[kSampleDestinationPointer + 1u] << 8);
  last_destination = destination;
  pool_offset = (size_t)(RomPtr(kSamplePoolAddress) - g_rom);
  success = sr_spc_upload_samples(
      g_rom, rom_size(), upload->script_offset,
      (uint8)upload->entry_point, pool_offset, destination,
      g_snes->apu->ram, &last_destination, &last_length);
  if (success) {
    uint16 d = cpu->D;
    g_ram[d] = 0u;
    g_ram[(uint16)(d + 1u)] = 0u;
    g_ram[(uint16)(d + 2u)] = (uint8)last_destination;
    g_ram[(uint16)(d + 3u)] = (uint8)(last_destination >> 8);
    g_ram[(uint16)(d + 8u)] = (uint8)last_length;
    g_ram[(uint16)(d + 9u)] = (uint8)(last_length >> 8);
  }
  return success;
}

void ActRaiser_SpcUploadCommit(Apu *apu, uint16 entry_point,
                               bool initial_upload) {
  if (apu == NULL) return;
  if (initial_upload) {
    if (entry_point == kBootstrapEntry) finish_bootstrap(apu);
    return;
  }
  if (!resident_uploader_present(apu)) return;
  if (resident_waiting(apu)) {
    apu->spc->pc = kResidentResume;
    s_resident_completion_pending = false;
  } else {
    s_resident_completion_pending = true;
  }
}

int ActRaiser_SpcUploadStackPop(const CpuState *cpu) {
  (void)cpu;
  return g_last_recomp_func != NULL &&
      strstr(g_last_recomp_func, "9A56") != NULL ? 2 : 3;
}

void ActRaiser_SpcUploaderCompleteTick(void) {
  if (!s_resident_completion_pending || g_snes == NULL) return;
  RtlApuLock();
  if (resident_waiting(g_snes->apu)) {
    g_snes->apu->spc->pc = kResidentResume;
    s_resident_completion_pending = false;
  }
  RtlApuUnlock();
}

#else

/* The legacy runner retains this policy internally until it is retired. */
void ar_uploader_complete_tick(void);

void ActRaiser_SpcUploaderCompleteTick(void) {
  ar_uploader_complete_tick();
}

#endif
