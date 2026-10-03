#include "diorama/diorama_layer_manifest.h"
/* ActRaiser frame plan: the per-frame presentation decisions for widescreen
 * and diorama (action-BG plans, canvas and vertical margins), the widescreen
 * policy that resolves them before scanout, and the pending/live latch the rest
 * of the runtime reads through the ActRaiser_Live* accessors.
 * Phase: game (frame transaction). */
#include "actraiser/enhancements/actraiser_enhancements_internal.h"

/* Per-frame widescreen policy — the single seam where game mode decides how
 * much of the extra-column budget (g_ws_extra, set at startup from
 * ExtendedAspectRatio) is visible this frame. Phase 1: pillarbox everywhere
 * (authentic 256 columns centered); later phases widen per mode via
 * $18/$19 and clamp per camera/level bounds in the frame-policy transaction.
 * Must run every frame: ppu_reset zeroes the PPU margin fields.
 * AR_WS_SURVEY=1 forces raw symmetric margins in EVERY mode — the Phase-2
 * artifact-survey knob (stale tiles/pop-in expected; not for normal play). */
enum {
  kNoActionBgPlanSource = -1,
};

/* ApplyWidescreenPolicy resolves these before scanout; the draw tail promotes
 * them into the live latch beside the exact margins after the pixels exist.
 * Keeping pending and live values separate prevents a surface rebind between
 * scanout and FrameSlot_Capture from describing the next policy state. */
static ActionBgPlan s_pending_action_bg_plan;
static bool s_pending_bg_capture_pad_to_budget;
static int s_pending_diorama_world_y0;
static int s_pending_diorama_world_height;
static int s_pending_diorama_world_x0;
static int s_pending_diorama_world_width;
static int s_pending_diorama_framing_x;
static int s_pending_diorama_framing_y;

static ActionBgPlan ActRaiser_NativeBgPresentationPlan(void) {
  ActionBgPlan plan;
  ActionBgPlan_InitNative(&plan);
  return plan;
}

/* Project an explicit final PPU policy into a plan without inspecting PPU
 * masks after the fact. This is used for non-action scenes and deliberate
 * global/debug overrides. The normal action path retains the canonical plan
 * verbatim, so map-specific action classification has only one owner. */
static void ActRaiser_ProjectBgPresentationPolicy(
    ActionBgPlan *plan, uint8 clamp, uint8 mirror, uint8 repeat,
    bool bound_canvas_to_world) {
  static bool reported_invalid_policy;
  if (!plan) return;
  ActionBgPresentationPolicy policy = {
    .clamp_layers = clamp,
    .mirror_layers = mirror,
    .repeat_layers = repeat,
    .bound_canvas_to_world = bound_canvas_to_world,
  };
  if (ActionBgPlan_ApplyPresentationPolicy(plan, &policy)) return;

  /* All callers use production masks/bands, so rejection is an invariant
   * failure. Publish a deterministic native plan rather than leaving stale
   * source/edge metadata in FrameSlot, and report it once for diagnosis. */
  ActionBgPlan_InitNative(plan);
  if (!reported_invalid_policy) {
    reported_invalid_policy = true;
    fprintf(stderr,
            "ERROR: invalid background presentation policy; using native plan\n");
  }
}

static SrPpuBackgroundFill ActRaiser_PpuBandFill(
    ActionBgEdgeMode edge) {
  switch (edge) {
    case kActionBgEdge_Transparent:
      return SR_PPU_BACKGROUND_FILL_TRANSPARENT;
    case kActionBgEdge_LiveWorld:
      return SR_PPU_BACKGROUND_FILL_LIVE_WORLD;
    case kActionBgEdge_Clamp:
      return SR_PPU_BACKGROUND_FILL_CLAMP;
    case kActionBgEdge_Mirror:
      return SR_PPU_BACKGROUND_FILL_MIRROR;
    case kActionBgEdge_Repeat:
      return SR_PPU_BACKGROUND_FILL_REPEAT;
    case kActionBgEdge_RawWrap:
    default:
      return SR_PPU_BACKGROUND_FILL_RAW_WRAP;
  }
}

static bool ActRaiser_ProjectBgPresentationBands(
    const ActionBgPresentationPolicy *policy,
    SrPpuFramePolicy *frame_policy,
    SrPpuFramePolicyBand *bands, size_t band_capacity) {
  if (!policy || !frame_policy ||
      policy->band_count > band_capacity)
    return false;
  frame_policy->layer_normal_scroll_mask =
      policy->normal_scroll_layers;
  for (unsigned i = 0; i < policy->band_count; i++) {
    const ActionBgPresentationBand *band = &policy->bands[i];
    bands[i] = (SrPpuFramePolicyBand) {
      .layer = band->layer,
      .y0 = band->y0,
      .y1 = band->y1,
      .fill = ActRaiser_PpuBandFill(band->edge),
      .motion = band->motion == kActionBgMotion_NormalScroll
          ? SR_PPU_BACKGROUND_MOTION_NORMAL_SCROLL
          : SR_PPU_BACKGROUND_MOTION_FILL_RELATIVE,
    };
  }
  frame_policy->bands = policy->band_count ? bands : NULL;
  frame_policy->band_count = policy->band_count;
  return true;
}

static uint32 ActRaiser_BgBandSignature(
    const ActionBgPresentationPolicy *policy) {
  if (!policy) return 0;
  uint32 signature = DETERMINISTIC_HASH_FNV1A32_OFFSET;
  for (unsigned i = 0; i < policy->band_count; i++) {
    const ActionBgPresentationBand *band = &policy->bands[i];
    const uint8 bytes[] = {
      band->layer, band->y0, band->y1,
      (uint8)band->edge, (uint8)band->motion,
    };
    for (unsigned byte = 0; byte < sizeof(bytes); byte++)
      signature = DeterministicHash_Fnv1a32Byte(
          signature, bytes[byte]);
  }
  return signature;
}

/* Build and publish the one canonical action-background decision before any
 * generic/debug presentation override runs. Draft application is explicit:
 * native 4:3 still observes the room for authoring but cannot apply synthetic
 * margin policy, while the normal wide path may A/B its session draft. */
