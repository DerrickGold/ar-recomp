/* ActRaiser diorama capture: prepares the diorama's captures each frame: HUD
 * and priority-band bindings, the OBJ apron, scene masks, and the town and
 * skybox views.
 * Phase: game (frame transaction). */
#include "actraiser/enhancements/actraiser_enhancements_internal.h"
#include "dev/host_dev_tools.h"
#include "diorama/diorama.h"
#include "sim/sim3d/sim3d_textures.h"
#include "host/host_frame_surfaces.h"

ActionApronGeometry ActRaiser_ObjApronGeometry(void) {
  ActionApronGeometry g = { g_ws_extra, 0 };
  /* The same condition host_display.c uses to pin the margin budget to
   * the ActRaiser 120-pixel display cap. Read from settings + geometry rather
   * than from a per-frame
   * diorama flag on purpose: the emitter runs during game logic, BEFORE
   * ActRaiserDrawPpuFrame sets g_diorama_frame_active, so a per-frame flag
   * would be one frame stale exactly when it matters. */
  if (g_settings.diorama_mode && g_ws_active)
    g.apron = SR_PPU_OBJ_APRON;
  return g;
}

/* Draw the apron part channel into the captured OBJ planes.
 *
 * Runs AFTER scanout, so the planes already hold this frame's in-window
 * sprites, and writes ONLY the two apron column bands. Never the display
 * window: those columns are scanout's, and writing them would both double-draw
 * a straddling part (with no z-test against the sprites it lost to) and break
 * the byte-identity gate this phase is judged on. The clip is structural --
 * PpuRasterizeParts takes the band as its `bounds` and crops to it.
 *
 * Ordering and band routing follow the hardware: OAM order decides who owns an
 * overlapping pixel via ONE shared z-test, and only the survivor's priority
 * decides which plane it lands on (see PpuWriteOverlayRenderLine's
 * priority-split resolve). So parts are drawn one at a time in list order and a
 * pixel already opaque in ANY of the four OBJ planes is left alone -- which is
 * exactly first-writer-wins across bands, with the planes themselves as the
 * claimed-set. That works because PpuClearOverlayRenderLine clears the full
 * bound pitch, apron included, every frame. */
