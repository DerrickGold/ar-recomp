#include "action/action_floor_support.h"
#include "action/action_effect_receivers.h"
#include "room_scene.h"
#include "diorama/diorama_snapshot.h"
#include "diorama/diorama_rom_backdrop.h"
#include "action/action_decoration_pass.h"
#include "action/action_environment_exposure.h"
#include "action/action_effect_projection.h"
#include "action/action_effect_render.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

ArRenderDevice *DioramaPreview_Device(void);
uint8_t *DioramaPreview_Input(void);
unsigned DioramaPreview_Capacity(void);
void DioramaPreview_Reset(void);
bool DioramaPreview_Begin(int width, int height);

static EditorRoomScene *s_room;
static ArRenderTexture s_textures[kDioramaPlane_Count];
static const uint8_t *s_pixels[kDioramaPlane_Count];
static uint32_t s_storage[7][640 * 352];
static uint32_t s_next[640 * 352];
static const int kPlanes[] = {kDioramaPlane_Backdrop, SR_PPU_OVERLAY_BG1,
  kDioramaPlane_Bg1Hi, kDioramaPlane_Bg1Far, SR_PPU_OVERLAY_BG2,
  kDioramaPlane_Bg2Hi, kDioramaPlane_Bg2Far};
static bool s_cached;
static int s_x, s_y, s_extra, s_vertical;
static uint32_t s_frame;
static unsigned s_uploads;
static uint64_t s_bg2_revision;
static ArRenderTexture s_page, s_view;
static uint32_t s_view_pixels[640 * 352];
static uint64_t s_view_revision;
static uint32_t s_page_pixels[256 * 256];
static int s_named_source;
static uint32_t s_named_art[256 * 256], s_named_default, s_named_fill;
static ArRenderTexture s_named_texture;
static bool s_named_uploaded;