static bool ActRaiser_ResolveActionBgPlan(
    uint8 map_group, uint8 map_number, bool apply_tuner_draft,
    ActionBgPlan *plan, ActionBgPresentationPolicy *presentation) {
  static bool reported_rejected_draft;
  if (!plan || !presentation || !ActRaiserActionBg_BuildCurrentPlan(
          g_ram, kActRaiserWramSize,
          g_settings.ws_bg2_padding, plan, presentation) ||
      !ActionBgTuner_ObservePlan(
          map_group, map_number, plan,
          (ActionBgTunerLimits) {
            SR_PPU_HORIZONTAL_MARGIN_MAX, SR_PPU_HORIZONTAL_MARGIN_MAX,
            SR_PPU_VERTICAL_MARGIN_MAX, SR_PPU_VERTICAL_MARGIN_MAX,
          }))
    return false;
  if (apply_tuner_draft && !ActionBgTuner_ApplyDraft(plan) &&
      !reported_rejected_draft) {
    reported_rejected_draft = true;
    fprintf(stderr,
            "[action-bg-tuner] rejected stale/invalid draft; using canonical "
            "room policy\n");
  }
  return ActionBgPlan_CompilePresentation(plan, presentation);
}

/* Resolve the finite playfield's remaining horizontal world space without
 * applying it. Keeping this calculation separate leaves the ordered PPU
 * setters and final immutable plan handoff together in the caller. */
static bool ActRaiser_CalculateCanvasMargins(
    uint8 map_group, uint8 map_number, int canvas_layer, int budget,
    int *margin_left, int *margin_right) {
  if (!margin_left || !margin_right || canvas_layer < 0 ||
      canvas_layer >= kActionBgPlanLayerCount || budget < 0)
    return false;
  const int layer_offset = canvas_layer * kActRaiserBgLayerStateStride;
  const int camera_x = ActRaiser_ReadWram16(
      kActRaiserWram_Bg1CameraX + layer_offset);
  int world_width = ActRaiser_IsSimulationTown(map_group, map_number)
      ? kActRaiserTownWorldWidth
      : ActRaiser_ReadWram16(kActRaiserWram_Bg1Width + layer_offset);
  int world_x0 = 0;
  if (Diorama_IsActiveThisFrame())
    ActRaiserActionBg_ResolveDioramaHorizontalExtent(
        ActRaiser_CurrentVirtualLayerRoom(), (unsigned)canvas_layer,
        world_width, &world_x0, &world_width);
  int available_left = camera_x - world_x0;
  int available_right =
      world_x0 + world_width - kActRaiserAuthenticWidth - camera_x;
  if (available_left < 0) available_left = 0;
  if (available_right < 0) available_right = 0;
  *margin_left = available_left < budget ? available_left : budget;
  *margin_right = available_right < budget ? available_right : budget;
  return true;
}

static void ActRaiser_LogWidescreenLayers(void) {
  if (!ActRaiser_DeveloperFlagEnabled(
          kActRaiserDeveloperFlag_WidescreenLayerLog))
    return;
  static int last_frame = -1;
  const unsigned game_frame =
      ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
  if ((int)game_frame == last_frame) return;
  last_frame = (int)game_frame;
  SrPpuStateSnapshot ppu;
  if (!ActRaiser_QueryPpuState(&ppu)) return;
  fprintf(stderr,
          "[ws-layers] gf=%u mode=%d main=%02x sub=%02x "
          "wsel=%06x cgwsel=%02x cgadsub=%02x",
          game_frame, ppu.bg_mode,
          ppu.main_screen, ppu.sub_screen,
          ppu.window_select, ppu.color_math_control,
          ppu.color_math_designation);
  for (int layer = 0; layer < 4; layer++)
    fprintf(stderr, " BG%d[w%d h%02x hs=%d]", layer + 1,
            ppu.background_tilemap_control[layer] & 1,
            ppu.background_tilemap_control[layer] & 0xfc,
            ppu.backgrounds[layer].h_scroll);
  fprintf(stderr, " win1=[%d,%d] win2=[%d,%d]\n",
          ppu.window1_left, ppu.window1_right,
          ppu.window2_left, ppu.window2_right);
}

/* The compositor writes only the active window. Finite action/town worlds can
 * leave steady strips at the framebuffer edges, so clear them every frame;
 * other modes retain the change-triggered full clear. */
void ActRaiser_ClearWidescreenMarginGaps(
    bool bounded_world_margins,
    const SrPpuFrameTransactionContext *context) {
  static int last_left = -1, last_right = -1;
  if (!context) return;
  const int left = context->state.margin_left;
  const int right = context->state.margin_right;
  if (bounded_world_margins && context->main.data) {
    /* In flat mode these strips are intentional black pillarbox. In Diorama
     * the framebuffer is an opaque backdrop plane, so use its own backdrop
     * colour when the accepted margin repair is active. */
    const uint32 gap_fill =
        (g_settings.diorama_mode && g_settings.diorama_margin_fix)
            ? ActRaiser_BackdropArgb(
                  context->cgram.data[0], context->state.brightness)
            : 0u;
    ActRaiserFillMarginGaps(
        context->main.data, (size_t)context->main.pitch_bytes,
        kActRaiserAuthenticHeight + context->state.margin_top +
            context->state.margin_bottom, context->frame.margin_budget,
        left, right, gap_fill);
    last_left = left;
    last_right = right;
  } else if (left != last_left || right != last_right) {
    last_left = left;
    last_right = right;
    if (context->main.data)
      memset(context->main.data, 0,
             (size_t)context->main.pitch_bytes * kActRaiserAuthenticHeight);
  }
}

/* Per-frame VERTICAL margin policy — the transpose of the bounded-world side
 * margin clamp in ActRaiser_ApplyWidescreenPolicy, and deliberately built the
 * same way: ask the game's own camera and layer-dimension state how much world
 * actually exists past the viewport edge, and never request more than that.
 * Unavailable lower rows are spent above (and vice versa), keeping the capture
 * window stable near an edge instead of discarding world context on landing.
 *
 * The camera routine at $02:B091 clamps V to [0, $30 - $E1] with $E1 = 225 --
 * the hardcoded viewport height, exactly as the H clamp's $100 is the
 * hardcoded 256 width (rendering-engine.md §6). So `camera_y` IS the number of
 * world rows above the viewport, and `height - 225 - camera_y` the number
 * below. At the top of a level both the camera and the available margin are 0,
 * which is what stops the band from showing the void the level ends at.
 *
 * Action stages only. Simulation towns get their 3D treatment from sim3d.c and
 * the world map is Mode 7, whose per-scanline matrix cannot be extrapolated
 * past the visible band (see PpuDrawBackground_mode7). */
