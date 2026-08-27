#include "actraiser_rtl.h"

#include "cpu_state.h"
#include "runner_next.h"
#include "spc_upload.h"

#include <stddef.h>
#include <stdint.h>
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
static SrRunnerHandle *s_runner;

void ActRaiser_SpcUploadBindRunner(SrRunnerHandle *runner) {
  s_runner = runner;
  if (runner == NULL) s_resident_completion_pending = false;
}

static bool upload_context_valid(const SrSpcUploadContext *upload) {
  return upload != NULL &&
      upload->struct_size >= SR_SPC_UPLOAD_CONTEXT_V2_SIZE &&
      upload->apu_ram != NULL &&
      upload->apu_ram_byte_size == SR_APU_RAM_BYTE_COUNT;
}

static bool bootstrap_present(const SrSpcUploadContext *upload) {
  static const uint8 entry[] = {
      0x20, 0xcd, 0xcf, 0xbd, 0xe8, 0x00, 0x5d, 0xaf,
      0xc8, 0xf0, 0xd0, 0xfb, 0xc5, 0xff, 0x11,
  };
  static const uint8 idle[] = {0xeb, 0xfd, 0xf0, 0xfc};
  return upload_context_valid(upload) &&
      memcmp(upload->apu_ram + kBootstrapEntry, entry, sizeof(entry)) == 0 &&
      memcmp(upload->apu_ram + kBootstrapIdle0, idle, sizeof(idle)) == 0;
}

static void finish_bootstrap(SrSpcUploadContext *upload) {
  if (!bootstrap_present(upload)) return;
  upload->control_flags |= SR_SPC_UPLOAD_CONTROL_RUN_UNTIL_PC;
  upload->max_cycles = kBootstrapMaxCycles;
  upload->stop_pc[0] = kBootstrapIdle0;
  upload->stop_pc[1] = kBootstrapIdle1;
  upload->stop_pc_count = 2u;
}

static bool resident_uploader_present(const SrSpcUploadContext *upload) {
  const uint8 *ram;
  if (!upload_context_valid(upload)) return false;
  ram = upload->apu_ram;
  return ram[kResidentResume] == 0xcdu &&
      ram[kResidentResume + 1] == 0x31u &&
      ram[kResidentResume + 2] == 0xd8u &&
      ram[kResidentResume + 3] == 0xf1u &&
      ram[kResidentResume + 4] == 0x6fu;
}

static bool resident_waiting(const SrSpcUploadContext *upload) {
  return resident_uploader_present(upload) &&
      upload->spc_pc >= kResidentWaitLow &&
      upload->spc_pc <= kResidentWaitHigh;
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
                                  const SrSpcUploadContext *upload,
                                  uint32 source24) {
  uint16 destination;
  uint16 last_destination;
  uint16 last_length = 0u;
  size_t pool_offset;
  bool success;
  (void)source24;
  if (cpu == NULL || !upload_context_valid(upload) ||
      upload->rom_data == NULL || upload->rom_byte_size == 0u ||
      upload->rom_byte_size > SIZE_MAX || upload->script_offset > SIZE_MAX)
    return false;
  if (g_last_recomp_func == NULL ||
      strstr(g_last_recomp_func, "9964") == NULL ||
      (upload->entry_point & 0xffu) == 0u)
    return true;
  destination = (uint16)g_ram[kSampleDestinationPointer] |
      ((uint16)g_ram[kSampleDestinationPointer + 1u] << 8);
  last_destination = destination;
  pool_offset = (size_t)(RomPtr(kSamplePoolAddress) - upload->rom_data);
  success = sr_spc_upload_samples(
      upload->rom_data, (size_t)upload->rom_byte_size,
      (size_t)upload->script_offset,
      (uint8)upload->entry_point, pool_offset, destination,
      upload->apu_ram, &last_destination, &last_length);
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

void ActRaiser_SpcUploadCommit(SrSpcUploadContext *upload) {
  if (upload == NULL) return;
  if ((upload->state_flags & SR_SPC_UPLOAD_STATE_INITIAL) != 0u) {
    if (upload->entry_point == kBootstrapEntry) finish_bootstrap(upload);
    return;
  }
  if (!resident_uploader_present(upload)) return;
  if (resident_waiting(upload)) {
    upload->control_flags |= SR_SPC_UPLOAD_CONTROL_SET_PC;
    upload->requested_pc = kResidentResume;
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
  static const uint8 signature[] = {0xcd, 0x31, 0xd8, 0xf1, 0x6f};
  const SnesRunnerApi *api;
  SrSpcPcControlRequest request = {
      .struct_size = SR_SPC_PC_CONTROL_REQUEST_V2_SIZE,
      .expected_pc_low = kResidentWaitLow,
      .expected_pc_high = kResidentWaitHigh,
      .replacement_pc = kResidentResume,
      .expected_aram_address = kResidentResume,
      .expected_aram_count = sizeof(signature),
  };
  SrSpcPcControlResult result = {
      .struct_size = SR_SPC_PC_CONTROL_RESULT_V2_SIZE,
  };
  if (!s_resident_completion_pending || s_runner == NULL) return;
  api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  if (api == NULL ||
      api->struct_size < SNES_RUNNER_API_SPC_CONTROL_SIZE ||
      (api->capabilities & SR_RUNNER_CAP_SPC_CONTROL) == 0u ||
      api->compare_exchange_spc_pc == NULL)
    return;
  memcpy(request.expected_aram, signature, sizeof(signature));
  if (api->compare_exchange_spc_pc(s_runner, &request, &result) ==
          SR_RESULT_OK &&
      (result.flags & SR_SPC_PC_CONTROL_WRITTEN) != 0u) {
    s_resident_completion_pending = false;
  }
}
