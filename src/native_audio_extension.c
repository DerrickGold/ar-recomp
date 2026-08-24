#include "native_audio_extension.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"
#include "snes/apu.h"
#include "snes/dsp.h"
#include "snes/spc.h"

enum {
  kEventTrack = 0x10,
  kOrdinarySfxTrack = 0x12,
  kEventHardwareVoice = 6,
  kOrdinarySfxHardwareVoice = 7,
  kEventVirtualVoice = 8,
  kOrdinarySfxVirtualVoice = 9,
  kEventMask = 0x40,
  kOrdinarySfxMask = 0x80,
  kEffectMask = kEventMask | kOrdinarySfxMask,
  kCurrentTrackMaskAddress = 0x47,
  kEffectOwnershipMaskAddress = 0x1a,
};

static bool s_enabled;
static bool s_log;
static bool (*s_previous_filter)(Apu *, uint8_t, uint8_t *);
static void (*s_previous_opcode_patch)(Spc *, uint16_t);
static uint8_t s_sequence_track = 0xff;
static uint8_t s_sequence_mask;
static uint8_t s_song_kon_pending_mask;
static uint8_t s_logged_routed_mask[0x80];
static uint8_t s_logged_routed_value[0x80];
static bool s_logged_routed_seen[0x80];

static bool IsPerVoiceWritable(uint8_t addr) {
  return addr < 0x80 && (addr & 0x0f) < 8;
}

static bool IsVoiceMaskRegister(uint8_t addr) {
  return addr == 0x2d || addr == 0x3d || addr == 0x4d ||
      addr == 0x4c || addr == 0x5c;
}

bool NativeAudioExtension_RouteVoiceWrite(
    uint8_t dsp_addr, uint8_t logical_track, uint8_t track_mask,
    uint8_t ownership_mask, int *hardware_voice, int *virtual_voice) {
  if (!IsPerVoiceWritable(dsp_addr)) return false;
  const int physical = dsp_addr >> 4;
  const uint8_t expected_mask = (uint8_t)(1u << physical);
  if (track_mask != expected_mask) return false;

  int mapped = -1;
  if (logical_track == kEventTrack &&
      physical == kEventHardwareVoice) {
    mapped = kEventVirtualVoice;
  } else if (logical_track == kOrdinarySfxTrack &&
             physical == kOrdinarySfxHardwareVoice) {
    mapped = kOrdinarySfxVirtualVoice;
  } else if (logical_track <= 0x0e && !(logical_track & 1) &&
             (logical_track >> 1) == physical) {
    return false; /* proven song write, even while the lane is owned */
  } else if (physical >= kEventHardwareVoice &&
             (ownership_mask & expected_mask)) {
    /* Shared driver helpers repurpose X for a DSP register address. The
     * ownership bit remains the proof that their current destination is the
     * effect lane, matching the mixer classifier's conservative fallback. */
    mapped = physical == kEventHardwareVoice
        ? kEventVirtualVoice : kOrdinarySfxVirtualVoice;
  }
  if (mapped < 0) return false;
  if (hardware_voice) *hardware_voice = physical;
  if (virtual_voice) *virtual_voice = mapped;
  return true;
}

uint8_t NativeAudioExtension_RoutedGlobalMask(
    uint8_t logical_track, uint8_t track_mask, uint8_t ownership_mask) {
  if (logical_track == kEventTrack && track_mask == kEventMask)
    return kEventMask;
  if (logical_track == kOrdinarySfxTrack &&
      track_mask == kOrdinarySfxMask)
    return kOrdinarySfxMask;
  if (logical_track <= 0x0e && !(logical_track & 1) &&
      track_mask == (uint8_t)(1u << (logical_track >> 1)))
    return 0; /* proven song mask write */

  /* The central DSP-mask flush at $0458 has already repurposed X and clears
   * $47. At that point $1A is the only authoritative split. A nonzero $47
   * fallback is used only when it agrees with ownership. */
  if (track_mask == 0)
    return ownership_mask & kEffectMask;
  return track_mask & ownership_mask & kEffectMask;
}

bool NativeAudioExtension_ShouldBypassMusicSuppression(
    uint16_t spc_pc, uint8_t logical_track, uint8_t track_mask,
    uint8_t ownership_mask) {
  if (spc_pc != 0x04d4 && spc_pc != 0x05b6 && spc_pc != 0x080e)
    return false;
  if (logical_track > 0x0e || (logical_track & 1)) return false;
  const uint8_t song_mask = (uint8_t)(1u << (logical_track >> 1));
  return track_mask == song_mask && (ownership_mask & song_mask) != 0;
}

