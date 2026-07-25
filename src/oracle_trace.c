#include "oracle_trace.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "actraiser_rtl.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "framedump.h"
#include "run_dir.h"
#include "snes/ppu.h"

enum {
  kWramByteCount = 0x20000,
  kWramLastAddress = kWramByteCount - 1,
  kGameModeAddress = 0x18,
  kGameSubmodeAddress = 0x1a,
  kGameFrameLowAddress = 0x88,
  kGameFrameHighAddress = 0x89,
  kActionStageMode = 0x01,
  kTraceFlushFrameInterval = 30,
  kOraclePathCapacity = 320,
  kUninitializedOption = -1,
  kUninitializedGameFrame = -2,
};

#ifndef _WIN32
static const mode_t kSnapshotDirectoryMode = 0755;
#endif

static FILE *s_wram_trace_file;
static FILE *s_mx_trace_file;
static uint8_t s_previous_wram[kWramByteCount];
static uint32_t s_trace_low_address;
static uint32_t s_trace_high_address = kWramLastAddress;
static bool s_previous_wram_initialized;

static int s_dump_action_frames = kUninitializedOption;
static bool s_first_action_frame_written;
static long s_dump_at_game_frame = kUninitializedGameFrame;
static bool s_vram_dump_list_initialized;
static const char *s_vram_dump_list;
static unsigned s_last_vram_dumped_game_frame = UINT_MAX;
static bool s_mx_trace_initialized;

static unsigned ReadGameFrame(const uint8_t *wram) {
  return (unsigned)wram[kGameFrameLowAddress] |
         ((unsigned)wram[kGameFrameHighAddress] << 8);
}

static bool WriteWramSnapshot(const char *relative_path,
                              const uint8_t *wram) {
  char output_path[kOraclePathCapacity];
  RunDirFile(output_path, sizeof(output_path), "%s", relative_path);
  FILE *output_file = fopen(output_path, "wb");
  if (!output_file) return false;
  const size_t written_bytes =
      fwrite(wram, 1, kWramByteCount, output_file);
  const int close_result = fclose(output_file);
  return written_bytes == kWramByteCount && close_result == 0;
}

static void DumpActionFrame(uint32_t host_frame, const uint8_t *wram) {
  if (s_dump_action_frames == kUninitializedOption)
    s_dump_action_frames = getenv("AR_DUMP_ACT") ? 1 : 0;
  if (!s_dump_action_frames || wram[kGameModeAddress] != kActionStageMode)
    return;

  if (!s_first_action_frame_written) {
    s_first_action_frame_written = true;
    if (WriteWramSnapshot("recomp_act1_first.bin", wram)) {
      fprintf(stderr,
              "[dump-act] FIRST action frame %u -> recomp_act1_first.bin\n",
              host_frame);
    }
  }
  WriteWramSnapshot("recomp_act1.bin", wram);
}

static void DumpRequestedGameFrame(const uint8_t *wram) {
  if (s_dump_at_game_frame == kUninitializedGameFrame) {
    const char *value = getenv("AR_DUMP_AT_GF");
    s_dump_at_game_frame = value ? atol(value) : -1;
  }
  if (s_dump_at_game_frame < 0) return;

  const unsigned game_frame = ReadGameFrame(wram);
  if ((long)game_frame != s_dump_at_game_frame) return;
  if (WriteWramSnapshot("recomp_at.bin", wram))
    fprintf(stderr, "[dump-at-gf] gf=%u -> recomp_at.bin\n", game_frame);
}

static void DumpPpuRegisters(unsigned game_frame) {
  if (!g_ppu) return;
  fprintf(stderr,
          "[ppureg] gf=%u bgmode=$%02x bgsc=[%02x %02x %02x %02x] "
          "bgTileAdr=$%04x\n",
          game_frame, g_ppu->bgmode, g_ppu->bgXsc[0], g_ppu->bgXsc[1],
          g_ppu->bgXsc[2], g_ppu->bgXsc[3], g_ppu->bgTileAdr);
}

static void DumpRequestedVramFrames(const uint8_t *wram) {
  if (!s_vram_dump_list_initialized) {
    s_vram_dump_list_initialized = true;
    s_vram_dump_list = getenv("AR_VRAMDUMP_GF");
  }
  if (!s_vram_dump_list || !s_vram_dump_list[0]) return;

  const unsigned game_frame = ReadGameFrame(wram);
  const char *list_position = s_vram_dump_list;
  while (*list_position) {
    const unsigned requested_frame =
        (unsigned)strtoul(list_position, NULL, 0);
    if (requested_frame == game_frame) {
      if (game_frame != s_last_vram_dumped_game_frame) {
        s_last_vram_dumped_game_frame = game_frame;
        char snapshot_directory[kOraclePathCapacity];
        char snapshot_prefix[kOraclePathCapacity];
        RunDirFile(snapshot_directory, sizeof(snapshot_directory),
                   "snapshots");
#ifndef _WIN32
        mkdir(snapshot_directory, kSnapshotDirectoryMode);
#endif
        RunDirFile(snapshot_prefix, sizeof(snapshot_prefix),
                   "snapshots/vd_gf%u", game_frame);
        ActRaiser_FullSnapshot(snapshot_prefix);
        DumpPpuRegisters(game_frame);
        fprintf(stderr,
                "[vramdump] gf=%u -> "
                "%s.{wram,vram,cgram,oam}.bin\n",
                game_frame, snapshot_prefix);
      }
      return;
    }
    const char *comma = strchr(list_position, ',');
    if (!comma) return;
    list_position = comma + 1;
  }
}