static void ActRaiser_ResolveVerticalMarginPolicy(
    uint8_t map_group, uint8_t map_number,
    SrPpuFramePolicy *frame_policy) {
  extern bool Diorama_IsActiveThisFrame(void);

  int extra_top = 0;
  int extra_bottom = 0;
  s_pending_diorama_world_y0 = 0;
  s_pending_diorama_world_height = 0;
  s_pending_diorama_world_x0 = 0;
  s_pending_diorama_world_width = 0;
  s_pending_diorama_framing_x = 0;
  s_pending_diorama_framing_y = 0;
  const DioramaRoomOverride *room = ActRaiser_CurrentVirtualLayerRoom();
  if (Diorama_IsActiveThisFrame() && room && room->framing[0].set) {
    s_pending_diorama_framing_x = room->framing[0].x;
    s_pending_diorama_framing_y = room->framing[0].y;
  }
  DisplayGeometry_SetVertical(0, 0);
  if (!frame_policy) return;

  const bool automatic = g_settings.extended_aspect == kScreenAspect_Auto;
  int budget = automatic
      ? g_actraiser_display_geometry->auto_vertical_budget
      : g_settings.diorama_vertical_extend;
  const int primary_layer =
      ActionBgPlan_PrimaryLayer(&s_pending_action_bg_plan);
  const int playfield = ActionBgPlan_PlayfieldLayer(&s_pending_action_bg_plan);
  if (Diorama_IsActiveThisFrame() && g_ws_active && playfield >= 0) {
    const ActionBgLayerPlan *layer = &s_pending_action_bg_plan.layer[playfield];
    if (layer->source == kActionBgSource_WorldMap && !layer->wrap_world_x &&
        layer->default_edge == kActionBgEdge_LiveWorld && !layer->band_count &&
        layer->horizontal_extent.mode == kActionBgExtent_Available)
      ActRaiserActionBg_ResolveDioramaHorizontalExtent(
          room, (unsigned)playfield, layer->world_width,
          &s_pending_diorama_world_x0, &s_pending_diorama_world_width);
  }
  const uint32_t ppu_display = RtlGamePpuDisplayState();
  if (budget > 0 && (automatic || Diorama_IsActiveThisFrame()) &&
      (RTL_GAME_PPU_BG_MODE_CONTROL(ppu_display) & 7u) != 7u &&
      ActRaiser_IsActionMapGroup(map_group) &&
      !ActRaiser_IsSimulationTown(map_group, map_number) &&
      primary_layer >= 0) {
    if (budget > (int)SR_PPU_VERTICAL_MARGIN_TOTAL_MAX / 2)
      budget = (int)SR_PPU_VERTICAL_MARGIN_TOTAL_MAX / 2;
    const int layer_offset =
        primary_layer * kActRaiserBgLayerStateStride;
    ActRaiserActionBg_ResolveDioramaVerticalExtent(
        ActRaiser_CurrentVirtualLayerRoom(), (unsigned)primary_layer,
        ActRaiser_ReadWram16(kActRaiserWram_Bg1Height + layer_offset),
        &s_pending_diorama_world_y0, &s_pending_diorama_world_height);
    (automatic ? ActRaiserActionBg_ResolveVerticalMargins
               : ActRaiserActionBg_ResolveVerticalCaptureMargins)(
        ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraY + layer_offset) -
            s_pending_diorama_world_y0,
        s_pending_diorama_world_height,
        budget, &extra_top, &extra_bottom);
  }
  DisplayGeometry_SetVertical(extra_top, extra_bottom);
  frame_policy->margin_top_pixels = (uint32_t)g_ws_extra_top;
  frame_policy->margin_bottom_pixels = (uint32_t)g_ws_extra_bottom;
  /* The role catalog chooses the primary plane that governs capture height,
   * but every layer has its OWN camera. Clip each layer independently
   * before its camera reaches row 0: otherwise a BG2 at Y=0 wraps negative
   * synthetic lines to the bottom of its tilemap while the playfield
   * legitimately extends above the viewport. Fillmore act 2 exposed that as
   * red BG2 geometry half-added over its grey BG1 castle wall. Keep these
   * clips native even when authored bounds expand the capture: pasted tiles
   * fill the extra rows after scanout, without wrapping the native map. */
  if (g_ws_extra_top > 0 || g_ws_extra_bottom > 0) {
    for (int layer = 0; layer < kActionBgPlanLayerCount; layer++) {
      const int offset = layer * kActRaiserBgLayerStateStride;
      int top_rows = 0, bottom_rows = 0;
      ActRaiserActionBg_ResolveVerticalMargins(
          ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraY + offset),
          ActRaiser_ReadWram16(kActRaiserWram_Bg1Height + offset),
          g_ws_extra_top > g_ws_extra_bottom ? g_ws_extra_top : g_ws_extra_bottom,
          &top_rows, &bottom_rows);
      frame_policy->vertical_clip_layer_mask |= 1u << layer;
      frame_policy->vertical_clip_top_rows[layer] = (uint32_t)top_rows;
      frame_policy->vertical_clip_bottom_rows[layer] =
          (uint32_t)bottom_rows;
    }
    /* BG3 is screen-space HUD/title text, not additional world. Auto must
     * not expose its staged/wrapped tilemap rows above or below the screen. */
    if (automatic)
      frame_policy->vertical_clip_layer_mask |=
          1u << SR_PPU_OVERLAY_BG3; /* Its zero row budgets remain native-only. */
  }

  /* AR_VEXT_TILES=1: dump the primary tilemap ids the band reads next to the
   * first visible row. A filler row is one whose ids do not belong to the
   * surrounding content; merely being uniform is insufficient because a real
   * all-sky row is uniform too (rendering-engine.md §4). */
  if (ActRaiser_DeveloperFlagEnabled(
          kActRaiserDeveloperFlag_VerticalExtensionTileLog) &&
      g_ws_extra_top > 0) {
    SrPpuStateSnapshot ppu;
    SrBorrowedU16Span vram = {.struct_size = sizeof(vram)};
    if (!ActRaiser_QueryPpuState(&ppu) || !ActRaiser_RunnerApi() ||
        !ActRaiser_RunnerApi()->borrow_u16_memory ||
        ActRaiser_RunnerApi()->borrow_u16_memory(
            ActRaiser_Runner(), SR_MEMORY_VRAM, &vram) != SR_RESULT_OK)
      return;
    int base = ppu.backgrounds[primary_layer].tilemap_base_word;
    bool wider = ppu.backgrounds[primary_layer].tilemap_width_tiles == 64u;
    bool higher = ppu.backgrounds[primary_layer].tilemap_height_tiles == 64u;
    const int layer_offset =
        primary_layer * kActRaiserBgLayerStateStride;
    int cam_y = ActRaiser_ReadWram16(
        kActRaiserWram_Bg1CameraY + layer_offset);
    char buf[512];
    int n = 0;
    for (int py = cam_y - g_ws_extra_top; py < cam_y + 16; py += 8) {
      int off = base + (((py >> 3) & 0x1f) << 5);
      if ((py & 0x100) && higher) off += wider ? 0x800 : 0x400;
      unsigned ids[4];
      for (int k = 0; k < 4; k++)
        ids[k] = vram.data[(off + k * 7) & 0x7fff] & 0x3ff;
      n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%03X,%03X,%03X,%03X",
                    py == cam_y ? " | vis:" : (n ? " " : ""),
                    ids[0], ids[1], ids[2], ids[3]);
      if (n >= (int)sizeof(buf) - 32) break;
    }
    fprintf(stderr, "[vext-tiles] gf=%u camY=%4d phase=%3d band: %s\n",
            ActRaiser_ReadWram16(kActRaiserWram_GameFrame), cam_y,
            cam_y & 0xFF, buf);
  }

  if (ActRaiser_DeveloperFlagEnabled(
          kActRaiserDeveloperFlag_VerticalExtensionLog) &&
      primary_layer >= 0) {
    static unsigned last;
    unsigned gf = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
    if (gf != last) {
      SrPpuStateSnapshot ppu;
      if (!ActRaiser_QueryPpuState(&ppu)) return;
      last = gf;
      fprintf(stderr,
              "[vext] gf=%u top=%d layer=%d camY=%d worldH=%d "
              "vscroll=%d/%d screenEn=$%02x tilemap=$%04x\n",
              gf, g_ws_extra_top, primary_layer + 1,
              ActRaiser_ReadWram16(
                  kActRaiserWram_Bg1CameraY +
                  primary_layer * kActRaiserBgLayerStateStride),
              ActRaiser_ReadWram16(
                  kActRaiserWram_Bg1Height +
                  primary_layer * kActRaiserBgLayerStateStride),
              ppu.backgrounds[primary_layer].v_scroll,
              ppu.backgrounds[1].v_scroll,
              ppu.main_screen,
              ppu.backgrounds[primary_layer].tilemap_base_word);
    }
  }
}

