/* ActRaiser frame draw: the per-frame PPU draw transaction
 * (ActRaiserDrawPpuFrame): scanout callbacks, publishing the scanout and the
 * captures, and the order the enhancement passes run in.
 * Phase: game (frame transaction). */
#include "actraiser/enhancements/actraiser_enhancements_internal.h"
#include "host/host_ppu_output.h"
#include "host/host_frame_surfaces.h"

/* The diorama skybox view the last frame published; ActRaiser_LiveDioramaSkybox
 * hands out a copy. */
static SrPpuSurfaceView s_live_diorama_skybox;

void ActRaiser_LiveDioramaSkybox(SrPpuSurfaceView *out) {
  if (out) *out = s_live_diorama_skybox;
}

/* Sim3D reports renderer-local contract state; this host seam owns the policy
 * decision to end the session. Keeping that conversion beside the capture
 * orchestration prevents the low-level renderer from depending on app state. */
void ActRaiser_ReportSim3DCaptureContractFailure(void) {
  switch (Sim3D_GetCaptureContractFailure()) {
    case kSim3DCaptureContract_RendererUnavailable:
      SessionFatal_Request(
          "Simulation town 3D is active, but its core renderer is unavailable. "
          "Restart after checking graphics memory and driver stability, or "
          "disable Simulation town 3D in settings.ini.");
      break;
    case kSim3DCaptureContract_SurfaceAllocation:
      SessionFatal_Request(
          "Simulation town 3D could not allocate or bind its core capture "
          "surfaces. Close other graphics-heavy applications and restart, or "
          "disable Simulation town 3D in settings.ini.");
      break;
    case kSim3DCaptureContract_ObjectSourcesUnavailable:
      SessionFatal_Request(
          "Simulation town 3D lost both of its object-rendering sources. "
          "Restart the game. If this repeats, disable Simulation object "
          "billboards in settings.ini and report the affected town/frame.");
      break;
    case kSim3DCaptureContract_Ok:
    default:
      break;
  }
}

typedef struct ActRaiserPpuScanoutContext {
  unsigned shape_game_frame;
  bool shape_trace;
  ActRaiserPpuShapeRegisters shape_before;
} ActRaiserPpuScanoutContext;

static void ActRaiser_PpuScanoutLineCallback(
    void *user_data, const SrPpuScanoutLineContext *context) {
  ActRaiserPpuScanoutContext *scanout = user_data;
  if (!scanout || !context) return;
  if ((context->flags & SR_PPU_SCANOUT_LINE_BEFORE) != 0u) {
    if (context->line > 0u)
      ActRaiserActionBg_ObserveRoomSceneFrameLine(
          &context->state, context->line - 1u);
    if (scanout->shape_trace)
      ActRaiser_PpuShapeCaptureRegisters(
          &context->state, &scanout->shape_before);
  } else if ((context->flags & SR_PPU_SCANOUT_LINE_AFTER_HDMA) != 0u &&
             scanout->shape_trace) {
    ActRaiser_PpuShapeTraceLine(
        scanout->shape_game_frame, (int)context->line,
        &scanout->shape_before, context);
  }
}

static void ActRaiser_PpuScanoutIrqCallback(
    void *user_data, uint32_t line) {
  CpuRegSnapshot snap;
  const bool observe_interrupt =
      RtlGameEventEnabled(SR_EVENT_MASK_INTERRUPT);
  const uint32 interrupt_pc =
      observe_interrupt ? ActRaiser_LastBlockPc() : 0u;
  const uint16 interrupt_vector =
      g_cpu.emulation ? 0xfffeu : 0xffeeu;
  (void)user_data;
  ActRaiser_SaveRegs(&g_cpu, &snap);
  if (observe_interrupt) {
    ActRaiser_EmitInterrupt(
        SR_INTERRUPT_IRQ, SR_EVENT_INTERRUPT_ENTER, interrupt_pc,
        interrupt_vector, (int32_t)line, "irq");
  }
  cpu_push_interrupt_frame(&g_cpu);
  g_sr_in_interrupt = 1;
  IrqHandler_M1X1(&g_cpu);
  g_sr_in_interrupt = 0;
  ActRaiser_RestoreRegs(&g_cpu, &snap);
  if (observe_interrupt) {
    ActRaiser_EmitInterrupt(
        SR_INTERRUPT_IRQ, SR_EVENT_INTERRUPT_EXIT, interrupt_pc,
        interrupt_vector, (int32_t)line, "irq");
  }
}

