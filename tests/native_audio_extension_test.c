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

int main(void) {
  TestPureRouting();
  TestInstalledBridge();
  if (s_failures) {
    fprintf(stderr, "native_audio_extension_test: %d failure(s)\n", s_failures);
    return 1;
  }
  puts("native_audio_extension_test: PASS");
  return 0;
}