const DioramaRoomOverride *ActRaiser_CurrentVirtualLayerRoom(void) {
  if (!g_settings.diorama_mode) return NULL;
  const DioramaRoomOverride *room = DioramaLayerOrder_Find(
      DioramaLayerManifest_Table(), g_ram[kActRaiserWram_MapGroup],
      g_ram[kActRaiserWram_CurrentMap]);
  /* Raw map 0701 is reused after the final boss. The face scene owns BG2SC
   * $70; the sky/cloud return scene switches it to $74. The manifest's virtual
   * face band must not make that later sky foreground-sharp merely because the
   * two scenes share map bytes. */
  SrPpuStateSnapshot ppu;
  if (room && g_ram[kActRaiserWram_MapGroup] == kActRaiserMapGroup_DeathHeim &&
      g_ram[kActRaiserWram_CurrentMap] == kActRaiserDeathHeimMap_Hub &&
      (!ActRaiser_QueryPpuState(&ppu) ||
       ppu.backgrounds[1].tilemap_base_word != 0x7000))
    return NULL;
  return DioramaLayerManifest_TerrainRoom(room, ActRaiserRegional_TerrainSnapshot());
}

static bool ActRaiser_CommitPpuFramePolicy(
    const SrPpuFramePolicy *policy) {
  const SrResult result = RtlGameApplyPpuFramePolicy(policy);
  if (result == SR_RESULT_OK) return true;
  SessionFatal_Request(
      "The runner rejected ActRaiser's frame presentation policy "
      "(error %u).", (unsigned)result);
  return false;
}