static void ActRaiser_ReportVerticalCaptureRows(void) {
  if (ActRaiser_DeveloperFlagEnabled(
          kActRaiserDeveloperFlag_VerticalExtensionLog)) {
    /* Where each destination's content actually LANDED, which is the check that
     * catches the row-origin class of bug: the HUD surfaces are consumed in
     * AUTHENTIC screen space and must not move when the vertical margin
     * changes, while the diorama planes are consumed in CAPTURE space and must
     * move by exactly the margin. Both on one line so a regression in either
     * is one diff apart. */
    int width = kActRaiserAuthenticWidth + 2 * g_ws_extra;
    size_t pitch = (size_t)width * 4;
    int hud0 = -1, hud1 = -1, plane0 = -1, plane1 = -1;
    for (int y = 0; y < kHostDisplayFramebufferHeight; y++) {
      const uint32_t *r =
          (const uint32_t *)(g_hud_bg_pixels + (size_t)y * pitch);
      for (int x = 0; x < width; x++)
        if (r[x]) {
          if (hud0 < 0)
            hud0 = y;
          hud1 = y;
          break;
        }
    }
    const uint8_t *bg2 = g_diorama_layer_pixels[SR_PPU_OVERLAY_BG2];
    /* The plane's own pitch, not the HUD's: the diorama planes are bound
     * apron-wide. A diagnostic that exists to catch origin bugs must not carry
     * one, and the wrong stride would slide its reported rows a little further
     * every row it walked. */
    const size_t plane_pitch =
        ActionApron_SurfacePitch(width, SR_PPU_OBJ_APRON);
    if (bg2)
      for (int y = 0; y < kHostDisplayFramebufferHeight; y++) {
        const uint32_t *r = (const uint32_t *)(bg2 + (size_t)y * plane_pitch);
        for (int x = 0; x < width + (int)SR_PPU_OBJ_APRON * 2; x++)
          if (r[x]) {
            if (plane0 < 0)
              plane0 = y;
            plane1 = y;
            break;
          }
      }
    int margin_top, margin_bottom;
    ActRaiser_LiveVerticalMargins(&margin_top, &margin_bottom);
    fprintf(stderr,
            "[vext-rows] gf=%u top=%d bottom=%d hudbg=[%d..%d] "
            "bg2plane=[%d..%d] "
            "objs_unlocked=%u\n",
            ActRaiser_ReadWram16(kActRaiserWram_GameFrame), margin_top,
            margin_bottom, hud0, hud1, plane0, plane1,
            ActRaiser_TakeVextUnlockedObjects());
  }
}

static void ActRaiser_ReportTitleScanout(const SnesRunnerApi *scanout_api,
                                         SrRunnerHandle *scanout_runner,
                                         bool scanout_ready,
                                         uint8_t hdma_active_mask) {
  /* AR_TITLELOG=1: per-frame title-screen PPU probe (map bytes, BG mode,
   * HDMAEN, Mode-7 matrix, INIDISP) for deriving/validating the settled-logo
   * gate above. Diagnostic only. */
  if (ActRaiser_DeveloperFlagEnabled(kActRaiserDeveloperFlag_TitleLog)) {
    static int last_gf = -1;
    int gf = (int)ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
    if (gf != last_gf) {
      SrPpuStateSnapshot scanout_initial = {
          .struct_size = sizeof(scanout_initial),
      };
      if (scanout_ready &&
          scanout_api->query_ppu_state(scanout_runner, &scanout_initial) ==
              SR_RESULT_OK) {
        last_gf = gf;
        fprintf(stderr,
                "[titlelog] gf=%d $18=%02x $19=%02x bgmode=%02x "
                "hdmaen=%02x m7=[%04x %04x %04x %04x] inidisp=%02x\n",
                gf, g_ram[kActRaiserWram_MapGroup],
                g_ram[kActRaiserWram_CurrentMap],
                scanout_initial.bg_mode_control, hdma_active_mask,
                (uint16)scanout_initial.mode7_matrix[0],
                (uint16)scanout_initial.mode7_matrix[1],
                (uint16)scanout_initial.mode7_matrix[2],
                (uint16)scanout_initial.mode7_matrix[3],
                scanout_initial.display_control);
      }
    }
  }
}

