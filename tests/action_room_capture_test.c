/* Whole-room capture oracle. Supply exported .arscene files locally:
 * action_room_capture_test diorama-layers.ini room1.arscene ...
 * The editor's call is renamed in its test object only; its real planner,
 * painted scenery, raster animation and camera policies remain unchanged. */
#include "room_scene.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SrSceneRenderer *reference;
static SrPpuBgPacket *packet;
static unsigned frames, full_add_frames, full_add_rows, owned_frames, fallback_frames;
static const char *current_room;
static unsigned current_view, current_config;

static void Equal(uint32_t expected, uint32_t actual, unsigned mode,
                  unsigned source, unsigned band, unsigned x, unsigned y) {
  if (expected != actual) {
    fprintf(stderr, "%s config=%u view=%u mode=%u source=%u band=%u at %u,%u: %08x != %08x\n",
        current_room, current_config, current_view, mode, source, band, x, y, actual, expected);
    abort();
  }
}

bool TestRoomCapture_Render(SrSceneRenderer *renderer, const SrSceneFrame *frame,
                           SrSceneSurfaces *out) {
  SrSceneFrame input = *frame;
  input.reference_renderer = true;
  input.background_packet = NULL;
  SrSceneSurfaces expected;
  assert(sr_scene_renderer_render(reference, &input, &expected));
  input.reference_renderer = false;
  const unsigned requests[] = {0, SR_PPU_BG_PACKET_TILES,
      SR_PPU_BG_PACKET_TILES | SR_PPU_BG_PACKET_FULL_ADD,
      SR_PPU_BG_PACKET_TILES | SR_PPU_BG_PACKET_FULL_ADD | SR_PPU_BG_PACKET_VALIDATE_TILES};
  for (unsigned mode = 0; mode < 4; ++mode) {
    input.background_packet = mode ? packet : NULL;
    packet->request_flags = requests[mode];
    assert(sr_scene_renderer_render(renderer, &input, out));
    assert(out->width == expected.width && out->height == expected.height &&
           out->pitch_pixels == expected.pitch_pixels && out->view_width == expected.view_width);
    if (mode) assert(SrPpuBgPacket_Valid(packet));
    for (unsigned y = 0; y < (unsigned)out->height; ++y) {
      for (unsigned x = 0; x < (unsigned)out->pitch_pixels; ++x) {
        const unsigned at = y * out->pitch_pixels + x;
        Equal(expected.backdrop[at], out->backdrop[at], mode, 3, 0, x, y);
        for (unsigned bg = 0; bg < 2; ++bg) for (unsigned band = 0; band < 3; ++band) {
          const uint32_t pixel = expected.bands[bg][band][at];
          if (mode && (packet->words[2] & (1u << bg)))
            Equal(pixel, SrPpuBgPacket_Color(packet, bg, band, x, y), mode, bg, band, x, y);
          if (!mode || !(packet->owned_sources & (1u << bg)))
            Equal(pixel, out->bands[bg][band][at], mode, bg, band, x, y);
        }
      }
    }
    assert(!!out->background_view == !!expected.background_view);
    if (out->background_view) for (unsigned y = 0; y < (unsigned)out->height; ++y)
      for (unsigned x = 0; x < (unsigned)out->view_width; ++x) {
        const uint32_t pixel = expected.background_view[y * out->view_width + x];
        if (mode && (packet->words[2] & 4u))
          Equal(pixel, SrPpuBgPacket_Color(packet, 2, 0, x, y), mode, 2, 0, x, y);
        if (!mode || !(packet->owned_sources & 4u))
          Equal(pixel, out->background_view[y * out->view_width + x], mode, 2, 0, x, y);
      }
    for (unsigned bg = 0; bg < 2; ++bg) {
      assert(!!out->native_pages[bg] == !!expected.native_pages[bg]);
      if (out->native_pages[bg])
        assert(!memcmp(out->native_pages[bg], expected.native_pages[bg], 256 * 256 * 4));
    }
    if (mode == 2) {
      unsigned rows = 0;
      for (unsigned y = 0; y < packet->words[1]; ++y)
        rows += SrPpuBgPacket_FullAddRow(packet, y) != NULL;
      full_add_frames += rows != 0;
      full_add_rows += rows;
      owned_frames += packet->owned_sources != 0;
      fallback_frames += (packet->owned_sources & 3u) != 3u;
    }
  }
  /* Validation retains CPU surfaces for the rest of the editor render. */
  assert(!packet->owned_sources);
  ++frames;
  return true;
}

static char *Read(const char *path, size_t *size) {
  FILE *file = fopen(path, "rb"); assert(file);
  assert(!fseek(file, 0, SEEK_END));
  const long length = ftell(file); assert(length >= 0 && length < 16 * 1024 * 1024);
  rewind(file);
  char *data = malloc((size_t)length + 1); assert(data);
  assert(fread(data, 1, (size_t)length, file) == (size_t)length);
  fclose(file); data[length] = 0; *size = (size_t)length; return data;
}

