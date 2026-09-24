#include "actraiser_rtl.h"

#include "snesrecomp/game/cpu.h"
#include "snesrecomp/runner.h"
#include "snesrecomp/spc_upload.h"
#include "actraiser/actraiser_regional_media.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8 test_rom[0x100000];
static uint8 test_aram[SR_APU_RAM_BYTE_COUNT];
uint8 g_ram[kSnesWramSize];
const uint8 *g_rom = test_rom;
const char *g_last_recomp_func;

static int failures;
static uint16_t fake_spc_pc;
static int control_calls;
static bool sequence_enabled,sequence_available,sequence_ready=true;
static unsigned sequence_calls;
static uint8_t sequence_data[2197];
bool ActRaiserRegional_BeginSongSequence(unsigned rule,bool *enabled) {
  ++sequence_calls;
  if(!enabled || rule>=2 || !sequence_ready)return false;
  *enabled=sequence_enabled;return true;
}
ArRegionalMediaBytes ActRaiserRegionalMedia_Sequence(unsigned rule,bool enabled) {
  return rule<2 && enabled && sequence_available?
      (ArRegionalMediaBytes){sequence_data,rule?1325u:2197u}:(ArRegionalMediaBytes){0};
}

static void check(bool condition, const char *message) {
  if (condition) return;
  fprintf(stderr, "ActRaiser SPC upload adapter failed: %s\n", message);
  ++failures;
}

uint8 *RomPtr(uint32 address) {
  size_t offset = ((size_t)(address >> 16) & 0x7fu) * 0x8000u +
      (address & 0x7fffu);
  return test_rom + (offset & (sizeof(test_rom) - 1u));
}

static SrResult fake_compare_exchange_spc_pc(
    SrRunnerHandle *runner, const SrSpcPcControlRequest *request,
    SrSpcPcControlResult *result) {
  bool matches;
  (void)runner;
  ++control_calls;
  if (request == NULL || result == NULL ||
      request->struct_size < SR_SPC_PC_CONTROL_REQUEST_V2_SIZE ||
      result->struct_size < SR_SPC_PC_CONTROL_RESULT_V2_SIZE)
    return SR_RESULT_INVALID_ARGUMENT;
  memset(result, 0, SR_SPC_PC_CONTROL_RESULT_V2_SIZE);
  result->struct_size = SR_SPC_PC_CONTROL_RESULT_V2_SIZE;
  result->observed_pc = fake_spc_pc;
  matches = fake_spc_pc >= request->expected_pc_low &&
      fake_spc_pc <= request->expected_pc_high &&
      request->expected_aram_count <= SR_SPC_PC_EXPECTED_ARAM_MAX &&
      memcmp(test_aram + request->expected_aram_address,
             request->expected_aram, request->expected_aram_count) == 0;
  if (matches) {
    fake_spc_pc = request->replacement_pc;
    result->flags = SR_SPC_PC_CONTROL_MATCHED |
                    SR_SPC_PC_CONTROL_WRITTEN;
  }
  result->current_pc = fake_spc_pc;
  return SR_RESULT_OK;
}

const SnesRunnerApi *sr_runner_get_api(uint32_t requested_version) {
  static const SnesRunnerApi api = {
      .abi_version = SR_RUNNER_ABI_VERSION,
      .struct_size = sizeof(SnesRunnerApi),
      .capabilities = SR_RUNNER_CAP_SPC_CONTROL,
      .compare_exchange_spc_pc = fake_compare_exchange_spc_pc,
  };
  return requested_version == SR_RUNNER_ABI_VERSION ? &api : NULL;
}

static SrSpcUploadContext make_upload(void) {
  SrSpcUploadContext upload;
  memset(&upload, 0, sizeof(upload));
  upload.struct_size = SR_SPC_UPLOAD_CONTEXT_V2_SIZE;
  upload.rom_data = test_rom;
  upload.apu_ram = test_aram;
  upload.rom_byte_size = sizeof(test_rom);
  upload.apu_ram_byte_size = sizeof(test_aram);
  return upload;
}

