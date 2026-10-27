#include "action/action_heat.h"
#include "action/action_floor_support.h"
#include "action/action_effect_members.h"
#include "action/action_effect_receivers.h"
#include "room_scene.h"
#include "room_preview_native.h"
#include "receiver_preview.h"
#include "action/action_effect_source.h"
#include "action/action_effect_mask_texture.h"
#include "diorama/diorama_snapshot.h"
#include "diorama/diorama_astral_rift.h"
#include "diorama/diorama_rom_backdrop.h"
#include "action/action_decoration_pass.h"
#include "action/action_environment_exposure.h"
#include "action/action_exposure.h"
#include "render/scenery_dimming.h"
#include "render/presentation_layout.h"
#include "present/display_geometry.h"
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
static EditorRoomCamera s_camera;
static DioramaCameraPose s_baseline = {.distance = 3.25f};
static int s_aspect_mode = 1;
static bool s_dynamic_camera = true;
static bool s_final_battle;

void RoomPreview_FinalBattle(int enabled) { s_final_battle = enabled != 0; }

/* 0 native 4:3, 1 fixed wide ratio, 2 Auto/custom monitor, 3 Stretch.
 * Capture budgets are independent of the display crop, just as in the game. */
int RoomPreview_SetView(float distance, float yaw, float pitch, int aspect_mode, int dynamic_camera) {
  if (!isfinite(distance) || distance < 0 || distance > 20 ||
      !isfinite(yaw) || fabsf(yaw) > 0.8f || !isfinite(pitch) || fabsf(pitch) > 0.8f ||
      aspect_mode < 0 || aspect_mode > 3 || dynamic_camera < 0 || dynamic_camera > 1) return 0;
  s_baseline = (DioramaCameraPose){.distance = distance, .tilt_x = pitch, .tilt_y = yaw};
  s_aspect_mode = aspect_mode;
  s_dynamic_camera = dynamic_camera != 0;
  return 1;
}
static DioramaView PreviewView(int width, int height, int pixels,
    float zoom, float yaw, float pitch) {
  ActRaiserAutoCanvas canvas = {0};
  if (s_aspect_mode == 2) DisplayGeometry_ResolveAutoCanvas(width, height, pixels == kPixelAspect_Crt43, &canvas);
  else if (s_aspect_mode != 0) canvas.extra_columns = DisplayGeometry_CalculateHorizontal(
      224, s_aspect_mode == 3 ? 16 : width, s_aspect_mode == 3 ? 9 : height,
      pixels == kPixelAspect_Crt43, false).display_extra;
  DioramaView view = {.camera = s_baseline, .distance_scale = zoom,
    .camera_framing_weight = s_dynamic_camera ? 1 : 0,
    .pixel_aspect = pixels, .ignore_aspect_ratio = s_aspect_mode == 3,
    .visible_width = 256 + 2 * canvas.extra_columns, .visible_height = 224 + 2 * canvas.extra_rows};
  view.camera.tilt_y += yaw; view.camera.tilt_x += pitch;
  view.viewport = ArPresentationLayout_ResolveViewport(width, height, view.ignore_aspect_ratio,
      pixels == kPixelAspect_Crt43, view.visible_width, view.visible_height);
  return view;
}
/* Uses exactly the baseline camera stop used by Render, without doing a
 * frame capture. Results are borrowed through the existing transport buffer. */