static void ActRaiser_PublishScanout(SrResult scanout_status,
                                     const SrPpuScanoutResult *result,
                                     const SrPpuBackgroundViewRequest *skybox,
                                     bool action) {
  if (skybox->pixels && scanout_status == SR_RESULT_OK &&
      (result->flags & SR_PPU_SCANOUT_BACKGROUND_VIEW_READY)) {
    s_live_diorama_skybox = (SrPpuSurfaceView){
        .flags = SR_PPU_SURFACE_BOUND | SR_PPU_SURFACE_HAS_CONTENT,
        .pixel_format = SR_PPU_PIXEL_FORMAT_ARGB8888_U32,
        .data = (const uint8_t *)skybox->pixels,
        .byte_size = skybox->pitch_bytes * skybox->height,
        .pitch_bytes = skybox->pitch_bytes,
        .width_pixels = skybox->width,
        .height_pixels = skybox->height,
        .origin_x = 0,
        .origin_y = -skybox->screen_y0,
        .scale = 1,
    };
  }
  if (scanout_status != SR_RESULT_OK) {
    SessionFatal_Request(
        "The runner could not execute the PPU scanout ABI service. "
        "Restart after rebuilding the game and runner together.");
  }
  const uint32_t authentic_camera_flags =
      SR_PPU_SCANOUT_AUTHENTIC_CAMERA_BG1 | SR_PPU_SCANOUT_AUTHENTIC_CAMERA_BG2;
  const bool authentic_frame_valid =
      scanout_status == SR_RESULT_OK &&
      (result->flags & SR_PPU_SCANOUT_AUTHENTIC_SURFACE_READY) != 0u &&
      (!action ||
       (result->final_state.flags & SR_PPU_STATE_FORCED_BLANK) != 0u ||
       result->final_state.bg_mode != 1u ||
       (result->flags & authentic_camera_flags) == authentic_camera_flags);
  HostPpuOutput_AuthenticFrameCompleted(authentic_frame_valid);
}

static void ActRaiser_FinishSceneCapture(void) {
  int width = kActRaiserAuthenticWidth + 2 * g_ws_extra;
  /* g_pixels is bound apron-wide; the authentic frame starts kPpuObjApron
   * columns in. Offset the base and pass the real pitch. */
  Sim3D_FinishCapture(g_pixels + ActionApron_DisplayOffset(SR_PPU_OBJ_APRON),
                      ActionApron_SurfacePitch(width, SR_PPU_OBJ_APRON),
                      ActRaiser_ReadWram16(kActRaiserWram_GameFrame));
  ActRaiser_ReportSim3DCaptureContractFailure();
  /* After scanout (the diorama planes only hold this frame's sprites now) and
   * before FrameSlot_Capture publishes them to the presentation path. */
  ActRaiser_DioramaHudObjFinish(width);
  ActRaiser_DioramaDeathHeimHubStatuesFinish(width);
  /* After the HUD-icon promote, not before: that pass PUNCHES the promoted
   * icon out of the OBJ planes, and the apron's claimed-set test reads those
   * planes. Running first would let a hole it is about to punch look like
   * free space. */
  const ActionApronGeometry apron_geom = ActRaiser_ObjApronGeometry();
  ActRaiser_DioramaApronFinish(&apron_geom);
}

