#include "native_audio_extension.h"

#include "settings.h"
#include "snes/apu.h"
#include "snes/dsp.h"
#include "snes/spc.h"

#include <stdio.h>
#include <string.h>

Settings g_settings;
bool (*g_apu_spc_dsp_write_filter_hook)(Apu *, uint8_t, uint8_t *);
void (*g_spc_opcode_patch_hook)(Spc *, uint16_t);
int (*g_spc_opcode_cycle_hook)(Spc *, uint16_t, int);
void (*g_apu_extra_saveload_hook)(Apu *, SaveLoadInfo *);
void (*g_rtl_spc_upload_hook)(uint32_t);
Snes *g_snes;

void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

static bool s_dsp_enabled;
static int s_bus_voice = -1;
static DspVoiceBus s_bus;
static int s_register_voice = -1;
static uint8_t s_register_source;
static uint8_t s_register_value;
static int s_control_voice = -1;
static uint8_t s_control_addr;
static bool s_control_enabled;
static uint8_t s_hardware_addr;
static uint8_t s_hardware_value;
static uint8_t s_hardware_update_mask;

void dsp_setExtendedVoicesEnabled(bool enabled) { s_dsp_enabled = enabled; }
void dsp_setVoiceBus(Dsp *dsp, int ch, DspVoiceBus bus) {
  (void)dsp;
  s_bus_voice = ch;
  s_bus = bus;
}
void dsp_writeVirtualVoiceRegister(Dsp *dsp, int ch, uint8_t source_addr,
                                   uint8_t val) {
  (void)dsp;
  s_register_voice = ch;
  s_register_source = source_addr;
  s_register_value = val;
}
void dsp_writeVirtualVoiceControl(Dsp *dsp, int ch, uint8_t global_addr,
                                  bool enabled) {
  (void)dsp;
  s_control_voice = ch;
  s_control_addr = global_addr;
  s_control_enabled = enabled;
}
void dsp_writeHardwareVoiceMask(Dsp *dsp, uint8_t addr, uint8_t val,
                                uint8_t update_mask) {
  (void)dsp;
  s_hardware_addr = addr;
  s_hardware_value = val;
  s_hardware_update_mask = update_mask;
}

static int s_failures;
#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

static void TestPureRouting(void) {
  int hardware = -1, virtual_voice = -1;
  CHECK(NativeAudioExtension_RouteVoiceWrite(
      0x64, 0x10, 0x40, 0x00, &hardware, &virtual_voice));
  CHECK(hardware == 6 && virtual_voice == 8);
  CHECK(NativeAudioExtension_RouteVoiceWrite(
      0x75, 0x12, 0x80, 0x00, &hardware, &virtual_voice));
  CHECK(hardware == 7 && virtual_voice == 9);
  CHECK(NativeAudioExtension_RouteVoiceWrite(
      0x64, 0x64, 0x40, 0x40, &hardware, &virtual_voice));
  CHECK(!NativeAudioExtension_RouteVoiceWrite(
      0x64, 0x0c, 0x40, 0x40, &hardware, &virtual_voice));
  CHECK(!NativeAudioExtension_RouteVoiceWrite(
      0x4c, 0x10, 0x40, 0x40, &hardware, &virtual_voice));

  CHECK(NativeAudioExtension_RoutedGlobalMask(0x10, 0x40, 0) == 0x40);
  CHECK(NativeAudioExtension_RoutedGlobalMask(0x12, 0x80, 0) == 0x80);
  CHECK(NativeAudioExtension_RoutedGlobalMask(0x45, 0, 0xc0) == 0xc0);
  CHECK(NativeAudioExtension_RoutedGlobalMask(0x0c, 0x40, 0x40) == 0);
  CHECK(NativeAudioExtension_RoutedGlobalMask(0x0c, 0x40, 0) == 0);

  CHECK(NativeAudioExtension_ShouldBypassMusicSuppression(
      0x04d4, 0x0c, 0x40, 0x40));
  CHECK(NativeAudioExtension_ShouldBypassMusicSuppression(
      0x05b6, 0x0e, 0x80, 0x80));
  CHECK(NativeAudioExtension_ShouldBypassMusicSuppression(
      0x080e, 0x0c, 0x40, 0x40));
  CHECK(!NativeAudioExtension_ShouldBypassMusicSuppression(
      0x04d4, 0x10, 0x40, 0x40));
  CHECK(!NativeAudioExtension_ShouldBypassMusicSuppression(
      0x04d0, 0x0c, 0x40, 0x40));
}