void ActRaiser_ApplyWidescreenPolicy(void) {
  SrPpuFramePolicyBand frame_bands[kActionBgPresentationBandMax];
  SrPpuFramePolicy frame_policy = {
    .struct_size = sizeof(frame_policy),
    .horizontal_mode = SR_PPU_HORIZONTAL_MARGIN_CENTERED,
    .margin_budget_pixels = (uint32_t)g_ws_extra,
  };
  static int survey = -1;
  if (survey < 0) {
    const char *e = getenv("AR_WS_SURVEY");
    survey = (e && e[0] && e[0] != '0') ? 1 : 0;
  }
  const uint8 map_group = g_ram[kActRaiserWram_MapGroup];
  const uint8 map_number = g_ram[kActRaiserWram_CurrentMap];
  uint8 hud_split_height = 0;
  uint8 hud_split_left_end = 0;
  uint8 hud_split_right_start = 0;
  uint8 hud_player_row_y = 0;
  uint8 hud_left_only_y = 0;

  /* Host-overlay HUD layout is independent of the world having side margins.
   * BG3 rows 0-3 are extracted into a transparent surface, then the host
   * scales and anchors the status groups after the framebuffer is presented.
   * Keeping this policy ahead of the no-widescreen-budget return is what makes
   * HUD scale work in authentic 4:3 as well as 16:x. With a fixed aspect ratio,
   * Wide Raw remains the unsplit comparison mode. Auto always splits the HUD. */
  if (!survey && (g_settings.extended_aspect == kScreenAspect_Auto ||
                  g_settings.display_mode != kDisplayMode_WideRaw)) {
    if (ActRaiser_IsActionMapGroup(map_group)) {
      /* y=0-19 ACT/TIME/SCORE; y=20-27 player; y=28-39 enemy. */
      hud_split_height = kActRaiserActionHudHeight;
      hud_split_left_end = kActRaiserActionHudLeftEnd;
      hud_split_right_start = kActRaiserActionHudRightStart;
      hud_player_row_y = kActRaiserActionHudPlayerRowY;
      hud_left_only_y = kActRaiserActionHudEnemyRowY;
    } else if (map_group == kActRaiserMapGroup_NonAction &&
               map_number >= kActRaiserSimulationTown_First &&
               map_number <= kActRaiserNonActionMap_SkyPalace) {
      hud_split_height = kActRaiserSimulationHudHeight;
      hud_split_left_end = kActRaiserSimulationHudSplit;
      hud_split_right_start = kActRaiserSimulationHudSplit;
      hud_player_row_y = kActRaiserSimulationHudHeight;
      hud_left_only_y = kActRaiserSimulationHudHeight;
    }
  }
  frame_policy.hud_split_height = hud_split_height;
  frame_policy.hud_left_end_x = hud_split_left_end;
  frame_policy.hud_right_start_x = hud_split_right_start;
  frame_policy.hud_player_row_y = hud_player_row_y;
  frame_policy.hud_left_only_y = hud_left_only_y;

  /* Sky Palace and simulation-town localization owns all 224 authentic BG3
   * rows, not merely the 32-row status band. Flat Diorama has the same
   * full-layer requirement for action title/pause cards. Resolve that one
   * capture height here so later renderer-specific setup only rebinds the
   * destination surface; it must not silently expand policy after another
   * owner has inspected it. */
  const bool scoped_text_scene =
      !survey && (ActRaiserLocalizationRoute_InScope(map_group, map_number) ||
                  (g_settings.localization_presentation &&
                   ActRaiserLocalizationRoute_FixedTextInScope(map_group, map_number)));
  const bool flat_diorama = !survey &&
      ((Diorama_IsActiveThisFrame() && g_settings.diorama_hud_flat) ||
       (g_settings.extended_aspect == kScreenAspect_Auto &&
        ActRaiser_IsActionMapGroup(map_group)));
  const int bg3_capture_height = ArBg3Composite_CaptureHeight(
      &(ArBg3CompositeCaptureInputs){
        .scoped_text_scene = scoped_text_scene,
        .flat_diorama = flat_diorama,
        .hud_split_height = hud_split_height,
        .authentic_height = kActRaiserAuthenticHeight,
      });
  const uint32_t bg3_capture_flags = SR_PPU_OVERLAY_REMOVE_FROM_GAME;

  ActionBgTuner_BeginFrame();
  s_pending_action_bg_plan = ActRaiser_NativeBgPresentationPlan();
  s_pending_bg_capture_pad_to_budget = false;
  if (!g_ws_active) {
    /* BH5 owns eligible authentic world layers independently of presentation
     * width. Keep the native 4:3 path in the same default-off provider census;
     * decorative policy is irrelevant without margins, but its source
     * classification remains the authority for which layers may bind. */
    bool bind_plan = false;
    ActionBgPlan plan;
    if (ActRaiser_IsActionMapGroup(map_group)) {
      ActionBgPresentationPolicy presentation;
      if (ActRaiser_ResolveActionBgPlan(
              map_group, map_number, false,
              &plan, &presentation)) {
        s_pending_action_bg_plan = plan;
        /* No side columns are rendered in this mode. Preserve source ownership
         * for the authentic provider, but describe the executed presentation
         * as raw/live so a mirror decision cannot imply nonexistent padding. */
        ActRaiser_ProjectBgPresentationPolicy(
            &s_pending_action_bg_plan, 0, 0, 0, false);
        bind_plan = true;
      }
    }
    /* Beginning the policy transaction clears any prior frame's virtual maps
     * and layer extents before the optional authentic provider is republished. */
    ActRaiser_ResolveVerticalMarginPolicy(
        map_group, map_number, &frame_policy);
    if (!ActRaiser_CommitPpuFramePolicy(&frame_policy)) return;
    if (bg3_capture_height)
      ActRaiser_ClaimOverlayCapture(
          SR_PPU_OVERLAY_BG3, 0, 0, kActRaiserAuthenticWidth,
          bg3_capture_height, bg3_capture_flags);
    if (bind_plan)
      ActRaiserActionBg_BindPlanWithVirtualLayers(
          g_ram, kActRaiserWramSize, &plan,
          ActRaiser_CurrentVirtualLayerRoom());
    return;
  }
  /* Per-mode widescreen policy (docs/rendering-engine.md section 13). Two knobs per
   * mode: (1) does it use the wide view at all, and (2) a per-layer clamp
   * mask (bit L keeps BG(L+1) at 256) for scenes that mix wide world layers
   * with 256-wide UI/dialog layers whose offscreen tilemap data must not tile
   * into the margins. We keep the classification explicit here (not an
   * auto-heuristic) so we don't touch game code while proving the base recomp
   * accurate — a mis-widened layer is a policy line, not a decode bug.
   * BG3 (layer 2, the HUD) is already margin-clamped by the engine default. */
  int wide = survey;
  uint8 clamp = 0;
  uint8 mirror = 0;
  uint8 repeat = 0;
  ActionBgPresentationPolicy bg_presentation = { 0 };
  int bg_plan_valid = 0;
  int bg_plan_source_bg1 = kNoActionBgPlanSource;
  int bg_plan_source_bg2 = kNoActionBgPlanSource;
  int bg_hle_allowed = 0;
  uint8 bg_hle_bindings = 0;
  ActionBgPlan bg_plan = { 0 };
  bool project_final_bg_policy = false;
  /* True when the current wide world has finite horizontal bounds. The PPU
   * still owns the fixed centering budget; this policy narrows the live left
   * and right margins as the camera approaches either world edge. */
  int bounded_world_margins = 0;
  int canvas_layer = -1;
  if (!survey && map_group == kActRaiserMapGroup_NonAction) {
    switch (map_number) {
      case kActRaiserNonActionMap_SkyPalace:
                            /* Sky Palace hub. BG1 = sky/clouds (wide, clean).
                               BG2 = pillars plus game-owned offscreen dialog
                               staging farther around its 64x64 tilemap. Keep
                               BG2 raw-wide; the render transaction decodes a
                               box-free ROM source into only margin columns.
                               The authentic center retains its BG2 box. */
        wide = 1;
        break;

      case kActRaiserNonActionMap_WorldMap:
                            /* Mode 7 world map: fully wide, no UI layers. */
        wide = 1;
        break;

      case kActRaiserNonActionMap_Title:
        // Title is always not wide screen since the backdrop is black
        wide = 0;
        break;

      case kActRaiserNonActionMap_Fillmore:
      case kActRaiserNonActionMap_Bloodpool:
      case kActRaiserNonActionMap_Kasandora:
      case kActRaiserNonActionMap_Aitos:
      case kActRaiserNonActionMap_Marahna:
      case kActRaiserNonActionMap_Northwall: {
        /* $01:B4C6 clamps camera X ($22) to $0000-$0100, proving 256px of
         * world on either side of the authentic viewport. AR_WS_SIM=0 is the
         * same-binary authentic baseline for town regression captures. BG2 is
         * the bounded dialog/overlay plane and remains center-clamped. The
         * separate ADAD/AE6F and B473 ports use this same $01-$06 range. */
        wide = g_settings.extended_aspect == kScreenAspect_Auto ||
            g_settings.ws_sim;
        bounded_world_margins = wide;
        canvas_layer = kActRaiserPpuLayer_Bg1;
        clamp = kActRaiserBgLayerMask_Bg2;
        break;
      }

      case kActRaiserNonActionMap_Temple:
      // Temple cut scenes don't need wide screen support
      // background is black
        wide = 0;
        break;

      default:              /* title(00), temple cutscene(08),
                               transitions, unknown: pillarbox (the temple is a
                               black backdrop with no wide-worthy layer). */
        wide = 0;
        break;
    }
  } else if (!survey && ActRaiser_IsActionMapGroup(map_group)) {
    /* Validated action-wide path, shared by all seven action-region handler
     * tables. Original tile streamers remain active for the authentic ring and
     * oracle; the bounded HLE provider supplies eligible world coordinates,
     * while the audited $8C98/$8D68 seams widen drawing and activation.
     * AR_WS_ACTION=0 restores the pillarboxed action baseline. */
    wide = g_settings.extended_aspect == kScreenAspect_Auto ||
        g_settings.ws_action;
    ActionBgPresentationPolicy bg_policy;
    if (ActRaiser_ResolveActionBgPlan(
            map_group, map_number, true, &bg_plan, &bg_policy)) {
      bg_plan_valid = 1;
      bg_plan_source_bg1 = bg_plan.layer[0].source;
      bg_plan_source_bg2 = bg_plan.layer[1].source;
      bg_hle_allowed = wide;
      clamp = bg_policy.clamp_layers;
      mirror = bg_policy.mirror_layers;
      repeat = bg_policy.repeat_layers;
      bg_presentation = bg_policy;
    }
  }
  /* AR_WS_ONLYBG=N (1..4): isolate one BG layer so a capture identifies which
   * layer carries scene elements such as sky, dialog, or pillars. */
  {
    const ActRaiserDeveloperEnvironment *developer_environment =
        ActRaiser_GetDeveloperEnvironment();
    if (developer_environment->widescreen_only_bg_present) {
      int L = developer_environment->widescreen_only_bg_layer;
      if (L >= 0 && L < 4)
        cpu_write8(&g_cpu, 0x00, 0x212c, (uint8)(1u << L));
      wide = 1;
      clamp = 0;
      mirror = 0;
      repeat = 0; /* raw tilemap data */
      bg_presentation = (ActionBgPresentationPolicy){ 0 };
      bg_hle_allowed = 0;
      project_final_bg_policy = true;
    }
  }
  /* AR_WS_CLAMP=<hex mask>: override the per-layer clamp for tuning. */
  {
    const ActRaiserDeveloperEnvironment *developer_environment =
        ActRaiser_GetDeveloperEnvironment();
    if (developer_environment->widescreen_clamp_present) {
      wide = 1;
      clamp = developer_environment->widescreen_clamp_mask;
      mirror = 0;
      repeat = 0;
      bg_presentation = (ActionBgPresentationPolicy){ 0 };
      bg_hle_allowed = 0;
      project_final_bg_policy = true;
    }
  }
  /* Capture presets are final policy overrides, intentionally after the
   * scene-specific rules and diagnostic clamp knob. This makes promotional
   * comparisons deterministic:
   *
   *   4:3  authentic centre 256, no HLE presentation
   *   RAW  full wide canvas, no clamp/pad/repeat/gap/world-edge correction
   *   FULL scene policy plus every HLE gate enabled by Settings_SetDisplayMode
   *
   * RAW must not inherit a sim BG2 clamp or an action finite-world margin just
   * because those policies are normally useful. The individual HLE builders
   * and sprite/activation seams are disabled by the RAW preset's ws_* flags. */
  /* Auto must keep finite-world and per-layer safety even if a previous
   * manual ratio used the debug RAW/43 profile. Do not rewrite that profile. */
  if (g_settings.extended_aspect != kScreenAspect_Auto &&
      g_settings.display_mode == kDisplayMode_43) {
    wide = 0;
    clamp = 0;
    mirror = 0;
    repeat = 0;
    bounded_world_margins = 0;
    bg_presentation = (ActionBgPresentationPolicy){ 0 };
    bg_hle_allowed = 0;
    project_final_bg_policy = true;
  } else if (g_settings.extended_aspect != kScreenAspect_Auto &&
             g_settings.display_mode == kDisplayMode_WideRaw) {
    wide = 1;
    clamp = 0;
    mirror = 0;
    repeat = 0;
    bounded_world_margins = 0;
    bg_presentation = (ActionBgPresentationPolicy){ 0 };
    bg_hle_allowed = 0;
    project_final_bg_policy = true;
  }

  frame_policy.horizontal_mode = wide
      ? SR_PPU_HORIZONTAL_MARGIN_AVAILABLE
      : SR_PPU_HORIZONTAL_MARGIN_CENTERED;
  if (wide) {
    frame_policy.margin_left_pixels = (uint32_t)g_ws_extra;
    frame_policy.margin_right_pixels = (uint32_t)g_ws_extra;
    frame_policy.layer_clamp_mask = clamp;
    frame_policy.layer_mirror_mask = mirror;
    frame_policy.layer_repeat_mask = repeat;
    /* Captured-layer padding. Only meaningful in diorama mode, which is
     * the only thing that captures these layers; gating on the setting keeps
     * this a live A/B and keeps flat output untouched either way. */
    if (g_settings.diorama_mode && g_settings.diorama_margin_fix)
      frame_policy.flags |=
          SR_PPU_FRAME_POLICY_PAD_CAPTURED_TO_BUDGET;
  }
  /* Seed vertical geometry from the resolved game-owned plan before BEGIN so
   * a stable extended frame does not momentarily collapse to 224 lines and
   * invalidate surface views again during FINALIZE. Provider fallback may
   * refine edge policy below, but it does not change the semantic owner used
   * to derive vertical camera bounds. */
  if (bg_plan_valid) s_pending_action_bg_plan = bg_plan;
  ActRaiser_ResolveVerticalMarginPolicy(
      map_group, map_number, &frame_policy);

  /* Begin clears the previous frame's providers, extents, row bands and HUD
   * state as one validated operation. Provider-dependent corrections are
   * resolved below and published through the matching finalize transaction. */
  if (!ActRaiser_CommitPpuFramePolicy(&frame_policy)) return;
  if (bg3_capture_height)
    ActRaiser_ClaimOverlayCapture(
        SR_PPU_OVERLAY_BG3, 0, 0, kActRaiserAuthenticWidth,
        bg3_capture_height, bg3_capture_flags);
  if (wide) {
    if (bg_hle_allowed && bg_plan_valid) {
      bg_hle_bindings = ActRaiserActionBg_BindPlanWithVirtualLayers(
          g_ram, kActRaiserWramSize, &bg_plan,
          ActRaiser_CurrentVirtualLayerRoom());
      /* If any planned world layer cannot bind, clamp that layer to its
       * authentic viewport instead of exposing stale/wrapped ring cells in
       * synthetic margins. Wide Raw never reaches this block. */
      uint8 visible_bg_layers = 0;
      const uint32_t ppu_display = RtlGamePpuDisplayState();
      if ((RTL_GAME_PPU_DISPLAY_CONTROL(ppu_display) & 0x80u) == 0u &&
          (RTL_GAME_PPU_BG_MODE_CONTROL(ppu_display) & 7u) == 1u) {
        visible_bg_layers = (uint8)(
            (RTL_GAME_PPU_MAIN_SCREEN(ppu_display) |
             RTL_GAME_PPU_SUB_SCREEN(ppu_display)) &
            ((1u << kActionBgPlanLayerCount) - 1u));
      }
      uint8 fallback_world_layers = ActionBgPlan_ClampUnboundWorldLayers(
          &bg_plan, bg_hle_bindings, visible_bg_layers);
      if (fallback_world_layers) {
        clamp |= fallback_world_layers;
        /* X Clamp cannot protect synthetic Y reads from the native streaming
         * ring. Fail each unbound world layer closed to its authentic rows,
         * without changing the requested/captured canvas for other layers. */
        frame_policy.vertical_clip_layer_mask |= fallback_world_layers;
        for (int layer = 0; layer < kActionBgPlanLayerCount; ++layer) {
          if (!(fallback_world_layers & (1u << layer))) continue;
          frame_policy.vertical_clip_top_rows[layer] = 0u;
          frame_policy.vertical_clip_bottom_rows[layer] = 0u;
        }
        /* The failed world layer was atomically converted to a viewport Clamp
         * plan above. Recompile before the one band-application site so none
         * of its tuner-authored row overrides can bypass that safe fallback. */
        if (!ActionBgPlan_CompilePresentation(
                &bg_plan, &bg_presentation))
          bg_presentation = (ActionBgPresentationPolicy){ 0 };
      }
      /* The role catalog, not a PPU layer-number convention, owns the finite
       * action canvas. A missing, ambiguous, or unbound owner fails closed to
       * the already-rendered symmetric presentation. */
      canvas_layer = ActionBgPlan_CanvasOwner(&bg_plan);
      if (canvas_layer >= 0 &&
          (bg_hle_bindings & (uint8)(1u << canvas_layer)))
        bounded_world_margins = 1;
    }
    if (bounded_world_margins) {
      /* Clamp each side to the catalogued playfield's real world space.
       * Simulation towns use BG1 and the fixed 512px world proven by
       * $01:B4C6's camera clamp. Outside [0,width) stays transparent/clear. */
      int margin_left = 0, margin_right = 0;
      if (ActRaiser_CalculateCanvasMargins(
              map_group, map_number, canvas_layer, g_ws_extra,
              &margin_left, &margin_right)) {
        frame_policy.margin_left_pixels = (uint32_t)margin_left;
        frame_policy.margin_right_pixels = (uint32_t)margin_right;
      }
    }
  }
  frame_policy.layer_clamp_mask = wide ? clamp : 0u;
  frame_policy.layer_mirror_mask = wide ? mirror : 0u;
  frame_policy.layer_repeat_mask = wide ? repeat : 0u;
  if (!ActRaiser_ProjectBgPresentationBands(
          &bg_presentation, &frame_policy, frame_bands,
          sizeof(frame_bands) / sizeof(frame_bands[0]))) {
    SessionFatal_Request(
        "ActRaiser's background presentation exceeded the runner's "
        "frame-policy capacity.");
    return;
  }
  /* BH6 immutable handoff. In the ordinary action path the pure plan survives
   * unchanged, including Bloodpool/Death Heim row bands. Other scene types and
   * explicit global overrides are projected from the final local variables
   * that were just passed to the PPU; there is no present-side mask reversal. */
  if (bg_plan_valid)
    s_pending_action_bg_plan = bg_plan;
  if (!bg_plan_valid || project_final_bg_policy || !wide) {
    ActRaiser_ProjectBgPresentationPolicy(
        &s_pending_action_bg_plan,
        wide ? clamp : 0, wide ? mirror : 0, wide ? repeat : 0,
        wide && bounded_world_margins);
  } else {
    s_pending_action_bg_plan.bound_canvas_to_world =
        bounded_world_margins;
  }
  frame_policy.flags |= SR_PPU_FRAME_POLICY_FINALIZE;
  if (!ActRaiser_CommitPpuFramePolicy(&frame_policy)) return;
  ActRaiserActionBg_ApplyPlanExtents(&s_pending_action_bg_plan);
  s_pending_bg_capture_pad_to_budget =
      (frame_policy.flags &
       SR_PPU_FRAME_POLICY_PAD_CAPTURED_TO_BUDGET) != 0u;
  const uint32 bg_band_signature =
      ActRaiser_BgBandSignature(&bg_presentation);
  /* One line per policy flip — cheap, and makes "why isn't this screen
   * wide?" diagnosable from any console.log (mode bytes included). */
  static int last_wide = -1, last_clamp = -1, last_mirror = -1,
             last_repeat = -1, last_bg_band_count = -1,
             last_bg_normal_scroll = -1,
             last_hud_split_height = -1, last_hud_split_left_end = -1,
             last_hud_split_right_start = -1,
             last_hud_left_only_y = -1, last_bg_plan_valid = -1,
             last_bg_plan_source_bg1 = -2, last_bg_plan_source_bg2 = -2,
             last_bg_hle_bindings = -1;
  static uint32 last_bg_band_signature;
  if (wide != last_wide || clamp != last_clamp || mirror != last_mirror ||
      repeat != last_repeat ||
      bg_presentation.band_count != last_bg_band_count ||
      bg_presentation.normal_scroll_layers != last_bg_normal_scroll ||
      bg_band_signature != last_bg_band_signature ||
      hud_split_height != last_hud_split_height ||
      hud_split_left_end != last_hud_split_left_end ||
      hud_split_right_start != last_hud_split_right_start ||
      hud_left_only_y != last_hud_left_only_y ||
      bg_plan_valid != last_bg_plan_valid ||
      bg_plan_source_bg1 != last_bg_plan_source_bg1 ||
      bg_plan_source_bg2 != last_bg_plan_source_bg2 ||
      bg_hle_bindings != last_bg_hle_bindings) {
    last_wide = wide;
    last_clamp = clamp;
    last_mirror = mirror;
    last_repeat = repeat;
    last_bg_band_count = bg_presentation.band_count;
    last_bg_normal_scroll = bg_presentation.normal_scroll_layers;
    last_bg_band_signature = bg_band_signature;
    last_hud_split_height = hud_split_height;
    last_hud_split_left_end = hud_split_left_end;
    last_hud_split_right_start = hud_split_right_start;
    last_hud_left_only_y = hud_left_only_y;
    last_bg_plan_valid = bg_plan_valid;
    last_bg_plan_source_bg1 = bg_plan_source_bg1;
    last_bg_plan_source_bg2 = bg_plan_source_bg2;
    last_bg_hle_bindings = bg_hle_bindings;
    fprintf(stderr, "[widescreen] gf=%u $18=%02x $19=%02x -> %s "
            "clamp=%02x mirror=%02x repeat=%02x bands=%u/%08x normal=%02x "
            "hud=%u/%u/%u left-only-y=%u bg-plan=%d source=%s/%s "
            "hle=%02x\n",
            (unsigned)ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
            map_group, map_number, wide ? "WIDE" : "pillarbox",
            clamp, mirror, repeat, (unsigned)bg_presentation.band_count,
            (unsigned)bg_band_signature,
            bg_presentation.normal_scroll_layers,
            (unsigned)hud_split_height, (unsigned)hud_split_left_end,
            (unsigned)hud_split_right_start, (unsigned)hud_left_only_y,
            bg_plan_valid,
            bg_plan_valid
                ? ActionBgSourceKind_Name(
                      (ActionBgSourceKind)bg_plan_source_bg1)
                : "none",
            bg_plan_valid
                ? ActionBgSourceKind_Name(
                      (ActionBgSourceKind)bg_plan_source_bg2)
                : "none",
            bg_hle_bindings);
  }
  ActRaiser_LogWidescreenLayers();
}