static void install_resident_signature(uint8_t *aram) {
  static const uint8 signature[] = {0xcd, 0x31, 0xd8, 0xf1, 0x6f};
  memcpy(aram + 0x0f48u, signature, sizeof(signature));
}

static void test_source_and_stack_policy(void) {
  CpuState cpu;
  uint32 source = 0u;
  memset(&cpu, 0, sizeof(cpu));
  cpu.D = 0xff80u;
  g_ram[0x0025] = 0x34u;
  g_ram[0x0026] = 0x12u;
  g_ram[0x0027] = 0x08u;
  check(ActRaiser_SpcUploadSource(&cpu, &source) && source == 0x081234u,
        "direct-page source pointer wraps at 16 bits");
  g_last_recomp_func = "Func_02_9A56";
  check(ActRaiser_SpcUploadStackPop(&cpu) == 2,
        "$9A56 wrapper retains its two-byte stack adjustment");
  g_last_recomp_func = "Func_02_9964";
  check(ActRaiser_SpcUploadStackPop(&cpu) == 3,
        "ordinary upload wrappers retain their three-byte adjustment");
}

static void test_sample_pool_policy(CpuState *cpu) {
  SrSpcUploadContext upload = make_upload();
  size_t pool = (size_t)(RomPtr(0x088000u) - test_rom);
  memset(test_rom, 0, sizeof(test_rom));
  memset(test_aram, 0, sizeof(test_aram));
  upload.entry_point = 2u;
  upload.script_offset = 2u;
  test_rom[2] = 1u;
  test_rom[3] = 0u;
  test_rom[pool] = 3u;
  test_rom[pool + 2u] = 0x10u;
  test_rom[pool + 3u] = 0x11u;
  test_rom[pool + 4u] = 0x12u;
  test_rom[pool + 5u] = 2u;
  test_rom[pool + 7u] = 0x20u;
  test_rom[pool + 8u] = 0x21u;
  g_ram[0x0358] = 0xfeu;
  g_ram[0x0359] = 0xffu;
  cpu->D = 0x0100u;
  g_last_recomp_func = "Func_02_9964";
  check(ActRaiser_SpcUploadCustomize(cpu, &upload, 0x088000u),
        "ActRaiser second-stage sample pool is accepted");
  check(test_aram[0xfffe] == 0x20u && test_aram[0xffff] == 0x21u &&
            test_aram[0] == 0x10u && test_aram[2] == 0x12u,
        "sample chunks pack and wrap in ARAM");
  check(g_ram[0x0102] == 0u && g_ram[0x0103] == 0u &&
            g_ram[0x0108] == 3u && g_ram[0x0109] == 0u,
        "upload result metadata is returned through direct page");

  upload.struct_size = sizeof(upload.struct_size);
  check(!ActRaiser_SpcUploadCustomize(cpu, &upload, 0x088000u),
        "undersized upload context is rejected");
}

static void test_bootstrap_and_resident_completion(void) {
  static const uint8 entry[] = {
      0x20, 0xcd, 0xcf, 0xbd, 0xe8, 0x00, 0x5d, 0xaf,
      0xc8, 0xf0, 0xd0, 0xfb, 0xc5, 0xff, 0x11,
  };
  static const uint8 idle[] = {0xeb, 0xfd, 0xf0, 0xfc};
  SrSpcUploadContext upload = make_upload();
  SrRunnerHandle *runner = (SrRunnerHandle *)(uintptr_t)1u;

  memset(test_aram, 0, sizeof(test_aram));
  memcpy(test_aram + 0x0400u, entry, sizeof(entry));
  memcpy(test_aram + 0x0460u, idle, sizeof(idle));
  upload.state_flags = SR_SPC_UPLOAD_STATE_INITIAL;
  upload.entry_point = 0x0400u;
  upload.spc_pc = 0x0400u;
  ActRaiser_SpcUploadCommit(&upload);
  check(upload.control_flags == SR_SPC_UPLOAD_CONTROL_RUN_UNTIL_PC &&
            upload.max_cycles == 131072u &&
            upload.stop_pc_count == 2u &&
            upload.stop_pc[0] == 0x0460u &&
            upload.stop_pc[1] == 0x0462u,
        "initial bootstrap requests a bounded synchronous run to idle");

  upload = make_upload();
  install_resident_signature(test_aram);
  upload.spc_pc = 0x0200u;
  ActRaiser_SpcUploadCommit(&upload);
  ActRaiser_SpcUploadBindRunner(runner);
  fake_spc_pc = 0x0f10u;
  control_calls = 0;
  ActRaiser_SpcUploaderCompleteTick();
  check(fake_spc_pc == 0x0f48u && control_calls == 1,
        "deferred resident upload resumes through atomic SPC control");
  ActRaiser_SpcUploaderCompleteTick();
  check(control_calls == 1,
        "completed resident upload clears its deferred request");
  ActRaiser_SpcUploadBindRunner(NULL);
}