void ActRaiser_DioramaApronFinish(const ActionApronGeometry *geom) {
  if (!geom || geom->apron <= 0 || !ActRaiser_PpuFrame() ||
      !ActionApron_Count())
    return;

  /* AR_APRONLOG=1: the channel's sizing verdict. peak/overflow answer "is
   * kActionApronMaxParts right?" without guessing, which is what the plan asks
   * for instead of assuming a capacity. */
  if (ActRaiser_DeveloperFlagEnabled(kActRaiserDeveloperFlag_ApronLog))
    fprintf(stderr, "[apron] gf=%u parts=%d peak=%d overflow=%d\n",
            ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
            ActionApron_Count(), ActionApron_PeakCount(),
            ActionApron_Overflow());

  const int surface_width = ActionApron_SurfaceWidth(geom);
  if (surface_width > (int)SR_PPU_SURFACE_MAX_WIDTH)
    return;
  const SrPpuObjPart *parts = ActionApron_Parts();
  const int count = ActionApron_Count();
  const int rows = kActRaiserAuthenticHeight +
      g_ws_extra_top + g_ws_extra_bottom;

  /* Big enough for the largest SNES sprite (64x64). */
  static uint32_t scratch[64 * 64];

  int spans[2][2];
  ActionApron_LeftSpan(geom, &spans[0][0], &spans[0][1]);
  ActionApron_RightSpan(geom, &spans[1][0], &spans[1][1]);

  /* Resolved once, not per pixel: the claimed-set test below reads all four,
   * and re-deriving them inside the innermost loop made the plane mapping the
   * hottest thing in the pass. */
  uint32_t *planes[4];
  for (int p = 0; p < 4; p++)
    planes[p] = (uint32_t *)g_diorama_layer_pixels[
        DioramaPlaneForObjectPriority(p)];

  for (int i = 0; i < count; i++) {
    const SrPpuObjPart *part = &parts[i];
    const int priority = (part->tile_attr >> 12) & 3;
    uint32_t *plane = planes[priority];
    if (!plane)
      continue;
    const SrPpuOverlayCaptureState *obj_capture =
        ActRaiser_PpuCapture(SR_PPU_OVERLAY_OBJ);
    const bool color_math =
        obj_capture &&
        (obj_capture->flags & SR_PPU_OVERLAY_MARK_OBJ_COLOR_MATH) != 0u &&
        ActionApron_PartUsesColorMath(part->tile_attr);

    for (int band = 0; band < 2; band++) {
      /* Intersect the part with this apron band; skip when it does not reach. */
      const int win_x0 =
          part->x > spans[band][0] ? part->x : spans[band][0];
      const int win_x1 = part->x + part->size < spans[band][1]
          ? part->x + part->size : spans[band][1];
      if (win_x1 <= win_x0)
        continue;
      const int win_y0 = part->y;
      const int win_y1 = part->y + part->size;
      const int w = win_x1 - win_x0, h = win_y1 - win_y0;
      if (w <= 0 || h <= 0 || w > 64 || h > 64)
        continue;
      if (!ActRaiser_RasterizePpuObjParts(
              part, 1u, win_x0, win_y0, win_x1, win_y1,
              scratch, (size_t)w * sizeof(uint32_t), sizeof(scratch)))
        continue;

      /* The destination columns are a contiguous run, so resolve the base once
       * per part instead of mapping and bounds-checking every pixel. The run is
       * inside the surface by construction: `win` was clipped to an apron band,
       * and a band's columns are always within [0, surface_width). */
      const int base_col = ActionApron_SurfaceColumn(geom, win_x0);
      if (base_col < 0 || base_col + w > surface_width)
        continue;

      for (int y = 0; y < h; y++) {
        /* Plane rows are CAPTURE space: row 0 is screen y = -g_ws_extra_top,
         * the same bias ActRaiser_DioramaHudObjFinish applies. */
        const int row = win_y0 + y + g_ws_extra_top;
        if (row < 0 || row >= rows)
          continue;
        const size_t row_base = (size_t)row * surface_width + base_col;
        for (int x = 0; x < w; x++) {
          uint32_t pixel = scratch[(size_t)y * w + x];
          if (!pixel)
            continue;
          const size_t index = row_base + x;
          bool claimed = false;
          for (int p = 0; p < 4 && !claimed; p++)
            if (planes[p] && planes[p][index])
              claimed = true;
          if (claimed)
            continue;
          if (color_math)
            pixel = (pixel & 0x00ffffffu) | 0x80000000u;
          plane[index] = pixel;
        }
      }
    }
  }
}
static uint32_t s_diorama_skybox_pixels[
    SR_PPU_SURFACE_MAX_WIDTH * SR_PPU_SURFACE_MAX_HEIGHT];

static void ActRaiser_BindDioramaHudCapture(int width, size_t pitch,
                                            int capture_height,
                                            uint8_t capture_screens,
                                            uint8_t full_add_sub_sources) {
  /* Rebind BG3 in both directions on every frame. Omitting it would retain
   * the previous destination across flat/tilted HUD toggles. The flat HUD uses
   * the narrow framebuffer pitch; tilted planes include the apron. */
  if (g_settings.diorama_hud_flat) {
    /* The NARROW pitch, deliberately -- not the apron-wide `pitch` the
     * diorama planes bind at. This surface is not a diorama plane: it feeds
     * the anchored flat HUD overlay, which present.c uploads at
     * snes_width*4 (PresentUpload's hud rect). Binding it apron-wide made
     * the PPU write rows 2*kPpuObjApron columns apart while the upload read
     * them snes_width apart, shearing the HUD across the top of the screen.
     * The apron is resolve headroom for content that slides in past a
     * tilted plane's edge; a screen-anchored HUD has no such edge. */
    ActRaiser_BindPpuOutput(SR_PPU_OUTPUT_OVERLAY, SR_PPU_OVERLAY_BG3, 0u,
                            g_hud_bg_pixels, (size_t)width * 4,
                            kHostDisplayFramebufferHeight);
    /* Capture extent is already final in
     * ArBg3Composite_CaptureHeight. Keeping this block to a pure rebind
     * makes ownership independent of renderer setup order and preserves
     * the same policy through flat/tilted/flat toggle sequences. */
  } else {
    ActRaiser_BindPpuOutput(SR_PPU_OUTPUT_OVERLAY, SR_PPU_OVERLAY_BG3, 0u,
                            HostFrameSurfaces_DioramaPlane(SR_PPU_OVERLAY_BG3),
                            pitch, kHostDisplayFramebufferHeight);
    if (capture_screens & (1 << SR_PPU_OVERLAY_BG3))
      ActRaiser_SetPpuOverlayCapture(
          SR_PPU_OVERLAY_BG3, -g_ws_extra, -g_ws_extra_top, width,
          capture_height,
          SR_PPU_OVERLAY_REMOVE_FROM_GAME |
              ((full_add_sub_sources & (1 << SR_PPU_OVERLAY_BG3))
                   ? SR_PPU_OVERLAY_MARK_FULL_ADD_SUBSCREEN
                   : 0u));
  }
}

