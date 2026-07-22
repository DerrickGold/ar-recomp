#include "sim_phase0_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser_game.h"
#include "snes/ppu.h"

typedef struct SimPhase0TraceKey {
  uint16 game_frame;
  uint16 picker_flag;
  uint16 pending_world_type;
  uint16 miracle_kind;
  uint8 map_group;
  uint8 map_number;
  uint8 bgmode;
  uint8 screen_main;
  uint8 screen_sub;
  uint8 cgwsel;
  uint8 cgadsub;
} SimPhase0TraceKey;

static FILE *g_sim_phase0_trace;
static bool g_sim_phase0_env_checked;
static bool g_sim_phase0_key_valid;
static bool g_sim_phase0_prior_town;
static SimPhase0TraceKey g_sim_phase0_key;

static uint16 ReadMirror16(const uint8 *wram, uint32 address) {
  return (uint16)(wram[address] | (wram[address + 1] << 8));
}

static bool TraceKeysEqual(const SimPhase0TraceKey *a,
                           const SimPhase0TraceKey *b) {
  return a->game_frame == b->game_frame &&
         a->picker_flag == b->picker_flag &&
         a->pending_world_type == b->pending_world_type &&
         a->miracle_kind == b->miracle_kind &&
         a->map_group == b->map_group &&
         a->map_number == b->map_number &&
         a->bgmode == b->bgmode &&
         a->screen_main == b->screen_main &&
         a->screen_sub == b->screen_sub &&
         a->cgwsel == b->cgwsel &&
         a->cgadsub == b->cgadsub;
}

bool SimPhase0Trace_Open(const char *path) {
  SimPhase0Trace_Close();
  g_sim_phase0_env_checked = true;
  if (!path || !path[0]) return false;
  g_sim_phase0_trace = fopen(path, "w");
  if (!g_sim_phase0_trace) {
    fprintf(stderr, "[sim3d-phase0] cannot open %s\n", path);
    return false;
  }
  fprintf(stderr, "[sim3d-phase0] state trace -> %s\n", path);
  return true;
}

void SimPhase0Trace_InitFromEnvironment(void) {
  if (g_sim_phase0_env_checked) return;
  g_sim_phase0_env_checked = true;
  const char *path = getenv("AR_SIM3D_TRACE");
  if (!path || !path[0]) return;
  g_sim_phase0_trace = fopen(path, "w");
  if (!g_sim_phase0_trace) {
    fprintf(stderr, "[sim3d-phase0] cannot open %s\n", path);
    return;
  }
  fprintf(stderr, "[sim3d-phase0] state trace -> %s\n", path);
}

static void WriteCaptureArray(FILE *out, const Ppu *ppu) {
  fputs("[", out);
  for (int source = 0; source < kPpuOverlaySource_Count; source++) {
    const PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
    if (source) fputc(',', out);
    fprintf(out,
            "{\"source\":%d,\"x0\":%d,\"y0\":%d,\"x1\":%d,"
            "\"y1\":%d,\"flags\":%u,\"oam_first\":%u,"
            "\"oam_count\":%u}",
            source, capture->x0, capture->y0, capture->x1, capture->y1,
            (unsigned)capture->flags, (unsigned)capture->oamFirst,
            (unsigned)capture->oamCount);
  }
  fputs("]", out);
}