static void ActRaiser_PublishFrameCapture(SrResult scanout_status,
                                          const SrPpuScanoutResult *result) {
  /* Latch the margin state the frame was ACTUALLY
   * rendered with, here, rather than letting FrameSlot_Capture read live g_ppu.
   * Between this function and the frame slot capture, the application loop may call
   * HostPpuOutput_Rebind(), whose public margin configuration
   * zeroes both live margins — reading g_ppu later would silently describe a
   * different frame than the pixels came from. The non-diorama rebind gate
   * normally prevents this mismatch; latching at the producer boundary makes
   * that safety independent of the gate. */
  const bool scanned = scanout_status == SR_RESULT_OK;
  ActRaiser_CommitFramePlan(scanned ? (int)result->final_state.margin_left : 0,
                            scanned ? (int)result->final_state.margin_right : 0,
                            scanned ? (int)result->final_state.margin_top : 0,
                            scanned ? (int)result->final_state.margin_bottom : 0);
  ActRaiser_ReportVerticalCaptureRows();
}

static SrResult ActRaiser_DrawPpuFrameTransaction(
    void *user_data, SrRunnerHandle *runner,
    const SrPpuFrameTransactionContext *context);

void ActRaiserDrawPpuFrame(void) {
  ActRaiserSimMenu_ObserveScene(ActRaiser_ReadWram16(kActRaiserWram_MapGroup));
  const PerformanceScope pipeline = PerformanceMetrics_Begin(kPerformance_Ppu);
  const uint8_t map_group = g_ram[kActRaiserWram_MapGroup];
  const uint8_t map_number = g_ram[kActRaiserWram_CurrentMap];
  const bool action = ActRaiser_IsActionMapGroup(map_group);
  const bool sim_town = ActRaiser_IsSimulationTown(map_group, map_number);

  /* Overlay bindings are host-owned and persistent; capture policy is
   * game-owned and rebuilt every frame so no prior mode can leak a region. */
  if (Sim3D_BeginFrame())
    HostPpuOutput_Rebind();
  if (!ActRaiser_ResetPpuFrameCaptures()) {
    SessionFatal_Request(
        "The runner could not reset frame capture policy. Restart after "
        "rebuilding the game and runner together.");
  }
  /* Exact-position overrides have two owners. Action rebuilds them during
   * its object scan and deliberately keeps them over pause/freeze redraws.
   * A sim town rebuilds its own sideband during the composition pass and must
   * likewise keep it over a redraw where the emulated game did not advance.
   * Any other scene clears the prior owner's positions: a stale action value
   * can resurrect a parked sim slot, while a stale sim value can displace the
   * next non-town scene (ledger §34). */
  const ActRaiserExactPositionOwner expected_owner = action
      ? kActRaiserExactPositionOwner_Action
      : sim_town ? kActRaiserExactPositionOwner_Sim
                 : kActRaiserExactPositionOwner_None;
  if (expected_owner == kActRaiserExactPositionOwner_None ||
      ActRaiser_GetExactPositionOwner() != expected_owner) {
    if (!ActRaiser_ClearPpuObjMetadata()) {
      SessionFatal_Request(
          "The runner could not clear stale object metadata. Restart after "
          "rebuilding the game and runner together.");
    }
    ActRaiser_MarkExactPositionOwner(kActRaiserExactPositionOwner_None);
  }
  ActRaiser_ApplyWidescreenPolicy();
  /* Stage D reconnaissance: read-only classification of objects that intersect
   * a live side margin but remain outside the authentic activation window. */
  ActRaiser_WidescreenSpriteActivationProbe();
  /* SPEC-bg-hle BH2 differential observer. Default-off and read-only: even
   * when enabled it compares the pure WRAM world against the native ring but
   * never supplies a tile to scanout. */
  ActRaiserActionBg_ObserveFrame(g_ram, kActRaiserWramSize);
  /* Sky Palace: synthesize only BG2's offscreen margin columns from its ROM
   * source page. The paired restore after scanout preserves UI staging. */
  ActRaiser_WidescreenSkyPalacePrepare(ActRaiser_Runner());
  ActRaiser_WidescreenHudObjPromote();
  /* Manifest-driven HD substitutions (game-assets/manifest.ini) — e.g. the
   * settled title logo. Runs after the HUD/OAM capture policies so a busy
   * source is detected rather than clobbered; entries without host-loaded
   * art never request captures, keeping headless/oracle output authentic. */
  HdReplacements_EvaluateFrame();

  if (!ActRaiser_Runner() || !ActRaiser_RunnerApi() ||
      ActRaiser_RunnerApi()->struct_size <
          SNES_RUNNER_API_PPU_FRAME_TRANSACTION_SIZE ||
      !ActRaiser_RunnerApi()->visit_ppu_frame_transaction) {
    SessionFatal_Request(
        "The runner does not provide coherent PPU frame access. Restart "
        "after rebuilding the game and runner together.");
    goto finish;
  }
  const SrPpuFrameTransactionRequest request = {
    .struct_size = sizeof(request),
    .callback = ActRaiser_DrawPpuFrameTransaction,
  };
  if (ActRaiser_RunnerApi()->visit_ppu_frame_transaction(
          ActRaiser_Runner(), &request) != SR_RESULT_OK) {
    SessionFatal_Request(
        "The runner rejected ActRaiser's PPU frame transaction. Restart "
        "after rebuilding the game and runner together.");
  }
finish:
  /* This function owns the temporary VRAM patch, including rejection before
   * the callback starts. No runner outcome may leave it for the next frame. */
  ActRaiser_WidescreenSkyPalaceRestore(ActRaiser_Runner());
  PerformanceMetrics_End(pipeline);
}