static void TestInstalledBridge(void) {
  g_settings.audio_extended_channels = true;
  NativeAudioExtension_Install();
  CHECK(NativeAudioExtension_IsEnabled());
  CHECK(s_dsp_enabled);
  CHECK(g_apu_spc_dsp_write_filter_hook != NULL);
  CHECK(g_spc_opcode_patch_hook != NULL);

  Apu apu;
  Spc spc;
  Dsp dsp;
  memset(&apu, 0, sizeof(apu));
  memset(&spc, 0, sizeof(spc));
  memset(&dsp, 0, sizeof(dsp));
  apu.spc = &spc;
  apu.dsp = &dsp;
  spc.apu = &apu;
  spc.x = 0x10;
  apu.ram[0x47] = 0x40;

  uint8_t value = 0x09;
  CHECK(!g_apu_spc_dsp_write_filter_hook(&apu, 0x64, &value));
  CHECK(s_register_voice == 8 && s_register_source == 0x64 &&
        s_register_value == 0x09);
  CHECK(s_bus_voice == 8 && s_bus == kDspVoiceBus_Sfx);

  value = 0x40;
  CHECK(!g_apu_spc_dsp_write_filter_hook(&apu, 0x4c, &value));
  CHECK(s_control_voice == 8 && s_control_addr == 0x4c &&
        s_control_enabled);
  CHECK(s_hardware_addr == 0x4c && s_hardware_value == 0x40 &&
        s_hardware_update_mask == 0xbf);

  /* The $0834 helper has replaced X with the DSP address and $1A can be zero;
   * provenance captured at $080A must still carry instrument writes to 8. */
  spc.x = 0x10;
  apu.ram[0x1a] = 0;
  g_spc_opcode_patch_hook(&spc, 0x080a);
  spc.x = 0x64;
  value = 0x0d;
  CHECK(!g_apu_spc_dsp_write_filter_hook(&apu, 0x64, &value));
  CHECK(s_register_voice == 8 && s_register_source == 0x64 &&
        s_register_value == 0x0d);

  spc.x = 0x0c;
  spc.z = false;
  apu.ram[0x1a] = 0x40;
  g_spc_opcode_patch_hook(&spc, 0x04d4);
  CHECK(spc.z);

  /* The following central KON applies the proven pending song bit to physical
   * voice 6 as well as virtual voice 8. */
  spc.x = 0x45;
  apu.ram[0x47] = 0;
  value = 0x40;
  CHECK(!g_apu_spc_dsp_write_filter_hook(&apu, 0x4c, &value));
  CHECK(s_hardware_value == 0x40 && s_hardware_update_mask == 0xff);

  /* Central effect KOF is virtual, but physical KOF must be actively cleared
   * so an earlier direct song KOF cannot stay held for the effect duration. */
  spc.x = 0x46;
  value = 0x40;
  CHECK(!g_apu_spc_dsp_write_filter_hook(&apu, 0x5c, &value));
  CHECK(s_control_voice == 8 && s_control_addr == 0x5c &&
        s_control_enabled);
  CHECK(s_hardware_value == 0 && s_hardware_update_mask == 0xff);
}

static void SetSequencePointer(Apu *apu, uint8_t id, uint16_t pointer) {
  const uint16_t address = (uint16_t)(0x2400 + id * 2);
  apu->ram[address] = (uint8_t)pointer;
  apu->ram[(uint16_t)(address + 1)] = (uint8_t)(pointer >> 8);
}

static void FlushVirtualLifecycle(Apu *apu, Spc *spc) {
  for (int pass = 0; pass < 2; pass++) {
    uint8_t value = 0;
    spc->x = 0x46;
    apu->ram[0x47] = 0;
    CHECK(g_apu_spc_dsp_write_filter_hook(apu, 0x5c, &value));
    spc->x = 0x45;
    CHECK(g_apu_spc_dsp_write_filter_hook(apu, 0x4c, &value));
  }
}

