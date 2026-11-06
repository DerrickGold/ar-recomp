/* Real editor materialization/projection, with a CPU texture recorder. No ROM,
 * window, or GPU is involved; uploaded words are checked against native winners. */
#include "object_preview.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { kWidth=640, kHeight=352, kArea=kWidth*kHeight, kSourceWidth=624 };
typedef struct TextureRecord {
  ArRenderTextureDesc desc;
  uint32_t *words;
} TextureRecord;
typedef struct Recorder {
  TextureRecord textures[32];
  unsigned next, live, uploads;
  bool fail_create, fail_upload;
} Recorder;

static bool Create(void *context,const ArRenderTextureDesc *desc,ArRenderTexture *out) {
  Recorder *r=context;
  if(r->fail_create)return false;
  assert(r->next+1<32);
  const unsigned id=++r->next;
  r->textures[id].desc=*desc;
  r->textures[id].words=calloc((size_t)desc->width*desc->height,sizeof(uint32_t));
  assert(r->textures[id].words);
  out->value=id;++r->live;return true;
}
static void Destroy(void *context,ArRenderTexture texture) {
  Recorder *r=context;
  assert(texture.value && texture.value<=r->next);
  assert(r->textures[texture.value].words);
  free(r->textures[texture.value].words);
  r->textures[texture.value].words=NULL;--r->live;
}
static bool Upload(void *context,ArRenderTexture texture,const ArRenderRectI *rect,
    const void *pixels,int pitch) {
  Recorder *r=context;++r->uploads;
  if(r->fail_upload)return false;
  assert(!rect && texture.value && texture.value<=r->next);
  TextureRecord *t=&r->textures[texture.value];
  assert(t->words && pitch>=t->desc.width*4);
  for(int y=0;y<t->desc.height;++y)
    memcpy(t->words+(size_t)y*t->desc.width,(const uint8_t *)pixels+(size_t)y*pitch,t->desc.width*4);
  return true;
}
/* Only resource operations are reached by this CPU materialization contract. */
static const ArRenderBackendOps kOps={.struct_size=sizeof(ArRenderBackendOps),
  .create_texture=Create,.destroy_texture=Destroy,.update_texture=Upload};

static uint32_t s_colors[4][kSourceWidth*kHeight],s_attrs[4][kSourceWidth*kHeight];
static uint32_t s_atlas[kVoxelSpriteAtlasWidth*kVoxelSpriteAtlasHeight];
static uint8_t s_packet[kDioramaObjectSnapshotCapacity];
static const DioramaRenderOptions kRender={.visible_planes=(1u<<kDioramaPlane_Count)-1,
  .skybox=kDioramaSky_Off,.margin_fix=true};