static void ActRaiser_BindDioramaPriorityBands(size_t pitch) {
  /* Priority-band splits: scanout routes each captured pixel to the
   * surface matching its hardware priority (Mode-1 tile priority bit for
   * BGs, the 2-bit OAM priority for sprites), so the diorama can draw
   * the true Mode-1 interleave — foreground tiles over sprites, low
   * priority sprites behind the playfield. Bound after their primaries
   * because a primary rebind drops the band family. */
  size_t band_count;
  const DioramaPriorityBand *bands = DioramaPlanes_PriorityBands(&band_count);
  for (size_t i = 0; i < band_count; i++) {
    if (bands[i].plane == kDioramaPlane_Bg1Far ||
        bands[i].plane == kDioramaPlane_Bg2Far) {
      const DioramaRoomOverride *virtual_room =
          ActRaiser_CurrentVirtualLayerRoom();
      const int virtual_bg =
          bands[i].plane == kDioramaPlane_Bg1Far ? 0 : 1;
      if (!virtual_room || !DioramaLayerOrder_VirtualLayerIsAuthored(
                               &virtual_room->virtual_layers[virtual_bg]))
        continue;
    }
    ActRaiser_BindPpuOutput(SR_PPU_OUTPUT_OVERLAY_PRIORITY, bands[i].source,
                            (uint32_t)bands[i].band,
                            HostFrameSurfaces_DioramaPlane(bands[i].plane),
                            pitch, kHostDisplayFramebufferHeight);
  }
}