static void TestVirtualLifecycleUsesCentralMaskFlush(void) {
  Apu apu;
  Spc spc;
  Dsp dsp;
  memset(&apu, 0, sizeof(apu));
  memset(&spc, 0, sizeof(spc));
  memset(&dsp, 0, sizeof(dsp));
  apu.spc = &spc;
  apu.dsp = &dsp;
  spc.apu = &apu;
  SetSequencePointer(&apu, 0x10, 0x2676);

  CHECK(NativeAudioExtension_QueueRequest(
      false, 0x10, 0x01bb6d, 150, 1, 2, 0));
  g_spc_opcode_patch_hook(&spc, 0x0da0);
  CHECK(NativeAudioExtension_ActiveInstanceCount() == 1);
  apu.ram[0x45] = 0x80;
  g_spc_opcode_patch_hook(&spc, 0x0f0b);

  /* Sequence work captures the pending key-on, but the virtual voice changes
   * state only when the original driver's central KOF/KON writers run. */
  s_control_voice = -1;
  uint8_t value = 0;
  spc.x = 0x46;
  apu.ram[0x47] = 0;
  CHECK(g_apu_spc_dsp_write_filter_hook(&apu, 0x5c, &value));
  CHECK(s_control_voice == 8 && s_control_addr == 0x5c &&
        s_control_enabled);
  spc.x = 0x45;
  CHECK(g_apu_spc_dsp_write_filter_hook(&apu, 0x4c, &value));
  CHECK(s_control_voice == 8 && s_control_addr == 0x4c &&
        s_control_enabled);

  spc.x = 0x46;
  CHECK(g_apu_spc_dsp_write_filter_hook(&apu, 0x5c, &value));
  CHECK(s_control_voice == 8 && s_control_addr == 0x5c &&
        !s_control_enabled);
  spc.x = 0x45;
  CHECK(g_apu_spc_dsp_write_filter_hook(&apu, 0x4c, &value));
  CHECK(s_control_voice == 8 && s_control_addr == 0x4c &&
        !s_control_enabled);

  g_spc_opcode_patch_hook(&spc, 0x0da0);
  g_spc_opcode_patch_hook(&spc, 0x0e7e);
  CHECK(NativeAudioExtension_ActiveInstanceCount() == 0);

  /* The ending slot remains reserved through KOF true, KOF clear, and the
   * matching KON clear. Releasing sooner can overlap a new KON on voice 8. */
  CHECK(NativeAudioExtension_QueueRequest(
      false, 0x10, 0x01bb6d, 151, 3, 4, 0));
  g_spc_opcode_patch_hook(&spc, 0x0da0);
  CHECK(NativeAudioExtension_ActiveInstanceCount() == 1);
  CHECK(s_bus_voice == 9);
  g_spc_opcode_patch_hook(&spc, 0x0e7e);
  FlushVirtualLifecycle(&apu, &spc);

  CHECK(NativeAudioExtension_QueueRequest(
      false, 0x10, 0x01bb6d, 152, 5, 6, 0));
  g_spc_opcode_patch_hook(&spc, 0x0da0);
  CHECK(NativeAudioExtension_ActiveInstanceCount() == 1);
  CHECK(s_bus_voice == 8);
  g_spc_opcode_patch_hook(&spc, 0x0e7e);
  FlushVirtualLifecycle(&apu, &spc);
}

typedef struct TestSaveLoad {
  SaveLoadInfo base;
  uint8_t bytes[32768];
  size_t offset;
  bool loading;
} TestSaveLoad;

static void TransferTestState(SaveLoadInfo *base, void *data, size_t size) {
  TestSaveLoad *state = (TestSaveLoad *)base;
  CHECK(state->offset + size <= sizeof(state->bytes));
  if (state->offset + size > sizeof(state->bytes)) return;
  if (state->loading)
    memcpy(data, state->bytes + state->offset, size);
  else
    memcpy(state->bytes + state->offset, data, size);
  state->offset += size;
}