/* Margin geometry of the last rendered frame,
 * latched at the end of ActRaiserDrawPpuFrame. See ActRaiser_LiveMargins. */
static int s_live_margin_left;
static int s_live_margin_right;
static int s_live_margin_top;
static int s_live_margin_bottom;
static ActionBgPlan s_live_action_bg_plan;
static bool s_live_bg_capture_pad_to_budget;
static int s_live_diorama_world_y0;
static int s_live_diorama_world_height;
static int s_live_diorama_world_x0;
static int s_live_diorama_world_width;
static int s_live_diorama_framing_x;
static int s_live_diorama_framing_y;

/* The draw tail publishes the frame's exact margins, and with them the plan
 * ApplyWidescreenPolicy resolved before scanout, once the pixels exist. */
void ActRaiser_CommitFramePlan(int left, int right, int top, int bottom) {
  s_live_margin_left = left;
  s_live_margin_right = right;
  s_live_margin_top = top;
  s_live_margin_bottom = bottom;
  s_live_action_bg_plan = s_pending_action_bg_plan;
  s_live_bg_capture_pad_to_budget = s_pending_bg_capture_pad_to_budget;
  s_live_diorama_world_y0 = s_pending_diorama_world_y0;
  s_live_diorama_world_height = s_pending_diorama_world_height;
  s_live_diorama_world_x0 = s_pending_diorama_world_x0;
  s_live_diorama_world_width = s_pending_diorama_world_width;
  s_live_diorama_framing_x = s_pending_diorama_framing_x;
  s_live_diorama_framing_y = s_pending_diorama_framing_y;
}