/* Shared native recipes, bounded scratch, and one submission per pass. */
static ActionSceneEffectRenderBatch s_effect_batch;
static bool s_effects_enabled = true;
static unsigned s_effect_vertices;
static bool s_probes;
static int s_probe_x[2],s_probe_y[2];
static ActionReceiverLighting s_receiver;
int RoomPreview_SetReceivers(int enabled,int player_x,int player_y,int enemy_x,int enemy_y) {
  if(player_x<-2048||player_x>16384||player_y<-2048||player_y>16384||
      enemy_x<-2048||enemy_x>16384||enemy_y<-2048||enemy_y>16384)return 0;
  s_probes=enabled!=0;s_probe_x[0]=player_x;s_probe_y[0]=player_y;s_probe_x[1]=enemy_x;s_probe_y[1]=enemy_y;return 1;
}
unsigned RoomPreview_EventCount(void){return ActionEffectPreview_Count();}
unsigned RoomPreview_EventKind(unsigned index){return ActionEffectPreview_Kind(index);}
int RoomPreview_SetEvent(unsigned index,int x,int y,int vx,int vy,unsigned start,unsigned duration,unsigned seed) {
  if(!s_room||index>ActionEffectPreview_Count()||x<-2048||x>16384||y<-2048||y>16384||
      vx<-16||vx>16||vy<-16||vy>16||start>65535||duration<1||duration>4096)return 0;
  ActionEffectPreviewEvent event={.kind=index?ActionEffectPreview_Kind(index-1):0,
    .x=x,.y=y,.velocity_x=vx,.velocity_y=vy,.start=start,.duration=duration,.seed=seed};
  EditorRoomScene_SetEvent(s_room,&event);s_cached=false;return 1;
}
void RoomPreview_EnableEffects(int enabled) { s_effects_enabled = enabled != 0; }
unsigned RoomPreview_EffectCount(void) {
  return s_room && s_effects_enabled ? EditorRoomScene_Effects(s_room)->decoration_count + EditorRoomScene_Effects(s_room)->authored_count + EditorRoomScene_Effects(s_room)->effect_count : 0;
}
unsigned RoomPreview_EffectVertices(void) { return s_effect_vertices; }
typedef struct RoomEffectPass {
  const ActionSceneEffectFrame *frame;
  ActionEffectProjectionContext projection;
  bool valid;
} RoomEffectPass;
static bool DrawEffectLayer(RoomEffectPass *pass, uint8_t layer,
    ArRenderBlendMode blend, bool lighting, bool particles) {
  if (!ActionSceneDecorationRender_Build(pass->frame, layer, lighting, particles,
          ActionEffectProjection_ProjectPoint, ActionEffectProjection_ClipBounds,
          &pass->projection, &s_effect_batch)) return false;
  if (!s_effect_batch.index_count) return true;
  s_effect_vertices += s_effect_batch.vertex_count;
  const ArRenderDrawState state = {.flags = kArRenderDrawState_Blend, .blend = blend};
  return ArRenderDevice_DrawGeometryWithState(DioramaPreview_Device(),
      ArRenderTexture_Invalid(), s_effect_batch.vertices, s_effect_batch.vertex_count,
      s_effect_batch.indices, s_effect_batch.index_count, &state);
}
static bool DrawProbes(RoomEffectPass *pass) {
  ArRenderVertex2D vertices[32];int32_t indices[48];
  static const float boxes[4][4]={{-3,-28,3,-22},{-6,-21,6,-10},{-5,-9,-1,0},{1,-9,5,0}};
  for(unsigned role=0;role<2;++role) {
    ActionEffectInstance e={.world_x=s_probe_x[role],.world_y=s_probe_y[role],
      .obj_priority=2,.projection_plane=kActionEffectProjectionPlane_Obj};
    float multiply[3]={1,1,1},add[3]={0};
    if(s_effects_enabled)ActionEffectReceivers_Sample(&s_receiver,role?kActionReceiver_Enemies:kActionReceiver_Player,e.world_x,e.world_y,multiply,add);
    const float base[2][3]={{.24f,.48f,.78f},{.75f,.32f,.16f}};
    ArRenderColorF color={fminf(1,base[role][0]*multiply[0]+add[0]),fminf(1,base[role][1]*multiply[1]+add[1]),fminf(1,base[role][2]*multiply[2]+add[2]),1};
    for(unsigned box=0;box<4;++box) {
      const unsigned n=role*16+box*4;
      for(unsigned corner=0;corner<4;++corner) {
        vertices[n+corner]=(ArRenderVertex2D){.color=color};
        if(!ActionEffectProjection_ProjectPoint(&pass->projection,&e,boxes[box][corner&1?2:0],boxes[box][corner&2?3:1],&vertices[n+corner].position))return false;
      }
      const int local[]={0,1,2,1,3,2};for(unsigned i=0;i<6;++i)indices[role*24+box*6+i]=n+local[i];
    }
  }
  const ArRenderDrawState state={.flags=kArRenderDrawState_Blend,.blend=kArRenderBlendMode_Alpha};
  return ArRenderDevice_DrawGeometryWithState(DioramaPreview_Device(),ArRenderTexture_Invalid(),vertices,32,indices,48,&state);
}
static void DrawPlaneEffects(void *userdata, int plane, const DioramaProjection *projection) {
  RoomEffectPass *context = userdata;
  if (!context->valid) return;
  context->projection.diorama_projection = projection;
  if(s_probes && plane==DioramaPlaneForObjectPriority(2) && !DrawProbes(context)){context->valid=false;return;}
  if(!s_effects_enabled)return;
  for (unsigned i = 0; i < sizeof(kDecorationPasses)/sizeof(kDecorationPasses[0]); ++i) {
    const ActionDecorationPass *pass = &kDecorationPasses[i];
    if (pass->attachment != plane ||
        (pass->finite_plane_only && projection->bg2_skybox.count)) continue;
    if (!DrawEffectLayer(context, pass->layer, pass->blend, pass->diorama_lighting, true)) {
      context->valid = false; return;
    }
  }
}