void ActRaiser_PrepareDioramaCapture(const SrPpuStateSnapshot *ppu) {
  /* Diorama per-layer capture: when active (D toggle) or armed for a one-shot
   * dump (Shift+D), override all existing capture policies with full-frame
   * RemoveFromGame captures for BG1/2/3/OBJ. Bind dedicated diorama buffers
   * so we don't collide with the HUD/HD overlay surfaces. The captures
   * overwrite whatever the widescreen HUD split and HD replacements set
   * above — mutual exclusion for this frame. */
  bool active = Diorama_IsActiveThisFrame();
  bool want_capture =
      active || (HostDevTools_DioramaDumpArmed() &&
                 ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup]));
  g_diorama_frame_active = active;
  if (want_capture) {
    int width = kActRaiserAuthenticWidth + 2 * g_ws_extra;
    /* Apron-wide, matching the main framebuffer bind: the capture rect stays
     * scanline-bounded (the scanline path cannot fill apron columns), but the
     * wider pitch makes PpuWriteOverlayRenderLine's texture_extra centre the
     * captured span, leaving the apron columns free for capture-time part
     * rasterization to fill. */
    size_t pitch = ActionApron_SurfacePitch(width, SR_PPU_OBJ_APRON);
    /* Capture rectangles are expressed in AUTHENTIC screen space, so
     * the vertical band starts at a negative y exactly as the side
     * margins start at -g_ws_extra. The PPU maps that onto row 0 of the
     * destination surface (PpuOutputRow), so a plane's texture row 0 is
     * screen y = -g_ws_extra_top -- the transpose of column 0 meaning
     * screen x = -g_ws_extra. */
    int capture_height =
        kActRaiserAuthenticHeight + g_ws_extra_top + g_ws_extra_bottom;
    /* BG3 has a separate binding below: a flat HUD retains its anchored
     * capture policy; a tilted HUD is captured as another scene plane. */
    static const uint32_t kCaptureLayersCommon[] = {
        SR_PPU_OVERLAY_BG1,
        SR_PPU_OVERLAY_BG2,
        SR_PPU_OVERLAY_OBJ,
    };
    /* Diorama owns visual sources, not just the SNES main screen. Marahna
     * keeps BG1 and OBJ exclusively on TS ($11) and full-adds them to
     * main-screen BG2 ($06); gating on TM alone produced an empty BG1 plane
     * and no sprites even though the native composite was healthy. The PPU
     * overlay exporter prefers TM and falls back to TS for a subscreen-only
     * BG, so the union is the frontend's correct eligibility test. */
    const uint8_t capture_screens =
        (uint8_t)(ppu->main_screen | ppu->sub_screen);
    const uint8_t full_add_sub_sources =
        DioramaCaptureBlend_FullAddSubscreenSources(
            ppu->color_math_control, ppu->color_math_designation,
            ppu->main_screen, ppu->sub_screen);
    uint8_t layer_group = 0, layer_map = 0;
    uint8_t layer_section = kDioramaLayerSection_Room;
    const bool layer_room_live =
        Diorama_LiveRoom(&layer_group, &layer_map, &layer_section);
    /* Captures represent half-add with alpha and disjoint full-add with
     * subscreen-source ownership. Subtract and overlapping full-add fail
     * closed. The hardware policy is tested in diorama_capture_blend.c. */
    for (int i = 0; i < (int)(sizeof(kCaptureLayersCommon) /
                              sizeof(kCaptureLayersCommon[0]));
         i++) {
      uint32_t src = kCaptureLayersCommon[i];
      ActRaiser_BindPpuOutput(SR_PPU_OUTPUT_OVERLAY, src, 0u,
                              HostFrameSurfaces_DioramaPlane((int)src), pitch,
                              kHostDisplayFramebufferHeight);
      if (capture_screens & (1 << src)) {
        uint32_t flags = SR_PPU_OVERLAY_REMOVE_FROM_GAME;
        if (full_add_sub_sources & (1 << src))
          flags |= SR_PPU_OVERLAY_MARK_FULL_ADD_SUBSCREEN;
        /* OBJ keeps its own per-palette-group flag; this one is BG-only. */
        if (src != SR_PPU_OVERLAY_OBJ &&
            DioramaCaptureBlend_LayerIsHalfAdded(
                ppu->color_math_control, ppu->color_math_designation,
                ppu->sub_screen, (uint8_t)(1 << src))) {
          flags |= SR_PPU_OVERLAY_MARK_BG_HALF_ADD;
          /* Once per source: this is a fidelity change to the captured image,
           * so it should be visible in a log rather than inferred from
           * pixels. Silent on every stage that does not use this math.
           * AR_DIORAMA_BLEND_LOG=1 makes it per-frame instead, which is how
           * to tell a stage that never qualifies from one that qualifies only
           * on some frames (CGWSEL/CGADSUB are HDMA-writable per scanline, so
           * the value at capture-setup time is not necessarily the value
           * during scanout). */
          static bool reported[SR_PPU_OVERLAY_SOURCE_COUNT];
          static int verbose = -1;
          if (verbose < 0)
            verbose = getenv("AR_DIORAMA_BLEND_LOG") ? 1 : 0;
          if (!reported[src] || verbose) {
            reported[src] = true;
            fprintf(stderr,
                    "[diorama-blend] gf=%u BG%d half-added with subscreen "
                    "(cgwsel=$%02x cgadsub=$%02x main=$%02x sub=$%02x) "
                    "-> captured at 50%% alpha\n",
                    ActRaiser_ReadWram16(kActRaiserWram_GameFrame), src + 1,
                    ppu->color_math_control, ppu->color_math_designation,
                    ppu->main_screen, ppu->sub_screen);
          }
        }
        /* Bloodpool 0204 dims BG1 with full fixed-colour subtraction
         * (cgwsel=$00 cgadsub=$81). Alpha cannot express subtraction, so ask
         * the PPU capture to bake it into this isolated plane in native
         * 5-bit colour space. */
        if (src != SR_PPU_OVERLAY_OBJ &&
            DioramaCaptureBlend_LayerUsesFixedColorSubtract(
                ppu->color_math_control, ppu->color_math_designation,
                ppu->fixed_color, (uint8_t)(1 << src))) {
          flags |= SR_PPU_OVERLAY_APPLY_BG_FIXED_COLOR_SUBTRACT;
        }
        ActRaiser_SetPpuOverlayCapture(src, -g_ws_extra, -g_ws_extra_top, width,
                                       capture_height, flags);
        if (flags & SR_PPU_OVERLAY_MARK_FULL_ADD_SUBSCREEN) {
          static bool reported_full_add;
          static int verbose_full_add = -1;
          if (verbose_full_add < 0)
            verbose_full_add = getenv("AR_DIORAMA_BLEND_LOG") ? 1 : 0;
          if (!reported_full_add || verbose_full_add) {
            reported_full_add = true;
            fprintf(stderr,
                    "[diorama-blend] gf=%u full subscreen add "
                    "(cgwsel=$%02x cgadsub=$%02x main=$%02x sub=$%02x) "
                    "-> resolved TS sources=$%02x use additive planes\n",
                    ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
                    ppu->color_math_control, ppu->color_math_designation,
                    ppu->main_screen, ppu->sub_screen, full_add_sub_sources);
          }
        }
      }
      /* Backing is presentation policy even when this particular live BG is
       * disabled: a named ROM skybox can source BG1/BG2 independently of the
       * current room's screen-enable bits. Geometry and fill setup are
       * deliberately order-independent within this per-frame policy. */
      if (layer_room_live &&
          (src == SR_PPU_OVERLAY_BG1 || src == SR_PPU_OVERLAY_BG2)) {
        DioramaTransparentFill fill = kDioramaTransparentFill_None;
        uint8_t fill_cgram = 0;
        if (DioramaLayerOrder_ResolveTransparentFill(
                Diorama_LayerOverrides(), layer_group, layer_map, layer_section,
                src, &fill, &fill_cgram)) {
          SrPpuTransparentFillMode mode = SR_PPU_TRANSPARENT_FILL_NONE;
          if (fill == kDioramaTransparentFill_Black)
            mode = SR_PPU_TRANSPARENT_FILL_BLACK;
          else if (fill == kDioramaTransparentFill_Cgram)
            mode = SR_PPU_TRANSPARENT_FILL_CGRAM;
          /* Unknown/corrupt values fail transparent rather than silently
           * becoming black and hiding a platform-specific capture fault.
           * Setting None is still significant: it carries authored Off into
           * the immutable frame snapshot used by ROM backdrop composition. */
          ActRaiser_SetPpuOverlayFill(src, mode, fill_cgram);
        }
      }
    }
    ActRaiser_BindDioramaHudCapture(width, pitch, capture_height,
                                    capture_screens, full_add_sub_sources);
    if (capture_screens & (1 << SR_PPU_OVERLAY_OBJ))
      ActRaiser_SetPpuOverlayOamRange(0, 128);
    ActRaiser_BindDioramaPriorityBands(pitch);
  }
}