static void TestIndependentSequencerInstances(void) {
  Apu apu;
  Spc spc;
  Dsp dsp;
  memset(&apu, 0, sizeof(apu));
  memset(&spc, 0, sizeof(spc));
  memset(&dsp, 0, sizeof(dsp));
  apu.spc = &spc;
  apu.dsp = &dsp;
  spc.apu = &apu;
  SetSequencePointer(&apu, 0x10, 0x2676);
  SetSequencePointer(&apu, 0x03, 0x2478);
  SetSequencePointer(&apu, 0x07, 0x2549);

  CHECK(NativeAudioExtension_QueueRequest(
      false, 0x10, 0x01bb6d, 100, 7, 9, 0));
  CHECK(NativeAudioExtension_QueueRequest(
      false, 0x10, 0x01bb6d, 100, 7, 9, 0));
  CHECK(NativeAudioExtension_QueuedRequestCount() == 1);
  /* Same producer and frame but a different actor remains independent. */
  CHECK(NativeAudioExtension_QueueRequest(
      false, 0x10, 0x01bb6d, 100, 7, 10, 0));
  CHECK(NativeAudioExtension_QueuedRequestCount() == 2);

  g_spc_opcode_patch_hook(&spc, 0x0da0);
  CHECK(NativeAudioExtension_QueuedRequestCount() == 0);
  CHECK(NativeAudioExtension_ActiveInstanceCount() == 2);
  CHECK(spc.pc == 0x0e7f && spc.x == 0x12);
  CHECK(apu.ram[0x00e6] == 0x76 && apu.ram[0x00e7] == 0x26);

  uint8_t value = 0x09;
  CHECK(!g_apu_spc_dsp_write_filter_hook(&apu, 0x74, &value));
  CHECK(s_register_voice == 8);
  apu.ram[0x0082] = 0x55; /* $70 + X=$12 */
  apu.ram[0x45] = 0x80;   /* this instance requested KON */
  g_spc_opcode_patch_hook(&spc, 0x0f0b);
  CHECK(spc.pc == 0x0e7f && spc.x == 0x12);
  value = 0x0a;
  CHECK(!g_apu_spc_dsp_write_filter_hook(&apu, 0x74, &value));
  CHECK(s_register_voice == 9);
  g_spc_opcode_patch_hook(&spc, 0x0e7e);
  CHECK(NativeAudioExtension_ActiveInstanceCount() == 1);

  /* The first instance's full track context was saved and is restored on its
   * next native interpreter tick. */
  g_spc_opcode_patch_hook(&spc, 0x0da0);
  CHECK(apu.ram[0x0082] == 0x55);
  g_spc_opcode_patch_hook(&spc, 0x0e7e);
  CHECK(NativeAudioExtension_ActiveInstanceCount() == 0);

  /* High-bit events are one request but retain the native paired X=$10/$12
   * contexts and delayed second-lane countdown. */
  CHECK(NativeAudioExtension_QueueRequest(
      true, 0x83, 0x00f68c, 101, 0, 0, 0));
  g_spc_opcode_patch_hook(&spc, 0x0da0);
  CHECK(NativeAudioExtension_ActiveInstanceCount() == 2);
  CHECK(spc.x == 0x10 && apu.ram[0x80] == 2);
  g_spc_opcode_patch_hook(&spc, 0x0f0b);
  CHECK(spc.x == 0x12 && apu.ram[0x82] == 3);
  g_spc_opcode_patch_hook(&spc, 0x0e7e);
  CHECK(NativeAudioExtension_ActiveInstanceCount() == 1);
  g_spc_opcode_patch_hook(&spc, 0x0da0);
  g_spc_opcode_patch_hook(&spc, 0x0e7e);
  CHECK(NativeAudioExtension_ActiveInstanceCount() == 0);

  CHECK(NativeAudioExtension_QueueRequest(
      true, 0x07, 0x01902d, 102, 1, 2, 0));
  CHECK(NativeAudioExtension_QueueRequest(
      true, 0x07, 0x01902d, 102, 30, 40, 0));
  CHECK(NativeAudioExtension_QueuedRequestCount() == 1);
  g_spc_opcode_patch_hook(&spc, 0x0da0);
  g_spc_opcode_patch_hook(&spc, 0x0e7e);

  for (uint16_t actor = 0; actor < 3; actor++)
    CHECK(NativeAudioExtension_QueueRequest(
        false, 0x10, 0x01bb6d, 103, actor, 0, 0));
  g_spc_opcode_patch_hook(&spc, 0x0da0);
  CHECK(g_spc_opcode_cycle_hook(&spc, 0x0e7f, 5) == 5);
  g_spc_opcode_patch_hook(&spc, 0x0f0b);
  CHECK(g_spc_opcode_cycle_hook(&spc, 0x0e7f, 5) == 0);
  g_spc_opcode_patch_hook(&spc, 0x0f0b);
  CHECK(g_spc_opcode_cycle_hook(&spc, 0x0e7f, 5) == 0);
  g_spc_opcode_patch_hook(&spc, 0x0e7e);
  g_spc_opcode_patch_hook(&spc, 0x0da0);
  g_spc_opcode_patch_hook(&spc, 0x0e7e);
  g_spc_opcode_patch_hook(&spc, 0x0e7e);
  CHECK(NativeAudioExtension_ActiveInstanceCount() == 0);
  FlushVirtualLifecycle(&apu, &spc);
}