/* The plan ApplyWidescreenPolicy resolved for the frame now being drawn. */
const ActionBgPlan *ActRaiser_PendingActionBgPlan(void) {
  return &s_pending_action_bg_plan;
}

/* Same latch, same reason (see ActRaiser_LiveMargins): the vertical bands the
 * frame was ACTUALLY rendered with, not whatever g_ppu holds by the time the
 * frame slot is captured. */
void ActRaiser_LiveVerticalMargins(int *top, int *bottom) {
  if (top) *top = s_live_margin_top;
  if (bottom) *bottom = s_live_margin_bottom;
}

void ActRaiser_LiveDioramaVerticalExtent(int *world_y0, int *world_height) {
  if (world_y0) *world_y0 = s_live_diorama_world_y0;
  if (world_height) *world_height = s_live_diorama_world_height;
}

void ActRaiser_LiveDioramaHorizontalExtent(int *world_x0, int *world_width) {
  if (world_x0) *world_x0 = s_live_diorama_world_x0;
  if (world_width) *world_width = s_live_diorama_world_width;
}

void ActRaiser_LiveDioramaFraming(int *x, int *y) {
  if (x) *x = s_live_diorama_framing_x;
  if (y) *y = s_live_diorama_framing_y;
}

/* See the latch above. Reports the margin geometry of the most recently rendered
 * frame, which is what a consumer of that frame's captured pixels must use. */
void ActRaiser_LiveMargins(int *left, int *right) {
  if (left) *left = s_live_margin_left;
  if (right) *right = s_live_margin_right;
}

bool ActRaiser_LiveActionBgPlan(ActionBgPlan *out,
                                bool *pad_captured_to_budget) {
  if (out) *out = s_live_action_bg_plan;
  if (pad_captured_to_budget)
    *pad_captured_to_budget = s_live_bg_capture_pad_to_budget;
  return s_live_action_bg_plan.valid;
}