void ActRaiser_PrepareSceneMasks(uint8_t map_group, uint8_t map_number) {
  /* Flat presentation has one already-composited framebuffer, so BG-local
   * enhancements need the PPU's real priority winners as occlusion masks.
   * Diorama owns isolated planes and inserts effects directly after BG1/BG2;
   * a one-shot dump likewise owns these capture slots. HD replacements get
   * first refusal above—never overwrite another source policy. */
  const bool action_effects_enabled =
      g_settings.action_effect_lighting || g_settings.action_effect_particles;
  if (action_effects_enabled && !g_diorama_frame_active &&
      !HostDevTools_DioramaDumpArmed() &&
      ActionSceneEffects_RoomUsesBg1Decorations(g_ram, kActRaiserWramSize)) {
    const SrPpuOverlayCaptureState *bg1 =
        ActRaiser_PpuCapture(SR_PPU_OVERLAY_BG1);
    if (bg1->x1 <= bg1->x0 || bg1->y1 <= bg1->y0) {
      const int width = kActRaiserAuthenticWidth + 2 * g_ws_extra;
      if (ActRaiser_BindPpuOutput(SR_PPU_OUTPUT_OVERLAY, SR_PPU_OVERLAY_BG1, 0u,
                                  g_action_bg1_mask_pixels, (size_t)width * 4,
                                  kHostDisplayFramebufferHeight)) {
        ActRaiser_SetPpuOverlayCapture(
            SR_PPU_OVERLAY_BG1, -g_ws_extra, 0, width,
            kActRaiserAuthenticHeight,
            SR_PPU_OVERLAY_MARK_OWNING_SCREEN_WINNER);
      }
    }
  }
  if (action_effects_enabled && !g_diorama_frame_active &&
      !HostDevTools_DioramaDumpArmed() && map_group == kActRaiserMapGroup_Aitos &&
      map_number >= 2 && map_number <= 3) {
    const SrPpuOverlayCaptureState *bg2 =
        ActRaiser_PpuCapture(SR_PPU_OVERLAY_BG2);
    if (bg2->x1 <= bg2->x0 || bg2->y1 <= bg2->y0) {
      const int width = kActRaiserAuthenticWidth + 2 * g_ws_extra;
      if (ActRaiser_BindPpuOutput(SR_PPU_OUTPUT_OVERLAY, SR_PPU_OVERLAY_BG2, 0u,
                                  g_action_bg2_mask_pixels, (size_t)width * 4,
                                  kHostDisplayFramebufferHeight)) {
        ActRaiser_SetPpuOverlayCapture(SR_PPU_OVERLAY_BG2, -g_ws_extra, 0,
                                       width, kActRaiserAuthenticHeight,
                                       SR_PPU_OVERLAY_MARK_MAIN_SCREEN_WINNER);
      }
    }
  }

  /* Palace replaces only actual BG1 main-screen winners. The public mask
   * leaves native scanout/HUD untouched and never takes an existing HD/dump
   * claim. Binding/capture policy is owned here, before the normal scanout. */
  if (g_settings.sim3d_world_navigation && g_settings.sim3d_sky_palace &&
      !g_diorama_frame_active && !HostDevTools_DioramaDumpArmed() &&
      map_group == kActRaiserMapGroup_NonAction &&
      map_number == kActRaiserNonActionMap_SkyPalace) {
    const SrPpuOverlayCaptureState *bg1 =
        ActRaiser_PpuCapture(SR_PPU_OVERLAY_BG1);
    const int width = kActRaiserAuthenticWidth + 2 * g_ws_extra;
    if (bg1 && (bg1->x1 <= bg1->x0 || bg1->y1 <= bg1->y0) &&
        width <= kSimWorldNavigationPalaceMaxWidth &&
        ActRaiser_BindPpuOutput(SR_PPU_OUTPUT_OVERLAY, SR_PPU_OVERLAY_BG1, 0u,
                                (uint8_t *)g_sim_sky_palace_mask_pixels,
                                (size_t)width * sizeof(uint32_t),
                                kSimWorldNavigationPalaceMaxHeight)) {
      ActRaiser_SetPpuOverlayCapture(SR_PPU_OVERLAY_BG1, -g_ws_extra, 0, width,
                                     kActRaiserAuthenticHeight,
                                     SR_PPU_OVERLAY_MARK_MAIN_SCREEN_WINNER);
    }
  }
}