int RoomPreview_SkyboxSource(void) {
  if (!s_room) return 0;
  DioramaResolvedLayer layers[kDioramaPlane_Count];
  int count = Diorama_ResolveSceneLayers(EditorRoomScene_Scene(s_room), layers);
  return DioramaLayerOrder_SkyboxSource(layers, count);
}
unsigned RoomPreview_SkyboxRoom(void) {
  uint8_t g, m, bg;
  return DioramaLayerOrder_DecodeActionBgSource(RoomPreview_SkyboxSource(), &g, &m, &bg)
      ? ((unsigned)g << 16) | ((unsigned)m << 8) | bg : 0;
}
int RoomPreview_LoadSkybox(unsigned size) {
  ActionSceneSnapshot assets;
  uint8_t g, m, bg;
  uint32_t default_fill;
  const int source = RoomPreview_SkyboxSource();
  if (!DioramaLayerOrder_DecodeActionBgSource(source, &g, &m, &bg) ||
      size > kActionSceneSnapshotMaxBytes ||
      !ActionSceneSnapshot_Decode(DioramaPreview_Input(), size, &assets) ||
      assets.scene.group != g || assets.scene.map != m ||
      !DioramaRomBackdrop_RenderScenePage(&assets.scene, bg, 0, true,
          &default_fill, s_next, 256 * 256)) return 0;
  memcpy(s_named_art, s_next, sizeof(s_named_art));
  s_named_default = default_fill; s_named_source = source; s_named_uploaded = false;
  return 1;
}
static ArRenderTexture ResolveSkybox(void *context, ArRenderDevice *device, int source,
    bool configured, uint32_t fill, bool *failed) {
  (void)context; *failed = false;
  if (source != s_named_source || !source) return ArRenderTexture_Invalid();
  if (!ArRenderTexture_IsValid(s_named_texture)) {
    const ArRenderTextureDesc desc = {.width = 256, .height = 256,
      .format = kArRenderPixelFormat_Argb8888, .usage = kArRenderTextureUsage_Streaming,
      .filter = kArRenderFilter_Nearest, .blend = kArRenderBlendMode_Alpha};
    if (!ArRenderDevice_CreateTexture(device, &desc, &s_named_texture)) {
      *failed = true; return ArRenderTexture_Invalid();
    }
  }
  if (!configured) fill = s_named_default;
  if (!s_named_uploaded || s_named_fill != fill) {
    for (unsigned i = 0; i < 256 * 256; ++i) s_next[i] = s_named_art[i] ? s_named_art[i] : fill;
    if (!ArRenderDevice_UpdateTexture(device, s_named_texture, NULL, s_next, 256 * 4)) {
      *failed = true; return ArRenderTexture_Invalid();
    }
    s_named_fill = fill; s_named_uploaded = true; ++s_uploads;
  }
  return s_named_texture;
}