static bool RouteDspWrite(Apu *apu, uint8_t addr, uint8_t *value) {
  if (s_previous_filter && !s_previous_filter(apu, addr, value))
    return false;
  if (!s_enabled || !apu || !apu->dsp || !apu->spc || !value)
    return true;

  const uint8_t logical_track = apu->spc->x;
  const uint8_t track_mask = apu->ram[kCurrentTrackMaskAddress];
  const uint8_t ownership = apu->ram[kEffectOwnershipMaskAddress];
  s_song_kon_pending_mask &= ownership;
  /* $080A still has the sequencer track in X. Its shared writer at $0834
   * replaces X with $64-$67/$74-$77 and can temporarily see $1A clear. Carry
   * the proven track across that helper so SRCN/ADSR/GAIN follow the same
   * route as pitch/volume instead of leaking back onto song voice 6/7. */
  uint8_t routed_track = logical_track;
  if (IsPerVoiceWritable(addr) && logical_track == addr &&
      track_mask == s_sequence_mask)
    routed_track = s_sequence_track;
  int hardware_voice = -1;
  int virtual_voice = -1;
  if (NativeAudioExtension_RouteVoiceWrite(
          addr, routed_track, track_mask, ownership,
          &hardware_voice, &virtual_voice)) {
    dsp_setVoiceBus(apu->dsp, hardware_voice, kDspVoiceBus_Music);
    dsp_setVoiceBus(apu->dsp, virtual_voice, kDspVoiceBus_Sfx);
    dsp_writeVirtualVoiceRegister(apu->dsp, virtual_voice, addr, *value);
    if (s_log) {
      fprintf(stderr,
              "[audio-ext] voice-write dsp=%02x x=%02x mask=%02x "
              "owner=%02x physical=%d virtual=%d value=%02x\n",
              addr, routed_track, track_mask, ownership,
              hardware_voice, virtual_voice, *value);
    }
    return false;
  }

  if (IsVoiceMaskRegister(addr)) {
    const uint8_t route_mask = NativeAudioExtension_RoutedGlobalMask(
        logical_track, track_mask, ownership);
    if (route_mask) {
      if (route_mask & kEventMask) {
        dsp_setVoiceBus(apu->dsp, kEventVirtualVoice, kDspVoiceBus_Sfx);
        dsp_writeVirtualVoiceControl(
            apu->dsp, kEventVirtualVoice, addr,
            (*value & kEventMask) != 0);
      }
      if (route_mask & kOrdinarySfxMask) {
        dsp_setVoiceBus(
            apu->dsp, kOrdinarySfxVirtualVoice, kDspVoiceBus_Sfx);
        dsp_writeVirtualVoiceControl(
            apu->dsp, kOrdinarySfxVirtualVoice, addr,
            (*value & kOrdinarySfxMask) != 0);
      }
      uint8_t hardware_value = *value;
      uint8_t hardware_update_mask = (uint8_t)~route_mask;
      if (track_mask == 0 && addr == 0x5c) {
        /* The central mask flush's owned bit describes the effect KOF, not
         * the song. Clear the physical KOF latch normally so a direct song
         * KOF allowed through $05B6 cannot remain held until effect end. */
        hardware_value &= (uint8_t)~route_mask;
        hardware_update_mask = 0xff;
      } else if (track_mask == 0 && addr == 0x4c) {
        /* A song note admitted at $04D4 sets the same driver KON bit as the
         * effect. Apply that one proven pending bit to both destinations;
         * otherwise the owned central KON remains virtual-only. */
        hardware_update_mask |= s_song_kon_pending_mask & route_mask;
        s_song_kon_pending_mask &= (uint8_t)~route_mask;
      }
      dsp_writeHardwareVoiceMask(
          apu->dsp, addr, hardware_value, hardware_update_mask);
      const uint8_t routed_value = *value & route_mask;
      if (s_log &&
          (!s_logged_routed_seen[addr] ||
           s_logged_routed_mask[addr] != route_mask ||
           s_logged_routed_value[addr] != routed_value)) {
        fprintf(stderr,
                "[audio-ext] mask-write dsp=%02x x=%02x mask=%02x "
                "owner=%02x routed=%02x value=%02x\n",
                addr, logical_track, track_mask, ownership,
                route_mask, *value);
        s_logged_routed_seen[addr] = true;
        s_logged_routed_mask[addr] = route_mask;
        s_logged_routed_value[addr] = routed_value;
      }
      return false;
    }
  }
  return true;
}

static void PatchSpcOpcode(Spc *spc, uint16_t pc) {
  if (s_previous_opcode_patch)
    s_previous_opcode_patch(spc, pc);
  if (!s_enabled || !spc || !spc->apu) return;
  if (pc == 0x080a) {
    s_sequence_track = spc->x;
    s_sequence_mask = spc->apu->ram[kCurrentTrackMaskAddress];
  }
  if (NativeAudioExtension_ShouldBypassMusicSuppression(
          pc, spc->x, spc->apu->ram[kCurrentTrackMaskAddress],
          spc->apu->ram[kEffectOwnershipMaskAddress])) {
    /* All three sites are BNE instructions whose Z flag was produced by
     * `$1A & $47`. Making only that proven branch fall through allows the
     * already-advancing song track to reach its normal DSP write. */
    spc->z = true;
    if (pc == 0x04d4)
      s_song_kon_pending_mask |=
          spc->apu->ram[kCurrentTrackMaskAddress];
    if (s_log) {
      fprintf(stderr,
              "[audio-ext] preserved song update pc=%04x x=%02x "
              "mask=%02x owner=%02x\n",
              pc, spc->x, spc->apu->ram[kCurrentTrackMaskAddress],
              spc->apu->ram[kEffectOwnershipMaskAddress]);
    }
  }
}

void NativeAudioExtension_Install(void) {
  s_enabled = g_settings.audio_extended_channels;
  const char *log = getenv("AR_AUDIO_EXTLOG");
  s_log = log && log[0] && log[0] != '0';
  s_sequence_track = 0xff;
  s_sequence_mask = 0;
  s_song_kon_pending_mask = 0;
  memset(s_logged_routed_seen, 0, sizeof(s_logged_routed_seen));
  dsp_setExtendedVoicesEnabled(s_enabled);
  if (!s_enabled) {
    fprintf(stderr,
            "[audio-ext] extended sound channels disabled "
            "(authentic 8 voices)\n");
    return;
  }
  s_previous_filter = g_apu_spc_dsp_write_filter_hook;
  g_apu_spc_dsp_write_filter_hook = RouteDspWrite;
  s_previous_opcode_patch = g_spc_opcode_patch_hook;
  g_spc_opcode_patch_hook = PatchSpcOpcode;
  fprintf(stderr, "[audio-ext] extended sound channels enabled (10 voices)\n");
}

bool NativeAudioExtension_IsEnabled(void) {
  return s_enabled;
}