static void test_regional_sequences(void) {
  uint8_t expected[SR_APU_RAM_BYTE_COUNT];
  for(unsigned i=0;i<sizeof(sequence_data);++i)sequence_data[i]=(uint8_t)(i*37+5);
  for(unsigned rule=0;rule<2;++rule)for(unsigned enabled=0;enabled<2;++enabled)
    for(unsigned available=0;available<2;++available)for(unsigned malformed=0;malformed<7;++malformed) {
    CpuState cpu={0};SrSpcUploadContext upload=make_upload();
    const uint32_t source=rule?0x19fa4b:0x0ef69f;
    upload.script_offset=(rule?0xcfa4bu:0x7769fu)+(rule?1435u:2353u);
    upload.entry_point=rule?0x1d06:0x1108;upload.block_count=5;
    memset(test_rom,0,sizeof(test_rom));memset(test_aram,0x5a,sizeof(test_aram));memset(g_ram,0,sizeof(g_ram));
    sequence_enabled=enabled;sequence_available=available;sequence_calls=0;
    g_last_recomp_func="Func_02_9964";
    if(malformed==1)++upload.script_offset;
    if(malformed==2)--upload.block_count;
    if(malformed==3)upload.entry_point^=0x100;
    if(malformed==4)g_last_recomp_func="Func_02_9A56";
    if(malformed==5)upload.entry_point=0;
    memcpy(expected,test_aram,sizeof(expected));
    if(!malformed && enabled && available)memcpy(expected+0x1200,sequence_data,rule?1325:2197);
    check(ActRaiser_SpcUploadCustomize(&cpu,&upload,source+(malformed==6)),"regional sequence upload accepted");
    check(sequence_calls==!malformed,"only the recognized song upload activates its sequence policy");
    check(!memcmp(test_aram,expected,sizeof(expected)),"regional sequence changes only its ARAM span; missing donors preserve US data");
    check(!cpu.A && !cpu.X && !cpu.Y && !cpu.P && !cpu.S && !cpu.D,"sequence projection preserves CPU state");
  }
  sequence_ready=false;sequence_calls=0;
  CpuState cpu={0};SrSpcUploadContext upload=make_upload();
  upload.script_offset=0x7769f+2353;upload.entry_point=0x1108;upload.block_count=5;
  g_last_recomp_func="Func_02_9964";
  memcpy(expected,test_aram,sizeof(expected));
  check(!ActRaiser_SpcUploadCustomize(&cpu,&upload,0x0ef69f),"failed policy activation refuses donor projection");
  check(sequence_calls==1 && !memcmp(expected,test_aram,sizeof(expected)),"failed activation leaves ARAM intact");
  sequence_ready=true;
}

int main(void) {
  CpuState cpu;
  memset(&cpu, 0, sizeof(cpu));
  test_source_and_stack_policy();
  test_sample_pool_policy(&cpu);
  test_bootstrap_and_resident_completion();
  test_regional_sequences();
  if (failures == 0) puts("ActRaiser SPC upload adapter: PASS");
  return failures == 0 ? 0 : 1;
}