void SimPhase0Trace_Frame(uint32 host_frame, const uint8 *wram,
                          const Ppu *ppu) {
  SimPhase0Trace_InitFromEnvironment();
  if (!g_sim_phase0_trace || !wram) return;

  uint8 map_group = wram[kActRaiserWram_MapGroup];
  uint8 map_number = wram[kActRaiserWram_CurrentMap];
  bool town = ActRaiser_IsSimulationTown(map_group, map_number);
  if (!town && !g_sim_phase0_prior_town) return;

  SimPhase0TraceKey key = {
    .game_frame = ReadMirror16(wram, kActRaiserWram_GameFrame),
    .picker_flag = ReadMirror16(wram, kActRaiserWram_SimMapPickerFlag),
    .pending_world_type =
        ReadMirror16(wram, kActRaiserWram_SimPendingWorldType),
    .miracle_kind = ReadMirror16(wram, kActRaiserWram_SimMiracleKind),
    .map_group = map_group,
    .map_number = map_number,
    .bgmode = ppu ? ppu->bgmode : 0,
    .screen_main = ppu ? ppu->screenEnabled[0] : 0,
    .screen_sub = ppu ? ppu->screenEnabled[1] : 0,
    .cgwsel = ppu ? ppu->cgwsel : 0,
    .cgadsub = ppu ? ppu->cgadsub : 0,
  };
  if (g_sim_phase0_key_valid && TraceKeysEqual(&key, &g_sim_phase0_key)) {
    g_sim_phase0_prior_town = town;
    return;
  }
  g_sim_phase0_key = key;
  g_sim_phase0_key_valid = true;
  g_sim_phase0_prior_town = town;

  bool picker_active = ActRaiser_SimMapPickerActiveForState(
      map_group, map_number, key.picker_flag);
  /* Phase 0 evidence describes ROM state, not the compiled render policy:
   * "authentic_picker" means $7F:9215 selected a position picker on this
   * frame. Whether those frames actually render flat is a build-time choice
   * (AR_SIM3D_PICKER_TOPDOWN) reported separately in the D1 trace. */
  const char *view = !town ? "out_of_scope" :
      picker_active ? "authentic_picker" : "enhanced_candidate";

  fprintf(g_sim_phase0_trace,
          "{\"host_frame\":%u,\"game_frame\":%u,\"map_group\":%u,"
          "\"map\":%u,\"town\":%s,\"picker_flag\":%u,"
          "\"pending_world_type\":%u,"
          "\"miracle_kind\":%u,"
          "\"picker_active\":%s,\"expected_view\":\"%s\","
          "\"aimed_cell_x\":%u,\"aimed_cell_y\":%u,"
          "\"sim_target_x\":%u,\"sim_target_y\":%u,"
          "\"camera_x\":%u,\"camera_y\":%u,",
          (unsigned)host_frame, (unsigned)key.game_frame,
          (unsigned)map_group, (unsigned)map_number,
          town ? "true" : "false", (unsigned)key.picker_flag,
          (unsigned)key.pending_world_type,
          (unsigned)key.miracle_kind,
          picker_active ? "true" : "false", view,
          (unsigned)ReadMirror16(wram, kActRaiserWram_SimAimedMapCellX),
          (unsigned)ReadMirror16(wram, kActRaiserWram_SimAimedMapCellY),
          (unsigned)ReadMirror16(wram, kActRaiserWram_SimCameraTargetX),
          (unsigned)ReadMirror16(wram, kActRaiserWram_SimCameraTargetY),
          (unsigned)ReadMirror16(wram, kActRaiserWram_Bg1CameraX),
          (unsigned)ReadMirror16(wram, kActRaiserWram_Bg1CameraY));

  if (!ppu) {
    fputs("\"ppu\":null}\n", g_sim_phase0_trace);
    fflush(g_sim_phase0_trace);
    return;
  }

  fprintf(g_sim_phase0_trace,
          "\"ppu\":{\"bgmode\":%u,\"inidisp\":%u,"
          "\"windowsel\":%u,\"wbgobjlog\":%u,"
          "\"screen_main\":%u,\"screen_sub\":%u,"
          "\"window_main\":%u,\"window_sub\":%u,"
          "\"cgwsel\":%u,\"cgadsub\":%u,"
          "\"bg_sc\":[%u,%u,%u,%u],\"bg_tile_adr\":%u,"
          "\"hscroll\":[%u,%u,%u,%u],"
          "\"vscroll\":[%u,%u,%u,%u],\"captures\":",
          (unsigned)ppu->bgmode, (unsigned)ppu->inidisp,
          (unsigned)ppu->windowsel, (unsigned)ppu->wbgobjlog,
          (unsigned)ppu->screenEnabled[0], (unsigned)ppu->screenEnabled[1],
          (unsigned)ppu->screenWindowed[0],
          (unsigned)ppu->screenWindowed[1],
          (unsigned)ppu->cgwsel, (unsigned)ppu->cgadsub,
          (unsigned)ppu->bgXsc[0], (unsigned)ppu->bgXsc[1],
          (unsigned)ppu->bgXsc[2], (unsigned)ppu->bgXsc[3],
          (unsigned)ppu->bgTileAdr,
          (unsigned)ppu->hScroll[0], (unsigned)ppu->hScroll[1],
          (unsigned)ppu->hScroll[2], (unsigned)ppu->hScroll[3],
          (unsigned)ppu->vScroll[0], (unsigned)ppu->vScroll[1],
          (unsigned)ppu->vScroll[2], (unsigned)ppu->vScroll[3]);
  WriteCaptureArray(g_sim_phase0_trace, ppu);
  fputs("}}\n", g_sim_phase0_trace);
  fflush(g_sim_phase0_trace);
}

void SimPhase0Trace_Close(void) {
  if (g_sim_phase0_trace) fclose(g_sim_phase0_trace);
  g_sim_phase0_trace = NULL;
  g_sim_phase0_key_valid = false;
  g_sim_phase0_prior_town = false;
}