uint32_t RoomPreview_EffectHash(void) { return EditorRoomScene_EffectHash(s_room); }
uint32_t RoomPreview_Hash(void) { return EditorRoomScene_Hash(s_room); }
unsigned RoomPreview_Width(void) { return s_room ? EditorRoomScene_Width(s_room) : 0; }
unsigned RoomPreview_Height(void) { return s_room ? EditorRoomScene_Height(s_room) : 0; }
unsigned RoomPreview_Uploads(void) { return s_uploads; }
void RoomPreview_Reset(void) {
  ArRenderDevice *device = DioramaPreview_Device();
  Diorama_ResetCompositorResources(device);
  for (unsigned i = 0; i < kDioramaPlane_Count; ++i) {
    ArRenderDevice_DestroyTexture(device, s_textures[i]);
    s_textures[i] = ArRenderTexture_Invalid(); s_pixels[i] = NULL;
  }
  ArRenderDevice_DestroyTexture(device, s_page);
  s_page = ArRenderTexture_Invalid();
  ArRenderDevice_DestroyTexture(device, s_view); s_view = ArRenderTexture_Invalid();
  ArRenderDevice_DestroyTexture(device, s_named_texture);
  s_named_texture = ArRenderTexture_Invalid(); s_named_source = 0; s_named_uploaded = false;
  EditorRoomScene_Destroy(s_room); s_room = NULL; s_cached = false;
  memset(s_storage, 0, sizeof(s_storage));
}
int RoomPreview_Load(unsigned size) {
  ActionSceneSnapshot assets;
  if (size > kActionSceneSnapshotMaxBytes ||
      !ActionSceneSnapshot_Decode(DioramaPreview_Input(), size, &assets)) return 0;
  EditorRoomScene *next = EditorRoomScene_Create(&assets);
  if (!next) return 0;
  RoomPreview_Reset(); DioramaPreview_Reset(); s_room = next;
  return 1;
}
int RoomPreview_Configure(unsigned size) {
  if (!s_room || size > 1024 * 1024 || memchr(DioramaPreview_Input(), 0, size)) return 0;
  DioramaPreview_Input()[size] = 0;
  if (!EditorRoomScene_Configure(s_room, (char *)DioramaPreview_Input())) return 0;
  s_cached = false;
  return 1;
}
static unsigned s_recipe_error_line;
unsigned RoomPreview_RecipeErrorLine(void) { return s_recipe_error_line; }
int RoomPreview_ConfigureEffects(unsigned size) {
  if (!s_room || size > kActionEffectRecipeMaxBytes ||
      !EditorRoomScene_ConfigureEffects(s_room,(char *)DioramaPreview_Input(),size,&s_recipe_error_line)) return 0;
  s_cached = false; return 1;
}
/* Explicit field access keeps the WASM ABI independent of C padding. */
unsigned RoomPreview_SourceValue(unsigned index, unsigned field) {
  if (!s_room) return 0;
  const ActionSceneEffectFrame *frame = EditorRoomScene_Effects(s_room);
  if (index >= frame->decoration_count + frame->authored_count + frame->effect_count) return 0;
  const bool actor=index>=frame->decoration_count+frame->authored_count;
  const bool authored=!actor && index>=frame->decoration_count;
  const ActionEffectInstance *e=actor?&frame->effects[index-frame->decoration_count-frame->authored_count]:
      authored?&frame->authored[index-frame->decoration_count]:&frame->decorations[index];
  switch (field) {
    case 0: return actor||e->kind==kActionEffect_LandingDust?0:e->generation;
    case 1: return e->kind;
    case 2: return (uint32_t)(int32_t)e->world_x;
    case 3: return (uint32_t)(int32_t)e->world_y;
    case 5: return authored;
    case 6: return authored ? frame->authored_floor[index-frame->decoration_count].count : 0;
    case 7: return actor||e->kind==kActionEffect_LandingDust;
    case 4: return e->flags & kActionEffectFlag_Visible;
    default: return 0;
  }
}
/* Row-major overlay bytes in the existing exchange buffer. This API uses the
 * same collision semantics as native recipe capture, before any GPU render. */