void ActRaiser_PrepareTownCapture(void) {
  /* D2: claim observational full-frame Mode-1 captures only after every
   * pre-existing HUD/HD/diorama policy has had a chance to declare a
   * conflict. The original PPU framebuffer remains intact as same-frame A0. */
  uint8_t map_group = g_ram[kActRaiserWram_MapGroup];
  uint8_t map_number = g_ram[kActRaiserWram_CurrentMap];
  bool town = ActRaiser_IsSimulationTown(map_group, map_number);
  /* Build D1b before choosing D2's physical planes. This is the same live
   * OAM/VRAM/CGRAM state the scanline renderer is about to consume, but now
   * an atlas or metadata failure can retain raw OBJ before scanout instead
   * of being discovered after those fallback pixels were omitted. */
  bool billboard_atlas_ready =
      town && SimRenderAtlas_Build(
                  ActRaiser_Runner(), ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraX),
                  ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraY));
  /* The compact menu needs independent town layers even with 3D disabled.
   * Menu sprites are not one OAM prefix: inventory icons and map brackets
   * can follow the hourglass. Recompose the flat scene from the same captured
   * planes/objects as enhanced mode, leaving the authentic scanout untouched. */
  const bool flat_menu = town && ActRaiserSimMenu_OwnsPresentation() &&
      !g_settings.sim3d_mode;
  Sim3DCaptureRequest request = {
      .town = town,
      .master_enabled = g_settings.sim3d_mode || flat_menu,
      /* The picker flag is in $7F WRAM; its 17-bit mirror address must not
       * pass through the 16-bit low-WRAM helper. */
      .picker_active = town && ActRaiser_SimMapPickerActiveForState(
                                   map_group, map_number,
                                   ActRaiser_ReadWramMirror16(
                                       kActRaiserWram_SimMapPickerFlag)),
      .renderer_ready = Sim3DTextures_Ready(),
      .billboard_atlas_ready = billboard_atlas_ready,
      .billboard_renderer_ready = Sim3DTextures_BillboardsReady(),
      .diorama_active = g_diorama_frame_active,
      /* The inspector panel is the only on-screen reader of the capture's
       * diagnostic hash; with it off, that pass is skipped. */
      .inspector_active = g_settings.scene_inspector,
      .requested_features = flat_menu ? kSimFeature_SeparatedComposite
          : Settings_Sim3DRequestedFeatures(),
      .diagnostic_layer_mask = g_settings.sim3d_diagnostic_layers,
      .width = kActRaiserAuthenticWidth + 2 * g_ws_extra,
      .height = kActRaiserAuthenticHeight,
  };
  Sim3D_PrepareCapture(ActRaiser_Runner(), &request);
  ActRaiser_ReportSim3DCaptureContractFailure();
}