static void TestExtensionStateSerialization(void) {
  Apu apu;
  Spc spc;
  Dsp dsp;
  memset(&apu, 0, sizeof(apu));
  memset(&spc, 0, sizeof(spc));
  memset(&dsp, 0, sizeof(dsp));
  apu.spc = &spc;
  apu.dsp = &dsp;
  spc.apu = &apu;
  SetSequencePointer(&apu, 0x10, 0x2676);

  CHECK(NativeAudioExtension_QueueRequest(
      false, 0x10, 0x01bb6d, 200, 1, 2, 0));
  TestSaveLoad state;
  memset(&state, 0, sizeof(state));
  state.base.func = TransferTestState;
  g_apu_extra_saveload_hook(&apu, &state.base);
  CHECK(state.offset > 0 && state.offset < sizeof(state.bytes));

  g_spc_opcode_patch_hook(&spc, 0x0da0);
  CHECK(NativeAudioExtension_QueuedRequestCount() == 0);
  CHECK(NativeAudioExtension_ActiveInstanceCount() == 1);

  state.offset = 0;
  state.loading = true;
  g_apu_extra_saveload_hook(&apu, &state.base);
  CHECK(NativeAudioExtension_QueuedRequestCount() == 1);
  CHECK(NativeAudioExtension_ActiveInstanceCount() == 0);
  g_spc_opcode_patch_hook(&spc, 0x0da0);
  g_spc_opcode_patch_hook(&spc, 0x0e7e);
  FlushVirtualLifecycle(&apu, &spc);
}

static void TestPoolBackpressureQueuesInsteadOfReplacing(void) {
  Apu apu;
  Spc spc;
  Dsp dsp;
  memset(&apu, 0, sizeof(apu));
  memset(&spc, 0, sizeof(spc));
  memset(&dsp, 0, sizeof(dsp));
  apu.spc = &spc;
  apu.dsp = &dsp;
  spc.apu = &apu;
  SetSequencePointer(&apu, 0x10, 0x2676);

  for (uint16_t actor = 0; actor < kDspExtendedVoiceCount + 1; actor++)
    CHECK(NativeAudioExtension_QueueRequest(
        false, 0x10, 0x01bb6d, 300, actor, 0, 0));
  g_spc_opcode_patch_hook(&spc, 0x0da0);
  CHECK(NativeAudioExtension_ActiveInstanceCount() ==
        kDspExtendedVoiceCount);
  CHECK(NativeAudioExtension_QueuedRequestCount() == 1);
  for (int i = 0; i < kDspExtendedVoiceCount; i++)
    g_spc_opcode_patch_hook(&spc, 0x0e7e);
  CHECK(NativeAudioExtension_ActiveInstanceCount() == 0);
  FlushVirtualLifecycle(&apu, &spc);
  g_spc_opcode_patch_hook(&spc, 0x0da0);
  CHECK(NativeAudioExtension_QueuedRequestCount() == 0);
  CHECK(NativeAudioExtension_ActiveInstanceCount() == 1);
  g_spc_opcode_patch_hook(&spc, 0x0e7e);
}

int main(void) {
  TestPureRouting();
  TestInstalledBridge();
  TestVirtualLifecycleUsesCentralMaskFlush();
  TestIndependentSequencerInstances();
  TestExtensionStateSerialization();
  TestPoolBackpressureQueuesInsteadOfReplacing();
  if (s_failures) {
    fprintf(stderr, "native_audio_extension_test: %d failure(s)\n", s_failures);
    return 1;
  }
  puts("native_audio_extension_test: PASS");
  return 0;
}