static DioramaScene Scene(void) {
  return (DioramaScene){.render=&kRender,.map_group=2,.map_number=1};
}
static uint32_t Color(unsigned band,unsigned offset) {
  return UINT32_C(0xff102030)+band*UINT32_C(0x00010101)+offset;
}
static uint32_t Attribute(unsigned owner,unsigned band,unsigned offset) {
  return owner<<24 | UINT32_C(0x00010203)+band*256+offset;
}
static size_t MakePacket(void) {
  memset(s_colors,0,sizeof(s_colors));memset(s_attrs,0,sizeof(s_attrs));
  memset(s_atlas,0,sizeof(s_atlas));
  const unsigned spot=100*kSourceWidth+200;
  DioramaObjectSnapshotView source={.map_group=2,.map_number=1,.game_frame=1234,
    .world_x=816,.world_y=236,.camera_x=1000,.camera_y=300,.extra_x=120,.extra_top=64,
    .obj_apron=64,.width=kSourceWidth,.height=kHeight,.owner_tags=true,
    .additive_priority_mask=5,.atlas=s_atlas,.atlas_pitch=kVoxelSpriteAtlasWidth,.atlas_height=34};
  for(unsigned band=0;band<4;++band) {
    source.obj[band]=s_colors[band];source.obj_pitch[band]=kSourceWidth;
    source.attributes[band]=s_attrs[band];source.attribute_pitch[band]=kSourceWidth;
    for(unsigned x=0;x<5;++x) {
      s_colors[band][spot+x]=Color(band,x);
      s_attrs[band][spot+x]=Attribute(x==2?9:7+x,band,x);
    }
  }
  source.voxel_sprites.count=2;
  for(unsigned i=0;i<2;++i) {
    source.voxel_sprites.sprites[i]=(ActionVoxelSprite){.owner=(uint16_t)(0x09a0+i*64),
      .shape=3,.priority=2,.width=3,.height=1,.owner_tag=(uint8_t)(i?9:7),
      .solid_height=1,.x=200,.y=100,.ambient=1,.rows={7}};
    for(unsigned x=0;x<3;++x)
      s_atlas[kVoxelSpriteAtlasWidth+1+i*kVoxelSpriteCellWidth+x]=Color(2,x);
  }
  size_t size=0;
  assert(DioramaObjectSnapshot_Encode(&source,s_packet,sizeof(s_packet),&size));
  return size;
}
static void CheckBand(const uint8_t *pixels[kDioramaPlane_Count],ArRenderTexture *textures,
    const Recorder *r,unsigned band,unsigned spot,bool voxel) {
  const unsigned plane=DioramaPlaneForObjectPriority(band),attribute=kDioramaPlane_ObjAttributes0+band;
  const uint32_t *color=(const uint32_t *)pixels[plane],*attr=(const uint32_t *)pixels[attribute];
  assert(color && attr);
  for(unsigned x=0;x<5;++x) {
    const bool removed=voxel && band==2 && (x==0 || x==2);
    const uint32_t expected=removed?0:Color(band,x);
    const uint32_t expected_attr=removed?0:(Attribute(x==2?9:7+x,band,x)|UINT32_C(0xff000000));
    assert(color[spot+x]==expected && attr[spot+x]==expected_attr);
    assert(r->textures[textures[plane].value].words[spot+x]==expected);
    assert(r->textures[textures[attribute].value].words[spot+x]==expected_attr);
  }
  assert(!color[spot-1] && !color[spot+5] && !attr[spot-1] && !attr[spot+5]);
}
static void CheckSources(const EditorObjectPreview *object) {
  for(unsigned band=0;band<4;++band) {
    const unsigned spot=100*kSourceWidth+200;
    for(unsigned x=0;x<5;++x) {
      assert(object->snapshot.view.obj[band][spot+x]==Color(band,x));
      assert(object->snapshot.view.attributes[band][spot+x]==Attribute(x==2?9:7+x,band,x));
    }
  }
  assert(object->snapshot.view.voxel_sprites.sprites[0].x==200);
  assert(object->snapshot.view.voxel_sprites.sprites[0].y==100);
}
int main(void) {
  const size_t size=MakePacket();
  Recorder recorder={0};ArRenderDevice device={.ops=&kOps,.context=&recorder};
  EditorObjectPreview object={0};
  assert(EditorObjectPreview_Load(&object,s_packet,size,2,1,0));
  uint32_t *retained=object.snapshot.storage,*working=object.pixels;
  assert(!EditorObjectPreview_Load(&object,s_packet,size,3,1,0));
  assert(!EditorObjectPreview_Load(&object,s_packet,size,2,2,0));
  assert(!EditorObjectPreview_Load(&object,s_packet,size,2,1,1));
  assert(!EditorObjectPreview_Load(&object,s_packet,size-1,2,1,0));
  s_packet[size-1]^=1;
  assert(!EditorObjectPreview_Load(&object,s_packet,size,2,1,0));
  s_packet[size-1]^=1;
  assert(object.snapshot.storage==retained && object.pixels==working);
  assert(object.snapshot.view.game_frame==1234);CheckSources(&object);

  DioramaCapture capture={.width=496,.height=kHeight,.authentic_y0=64,.obj_apron=64,
    .bg1_camera_x=1010,.camera_y=308};
  DioramaView view={.distance_scale=1,.camera_framing_weight=1,.visible_width=496,
    .viewport={0,0,1280,800}};
  ArRenderTexture textures[kDioramaPlane_Count]={0};
  const uint8_t *pixels[kDioramaPlane_Count]={0};unsigned uploads=0;
  /* Actual shared projection must resolve OBJ footprints for this fixture. */
  DioramaScene scene=Scene();scene.effect_obj_priority_mask=15;
  for(unsigned band=0;band<4;++band) {
    ArRenderRectF footprint;
    assert(Diorama_ResolveCaptureFootprint(&capture,&view,&scene,DioramaPlaneForObjectPriority(band),&footprint));
    assert(footprint.w>0 && footprint.h>0);
  }
  const unsigned target=92*kWidth+190;
  for(unsigned pass=0;pass<3;++pass) {
    const bool voxel=pass==1;
    /* The final ordinary draw deliberately reuses the prior Voxel scene. */
    if(pass!=2)scene=Scene();
    assert(EditorObjectPreview_Capture(&object,&device,&capture,&view,&scene,voxel,textures,pixels,&uploads));
    for(unsigned band=0;band<4;++band) {
      CheckBand(pixels,textures,&recorder,band,target,voxel);
      assert(!!(scene.additive_plane_mask&(1u<<DioramaPlaneForObjectPriority(band)))==!(band&1));
    }
    if(voxel) {
      assert(scene.voxel_sprites==&object.props && object.props.count==2);
      assert(object.props.sprites[0].x==190 && object.props.sprites[0].y==92);
      assert(ArRenderTexture_IsValid(scene.voxel_sprite_atlas));
    } else assert(!scene.voxel_sprites && !object.props.count &&
        !ArRenderTexture_IsValid(scene.voxel_sprite_atlas));
    CheckSources(&object);
  }
  /* Rebase both axes in the opposite direction; old working pixels disappear. */
  capture.bg1_camera_x=990;capture.camera_y=292;scene=Scene();
  assert(EditorObjectPreview_Capture(&object,&device,&capture,&view,&scene,true,textures,pixels,&uploads));
  for(unsigned band=0;band<4;++band)CheckBand(pixels,textures,&recorder,band,108*kWidth+210,true);
  assert(object.props.sprites[1].x==210 && object.props.sprites[1].y==108);
  assert(!((const uint32_t *)pixels[DioramaPlaneForObjectPriority(2)])[target+1]);
  CheckSources(&object);

  /* A failed atlas handoff cannot erase native prop artwork or publish models. */
  object.atlas_dirty=true;recorder.fail_upload=true;scene=Scene();
  assert(!EditorObjectPreview_Capture(&object,&device,&capture,&view,&scene,true,textures,pixels,&uploads));
  assert(!scene.voxel_sprites && object.atlas_dirty);
  assert(object.pixels[4*kArea+108*kWidth+210]==Color(2,0));
  CheckSources(&object);recorder.fail_upload=false;
  ArRenderDevice_DestroyTexture(&device,object.atlas);object.atlas=ArRenderTexture_Invalid();
  recorder.fail_create=true;scene=Scene();
  assert(!EditorObjectPreview_Capture(&object,&device,&capture,&view,&scene,true,textures,pixels,&uploads));
  assert(!scene.voxel_sprites && object.pixels[4*kArea+108*kWidth+210]==Color(2,0));
  recorder.fail_create=false;

  /* Camera travel beyond finite native coverage fails before texture uploads. */
  const unsigned before=recorder.uploads;
  capture.bg1_camera_x=1640;scene=Scene();
  assert(!EditorObjectPreview_Capture(&object,&device,&capture,&view,&scene,true,textures,pixels,&uploads));
  capture.bg1_camera_x=1000;capture.camera_y=652;scene=Scene();
  assert(!EditorObjectPreview_Capture(&object,&device,&capture,&view,&scene,false,textures,pixels,&uploads));
  capture.camera_y=300;scene=Scene();scene.layer_section=1;
  assert(!EditorObjectPreview_Capture(&object,&device,&capture,&view,&scene,true,textures,pixels,&uploads));
  assert(recorder.uploads==before);
  scene=Scene();view.camera.tilt_x=.12f;view.camera.tilt_y=-.18f;
  assert(EditorObjectPreview_Capture(&object,&device,&capture,&view,&scene,true,textures,pixels,&uploads));
  CheckSources(&object);

  EditorObjectPreview_Reset(&object,&device);
  assert(!object.snapshot.storage && !object.pixels && !ArRenderTexture_IsValid(object.atlas));
  EditorObjectPreview_Reset(&object,&device);
  for(unsigned i=0;i<kDioramaPlane_Count;++i)ArRenderDevice_DestroyTexture(&device,textures[i]);
  assert(!recorder.live);
  puts("editor objects: four native priorities, owner overlap, mode preservation, rebase, upload safety, and finite camera coverage passed");
  return 0;
}