static void WriteMxTrace(uint32_t host_frame, const uint8_t *wram) {
  if (!s_mx_trace_initialized) {
    s_mx_trace_initialized = true;
    const char *output_path = getenv("AR_MX_OUT");
    if (output_path && output_path[0])
      s_mx_trace_file = fopen(output_path, "w");
  }
  if (!s_mx_trace_file) return;

  fprintf(s_mx_trace_file, "%u %d %d %u %u\n", ReadGameFrame(wram),
          g_cpu.m_flag & 1, g_cpu.x_flag & 1, wram[kGameModeAddress],
          wram[kGameSubmodeAddress]);
  if ((host_frame % kTraceFlushFrameInterval) == 0)
    fflush(s_mx_trace_file);
}

static void WriteWramChanges(uint32_t host_frame, const uint8_t *wram) {
  if (!s_wram_trace_file) return;
  if (!s_previous_wram_initialized) {
    const size_t trace_byte_count =
        (size_t)(s_trace_high_address - s_trace_low_address + 1);
    memcpy(s_previous_wram + s_trace_low_address,
           wram + s_trace_low_address, trace_byte_count);
    s_previous_wram_initialized = true;
    return;
  }

  for (uint32_t address = s_trace_low_address;
       address <= s_trace_high_address; address++) {
    if (wram[address] == s_previous_wram[address]) continue;
    fprintf(s_wram_trace_file,
            "{\"f\":%u,\"adr\":\"0x%05x\",\"old\":\"0x%02x\","
            "\"val\":\"0x%02x\"}\n",
            host_frame, address, s_previous_wram[address], wram[address]);
    s_previous_wram[address] = wram[address];
  }
  if ((host_frame % kTraceFlushFrameInterval) == 0)
    fflush(s_wram_trace_file);
}

static void OracleTrace_FrameCallback(uint32_t host_frame,
                                      const uint8_t *wram) {
  DumpActionFrame(host_frame, wram);
  DumpRequestedGameFrame(wram);
  DumpRequestedVramFrames(wram);
  WriteMxTrace(host_frame, wram);
  WriteWramChanges(host_frame, wram);
}

static bool AnySnapshotControlEnabled(void) {
  return getenv("AR_DUMP_ACT") || getenv("AR_DUMP_AT_GF") ||
         getenv("AR_MX_OUT") || getenv("AR_VRAMDUMP_GF");
}

void OracleTrace_Init(void) {
  const char *trace_path = getenv("AR_WRAM_TRACE");
  if (!trace_path || !trace_path[0]) {
    if (AnySnapshotControlEnabled())
      g_framedump_callback = OracleTrace_FrameCallback;
    return;
  }

  const char *low_address = getenv("AR_TRACE_LO");
  const char *high_address = getenv("AR_TRACE_HI");
  if (low_address && low_address[0])
    s_trace_low_address =
        (uint32_t)strtoul(low_address, NULL, 0);
  if (high_address && high_address[0])
    s_trace_high_address =
        (uint32_t)strtoul(high_address, NULL, 0);
  if (s_trace_high_address > kWramLastAddress)
    s_trace_high_address = kWramLastAddress;
  if (s_trace_low_address > s_trace_high_address) {
    fprintf(stderr,
            "AR_WRAM_TRACE: invalid range [0x%05x,0x%05x]\n",
            s_trace_low_address, s_trace_high_address);
    return;
  }

  s_wram_trace_file = fopen(trace_path, "w");
  if (!s_wram_trace_file) {
    fprintf(stderr, "AR_WRAM_TRACE: cannot open %s\n", trace_path);
    return;
  }
  g_framedump_callback = OracleTrace_FrameCallback;
  fprintf(stderr, "[wram-trace] -> %s  range=[0x%05x,0x%05x]\n",
          trace_path, s_trace_low_address, s_trace_high_address);
}

void OracleTrace_Shutdown(void) {
  g_framedump_callback = NULL;
  if (s_wram_trace_file) fclose(s_wram_trace_file);
  if (s_mx_trace_file) fclose(s_mx_trace_file);
  s_wram_trace_file = NULL;
  s_mx_trace_file = NULL;
}
