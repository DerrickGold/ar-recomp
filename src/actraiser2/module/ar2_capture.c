#include "ar2_capture.h"
#include "ar2_sprites.h"
#include "diorama/diorama_camera_policy.h"
#include <string.h>

static uint32_t main_pixels[AR2_CAPTURE_PITCH * AR2_CAPTURE_HEIGHT];
static uint32_t layers[kDioramaPlane_Count][AR2_CAPTURE_PITCH * AR2_CAPTURE_HEIGHT];
static uint32_t hud[AR2_CAPTURE_PITCH * AR2_CAPTURE_HEIGHT];
static uint32_t screen_objects[AR2_CAPTURE_PITCH * AR2_CAPTURE_HEIGHT];
static uint8_t bg3_owner[224];
static Ar2World world;
static Ar2VideoFrame frame;
typedef struct RasterCapture {
  SrRunnerHandle *runner;
  const SnesRunnerApi *api;
  uint64_t generation;
  SrPpuOverlayCaptureState bg3;
  bool separate_hud, failed;
} RasterCapture;

static void line_state(void *user_data, const SrPpuScanoutLineContext *context) {
  RasterCapture *capture = user_data;
  /* Ownership must be decided before this row is rendered. AFTER_HDMA
   * describes the next row's registers and applies removal one row late. */
  if (!(context->flags & SR_PPU_SCANOUT_LINE_BEFORE) || context->line < 1 ||
      context->line > 224)
    return;
  const SrPpuStateSnapshot *state = &context->state;
  uint8_t owner = 0;
  /* Raster ownership in the checked entrances, not a colour/pixel heuristic.
   * A different BG3 producer leaves the separated view unavailable. */
  if (!((state->main_screen | state->sub_screen) & 4))
    owner = 3;
  else if (state->backgrounds[2].tilemap_base_word == 0x6800) {
    /* The HUD map's final four tile rows ($6B80..$6BFF) are the
     * cartridge's frame border, filled with $3440. Scanout row 224
     * reaches that border even with scroll=0. It belongs at the native
     * frame edge, never in the independently scaled HUD surface. */
    const unsigned map_y = (context->line + state->backgrounds[2].v_scroll) & 255;
    owner = map_y < 224 ? 1 : 4;
  }
  else if (state->backgrounds[2].tilemap_base_word == 0x6c00 &&
           (state->color_math_designation & 0xc4) == 4)
    owner = 2;
  bg3_owner[context->line - 1] = owner;
  const uint32_t flags = capture->separate_hud && owner == 1 ? SR_PPU_OVERLAY_REMOVE_FROM_GAME : 0;
  if (capture->bg3.flags != flags) {
    SrPpuOverlayCaptureExchangeRequest request = {
        .struct_size = SR_PPU_OVERLAY_CAPTURE_EXCHANGE_REQUEST_V2_SIZE,
        .lifetime_generation = capture->generation,
        .source_mask = 1u << SR_PPU_OVERLAY_BG3};
    request.expected[SR_PPU_OVERLAY_BG3] = capture->bg3;
    request.replacement[SR_PPU_OVERLAY_BG3] = capture->bg3;
    request.replacement[SR_PPU_OVERLAY_BG3].flags = flags;
    if (capture->api->compare_exchange_ppu_overlay_captures(capture->runner, &request) !=
        SR_RESULT_OK)
      capture->failed = true;
    else
      capture->bg3.flags = flags;
  }
}