unsigned RoomPreview_CollisionGrid(void) {
  if (!s_room) return 0;
  const ActionEnvironmentScene *scene=EditorRoomScene_Environment(s_room);
  const unsigned w=scene->maps[0].world_width/16,h=scene->maps[0].world_height/16;
  if (!w || !h || h>DioramaPreview_Capacity()/w) return 0;
  uint8_t *out=DioramaPreview_Input();
  for (unsigned y=0;y<h;++y) for (unsigned x=0;x<w;++x)
    out[y*w+x]=(uint8_t)ActionFloorSupport_Cell(scene,x*16,y*16);
  return w*h;
}
const char *RoomPreview_KindName(unsigned kind) { return ActionEffectRecipes_KindName(kind); }
unsigned RoomPreview_ReachSupported(unsigned kind) { return ActionEffectRecipes_ReachSupported(kind); }
static bool Upload(void) {
  ArRenderDevice *device = DioramaPreview_Device();
  const DioramaCapture *capture = EditorRoomScene_Capture(s_room);
  const SrSceneSurfaces *surfaces = EditorRoomScene_Surfaces(s_room);
  for (unsigned i = 0; i < sizeof(kPlanes) / sizeof(kPlanes[0]); ++i) {
    int plane = kPlanes[i];
    const uint8_t *source = capture->pixels[plane];
    s_pixels[plane] = NULL;
    if (!source) continue;
    memset(s_next, 0, sizeof(s_next));
    for (int y = 0; y < surfaces->height; ++y) {
      uint32_t *row = s_next + y * 640;
      memcpy(row, source + (size_t)y * surfaces->pitch_pixels * 4, surfaces->pitch_pixels * 4);
      if (plane == kDioramaPlane_Backdrop)
        for (int x = 64; x < surfaces->width + 64; ++x) row[x] |= UINT32_C(0xff000000);
    }
    bool created = false;
    if (!ArRenderTexture_IsValid(s_textures[plane])) {
      const ArRenderTextureDesc desc = {.width = 640, .height = 352,
        .format = kArRenderPixelFormat_Argb8888, .usage = kArRenderTextureUsage_Streaming,
        .filter = kArRenderFilter_Nearest, .blend = kArRenderBlendMode_Alpha};
      if (!ArRenderDevice_CreateTexture(device, &desc, &s_textures[plane])) return false;
      created = true;
    }
    if (created || memcmp(s_storage[i], s_next, sizeof(s_next))) {
      if (!ArRenderDevice_UpdateTexture(device, s_textures[plane], NULL, s_next, 640 * 4)) return false;
      memcpy(s_storage[i], s_next, sizeof(s_next)); ++s_uploads;
      if (plane == SR_PPU_OVERLAY_BG2) ++s_bg2_revision;
    }
    s_pixels[plane] = (const uint8_t *)s_storage[i];
  }
  if (surfaces->background_view) {
    bool created = false;
    if (!ArRenderTexture_IsValid(s_view)) {
      const ArRenderTextureDesc desc = {.width = 640, .height = 352,
        .format = kArRenderPixelFormat_Argb8888, .usage = kArRenderTextureUsage_Streaming,
        .filter = kArRenderFilter_Nearest, .blend = kArRenderBlendMode_Alpha};
      if (!ArRenderDevice_CreateTexture(device, &desc, &s_view)) return false;
      created = true;
    }
    memset(s_next, 0, sizeof(s_next));
    for (int y = 0; y < surfaces->height; ++y)
      memcpy(s_next + y * 640, surfaces->background_view + y * surfaces->view_width,
          surfaces->view_width * sizeof(uint32_t));
    if (created || memcmp(s_view_pixels, s_next, sizeof(s_view_pixels))) {
      if (!ArRenderDevice_UpdateTexture(device, s_view, NULL, s_next, 640 * 4)) return false;
      memcpy(s_view_pixels, s_next, sizeof(s_view_pixels)); ++s_uploads; ++s_view_revision;
    }
  }
  if (surfaces->native_pages[1]) {
    bool created = false;
    if (!ArRenderTexture_IsValid(s_page)) {
      const ArRenderTextureDesc desc = {.width = 256, .height = 256,
        .format = kArRenderPixelFormat_Argb8888, .usage = kArRenderTextureUsage_Streaming,
        .filter = kArRenderFilter_Linear, .blend = kArRenderBlendMode_Alpha};
      if (!ArRenderDevice_CreateTexture(device, &desc, &s_page)) return false;
      created = true;
    }
    if (created || memcmp(s_page_pixels, surfaces->native_pages[1], sizeof(s_page_pixels))) {
      if (!ArRenderDevice_UpdateTexture(device, s_page, NULL, surfaces->native_pages[1], 256 * 4)) return false;
      memcpy(s_page_pixels, surfaces->native_pages[1], sizeof(s_page_pixels)); ++s_uploads;
    }
  }
  return true;
}
int RoomPreview_Render(int x, int y, uint32_t frame, int extra, int vertical,
    int width, int height, float distance, float yaw, float pitch, int skybox, int pixel_aspect) {
  if (!s_room || !(distance >= 0.5f && distance <= 2.5f) || !(yaw >= -0.7f && yaw <= 0.7f) ||
      !(pitch >= -0.7f && pitch <= 0.7f) || skybox < 0 || skybox > 2 ||
      pixel_aspect < 0 || pixel_aspect > 1 || width < 1 || height < 1 ||
      width > 2048 || height > 2048) return 0;
  s_uploads = 0; s_effect_vertices = 0;
  if (!s_cached || x != s_x || y != s_y || frame != s_frame || extra != s_extra || vertical != s_vertical) {
    s_cached = false;
    if (!EditorRoomScene_Render(s_room, x, y, frame, extra, vertical) || !Upload()) return 0;
    s_x = x; s_y = y; s_frame = frame; s_extra = extra; s_vertical = vertical; s_cached = true;
  }
  if (!DioramaPreview_Begin(width, height)) return 0;
  DioramaCapture capture = *EditorRoomScene_Capture(s_room);
  capture.textures = s_textures; capture.pixels = s_pixels;
  /* Orbit/aspect changes reuse the filtered skybox until its pixels change. */
  capture.bg2_revision = s_bg2_revision;
  capture.bg2_dynamic = false;
  DioramaSkyboxView extended = *capture.skybox;
  if (EditorRoomScene_Surfaces(s_room)->background_view) {
    extended.texture = s_view; extended.revision = s_view_revision; capture.skybox = &extended;
  }
  const DioramaSkyboxView page = {.texture = s_page, .width = 256, .periodic = true};
  if (skybox == kDioramaSky_Only && EditorRoomScene_Surfaces(s_room)->native_pages[1])
    capture.skybox = &page;
  DioramaScene scene = *EditorRoomScene_Scene(s_room);
  DioramaRenderOptions options = *scene.render;
  options.resolve_skybox = ResolveSkybox;
  options.skybox = (DioramaSkyMode)skybox; scene.render = &options;
  DioramaView view = {.camera = {.tilt_x = pitch, .tilt_y = yaw},
    .distance_scale = distance, .camera_framing_weight = 1, .pixel_aspect = pixel_aspect,
    .visible_width = capture.width, .viewport = {0,0,width,height}};
  const ActionEnvironmentScene *environment = EditorRoomScene_Environment(s_room);
  RoomEffectPass effects = {.frame = EditorRoomScene_Effects(s_room), .valid = true,
    .projection = {.bg1_camera_x = environment->camera_x[0], .bg1_camera_y = environment->camera_y[0],
      .bg2_camera_x = environment->camera_x[1], .bg2_camera_y = environment->camera_y[1],
      .ws_extra = extra, .ws_extra_top = capture.authentic_y0,
      .visible_width = capture.width, .snes_height = capture.height, .viewport = view.viewport}};
  if(s_probes && !ActionEffectReceivers_Prepare(effects.frame,environment->group,environment->room,
      environment->camera_x[0],environment->camera_y[0],environment->camera_x[1],environment->camera_y[1],s_effects_enabled,&s_receiver))return 0;
  if (s_effects_enabled || s_probes) {
    scene.bg1_dimming = s_effects_enabled ? ActionEnvironment_Bg1Dimming(effects.frame, environment->group, environment->room) : 0;
    scene.bg1_dimming_ramp = ActionEnvironment_Bg1DimmingRamp(environment->group, environment->room);
    scene.effect_obj_priority_mask = ActionEffectProjection_RequiredObjPriorityMask(NULL, effects.frame) | (s_probes?1u<<2:0);
    scene.effect_bg_plane_mask = ActionEffectProjection_RequiredBgPlaneMask(NULL, effects.frame);
    scene.plane_effect = DrawPlaneEffects; scene.plane_effect_userdata = &effects;
  }
  DioramaProjection projection;
  if (!PresentationOutcome_IsUsable(Diorama_Composite(
      DioramaPreview_Device(), &capture, &view, &scene, &projection)) || !effects.valid) return 0;
  if (!s_effects_enabled) return 1;
  effects.projection.diorama_projection = &projection;
  if(!ActionSceneEffectRender_Build(effects.frame,true,true,ActionEffectProjection_ProjectPoint,&effects.projection,&s_effect_batch))return 0;
  if(s_effect_batch.index_count) {
    s_effect_vertices+=s_effect_batch.vertex_count;
    const ArRenderDrawState state={.flags=kArRenderDrawState_Blend,.blend=kArRenderBlendMode_Add};
    if(!ArRenderDevice_DrawGeometryWithState(DioramaPreview_Device(),ArRenderTexture_Invalid(),s_effect_batch.vertices,s_effect_batch.vertex_count,s_effect_batch.indices,s_effect_batch.index_count,&state))return 0;
  }
  return DrawEffectLayer(&effects, kActionEffectRenderLayer_WorldOverlay, kArRenderBlendMode_Add, true, true) &&
      DrawEffectLayer(&effects, kActionEffectRenderLayer_WorldDust, kArRenderBlendMode_Alpha, false, true) &&
      DrawEffectLayer(&effects, kActionEffectRenderLayer_ForegroundLight, kArRenderBlendMode_Light, true, false);
}