SrPpuBackgroundViewRequest
ActRaiser_PrepareSkyboxView(const SrPpuFrameTransactionContext *context,
                            const SnesRunnerApi *scanout_api,
                            bool scanout_ready, bool profile_diorama) {
  /* A finite scrolling background needs its own fixed-size view at world
   * edges. The normal BG capture remains registered to the gameplay camera.
   * Named ROM, repeated, mirrored and row-banded skies retain their policies.
   */
  SrPpuBackgroundViewRequest skybox_view = {0};
  const ActionBgLayerPlan *sky_layer = &ActRaiser_PendingActionBgPlan()->layer[1];
  const DioramaRoomOverride *sky_room = ActRaiser_CurrentVirtualLayerRoom();
  const DioramaPlaneOverride *backdrop_override =
      sky_room ? &sky_room->planes[kDioramaPlane_Backdrop] : NULL;
  const bool skybox_capable =
      scanout_ready &&
      scanout_api->struct_size >= SNES_RUNNER_API_PPU_BACKGROUND_VIEW_SIZE &&
      (scanout_api->capabilities & SR_RUNNER_CAP_PPU_BACKGROUND_VIEW) != 0u &&
      scanout_api->run_ppu_scanout_with_background_view;
  if (skybox_capable && profile_diorama && g_settings.diorama_margin_fix &&
      g_settings.diorama_skybox != kDioramaSky_Off && sky_layer->valid &&
      sky_layer->source == kActionBgSource_WorldMap &&
      !sky_layer->wrap_world_x && sky_layer->band_count == 0 &&
      sky_layer->default_edge == kActionBgEdge_LiveWorld &&
      (!backdrop_override || !backdrop_override->set_source ||
       backdrop_override->source == kDioramaLayerSource_Captured) &&
      (!sky_room || !DioramaLayerOrder_VirtualLayerHasClassification(
                        &sky_room->virtual_layers[1]))) {
    unsigned left = context->frame.margin_budget;
    unsigned right = left;
    if (sky_layer->horizontal_extent.mode == kActionBgExtent_Fixed) {
      if (left > sky_layer->horizontal_extent.left)
        left = sky_layer->horizontal_extent.left;
      if (right > sky_layer->horizontal_extent.right)
        right = sky_layer->horizontal_extent.right;
    }
    unsigned width = kActRaiserAuthenticWidth + left + right;
    if (width > sky_layer->world_width)
      width = sky_layer->world_width;
    skybox_view = (SrPpuBackgroundViewRequest){
        .struct_size = sizeof(skybox_view),
        .layer = SR_PPU_OVERLAY_BG2,
        .world_width = sky_layer->world_width,
        .world_height = sky_layer->world_height,
        .screen_x0 = -(int)left,
        .screen_y0 = -(int)context->state.margin_top,
        .width = width,
        .height = SR_PPU_NATIVE_HEIGHT + context->state.margin_top +
                  context->state.margin_bottom,
        .pixels = s_diorama_skybox_pixels,
        .pitch_bytes = width * sizeof(uint32_t),
        .pixel_byte_size = sizeof(s_diorama_skybox_pixels),
    };
  }

  return skybox_view;
}