static void FlatCapture(EditorRoomScene *scene, const ActionSceneSnapshot *assets,
                        int x, int y, uint32_t clock, int extra) {
  EditorRoomCamera camera;
  assert(EditorRoomScene_RenderFlat(scene, x, y, clock, extra, &camera));
  const SrSceneSurfaces *surface = EditorRoomScene_Surfaces(scene);
  assert(surface->width == 256 + 2 * extra && surface->height == 224);
  assert(!surface->background_view && !surface->native_pages[0] && !surface->native_pages[1]);
  assert(!EditorRoomScene_Effects(scene)->authored_count && !EditorRoomScene_Effects(scene)->effect_count);
  ActionRoomSceneFrameRequest request = assets->frame;
  request.camera_x = camera.x; request.camera_y = camera.y; request.game_frame = clock;
  ActionRoomSceneFrameState state;
  static uint32_t original[kActionRoomSceneFramePixels];
  assert(ActionRoomScene_BuildFrameState(&assets->scene, &request, &state));
  /* The small original-frame oracle uses 10-bit PPU addresses, rather than
   * the runtime's unwrapped world provider. Use it only inside that window;
   * the production/reference PPU comparison still covers all room positions. */
  for (unsigned bg = 0; bg < 2; ++bg)
    if ((assets->scene.bg[bg].pages_wide > 4 && state.layer_camera_x[bg] + 256 > 1024) ||
        (assets->scene.bg[bg].pages_high > 4 && state.layer_camera_y[bg] + 225 > 1024)) return;
  assert(ActionRoomScene_RenderNativeFrame(&assets->scene, &state, original, kActionRoomSceneFramePixels));
  /* An independent native-frame compositor verifies priority, palette and
   * color math in the original 256px area. The interposed PPU oracle above
   * also verifies every widescreen pixel against its reference rasterizer. */
  for (unsigned row = 0; row < 224; ++row) for (unsigned col = 0; col < 256; ++col)
    Equal(original[row * 256 + col] & 0xffffff,
        surface->backdrop[row * surface->pitch_pixels + 64 + extra + col] & 0xffffff,
        4, 3, 0, col, row);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s diorama-layers.ini room.arscene ...\n", argv[0]); return 2;
  }
  size_t size;
  char *ini = Read(argv[1], &size);
  reference = sr_scene_renderer_create(); assert(reference);
  packet = calloc(1, sizeof(*packet)); assert(packet);
  ActionSceneSnapshot *assets = malloc(sizeof(*assets)); assert(assets);
  for (int room = 2; room < argc; ++room) {
    current_room = argv[room];
    char *data = Read(current_room, &size);
    assert(ActionSceneSnapshot_Decode((const uint8_t *)data, size, assets)); free(data);
    EditorRoomScene *scene = EditorRoomScene_Create(assets); assert(scene);
    const unsigned first_full = full_add_frames, first_owned = owned_frames, first_fallback = fallback_frames;
    for (current_config = 0; current_config < 2; ++current_config) {
      assert(EditorRoomScene_Configure(scene, current_config ? ini : ""));
      const int width = (int)EditorRoomScene_Width(scene), height = (int)EditorRoomScene_Height(scene);
      /* Subtile phases, native/extended captures, both axes, edges and a wide
       * projected footprint including the BG aprons. Frame clocks also move
       * HDMA bands, animated tile pages and palettes. */
      const int views[][6] = {{0,0,0,0,0,800}, {1,7,17,72,32,960},
        {width/3, height/3,47,120,64,960}, {width/2+3,height/2+5,137,128,64,1260},
        {width-256,height-224,239,120,64,960}, {width+128,height+64,509,128,64,1260}};
      for (current_view = 0; current_view < sizeof(views)/sizeof(*views); ++current_view) {
        const int *v = views[current_view];
        const DioramaView view = {.distance_scale=1, .camera_framing_weight=1,
          .visible_width=256+2*v[3], .viewport={0,0,v[5],600}};
        EditorRoomCamera camera;
        assert(EditorRoomScene_ResolveCamera(scene,v[0],v[1],v[2],v[3],v[4],&view,kDioramaSky_Only,&camera));
        assert(EditorRoomScene_Render(scene,camera.x,camera.y,v[2],v[3],v[4],&camera));
        const uint32_t projected = EditorRoomScene_Hash(scene);
        FlatCapture(scene, assets, v[0], v[1], v[2], v[3]);
        /* Switching back must not reuse the flat framebuffer as a cached
         * projected capture, including when authored tile edits are loaded. */
        assert(EditorRoomScene_Render(scene,camera.x,camera.y,v[2],v[3],v[4],&camera));
        assert(EditorRoomScene_Hash(scene) == projected);
      }
    }
    printf("%02x:%02x profile=%u captures=36 full-add=%u owned=%u fallback=%u\n", assets->scene.group,
        assets->scene.map, assets->terrain_profile, full_add_frames-first_full,
        owned_frames-first_owned, fallback_frames-first_fallback); fflush(stdout);
    EditorRoomScene_Destroy(scene);
  }
  printf("PASS rooms=%d captures=%u scanouts=%u full-add=%u rows=%u owned=%u fallback=%u\n",
      argc-2, frames, frames*5, full_add_frames, full_add_rows, owned_frames, fallback_frames);
  sr_scene_renderer_destroy(reference); free(packet); free(assets); free(ini);
  return 0;
}