bool Ar2Capture_Draw(SrRunnerHandle *runner, const SnesRunnerApi *api, const uint8_t *wram,
                     ArImportedGameView view, int camera_shift,
                     SrPpuScanoutIrqCallback irq) {
  SrGenerationSnapshot generation = {.struct_size = SR_GENERATION_SNAPSHOT_V2_SIZE};
  SrPpuStateSnapshot state = {.struct_size = SR_PPU_STATE_SNAPSHOT_V2_SIZE};
  if (api->query_generations(runner, &generation) != SR_RESULT_OK ||
      api->query_ppu_state(runner, &state) != SR_RESULT_OK)
    return false;
  const bool level = Ar2World_Update(&world, wram, 0x20000);
  const int display_extra = level ? view.extra_columns : 0;
  if (display_extra < 0 || display_extra > 128 ||
      view.extra_rows < 0 || view.extra_rows > 64) return false;
  const int extra = level && view.diorama ? 128 : display_extra;
  const int width = 256 + extra * 2;
  const int view_shift = level ? Ar2World_CaptureShift(world.layers[0].camera_x,
      world.layers[0].width * 8, display_extra) : 0;
  const int background_shift = Ar2World_BackgroundCaptureShift(&world, camera_shift, view_shift);
  int top = 0, bottom = 0;
  if (level) {
    DioramaCameraPolicy_VerticalMargins(world.layers[0].camera_y,
        world.layers[0].height * 8, view.extra_rows, &top, &bottom);
    /* Flat Auto expands the requested canvas symmetrically, as in AR1.
     * Diorama can spend unused edge coverage on the opposite side. */
    if (!view.diorama) {
      if (top > view.extra_rows) top = view.extra_rows;
      if (bottom > view.extra_rows) bottom = view.extra_rows;
    }
  }
  const int height = 224 + top + bottom;
  memset(layers, 0, sizeof(layers));
  memset(hud, 0, sizeof(hud));
  memset(screen_objects, 0, sizeof(screen_objects));
  memset(bg3_owner, 0, sizeof(bg3_owner));
  SrPpuOutputBindingRequest binding = {
      .struct_size = SR_PPU_OUTPUT_BINDING_REQUEST_V2_SIZE,
      .lifetime_generation = generation.lifetime_generation,
      .kind = SR_PPU_OUTPUT_MAIN,
      .pixels = (uint8_t *)main_pixels,
      .pixel_byte_size = sizeof(main_pixels),
      .pitch_bytes = AR2_CAPTURE_PITCH * 4,
      .height_pixels = AR2_CAPTURE_HEIGHT,
  };
  if (api->bind_ppu_output_surface(runner, &binding) != SR_RESULT_OK) return false;
  SrPpuOutputBindingRequest clear = {.struct_size = SR_PPU_OUTPUT_BINDING_REQUEST_V2_SIZE,
                                     .lifetime_generation = generation.lifetime_generation,
                                     .kind = SR_PPU_OUTPUT_CLEAR_OVERLAY_SOURCES};
  if (api->bind_ppu_output_surface(runner, &clear) != SR_RESULT_OK) return false;
  SrPpuFramePolicyRequest policy = {
      .struct_size = SR_PPU_FRAME_POLICY_REQUEST_V2_SIZE,
      .lifetime_generation = generation.lifetime_generation,
      .policy = {.struct_size = SR_PPU_FRAME_POLICY_V2_SIZE,
                 .horizontal_mode = SR_PPU_HORIZONTAL_MARGIN_AVAILABLE,
                 .margin_budget_pixels = extra,
                 .margin_left_pixels = extra,
                 .margin_right_pixels = extra,
                 .margin_top_pixels = top,
                 .margin_bottom_pixels = bottom,
                 /* BG3's HUD and raster mist do not repeat into world rows. */
                 .vertical_clip_layer_mask = 1u << SR_PPU_OVERLAY_BG3}};
  if (api->apply_ppu_frame_policy(runner, &policy) != SR_RESULT_OK) return false;
  {
    SrPpuVirtualTilemapRequest maps = {.struct_size = SR_PPU_VIRTUAL_TILEMAP_REQUEST_V2_SIZE,
                                       .lifetime_generation = generation.lifetime_generation,
                                       .layer_mask = level ? 3 : 0};
    for (unsigned i = 0; i < 2; ++i) {
      if (!level) continue; /* Clear a previous room's virtual bindings. */
      const int shift = i ? background_shift : camera_shift;
      maps.bindings[i] =
          (SrPpuVirtualTilemapBinding){.lookup = Ar2World_Lookup,
                                       .user_data = &world.layers[i],
                                       .camera_x = world.layers[i].camera_x + shift,
                                       .camera_y = world.layers[i].camera_y,
                                       .hscroll_anchor = state.backgrounds[i].h_scroll,
                                       .vscroll_anchor = state.backgrounds[i].v_scroll,
                                       .flags = shift
                                           ? SR_PPU_VIRTUAL_TILEMAP_INCLUDE_AUTHENTIC : 0};
    }
    if (api->replace_ppu_virtual_tilemaps(runner, &maps) != SR_RESULT_OK) return false;
  }
  for (unsigned source = 0; source < SR_PPU_OVERLAY_SOURCE_COUNT; ++source) {
    if (source == SR_PPU_OVERLAY_BG4) continue;
    binding.kind = SR_PPU_OUTPUT_OVERLAY;
    binding.source = source;
    binding.pixels = (uint8_t *)layers[source];
    if (api->bind_ppu_output_surface(runner, &binding) != SR_RESULT_OK) return false;
  }
  size_t band_count;
  const DioramaPriorityBand *bands = DioramaPlanes_PriorityBands(&band_count);
  for (size_t i = 0; i < band_count; ++i) {
    if (bands[i].plane == kDioramaPlane_Bg1Far || bands[i].plane == kDioramaPlane_Bg2Far) continue;
    binding.kind = SR_PPU_OUTPUT_OVERLAY_PRIORITY;
    binding.source = bands[i].source;
    binding.band = bands[i].band;
    binding.pixels = (uint8_t *)layers[bands[i].plane];
    if (api->bind_ppu_output_surface(runner, &binding) != SR_RESULT_OK) return false;
  }
  SrPpuOverlayCaptureExchangeRequest captures = {
      .struct_size = SR_PPU_OVERLAY_CAPTURE_EXCHANGE_REQUEST_V2_SIZE,
      .lifetime_generation = generation.lifetime_generation,
      .source_mask = 0x17,
  };
  for (unsigned i = 0; i < SR_PPU_OVERLAY_SOURCE_COUNT; ++i) {
    if (!(captures.source_mask & (1u << i))) continue;
    captures.replacement[i] =
        (SrPpuOverlayCaptureState){.x0 = -extra, .x1 = 256 + extra,
                                   .y0 = -top, .y1 = 224 + bottom};
  }
  captures.replacement[SR_PPU_OVERLAY_OBJ].oam_count = 128;
  if (api->compare_exchange_ppu_overlay_captures(runner, &captures) != SR_RESULT_OK) return false;
  unsigned text_first, text_count;
  Ar2Sprites_ScreenRange(&text_first, &text_count);
  SrPpuObjCaptureRequest objects = {
      .struct_size = SR_PPU_OBJ_CAPTURE_REQUEST_V2_SIZE,
      .flags = SR_PPU_OBJ_CAPTURE_RANGE | SR_PPU_OBJ_CAPTURE_HANDOFF,
      .lifetime_generation = generation.lifetime_generation};
  if (level && text_count) {
    objects.range_first = text_first;
    objects.range_count = text_count;
    objects.range_width = 256;
    objects.range_height = 224;
    objects.range_pixels = (uint8_t *)(screen_objects + top * AR2_CAPTURE_PITCH);
    objects.range_pixel_byte_size = sizeof(screen_objects) - top * AR2_CAPTURE_PITCH * 4;
    objects.range_pitch_bytes = AR2_CAPTURE_PITCH * 4;
  }
  if (api->configure_ppu_obj_capture(runner, &objects) != SR_RESULT_OK) return false;
  SrBorrowedU16Span palette = {.struct_size = SR_BORROWED_U16_SPAN_V2_SIZE};
  if (api->borrow_u16_memory(runner, SR_MEMORY_CGRAM, &palette) != SR_RESULT_OK ||
      palette.element_count != 256)
    return false;
  uint16_t color = palette.data[0];
  const unsigned brightness = state.flags & SR_PPU_STATE_FORCED_BLANK ? 0 : state.brightness;
  const uint32_t backdrop = 0xff000000u | (((color & 31) * 255 * brightness / (31 * 15)) << 16) |
                            ((((color >> 5) & 31) * 255 * brightness / (31 * 15)) << 8) |
                            (((color >> 10) & 31) * 255 * brightness / (31 * 15));
  RasterCapture raster = {.runner = runner,
                          .api = api,
                          .generation = generation.lifetime_generation,
                          .bg3 = captures.replacement[SR_PPU_OVERLAY_BG3],
                          .separate_hud = level};
  SrPpuScanoutRequest scanout = {.struct_size = SR_PPU_SCANOUT_REQUEST_V2_SIZE,
                                 .lifetime_generation = generation.lifetime_generation,
                                 .irq_callback = irq,
                                 .line_callback = line_state,
                                 .user_data = &raster};
  SrPpuScanoutResult result = {.struct_size = SR_PPU_SCANOUT_RESULT_V2_SIZE};
  if (api->run_ppu_scanout(runner, &scanout, &result) != SR_RESULT_OK || raster.failed)
    return false;
  /* The PPU centers the active canvas in a wider bound surface. The
   * compositor consumes a compact canvas at the start of each 640px row. */
  const int apron = (AR2_CAPTURE_PITCH - width) / 2;
  for (int y = 0; y < height; ++y) {
    const unsigned row = y * AR2_CAPTURE_PITCH;
    memmove(main_pixels + row, main_pixels + row + apron, width * 4);
    memmove(screen_objects + row, screen_objects + row + apron, width * 4);
    for (int plane = 0; plane < kDioramaPlane_Count; ++plane)
      memmove(layers[plane] + row, layers[plane] + row + apron, width * 4);
  }
  frame = (Ar2VideoFrame){.main = main_pixels,
                          .hud = hud,
                          .screen_objects = screen_objects,
                          .width = width,
                          .height = height,
                          .extra_top = top,
                          .extra_bottom = bottom,
                          .display_height = view.diorama ? 224 : height,
                          .render_extra = extra,
                          .display_extra = display_extra,
                          .level = level};
  for (int y = 0; y < height; ++y) {
    const unsigned row = y * AR2_CAPTURE_PITCH;
    for (int x = 0; x < width; ++x)
      layers[kDioramaPlane_Backdrop][row + x] = backdrop;
    const int native_y = y - top;
    if (native_y < 0 || native_y >= 224) continue;
    if (level && bg3_owner[native_y] == 1)
      memcpy(hud + row + extra, layers[SR_PPU_OVERLAY_BG3] + row + extra, 256 * 4);
    else if (bg3_owner[native_y] == 2)
      memcpy(layers[kDioramaPlane_Bg2Far] + row, layers[SR_PPU_OVERLAY_BG3] + row, width * 4);
    else if (!bg3_owner[native_y])
      frame.level = false;
  }
  if (level) {
    frame.camera_x = world.layers[0].camera_x + camera_shift;
    frame.camera_y = world.layers[0].camera_y;
    frame.world_width = world.layers[0].width * 8;
    /* Keep the entire finite-edge capture filled while framing the visible
     * viewport independently. Storage headroom must not displace the player. */
    frame.view_offset_x = view_shift - camera_shift;
    frame.world_height = world.layers[0].height * 8;
    frame.bg2_camera_x = world.layers[1].camera_x + background_shift;
    frame.bg2_camera_y = world.layers[1].camera_y;
    frame.bg2_world_height = world.layers[1].height * 8;
  }
  for (int i = 0; i < kDioramaPlane_Count; ++i)
    frame.planes[i] = layers[i];
  return true;
}

const Ar2VideoFrame *Ar2Capture_Frame(void) { return &frame; }
