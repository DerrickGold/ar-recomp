/* Whole-room capture oracle. Supply exported .arscene files locally:
 * action_room_capture_test diorama-layers.ini room1.arscene ...
 * The editor's call is renamed in its test object only; its real planner,
 * painted scenery, raster animation and camera policies remain unchanged. */
#include "room_scene.h"
#include "action/action_voxel.h"
#include "support/test_assert.h"
#include <math.h>
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
  static uint32_t guard_expected[2][4][640 * 64];
  assert(input.background_guard_count <= 2);
  for (unsigned i = 0; i < input.background_guard_count; ++i) {
    const SrPpuBackgroundGuardRequest *guard = &input.background_guards[i];
    assert(expected.background_guards_ready && guard->height == 64);
    if (guard->pixels) {
      assert(guard->pitch_bytes == 640 * 4);
      memcpy(guard_expected[i][0], guard->pixels, sizeof(guard_expected[i][0]));
    }
    const SrPpuBackgroundGuardBands *bands = input.background_guard_bands[i];
    if (bands) for (unsigned band = 0; band < 3; ++band) {
      assert(bands->bands[band].pixels && bands->bands[band].pitch_bytes == 640 * 4);
      memcpy(guard_expected[i][band + 1], bands->bands[band].pixels, sizeof(guard_expected[i][0]));
    }
  }
  input.reference_renderer = false;
  if (input.background_guard_count && input.background_guard_bands[0]) {
    /* Existing scene-frame callers do not own the appended band pointers.
     * A short legacy request must ignore even a poisoned new tail. */
    SrSceneFrame legacy = input;
    legacy.struct_size = SR_SCENE_FRAME_BACKGROUND_GUARDS_SIZE;
    legacy.background_guards = NULL;
    legacy.background_guard_count = 0;
    legacy.background_guard_bands[0] = legacy.background_guard_bands[1] =
        (const SrPpuBackgroundGuardBands *)(uintptr_t)1;
    assert(sr_scene_renderer_render(renderer, &legacy, out));
    assert(!out->background_guards_ready);
    for (unsigned bg = 0; bg < 2; ++bg) for (unsigned band = 0; band < 3; ++band)
      for (unsigned y = 0; y < (unsigned)out->height; ++y)
        for (unsigned x = 0; x < (unsigned)out->pitch_pixels; ++x) {
          const size_t at = (size_t)y * out->pitch_pixels + x;
          Equal(expected.bands[bg][band][at], out->bands[bg][band][at], 4, bg, band, x, y);
        }
  }
  const unsigned requests[] = {0, SR_PPU_BG_PACKET_TILES,
      SR_PPU_BG_PACKET_TILES | SR_PPU_BG_PACKET_FULL_ADD,
      SR_PPU_BG_PACKET_TILES | SR_PPU_BG_PACKET_FULL_ADD | SR_PPU_BG_PACKET_VALIDATE_TILES};
  for (unsigned mode = 0; mode < 4; ++mode) {
    input.background_packet = mode ? packet : NULL;
    packet->request_flags = requests[mode];
    assert(sr_scene_renderer_render(renderer, &input, out));
    assert(out->background_guards_ready == expected.background_guards_ready);
    for (unsigned i = 0; i < input.background_guard_count; ++i) {
      if (input.background_guards[i].pixels)
        assert(!memcmp(guard_expected[i][0], input.background_guards[i].pixels, sizeof(guard_expected[i][0])));
      const SrPpuBackgroundGuardBands *bands = input.background_guard_bands[i];
      if (bands) for (unsigned band = 0; band < 3; ++band)
        assert(!memcmp(guard_expected[i][band + 1], bands->bands[band].pixels, sizeof(guard_expected[i][0])));
    }
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

static void VoxelCutoutModes(EditorRoomScene *scene) {
  assert(EditorRoomScene_Configure(scene,""));
  const Action3DMode modes[]={kAction3D_Voxel,kAction3D_Diorama,kAction3D_Off,kAction3D_Voxel};
  uint32_t voxel_hash=0;
  for(unsigned i=0;i<sizeof(modes)/sizeof(*modes);++i) {
    EditorRoomScene_Set3DMode(scene,modes[i]);
    assert(EditorRoomScene_Render(scene,240,272,1200,72,32,NULL));
    const ActionEnvironmentScene *environment=EditorRoomScene_Environment(scene);
    ActionEnvironmentTileEdit tile;
    assert(environment->tile_edit(environment->tile_edit_context,0,40,47,&tile));
    assert(tile.transparent[7]==(modes[i]==kAction3D_Voxel?0xff:0));
    assert(!tile.black[7] && tile.band==1);
    /* The raw native source and environmental receivers share the mask. */
    const DioramaCapture *capture=EditorRoomScene_Capture(scene);
    const SrSceneSurfaces *surfaces=EditorRoomScene_Surfaces(scene);
    const int ox=capture->bg1_camera_x-(capture->width-256)/2-capture->obj_apron;
    const int oy=capture->camera_y-capture->authentic_y0+1;
    const uint32_t pixel=surfaces->bands[0][0][(383-oy)*surfaces->pitch_pixels+320-ox];
    assert(!!(pixel>>24)==(modes[i]!=kAction3D_Voxel));
    if(i==0)voxel_hash=EditorRoomScene_Hash(scene);
    if(i==3)assert(voxel_hash==EditorRoomScene_Hash(scene));
  }
}
static void VoxelMarshModes(EditorRoomScene *room,const ActionRoomScene *source) {
  assert(EditorRoomScene_Configure(room,""));
  const Action3DMode modes[]={kAction3D_Voxel,kAction3D_Diorama,kAction3D_Off,kAction3D_Voxel};
  int tx=-1,ty=-1,px=0,py=0;unsigned high=0;uint32_t first=0;
  for(unsigned i=0;i<4;++i) {
    EditorRoomScene_Set3DMode(room,modes[i]);
    assert(EditorRoomScene_Render(room,448,224,1200,72,32,NULL));
    const DioramaScene *scene=EditorRoomScene_Scene(room);
    if(!i) {
      assert(ActionVoxel_Active(scene->voxel_geometry,2,1));
      for(int y=36;y<52 && tx<0;++y)for(int x=50;x<85 && tx<0;++x) {
        uint8_t rows[8]={0};
        if(!ActionVoxel_CutoutRows(scene->voxel_geometry,x,y,rows))continue;
        for(int yy=0;yy<8 && tx<0;++yy)for(int xx=0;xx<8 && tx<0;++xx)
          if(rows[yy]&(0x80u>>xx)) {tx=x;ty=y;px=xx;py=yy;}
      }
      assert(tx>=0);uint16_t word;
      assert(ActionRoomScene_LookupTile(source,1,tx,ty,&word,NULL));high=!!(word&0x2000);
    }
    const ActionEnvironmentScene *environment=EditorRoomScene_Environment(room);
    ActionEnvironmentTileEdit edit;
    assert(environment->tile_edit(environment->tile_edit_context,0,tx,ty,&edit));
    assert(!!(edit.transparent[py]&(0x80u>>px))==(modes[i]==kAction3D_Voxel));
    const DioramaCapture *capture=EditorRoomScene_Capture(room);
    const SrSceneSurfaces *surfaces=EditorRoomScene_Surfaces(room);
    const int ox=capture->bg1_camera_x-(capture->width-256)/2-capture->obj_apron;
    const int oy=capture->camera_y-capture->authentic_y0+1;
    const uint32_t pixel=surfaces->bands[0][high][(ty*8+py-oy)*surfaces->pitch_pixels+tx*8+px-ox];
    assert(!!(pixel>>24)==(modes[i]!=kAction3D_Voxel));
    if(!i)first=EditorRoomScene_Hash(room);
    if(i==3)assert(first==EditorRoomScene_Hash(room));
  }
}
static void VoxelPolicyModes(EditorRoomScene *room,unsigned group,unsigned map) {
  const bool temple=group==1 && map==3, tower=group==1 && map==4,jungle=group==5 && map<=3;
  char text[256];snprintf(text,sizeof(text),
      "[layers:%02x:%02x]\nbg1 = alpha:0\nbg2 = alpha:255 z:0.1 stack:0.2 copies:3\nbg2hi = alpha:255\nbackdrop = source:captured\n",group,map);
  assert(EditorRoomScene_Configure(room,text));
  const Action3DMode modes[]={kAction3D_Voxel,kAction3D_Diorama,kAction3D_Off,kAction3D_Voxel};
  uint32_t first=0;
  for(unsigned i=0;i<sizeof(modes)/sizeof(*modes);++i) {
    EditorRoomScene_Set3DMode(room,modes[i]);
    assert(EditorRoomScene_Render(room,0,0,1200,72,32,NULL));
    const DioramaScene *scene=EditorRoomScene_Scene(room);
    DioramaResolvedLayer layers[kDioramaPlane_Count];
    const int count=Diorama_ResolveSceneLayers(scene,layers);assert(count>0 && count<=kDioramaPlane_Count);
    unsigned found=0;
    for(int j=0;j<count;++j) {
      const DioramaResolvedLayer *layer=&layers[j];
      if(layer->plane==SR_PPU_OVERLAY_BG1) { assert(layer->alpha==(modes[i]==kAction3D_Voxel?255:0));found|=1; }
      if(temple && (layer->plane==SR_PPU_OVERLAY_BG2 || layer->plane==kDioramaPlane_Bg2Hi))
        { assert(layer->alpha==(modes[i]==kAction3D_Voxel?0:255));found|=layer->plane==SR_PPU_OVERLAY_BG2?2:4; }
      if(temple && layer->plane==kDioramaPlane_Backdrop) {
        assert(layer->source==(modes[i]==kAction3D_Voxel?DioramaLayerOrder_ActionBgSource(1,4,2):kDioramaLayerSource_Captured));found|=8;
      }
    }
    if(jungle)for(int j=0;j<count;++j)if(layers[j].plane==SR_PPU_OVERLAY_BG2) {
      assert(layers[j].z==(modes[i]==kAction3D_Voxel?.5f:.1f));
      assert(layers[j].stack_copies==(modes[i]==kAction3D_Voxel?1:3));found|=2;
    }
    assert(found==(temple?15:jungle?3:1));
    if(modes[i]!=kAction3D_Off) {
      /* Model policy must also reach pure camera/footprint queries, including
       * free orbit. Switching back to Diorama restores the saved facade bias. */
      const DioramaView view={.viewport={0,0,960,600},.visible_width=400,
        .distance_scale=1,.camera={.tilt_x=.15f,.tilt_y=-.4f}};
      for(int dynamic=0;dynamic<=1;++dynamic) {
        DioramaView framing_view=view;framing_view.camera_framing_weight=(float)dynamic;
        DioramaCapture capture=*EditorRoomScene_Capture(room);
        ArRenderRectF biased,native,horizontal;
        capture.framing_x=-34;capture.framing_y=-32;
        assert(Diorama_ResolveCaptureFootprint(&capture,&framing_view,scene,kDioramaPlane_Obj2,&biased));
        capture.framing_y=0;
        assert(Diorama_ResolveCaptureFootprint(&capture,&framing_view,scene,kDioramaPlane_Obj2,&native));
        const bool neutral=tower && modes[i]==kAction3D_Voxel;
        assert((fabsf(biased.y-native.y)<.001f)==neutral);
        capture.framing_x=0;
        assert(Diorama_ResolveCaptureFootprint(&capture,&framing_view,scene,kDioramaPlane_Obj2,&horizontal));
        /* Dynamic edge stops can legitimately cancel horizontal framing. */
        if(!dynamic)assert(fabsf(horizontal.x-native.x)>1);
      }
    }
    if(!i)first=EditorRoomScene_Hash(room);
    if(i==3)assert(first==EditorRoomScene_Hash(room));
  }
}
static void TrunkGuardModes(EditorRoomScene *scene) {
  const int x = 392, y = (int)EditorRoomScene_Height(scene) / 2;
  const Action3DMode modes[] = {kAction3D_Voxel, kAction3D_Diorama, kAction3D_Off, kAction3D_Voxel};
  for (unsigned i = 0; i < 4; ++i) {
    EditorRoomScene_Set3DMode(scene, modes[i]);
    assert(EditorRoomScene_Render(scene, x, y, 16, 120, 64, NULL));
    const DioramaTrunkGuardSource *guard = EditorRoomScene_TrunkGuard(scene);
    assert(!!guard == (modes[i] == kAction3D_Voxel));
    if (guard) {
      assert(guard->pitch_pixels == 640 && guard->pixel_count == 640 * 128);
      assert(guard->layout.strips[0].world_y + 64 == guard->layout.primary_y);
      assert(guard->layout.strips[1].world_y == guard->layout.primary_y + guard->layout.primary_height);
      const uint64_t generation = guard->capture_generation;
      const unsigned rasters = EditorRoomScene_RasterCount(scene);
      assert(EditorRoomScene_Render(scene, x, y, 16, 120, 64, NULL));
      assert(EditorRoomScene_RasterCount(scene) == rasters);
      assert(EditorRoomScene_TrunkGuard(scene)->capture_generation == generation);
      assert(EditorRoomScene_Render(scene, x, y, 24, 120, 64, NULL));
      assert(EditorRoomScene_TrunkGuard(scene)->capture_generation != generation);
    }
  }
}

#include "action_editor_voxel_guard.inc"

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
    const unsigned first_full = full_add_frames, first_owned = owned_frames, first_fallback = fallback_frames, first_frame=frames;
    if (getenv("AR_EDITOR_VOXEL_GUARD_ONLY")) {
      EditorVoxelGuardFixtures(scene, assets, ini);
      if (assets->scene.group == 6 && (assets->scene.map == 6 || assets->scene.map == 7)) TrunkGuardModes(scene);
      EditorRoomScene_Destroy(scene);
      continue;
    }
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
    if(assets->scene.group==2 && assets->scene.map==1) VoxelMarshModes(scene,&assets->scene);
    if(assets->scene.group==1 && assets->scene.map==2) VoxelCutoutModes(scene);
    if(assets->scene.group==1 && (assets->scene.map==3 || assets->scene.map==4))
      VoxelPolicyModes(scene,1,assets->scene.map);
    if(assets->scene.group==3 && assets->scene.map>=1 && assets->scene.map<=6)
      VoxelPolicyModes(scene,3,assets->scene.map);
    if(assets->scene.group==4 && assets->scene.map>=1 && assets->scene.map<=7)
      VoxelPolicyModes(scene,4,assets->scene.map);
    if(assets->scene.group==5 && assets->scene.map>=1 && assets->scene.map<=3)
      VoxelPolicyModes(scene,5,assets->scene.map);
    if(assets->scene.group==6 && (assets->scene.map==6 || assets->scene.map==7)) TrunkGuardModes(scene);
    printf("%02x:%02x profile=%u captures=%u full-add=%u owned=%u fallback=%u\n", assets->scene.group,
        assets->scene.map, assets->terrain_profile, frames-first_frame, full_add_frames-first_full,
        owned_frames-first_owned, fallback_frames-first_fallback); fflush(stdout);
    EditorRoomScene_Destroy(scene);
  }
  printf("PASS rooms=%d captures=%u scanouts=%u full-add=%u rows=%u owned=%u fallback=%u\n",
      argc-2, frames, frames*5, full_add_frames, full_add_rows, owned_frames, fallback_frames);
  sr_scene_renderer_destroy(reference); free(packet); free(assets); free(ini);
  return 0;
}