unsigned RoomPreview_Measure(int x, int y, uint32_t frame, int extra, int vertical,
    int width, int height, float zoom, float yaw, float pitch, int skybox, int pixels, unsigned bg) {
  if (!s_room || width < 1 || height < 1 || width > 2048 || height > 2048 ||
      !isfinite(zoom) || zoom < 0.5f || zoom > 2.5f || !isfinite(yaw) || fabsf(yaw) > 0.7f ||
      !isfinite(pitch) || fabsf(pitch) > 0.7f || skybox < 0 || skybox > 2 || pixels < 0 || pixels > 1) return 0;
  const DioramaView baseline = PreviewView(width, height, pixels, 1, 0, 0);
  const DioramaView view = PreviewView(width, height, pixels, zoom, yaw, pitch);
  EditorRoomCamera camera;
  ArRenderRectF rect; int fill;
  if (!EditorRoomScene_ResolveCamera(s_room, x, y, frame, extra, vertical, &baseline, skybox, &camera) ||
      !EditorRoomScene_MeasureView(s_room, frame, extra, vertical, &view, skybox, &camera, bg, &rect, &fill)) return 0;
  const float result[] = {camera.x, camera.y, camera.travel.minimum, camera.travel.maximum, camera.maximum_y,
    rect.x, rect.y, rect.x + rect.w, rect.y + rect.h, fill, view.visible_width, view.visible_height,
    view.viewport.x, view.viewport.y, view.viewport.w, view.viewport.h};
  memcpy(DioramaPreview_Input(), result, sizeof(result));
  return sizeof(result) / sizeof(*result);
}
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
static PresentActionSourceDraw s_source_draw;
static DioramaSkyboxDraw s_skybox_draw;
static DioramaSourceEffectBatch s_source_batch;
static const ArRenderPointF *s_projection_offsets;
static ArRenderPointF s_skybox_offset;
static ActionEffectSourcePrimitive s_source_primitives[kActionSourceMaximumPrimitives];
static ActionEffectSourceLightJob s_source_lights[kActionSourceMaxLightJobs];
void RoomPreview_SetSourceProjection(PresentActionSourceDraw effects, DioramaSkyboxDraw skybox) {
  s_source_draw = effects; s_skybox_draw = skybox;
}
void RoomPreview_SetSourceEffectBatch(DioramaSourceEffectBatch batch) { s_source_batch = batch; }
void RoomPreview_SetProjectionOffsets(const ArRenderPointF *planes, ArRenderPointF skybox) {
  s_projection_offsets=planes;s_skybox_offset=skybox;
}
static bool s_effects_enabled = true;
static unsigned s_effect_vertices;
static EditorReceiverPreviewSettings s_receivers;
static bool s_receiver_rim;
static DioramaProjection s_receiver_projection;
static ActionReceiverLighting s_receiver;
int RoomPreview_LoadPlayer(unsigned size) {
  return size<=DioramaPreview_Capacity() && EditorReceiverPreview_Load(DioramaPreview_Input(),size);
}
int RoomPreview_ReceiverStyle(unsigned flipped,unsigned rim) {
  if(flipped>1||rim>1)return 0;
  s_receivers.flipped=flipped!=0;s_receiver_rim=rim!=0;return 1;
}
int RoomPreview_SetReceivers(int mask,int player_x,int player_y,int enemy_x,int enemy_y) {
  if(mask<0||mask>3||player_x<-2048||player_x>16384||player_y<-2048||player_y>16384||
      enemy_x<-2048||enemy_x>16384||enemy_y<-2048||enemy_y>16384)return 0;
  if(s_receivers.mask!=(unsigned)mask||s_receivers.x[0]!=player_x||s_receivers.feet_y[0]!=player_y||
      s_receivers.x[1]!=enemy_x||s_receivers.feet_y[1]!=enemy_y)s_cached=false;
  s_receivers.mask=(unsigned)mask;
  s_receivers.x[0]=player_x;s_receivers.feet_y[0]=player_y;
  s_receivers.x[1]=enemy_x;s_receivers.feet_y[1]=enemy_y;return 1;
}
int RoomPreview_PickReceiver(float screen_x,float screen_y) {
  ArRenderPointF point;
  if(!s_room||!EditorReceiverPreview_Pick(&s_receiver_projection,screen_x,screen_y,&point))return 0;
  const DioramaCapture *capture=EditorRoomScene_Capture(s_room);
  point.x+=s_x-s_extra;point.y+=s_y-capture->authentic_y0;
  if(point.x<-2048||point.x>16384||point.y<-2048||point.y>16384)return 0;
  const int32_t world[2]={(int32_t)lroundf(point.x),(int32_t)lroundf(point.y)};
  memcpy(DioramaPreview_Input(),world,sizeof(world));return 2;
}
unsigned RoomPreview_EventCount(void){return ActionEffectPreview_Count();}
unsigned RoomPreview_EventKind(unsigned index){return ActionEffectPreview_Kind(index);}
int RoomPreview_PreviewBinding(unsigned enabled,unsigned source,int x,int y,unsigned start) {
  if(enabled>1||!EditorRoomScene_PreviewBinding(s_room,enabled!=0,source,x,y,start))return 0;
  s_cached=false;return 1;
}
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
static ArRenderTexture s_effect_masks;
static bool DrawEffectLayer(RoomEffectPass *pass, uint8_t layer,
    ArRenderBlendMode blend, bool lighting, bool particles) {
  if (s_source_draw) {
    ActionEffectSourceBatch source = {.context=pass->projection,
        .primitives=s_source_primitives,.capacity=kActionSourceMaximumPrimitives,
        .lights=s_source_lights,.light_capacity=kActionSourceMaxLightJobs};
    if (!ActionSceneDecorationRender_Build(pass->frame,layer,lighting,particles,
        ActionEffectSource_ProjectPoint,ActionEffectSource_ClipBounds,&source,&s_effect_batch)) return false;
    s_effect_vertices += source.count;
    return s_source_draw(DioramaPreview_Device(),&source,pass->projection.diorama_projection,
        &pass->frame->scenery,blend,pass->projection.scenery_lighting_only?1.f/kActionReceiverMaxIllumination:1);
  }
  if (!ActionSceneDecorationRender_Build(pass->frame, layer, lighting, particles,
          ActionEffectProjection_ProjectPoint, ActionEffectProjection_ClipBounds,
          &pass->projection, &s_effect_batch)) return false;
  if (!s_effect_batch.index_count) return true;
  if (pass->projection.scenery_lighting_only)
    for (int i=0;i<s_effect_batch.vertex_count;++i) {
      s_effect_batch.vertices[i].color.r/=kActionReceiverMaxIllumination;
      s_effect_batch.vertices[i].color.g/=kActionReceiverMaxIllumination;
      s_effect_batch.vertices[i].color.b/=kActionReceiverMaxIllumination;
    }
  s_effect_vertices += s_effect_batch.vertex_count;
  const bool textured=ActionEffectMasks_UsesTexture(s_effect_batch.vertices,s_effect_batch.vertex_count);
  if(textured&&!ActionEffectMasks_EnsureTexture(DioramaPreview_Device(),&s_effect_masks))return false;
  const ArRenderDrawState state = {.flags = kArRenderDrawState_Blend, .blend = blend};
  return ArRenderDevice_DrawGeometryWithState(DioramaPreview_Device(),
      textured?s_effect_masks:ArRenderTexture_Invalid(), s_effect_batch.vertices, s_effect_batch.vertex_count,
      s_effect_batch.indices, s_effect_batch.index_count, &state);
}
static void DrawPlaneEffects(void *userdata, int plane, const DioramaProjection *projection) {
  RoomEffectPass *context = userdata;
  if (!context->valid) return;
  context->projection.diorama_projection = projection;
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

static bool DrawSceneryLight(void *userdata, const DioramaProjection *projection) {
  RoomEffectPass *pass = userdata;
  pass->projection.diorama_projection = projection;
  pass->projection.scenery_lighting_only = true;
  const unsigned layers[] = {kActionEffectRenderLayer_Bg1Light,
      kActionEffectRenderLayer_Bg1Plane, kActionEffectRenderLayer_Bg1HighPlane};
  for (unsigned i = 0; i < sizeof(layers)/sizeof(layers[0]); ++i)
    if (!DrawEffectLayer(pass, layers[i], kArRenderBlendMode_Add, true, false)) return false;
  pass->projection.scenery_lighting_only = false;
  pass->projection.scenery_lighting_in_material = true;
  return true;
}

static bool DrawActorLight(void *userdata,const DioramaProjection *projection,unsigned receiver) {
  RoomEffectPass *pass=userdata;
  pass->projection.diorama_projection=projection;
  pass->projection.scenery_lighting_only=true;
  pass->projection.light_receiver=receiver;
  bool ok=true;
  for(unsigned layer=0;layer<kActionEffectRenderLayer_Count && ok;++layer)
    ok=DrawEffectLayer(pass,layer,kArRenderBlendMode_Add,true,false);
  pass->projection.scenery_lighting_only=false;
  pass->projection.light_receiver=0;
  return ok;
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
int RoomPreview_CameraX(void) { return s_cached ? s_x : 0; }
unsigned RoomPreview_Uploads(void) { return s_uploads; }
void RoomPreview_Reset(void) {
  ArRenderDevice *device = DioramaPreview_Device();
  ArRenderDevice_DestroyTexture(device,s_effect_masks);s_effect_masks=ArRenderTexture_Invalid();
  DioramaAstralRift_Reset(device);
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
  EditorReceiverPreview_Reset();s_receiver_projection=(DioramaProjection){0};
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
/* Two 49-word records: 17 layer fields followed by four 8-word bands.
 * Enum fields retain the action_bg_plan.h values. No browser policy model. */
unsigned RoomPreview_Policy(int x, int y, uint32_t frame, unsigned defaults) {
  ActionBgPlan plan;
  int camera_x[2];
  if (defaults > 1 || !EditorRoomScene_BgPolicy(s_room, x, y, frame, defaults != 0,
                                               &plan, camera_x)) return 0;
  int32_t *out = (int32_t *)DioramaPreview_Input();
  for (unsigned bg = 0; bg < 2; bg++) {
    const ActionBgLayerPlan *p = &plan.layer[bg];
    const int32_t fields[17] = {
      p->valid, p->role, p->source, p->default_edge, p->default_motion,
      p->horizontal_extent.mode, p->horizontal_extent.left, p->horizontal_extent.right,
      p->vertical_extent.mode, p->vertical_extent.top, p->vertical_extent.bottom,
      p->band_count, camera_x[bg], p->camera_y, p->world_width, p->world_height,
      p->wrap_world_x,
    };
    memcpy(out, fields, sizeof(fields));
    out += 17;
    for (unsigned i = 0; i < kActionBgMaxBands; i++) {
      const ActionBgBand *b = &p->bands[i];
      const int32_t fields[8] = {b->y0, b->y1, b->edge, b->motion, b->anchor,
          b->horizontal_extent.mode, b->horizontal_extent.left, b->horizontal_extent.right};
      memcpy(out, fields, sizeof(fields));
      out += 8;
    }
  }
  return 98;
}
static unsigned s_recipe_error_line;
unsigned RoomPreview_RecipeErrorLine(void) { return s_recipe_error_line; }
const char *RoomPreview_RecipeError(void) { return EditorRoomScene_RecipeError(s_room); }
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
    case 0: return authored ? frame->authored_sources[index-frame->decoration_count] :
        actor||e->kind==kActionEffect_LandingDust ? 0 : e->generation;
    case 1: return e->kind;
    case 2: return (uint32_t)(int32_t)e->world_x;
    case 3: return (uint32_t)(int32_t)e->world_y;
    case 8:
      return !actor && !authored && ActionEffectMembers_MovableSource(e->kind);
    case 9:
      return (unsigned)fmaxf(4, e->geometry.data.rect.x1 - e->geometry.data.rect.x0);
    case 10:
      return (unsigned)fmaxf(4, e->geometry.data.rect.y1 - e->geometry.data.rect.y0);
    case 5: return authored;
    case 6: return authored ? frame->authored_floor[index-frame->decoration_count].count : 0;
    case 7: return actor||e->kind==kActionEffect_LandingDust;
    case 4: return e->flags & kActionEffectFlag_Visible;
    default: return 0;
  }
}
/* Catalogue coordinates are independent of the camera. Complete definitions
 * own their member bases; sparse transforms remain relative to those bases. */
static unsigned MemberCount(unsigned kind) {
  if(kind==kActionEffect_CastleLight)return (unsigned)EditorRoomScene_CastleField(s_room)->SourceCount[0];
  if(ActionEffectMembers_WaterLimit(kind))
    return ActionEffectMembers_WaterCount(kind,EditorRoomScene_WaterField(s_room));
  const ActionEnvironmentScene *scene=EditorRoomScene_Environment(s_room);
  return ActionEffectMembers_Count(kind,scene->group,scene->room);
}
static bool MemberSource(unsigned kind,unsigned id,ActionNativeMemberSource *out,bool *angled) {
  if(ActionEffectMembers_WaterLimit(kind)) {
    *angled=false;return ActionEffectMembers_WaterSource(kind,id,EditorRoomScene_WaterField(s_room),out);
  }
  if(kind==kActionEffect_CastleLight) {
    const ActionCastleField *f=EditorRoomScene_CastleField(s_room);
    if(!id||id>(unsigned)f->SourceCount[0])return false;
    const ActionCastleSource *s=&f->sources[id-1];
    *out=(ActionNativeMemberSource){s->x,s->y,s->width*2,s->length};
    *angled=s->kind!=kActionCastleSource_Torch;return true;
  }
  const ActionEnvironmentScene *scene=EditorRoomScene_Environment(s_room);
  *angled=ActionEffectMembers_Angled(kind,scene->group,scene->room,id);
  return ActionEffectMembers_Source(kind,scene->group,scene->room,id,out);
}
unsigned RoomPreview_MemberCount(unsigned index) {
  if (!s_room) return 0;
  const ActionSceneEffectFrame *f = EditorRoomScene_Effects(s_room);
  if (index >= f->decoration_count) return 0;
  return MemberCount(f->decorations[index].kind);
}
unsigned RoomPreview_CatalogueCount(void) { return EditorRoomScene_DefaultSourceCount(s_room); }
unsigned RoomPreview_CatalogueGuideCount(unsigned index) {
  return EditorRoomScene_DefaultGuideCount(s_room, index);
}
double RoomPreview_CatalogueGuideValue(unsigned index, unsigned guide, unsigned field) {
  ArRenderRectF r;
  if (!EditorRoomScene_DefaultGuide(s_room, index, guide, &r)) return 0;
  switch (field) {
  case 0: return r.x + r.w * .5f;
  case 1: return r.y + r.h * .5f;
  case 2: return r.w;
  case 3: return r.h;
  default: return 0;
  }
}
double RoomPreview_CatalogueValue(unsigned index, unsigned field) {
  const EditorRoomEffectSource *s = EditorRoomScene_DefaultSource(s_room, index);
  if (!s) return 0;
  const ActionEffectInstance *e = &s->effect;
  switch (field) {
  case 0:
    return e->generation;
  case 1:
    return e->kind;
  case 2:
    return e->world_x;
  case 3:
    return e->world_y;
  case 4:
    return fmaxf(4, e->geometry.data.rect.x1 - e->geometry.data.rect.x0);
  case 5:
    return fmaxf(4, e->geometry.data.rect.y1 - e->geometry.data.rect.y0);
  case 6:
    return ActionEffectMembers_MovableSource(e->kind);
  case 7:
    return MemberCount(e->kind);
  case 8:
    return s->map_scale_x;
  case 9:
    return s->map_scale_y;
  case 10:
    return e->projection_plane==kActionEffectProjectionPlane_Bg2 ?
        (e->flags&kActionEffectFlag_StaticAnchor ? 1 : 2) : 0;
  default:
    return 0;
  }
}
/* Static editor footprints use the active recipe, before motion/occlusion.
 * Keep these in layer coordinates so nonuniform parallax does not change the
 * meaning of an angle. No extra geometry is generated in the game renderer. */
static double MemberGuideValue(unsigned kind, unsigned member, unsigned field) {
  if (kind == kActionEffect_ForestCanopyLight || kind == kActionEffect_ForestForwardLight) {
    const ActionRayField *f = EditorRoomScene_RayField(s_room);
    if (!f || member >= f->ray_count) return 0;
    const ActionRayOpening *r = &f->rays[member];
    const float source_y = r->fan ? f->fans[r->fan].y : 0;
    switch (field) {
    case 6: return 1; /* A sheared, widening ray. */
    case 7: return r->fan ? (f->fans[r->fan].x-r->x)/-source_y : f->values[kActionRay_IsolatedSlope];
    case 8: return f->values[kActionRay_WidthBase];
    case 9: return r->fan ? f->values[kActionRay_WidthBase]*(544-source_y)/-source_y :
      f->values[kActionRay_WidthBase]+f->values[kActionRay_WidthGrowth]*fminf(544,f->values[kActionRay_WidthGrowthLimit]);
    }
  } else if (kind == kActionEffect_CastleLight) {
    const ActionCastleField *f = EditorRoomScene_CastleField(s_room);
    if (!f || member >= (unsigned)f->SourceCount[0]) return 0;
    const ActionCastleSource *s = &f->sources[member];
    if (s->kind == kActionCastleSource_Torch) return 0;
    switch (field) {
    case 6: return 2; /* Window fan, rooted at its sill. */
    case 7: return s->length ? (double)s->lean/s->length : 0;
    case 8: return 1;
    case 9: return s->width ? (double)(s->width+s->spread)/s->width : 1;
    case 10: return s->kind == kActionCastleSource_Window ? s->sill-s->y : 0;
    }
  }
  return 0;
}
double RoomPreview_CatalogueMemberValue(unsigned index, unsigned member, unsigned field) {
  const EditorRoomEffectSource *s = EditorRoomScene_DefaultSource(s_room, index);
  ActionNativeMemberSource source;bool angled;
  if (!s ||
      !MemberSource(s->effect.kind, member + 1, &source,&angled))
    return 0;
  switch (field) {
  case 0:
    return member + 1;
  case 1:
    return source.x;
  case 2:
    return source.y;
  case 3:
    return source.width;
  case 4:
    return source.height;
  case 5:
    return angled;
  default:
    return MemberGuideValue(s->effect.kind, member, field);
  }
}
double RoomPreview_MemberValue(unsigned index, unsigned member, unsigned field) {
  if (member >= RoomPreview_MemberCount(index)) return 0;
  const ActionEffectInstance *e = &EditorRoomScene_Effects(s_room)->decorations[index];
  ActionNativeMemberSource source;bool angled;
  if (!MemberSource(e->kind, member + 1, &source,&angled)) return 0;
  switch (field) {
  case 0:
    return member + 1;
  case 1:
    return source.x;
  case 2:
    return source.y;
  case 3:
    return source.width;
  case 4:
    return source.height;
  case 5:
    return angled;
  default:
    return MemberGuideValue(e->kind, member, field);
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
const char *RoomPreview_RayFieldText(void) {
  static char text[16384];
  const ActionRayField *field=EditorRoomScene_RayField(s_room);
  return field&&ActionRayField_Write(field,text,sizeof(text))?text:NULL;
}
const char *RoomPreview_SurfaceFieldText(unsigned kind){
  static char text[16384];const ActionSurfaceField *field=EditorRoomScene_SurfaceField(s_room,kind);
  return field&&ActionSurfaceField_Write(field,text,sizeof(text))?text:NULL;
}
const char *RoomPreview_ProjectileFieldText(unsigned kind){
  static char text[16384];const ActionProjectileField *field=EditorRoomScene_ProjectileField(s_room,kind);
  return field&&ActionProjectileField_Write(field,text,sizeof(text))?text:NULL;
}
const char *RoomPreview_ArcFieldText(unsigned kind){
  static char text[16384];const ActionArcField *field=EditorRoomScene_ArcField(s_room,kind);
  return field&&ActionArcField_Write(field,text,sizeof(text))?text:NULL;
}
const char *RoomPreview_GlowFieldText(void) {
  static char text[16384];const ActionGlowField *field=EditorRoomScene_GlowField(s_room);
  return field&&ActionGlowField_Write(field,text,sizeof(text))?text:NULL;
}
const char *RoomPreview_CastleFieldText(void) {
  static char text[16384];const ActionCastleField *field=EditorRoomScene_CastleField(s_room);
  return field&&ActionCastleField_Write(field,text,sizeof(text))?text:NULL;
}
const char *RoomPreview_MarshFieldText(void) {
  static char text[16384];const ActionMarshField *field=EditorRoomScene_MarshField(s_room);
  return field&&ActionMarshField_Write(field,text,sizeof(text))?text:NULL;
}
const char *RoomPreview_MoonFieldText(void) {
  static char text[16384];const ActionMoonField *field=EditorRoomScene_MoonField(s_room);
  return field&&ActionMoonField_Write(field,text,sizeof(text))?text:NULL;
}
const char *RoomPreview_WaterFieldText(void) {
  static char text[16384];const ActionWaterField *field=EditorRoomScene_WaterField(s_room);
  return field&&ActionWaterField_Write(field,text,sizeof(text))?text:NULL;
}
const char *RoomPreview_AtmosphereFieldText(void) {
  static char text[16384];const ActionAtmosphereField *field=EditorRoomScene_AtmosphereField(s_room);
  return field&&ActionAtmosphereField_Write(field,text,sizeof(text))?text:NULL;
}
double RoomPreview_WaterFieldMapScale(unsigned axis) {
  return EditorRoomScene_WaterFieldMapScale(s_room,axis);
}
double RoomPreview_RayFieldMapScale(unsigned axis) {
  return EditorRoomScene_RayFieldMapScale(s_room, axis);
}
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
      pixel_aspect < 0 || pixel_aspect > 1 || extra < 0 || extra > 128 ||
      vertical < 0 || vertical > 64 || width < 1 || height < 1 ||
      width > 2048 || height > 2048) return 0;
  s_uploads = 0; s_effect_vertices = 0;
  s_receiver_projection.valid=false;
  DioramaView view = PreviewView(width, height, pixel_aspect, distance, yaw, pitch);
  const DioramaView baseline = PreviewView(width, height, pixel_aspect, 1, 0, 0);
  EditorRoomCamera camera;
  if (!EditorRoomScene_ResolveCamera(s_room, x, y, frame, extra, vertical, &baseline, skybox, &camera)) return 0;
  x = camera.x; y = camera.y;
  if (!s_cached || x != s_x || y != s_y || frame != s_frame || extra != s_extra || vertical != s_vertical ||
      camera.bg_apron_mask != s_camera.bg_apron_mask ||
      camera.travel.minimum != s_camera.travel.minimum || camera.travel.maximum != s_camera.travel.maximum) {
    s_cached = false;
    EditorRoomScene_AdvanceReceivers(s_room,s_receivers.mask,s_receivers.x,s_receivers.feet_y,frame);
    if (!EditorRoomScene_Render(s_room, x, y, frame, extra, vertical, &camera) || !Upload()) return 0;
    s_x = x; s_y = y; s_frame = frame; s_extra = extra; s_vertical = vertical; s_cached = true;
    s_camera = camera;
  }
  if (!DioramaPreview_Begin(width, height)) return 0;
  DioramaCapture capture = *EditorRoomScene_Capture(s_room);
  capture.textures = s_textures; capture.pixels = s_pixels;
  capture.plane_capture_offsets=s_projection_offsets;
  /* Orbit/aspect changes reuse the filtered skybox until its pixels change. */
  capture.bg2_revision = s_bg2_revision;
  capture.bg2_dynamic = false;
  DioramaSkyboxView extended = *capture.skybox;
  extended.capture_offset.x+=s_skybox_offset.x;
  extended.capture_offset.y+=s_skybox_offset.y;
  if (EditorRoomScene_Surfaces(s_room)->background_view) {
    extended.texture = s_view; extended.revision = s_view_revision; capture.skybox = &extended;
  }
  const DioramaSkyboxView page = {.texture = s_page, .width = 256, .periodic = true};
  if (skybox == kDioramaSky_Only && EditorRoomScene_Surfaces(s_room)->native_pages[1])
    capture.skybox = &page;
  DioramaScene scene = *EditorRoomScene_Scene(s_room);
  DioramaRenderOptions options = *scene.render;
  options.rim_light=s_receiver_rim;
  options.resolve_skybox = ResolveSkybox;
  options.draw_resident_skybox = s_skybox_draw;
  options.source_effect_batch = s_source_draw ? s_source_batch : NULL;
  options.skybox = (DioramaSkyMode)skybox; scene.render = &options;
  const ActionEnvironmentScene *environment = EditorRoomScene_Environment(s_room);
  RoomEffectPass effects = {.frame = EditorRoomScene_Effects(s_room), .valid = true,
    .projection = {.bg1_camera_x = environment->camera_x[0], .bg1_camera_y = environment->camera_y[0],
      .bg2_camera_x = environment->camera_x[1], .bg2_camera_y = environment->camera_y[1],
      .ws_extra = extra, .ws_extra_top = capture.authentic_y0, .scenery_exposure_in_mesh=true,
      .visible_width = capture.width, .snes_height = capture.height, .viewport = view.viewport}};
  if(s_receivers.mask && s_effects_enabled && !ActionEffectReceivers_Prepare(effects.frame,environment->group,environment->room,
      environment->camera_x[0],environment->camera_y[0],environment->camera_x[1],environment->camera_y[1],false,&s_receiver))return 0;
  const int receiver_plane=DioramaPlaneForObjectPriority(2);
  if(!EditorReceiverPreview_Capture(DioramaPreview_Device(),&s_receivers,
      s_effects_enabled?&s_receiver:NULL,environment->camera_x[0],environment->camera_y[0],extra,
      &capture,&s_textures[receiver_plane],&s_pixels[receiver_plane],
      &s_textures[kDioramaPlane_ObjAttributes2],&s_pixels[kDioramaPlane_ObjAttributes2],&s_uploads))return 0;
  ActionExposureLighting exposure={.origin_x=environment->camera_x[0]-extra-capture.obj_apron,
      .origin_y=environment->camera_y[0]-capture.authentic_y0,
      .width=SR_PPU_SURFACE_MAX_WIDTH,.height=SR_PPU_SURFACE_MAX_HEIGHT};
  ActionExposure_Prepare(&exposure,s_effects_enabled?effects.frame:NULL);
  scene.scenery_exposure=exposure.count?&exposure.field:NULL;
  scene.scenery_grade=s_effects_enabled ? DioramaLayerOrder_SceneryGrade(options.layers,
      scene.map_group,scene.map_number,scene.layer_section) : (DioramaSceneryGrade){0};
  scene.background_heat=ActionHeat_Prepare(s_effects_enabled?effects.frame:NULL,
      environment->camera_x[1]-extra-capture.obj_apron,environment->camera_y[1]-capture.authentic_y0,
      SR_PPU_SURFACE_MAX_WIDTH,SR_PPU_SURFACE_MAX_HEIGHT);
  scene.effect_obj_priority_mask=EditorReceiverPreview_Ready()?1u<<2:0;
  if (s_effects_enabled) {
    scene.actor_light=DrawActorLight;
    scene.actor_light_reuse[0]=ActionEffectReceivers_MapsMatch(effects.frame,1,2)?1:0;
    scene.actor_light_reuse[1]=ActionEffectReceivers_MapsMatch(effects.frame,1,4)?1:
        ActionEffectReceivers_MapsMatch(effects.frame,2,4)?2:0;
    scene.scenery_light = ActionEffectReceivers_HasSurfaceLights(effects.frame) ? DrawSceneryLight : NULL;
    scene.bg1_dimming = ActionEnvironment_Bg1Dimming(effects.frame, environment->group, environment->room);
    scene.bg1_dimming_ramp = SceneryDimming_ToUv(ActionEnvironment_Bg1DimmingRamp(effects.frame,environment->group, environment->room),
        exposure.origin_x,exposure.origin_y,exposure.width,exposure.height);
    scene.effect_obj_priority_mask |= ActionEffectProjection_RequiredObjPriorityMask(NULL, effects.frame);
    scene.effect_bg_plane_mask = ActionEffectProjection_RequiredBgPlaneMask(NULL, effects.frame);
    scene.plane_effect = DrawPlaneEffects; scene.plane_effect_userdata = &effects;
  }
  DioramaProjection projection;
  DioramaAstralRiftFrame rift = {.device=DioramaPreview_Device(),
      .yaw=view.camera.tilt_y, .pitch=view.camera.tilt_x,
      .tick=(uint16_t)frame, .inidisp=15, .projection=&projection,
      .floor_capture_x=128-environment->camera_x[0]+extra,
      .floor_capture_y=kDioramaAstralFloorWorldY-environment->camera_y[0]+capture.authentic_y0};
  DioramaAstralRift_Attach(s_final_battle,&scene,&options,&rift);
  if (!PresentationOutcome_IsUsable(Diorama_Composite(
      DioramaPreview_Device(), &capture, &view, &scene, &projection)) || !effects.valid) return 0;
  s_receiver_projection=projection;
  if (!s_effects_enabled) return 1;
  effects.projection.diorama_projection = &projection;
  if(s_source_draw) {
    ActionEffectSourceBatch source = {.context=effects.projection,
        .primitives=s_source_primitives,.capacity=kActionSourceMaximumPrimitives,
        .lights=s_source_lights,.light_capacity=kActionSourceMaxLightJobs};
    if(!ActionSceneEffectRender_Build(effects.frame,true,true,ActionEffectSource_ProjectPoint,&source,&s_effect_batch) ||
       !s_source_draw(DioramaPreview_Device(),&source,&projection,&effects.frame->scenery,kArRenderBlendMode_Add,1))return 0;
  } else {
  if(!ActionSceneEffectRender_Build(effects.frame,true,true,ActionEffectProjection_ProjectPoint,&effects.projection,&s_effect_batch))return 0;
  if(s_effect_batch.index_count) {
    s_effect_vertices+=s_effect_batch.vertex_count;
    const ArRenderDrawState state={.flags=kArRenderDrawState_Blend,.blend=kArRenderBlendMode_Add};
    if(!ArRenderDevice_DrawGeometryWithState(DioramaPreview_Device(),ArRenderTexture_Invalid(),s_effect_batch.vertices,s_effect_batch.vertex_count,s_effect_batch.indices,s_effect_batch.index_count,&state))return 0;
  }
  }
  if (options.source_effect_batch && !options.source_effect_batch(DioramaPreview_Device(), true)) return 0;
  bool drawn = DrawEffectLayer(&effects, kActionEffectRenderLayer_WorldOverlay, kArRenderBlendMode_Add, true, true) &&
      DrawEffectLayer(&effects, kActionEffectRenderLayer_WorldDust, kArRenderBlendMode_Alpha, false, true) &&
      DrawEffectLayer(&effects, kActionEffectRenderLayer_ForegroundLight, kArRenderBlendMode_Light, true, false);
  if (options.source_effect_batch) drawn = options.source_effect_batch(DioramaPreview_Device(), false) && drawn;
  return drawn;
}