static SrResult ActRaiser_DrawPpuFrameTransaction(
    void *user_data, SrRunnerHandle *runner,
    const SrPpuFrameTransactionContext *context) {
  const uint8_t map_group = g_ram[kActRaiserWram_MapGroup];
  const uint8_t map_number = g_ram[kActRaiserWram_CurrentMap];
  const bool action = ActRaiser_IsActionMapGroup(map_group);
  const SrPpuStateSnapshot *ppu;
  extern bool Diorama_IsActiveThisFrame(void);
  const bool profile_diorama = action && Diorama_IsActiveThisFrame();
  DioramaPerformanceScope producer_setup_performance = {0};
  ActRaiserPpuFrameAccess frame_access = {
    .context = context,
  };
  (void)user_data;
  (void)runner;
  if (!context) return SR_RESULT_INVALID_ARGUMENT;
  s_live_diorama_skybox = (SrPpuSurfaceView){0};
  PerformanceScope pipeline = PerformanceMetrics_Begin(kPerformance_PpuSetup);
  ppu = &context->state;
  for (uint32_t source = 0; source < SR_PPU_OVERLAY_SOURCE_COUNT; source++)
    frame_access.captures[source] =
        ActRaiser_OverlayCaptureState(&context->frame.overlays[source]);
  ActRaiser_BeginPpuFrameAccess(&frame_access);
  if (profile_diorama)
    producer_setup_performance =
        DioramaPerformance_Begin(kDioramaPerformance_ProducerSetup);
  ActRaiser_ClearWidescreenMarginGaps(
      ActRaiser_PendingActionBgPlan()->bound_canvas_to_world, context);

  ActRaiser_PrepareDioramaCapture(ppu);
  ActRaiser_PrepareSceneMasks(map_group, map_number);
  ActRaiser_PrepareTownCapture();

  /* AR_TILE_CENSUS=1: read-only HD tile-pack sizing survey (hd_tile_census.c). */
  HdTileCensus_Frame(ActRaiser_Runner());
  const SnesRunnerApi *scanout_api =
      sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  SrRunnerHandle *scanout_runner = ActRaiser_Runner();
  SrGenerationSnapshot scanout_generations = {
      .struct_size = sizeof(scanout_generations),
  };
  SrPpuScanoutResult scanout_result = {
      .struct_size = sizeof(scanout_result),
  };
  SrResult scanout_status = SR_RESULT_UNAVAILABLE;
  const bool scanout_ready =
      scanout_runner != NULL && scanout_api != NULL &&
      scanout_api->struct_size >= SNES_RUNNER_API_PPU_SCANOUT_SIZE &&
      (scanout_api->capabilities & SR_RUNNER_CAP_PPU_SCANOUT) != 0u &&
      scanout_api->query_generations(
          scanout_runner, &scanout_generations) == SR_RESULT_OK;
  const uint8_t hdma_active_mask = ActRaiser_QueryHdmaActiveMask();
  ActRaiser_ReportTitleScanout(scanout_api, scanout_runner, scanout_ready,
                               hdma_active_mask);
  /* The immutable action-room authority resolves the same persistent raster
   * state from ROM + camera + frame clock. Its default-off shadow is prepared
   * once here, then samples the live registers immediately before each visible
   * line so HDMA timing remains part of the comparison. */
  ActRaiserActionBg_BeginRoomSceneFrame(
      g_ram, kActRaiserWramSize);
  ActRaiserPpuScanoutContext scanout_context = {
      .shape_game_frame =
          (unsigned)ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
  };
  scanout_context.shape_trace =
      ActRaiser_PpuShapeTraceActive(scanout_context.shape_game_frame);
  const bool observe_lines =
      scanout_context.shape_trace ||
      ActRaiserActionBg_RoomSceneFrameObserverActive();
  const SrPpuScanoutRequest scanout_request = {
      .struct_size = sizeof(scanout_request),
      .lifetime_generation = scanout_generations.lifetime_generation,
      .hdma_suppress_mask = 0u,
      .line_callback = observe_lines
          ? ActRaiser_PpuScanoutLineCallback : NULL,
      .irq_callback = ActRaiser_PpuScanoutIrqCallback,
      .user_data = &scanout_context,
  };

  /* Resolve the stable OAM footprint before scanout; the live sprite evaluator
   * writes the selected range to the HUD surface while each line is fetched. */
  ActRaiser_DioramaHudObjPrepare();
  ActRaiser_DioramaDeathHeimEyesPrepare();

  SrPpuBackgroundViewRequest skybox_view = ActRaiser_PrepareSkyboxView(
      context, scanout_api, scanout_ready, profile_diorama);

  DioramaPerformance_End(producer_setup_performance);
  PerformanceMetrics_End(pipeline);
  pipeline = PerformanceMetrics_Begin(kPerformance_PpuScanout);
  DioramaPerformanceScope scanout_performance = {0};
  if (profile_diorama)
    scanout_performance =
        DioramaPerformance_Begin(kDioramaPerformance_Scanout);

  if (scanout_ready) {
    scanout_status = skybox_view.pixels
        ? scanout_api->run_ppu_scanout_with_background_view(
            scanout_runner, &scanout_request, &skybox_view, &scanout_result)
        : scanout_api->run_ppu_scanout(
            scanout_runner, &scanout_request, &scanout_result);
  }
  ActRaiser_PublishScanout(scanout_status, &scanout_result, &skybox_view,
                           action);
  DioramaPerformance_End(scanout_performance);
  PerformanceMetrics_End(pipeline);
  pipeline = PerformanceMetrics_Begin(kPerformance_PpuFinish);
  DioramaPerformanceScope producer_finish_performance = {0};
  if (profile_diorama)
    producer_finish_performance =
        DioramaPerformance_Begin(kDioramaPerformance_ProducerFinish);
  ActRaiser_FinishSceneCapture();
  ActRaiser_PublishFrameCapture(scanout_status, &scanout_result);

  /* Borrowed frame access ends here on every scanout outcome. The caller
   * restores its temporary Sky Palace patch even if this callback is rejected.
   */
  DioramaPerformance_End(producer_finish_performance);
  PerformanceMetrics_End(pipeline);
  ActRaiser_EndPpuFrameAccess();
  return SR_RESULT_OK;
}
