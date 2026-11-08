#include "present/present.h"
#include "actraiser/actraiser_sprite_ownership.h"
#include "actraiser_game.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <string.h>
#include "present/frame_slot_voxel_sprites.inc"

uint8_t g_ram[kActRaiserWramSize];
static unsigned resolves,rasters,fail_raster_at;
static bool fail_resolve,fail_raster,oversize,outline=true;
static bool active_geometry=true;
bool ActionVoxel_Active(const ActionVoxelGeometry *g,unsigned group_id,unsigned room_id) {
  (void)g;(void)group_id;(void)room_id;return active_geometry;
}
static unsigned group=2,room=1,source=0xb4c0,animation=0x4000,composition=0x45bc;
static int width=16,height=16,clip_x,clip_y,body_y,body_height=16;
static int origin_x=20,origin_y=80;
bool ActionEffectReceivers_Prepare(const ActionSceneEffectFrame *f,unsigned g,unsigned r,
    int x,int y,int bx,int by,bool enabled,ActionReceiverLighting *out) {
  (void)f;(void)g;(void)r;(void)x;(void)y;(void)bx;(void)by;(void)enabled;(void)out;
  return true;
}
float ActionEffectReceivers_Ambient(const ActionReceiverLighting *l,unsigned receiver,
    float x,float y) {
  (void)l;(void)x;(void)y;
  assert(receiver==kActionReceiver_Enemies);
  return .5f;
}
static SrResult Resolve(SrRunnerHandle *runner,const SrPpuObjResolveRequest *r,
    SrPpuObjResolveResult *b) {
  (void)runner;++resolves;
  assert(r->lifetime_generation==42 && r->first_sprite<128 && r->part_capacity>=1);
  if(fail_resolve)return SR_RESULT_STALE_VIEW;
  if(r->priority!=2)return SR_RESULT_OK;
  for(int y=clip_y;y<height;y+=16)for(int x=clip_x;x<width;x+=16)
    r->parts[b->part_count++]=(SrPpuObjPart){.x=origin_x+x,.y=origin_y+y,.size=16};
  b->x0=origin_x+clip_x;b->x1=origin_x+width+(oversize?64:0);
  b->y0=origin_y+clip_y;b->y1=origin_y+height;
  return SR_RESULT_OK;
}
static SrResult Raster(SrRunnerHandle *runner,const SrPpuObjPartsRasterRequest *r,
    SrPpuObjRasterResult *b) {
  (void)runner;(void)b;++rasters;
  assert(r->lifetime_generation==42 && r->part_count>0 && r->x1-r->x0<=kVoxelSpriteWidth+2);
  if(fail_raster || (fail_raster_at && rasters==fail_raster_at))return SR_RESULT_UNAVAILABLE;
  for(int y=r->y0;y<r->y1;++y) {
    uint32_t *row=(uint32_t *)((uint8_t *)r->pixels+(y-r->y0)*r->pitch_bytes);
    for(int x=r->x0;x<r->x1;++x) {
      const int px=x-origin_x,py=y-origin_y;
      bool filled=px>=clip_x && px<width && py>=clip_y && py<height;
      if(outline)filled=filled && px>=2 && px<width-2 && py>=1 && py<height-1;
      else filled=filled && ((py>=body_y && py<body_y+body_height) || (px%8>=3 && px%8<5));
      row[x-r->x0]=filled?0xff806040:0;
      /* Opaque black is part of the native silhouette, never a colour key. */
      if(filled && px==2 && py==1)row[x-r->x0]=0xff000000;
    }
  }
  return SR_RESULT_OK;
}
static void Owners(unsigned count) {
  uint8_t shadow[544]={0};
  ActRaiserSpriteOwnership_Reset();ActRaiserSpriteOwnership_Begin(group,room,0);
  for(unsigned i=0;i<count;++i) {
    uint16_t record=0x8e0+i*64;
    ActRaiserSpriteOwnership_RecordActionObject(source,record,1768,232,i*4,i*4+4);
    ActRaiserSpriteOwnership_RecordActionVisual(source,record,animation,0x7e,composition,
        origin_x,origin_y,i*4,i*4+4);
  }
  ActRaiserSpriteOwnership_Complete(shadow);ActRaiserSpriteOwnership_Upload(group,room,shadow);
}
static void SelectTemple(unsigned map,unsigned src,unsigned comp,int w,int h,int sy,int sh) {
  group=5;room=map;source=src;composition=comp;animation=map==8?0x5000:0x4000;
  width=w;height=h;body_y=sy;body_height=sh;clip_x=clip_y=0;outline=false;
}
static void TestNativeHandoff(void) {
  enum { W=8,H=6,P=11,A=13 };
  uint32_t original[H*P],out[H*P],attributes[H*A];
  for(unsigned i=0;i<H*P;++i)original[i]=0xff000000u;
  for(unsigned i=0;i<H*A;++i)attributes[i]=0x074000ffu;
  ActionVoxelSprite p={.owner=0x980,.owner_tag=7,.x=1,.y=2,.width=4,.height=3,
      .rows={15,13,15}};
  /* Identical black artwork can belong to the player or a different log.
   * Only exact OAM winners inside the modeled silhouette may be removed. */
  attributes[2*A+2]=0xff40ff00u;
  attributes[2*A+3]=0x084000ffu;
  memcpy(out,original,sizeof(out));
  ActionVoxelSprite_RemoveNative(&p,W,H,out,P,attributes,A);
  unsigned removed=0;
  for(unsigned y=0;y<H;++y)for(unsigned x=0;x<P;++x) {
    const bool gone=(y==2 && (x==1 || x==4)) ||
        (y==3 && (x==1 || x==3 || x==4)) || (y==4 && x>=1 && x<=4);
    assert(out[y*P+x]==(gone?0:0xff000000u));removed+=gone;
  }
  assert(removed==9 && attributes[2*A+2]==0xff40ff00u);
  ActionVoxelSprite other=p;other.owner_tag=128;
  assert(ActionVoxelSprite_SameComponent(&p,&other)); /* OAM allocation can move. */
  p.owner_tag=0;memcpy(out,original,sizeof(out));
  ActionVoxelSprite_RemoveNative(&p,W,H,out,P,attributes,A);
  assert(!memcmp(out,original,sizeof(out))); /* No verified replacement. */
  p.owner_tag=7;p.x=-2;p.y=-1;
  memcpy(out,original,sizeof(out));
  ActionVoxelSprite_RemoveNative(&p,W,H,out,P,attributes,A);
  assert(out[0]==0 && out[1]==0 && out[2]==0xff000000u);
  assert(out[P]==0 && out[P+1]==0 && out[2*P]==0xff000000u);
}
int main(void) {
  TestNativeHandoff();
  const SnesRunnerApi api={.struct_size=sizeof(api),.resolve_ppu_obj_range=Resolve,
      .rasterize_ppu_obj_parts=Raster};
  FrameSlot f={.diorama_active=true,.diorama_map_group=2,.diorama_map_number=1,
      .diorama_obj_owner_tags=true,
      .diorama_voxel=(const struct ActionVoxelGeometry *)1,
      .diorama_plane_request_mask=1u<<SR_PPU_OVERLAY_OBJ,
      .snes_width=384,.snes_height=224,.ws_extra=64,.obj_apron=64,.ws_extra_top=32};
  /* Floating marsh logs never enter extraction. Without a modeled owner,
   * their native OBJ winners remain intact for the usual compositor. */
  source=0xb499;composition=0x45a2;Owners(1);CaptureVoxelSprites(&f,&api,NULL,42);
  assert(!f.voxel_sprites.count && !f.voxel_sprite_atlas.data && !resolves && !rasters);
  /* A bridge log is one exact 16x16 rectangle with a full solid band. */
  source=0xb4c0;composition=0x45bc;Owners(1);CaptureVoxelSprites(&f,&api,NULL,42);
  assert(f.voxel_sprites.count==1 && resolves==4 && rasters==1);
  assert(f.voxel_sprites.sprites[0].shape==kActRaiserSpriteProp_Log);
  assert(f.voxel_sprites.sprites[0].owner==0x8e0 && f.voxel_sprites.sprites[0].owner_tag==1);
  assert(f.voxel_sprites.sprites[0].width==16 && f.voxel_sprites.sprites[0].height==16);
  assert(f.voxel_sprites.sprites[0].solid_height==16 && !f.voxel_sprites.sprites[0].solid_y);
  resolves=rasters=0;
  group=4;room=5;source=0xd601;composition=0x4750;
  f.diorama_map_group=4;f.diorama_map_number=5;
  Owners(1);CaptureVoxelSprites(&f,&api,NULL,42);
  assert(f.voxel_sprites.count==1 && resolves==4 && rasters==1);
  const ActionVoxelSprite *p=&f.voxel_sprites.sprites[0];
  assert(p->owner==0x8e0 && p->owner_tag==1 && p->x==148 && p->y==112 && p->width==16 && p->height==16);
  assert(p->solid_height==16 && !p->solid_y && !p->component_x && !p->component_y);
  assert(!p->rows[0] && p->rows[1]==0x3ffc && !p->rows[15]);
  const uint32_t *pixels=(const uint32_t *)f.voxel_sprite_atlas.data;
  assert(!pixels[0] && pixels[2*kVoxelSpriteAtlasWidth+3]==0xff000000);
  f.action_environmental_effects=true;CaptureVoxelSprites(&f,&api,NULL,42);
  pixels=(const uint32_t *)f.voxel_sprite_atlas.data;
  assert(pixels[3*kVoxelSpriteAtlasWidth+4]==0xff806040 && p->ambient==.5f);
  fail_resolve=true;CaptureVoxelSprites(&f,&api,NULL,42);
  assert(!f.voxel_sprites.count && !f.voxel_sprite_atlas.data);fail_resolve=false;
  fail_raster=true;CaptureVoxelSprites(&f,&api,NULL,42);
  assert(!f.voxel_sprites.count && !f.voxel_sprite_atlas.data);fail_raster=false;
  oversize=true;CaptureVoxelSprites(&f,&api,NULL,42);
  assert(!f.voxel_sprites.count);oversize=false;
  Owners(kVoxelSpriteLimit+1);CaptureVoxelSprites(&f,&api,NULL,42);
  assert(!f.voxel_sprites.count && !f.voxel_sprite_atlas.data);
  Owners(1);f.diorama_voxel=NULL;const unsigned calls=resolves;
  CaptureVoxelSprites(&f,&api,NULL,42);
  assert(!f.voxel_sprites.count && resolves==calls);
  f.diorama_voxel=(const struct ActionVoxelGeometry *)1;f.diorama_obj_owner_tags=false;
  CaptureVoxelSprites(&f,&api,NULL,42);
  assert(!f.voxel_sprites.count && resolves==calls);
  f.diorama_obj_owner_tags=true;
  active_geometry=false;CaptureVoxelSprites(&f,&api,NULL,42);
  assert(!f.voxel_sprites.count && resolves==calls);active_geometry=true;
  group=4;room=5;source=0xd601;composition=0x4750;width=32;
  Owners(1);f.diorama_map_group=4;f.diorama_map_number=5;
  f.diorama_voxel=(const struct ActionVoxelGeometry *)1;CaptureVoxelSprites(&f,&api,NULL,42);
  assert(f.voxel_sprites.count==1 && p->shape==kActRaiserSpriteProp_RockPlatform);
  assert(p->width==32 && p->height==16 && p->solid_height==16);
  assert(p->rows[1]==UINT64_C(0x3ffffffc));

  SelectTemple(4,0xe42a,0x49f4,192,32,0,32);Owners(1);
  f.diorama_map_group=5;f.diorama_map_number=4;CaptureVoxelSprites(&f,&api,NULL,42);
  assert(f.voxel_sprites.count==3);
  const ActionVoxelSpriteFrame full=f.voxel_sprites;
  for(unsigned i=0;i<3;++i) {
    const ActionVoxelSprite *a=&full.sprites[i];
    assert(a->component_x==(int)i*64 && a->component_y==0 &&
        a->width==64 && a->height==32 && a->solid_height==32);
    assert(a->rows[2]==UINT64_MAX);
    assert(a->join_left==(i?UINT32_MAX:0) && a->join_right==(i==2?0:UINT32_MAX));
    assert(!i || !ActionVoxelSprite_SameComponent(a,&full.sprites[i-1]));
  }
  /* The visible left part changes, but the next full section keeps identity.
   * Two identical pieces of one owner cannot match each other by accident. */
  clip_x=48;CaptureVoxelSprites(&f,&api,NULL,42);
  assert(f.voxel_sprites.count==3 && p->width==16 && p->component_x==48);
  assert(ActionVoxelSprite_SameComponent(&full.sprites[1],&f.voxel_sprites.sprites[1]));
  origin_x+=7;origin_y+=2;Owners(1);CaptureVoxelSprites(&f,&api,NULL,42);
  assert(ActionVoxelSprite_SameComponent(&full.sprites[1],&f.voxel_sprites.sprites[1]));
  assert(f.voxel_sprites.sprites[1].x==full.sprites[1].x+7 && f.voxel_sprites.sprites[1].y==full.sprites[1].y+2);
  fail_raster_at=rasters+2;CaptureVoxelSprites(&f,&api,NULL,42);
  assert(!f.voxel_sprites.count && !f.voxel_sprite_atlas.data);fail_raster_at=0;
  clip_x=0;Owners(6);CaptureVoxelSprites(&f,&api,NULL,42);
  assert(!f.voxel_sprites.count && !f.voxel_sprite_atlas.data);

  SelectTemple(7,0xe292,0x488f,32,48,16,16);Owners(1);
  f.diorama_map_number=7;CaptureVoxelSprites(&f,&api,NULL,42);
  assert(f.voxel_sprites.count==2 && p->height==32 && p->solid_y==16 && p->solid_height==16);
  assert(p->rows[0]!=UINT64_C(0xffffffff) && p->rows[16]==UINT64_C(0xffffffff));
  assert(f.voxel_sprites.sprites[1].component_y==32 && !f.voxel_sprites.sprites[1].solid_height);
  clip_y=16;CaptureVoxelSprites(&f,&api,NULL,42);
  assert(f.voxel_sprites.count==2 && p->height==16 && p->component_y==16 && !p->solid_y && p->solid_height==16);
  SelectTemple(7,0xe2bd,0x48d1,32,16,0,0);Owners(1);CaptureVoxelSprites(&f,&api,NULL,42);
  assert(f.voxel_sprites.count==1 && !p->solid_height && p->shape==kActRaiserSpriteProp_TempleSpikes);
  SelectTemple(8,0xe606,0x6022,32,16,0,3);Owners(1);f.diorama_map_number=8;
  CaptureVoxelSprites(&f,&api,NULL,42);
  assert(f.voxel_sprites.count==1 && p->solid_height==3 && !p->solid_y);
  SelectTemple(5,0xe2d1,0x48be,32,16,0,16);Owners(1);f.diorama_map_number=5;
  CaptureVoxelSprites(&f,&api,NULL,42);
  assert(f.voxel_sprites.count==1 && p->solid_height==16);
  clip_x=16;CaptureVoxelSprites(&f,&api,NULL,42);
  assert(f.voxel_sprites.count==1 && p->width==16 && p->component_x==16);
  oversize=true;CaptureVoxelSprites(&f,&api,NULL,42);assert(!f.voxel_sprites.count);oversize=false;
  group=2;room=3;source=0xba01;animation=0x4000;composition=0x49ff;
  width=height=16;clip_x=clip_y=0;outline=true;
  Owners(3);f.diorama_map_group=2;f.diorama_map_number=3;
  CaptureVoxelSprites(&f,&api,NULL,42);
  assert(f.voxel_sprites.count==3 && p->shape==kActRaiserSpriteProp_WallTorch);
  assert(p->priority==2 && p->width==16 && p->height==16 && p->owner_tag==1);
  assert(f.voxel_sprites.sprites[2].owner_tag==3);
  /* Ordinary Diorama and Off never extract the source pixels. In Voxel mode,
   * capture failure retains native artwork through the existing handoff. */
  f.diorama_voxel=NULL;CaptureVoxelSprites(&f,&api,NULL,42);
  assert(!f.voxel_sprites.count && !f.voxel_sprite_atlas.data);
  f.diorama_voxel=(const struct ActionVoxelGeometry *)1;
  fail_raster=true;CaptureVoxelSprites(&f,&api,NULL,42);
  assert(!f.voxel_sprites.count && !f.voxel_sprite_atlas.data);fail_raster=false;
  origin_x=-100;origin_y=80;
  for(unsigned in_oam=0;in_oam<2;++in_oam) {
    uint8_t shadow[544]={0};ActRaiserSpriteOwnership_Begin(2,3,0);
    if(in_oam) {
      ActRaiserSpriteOwnership_RecordActionObject(0xba01,0xfa0,80,464,0,4);
      ActRaiserSpriteOwnership_RecordActionVisual(0xba01,0xfa0,0x4000,0x7e,0,
          origin_x,origin_y,0,4);
    }
    ActRaiserSpriteOwnership_RecordWallTorchPart(0xba01,0xfa0,0x4000,0x7e,
        origin_x,origin_y,80,464,origin_x,origin_y,0x2000,16);
    ActRaiserSpriteOwnership_Complete(shadow);ActRaiserSpriteOwnership_Upload(2,3,shadow);
    const unsigned before_resolve=resolves;
    CaptureVoxelSprites(&f,&api,NULL,42);
    assert(f.voxel_sprites.count==1 && p->owner==0xfa0 && p->shape==kActRaiserSpriteProp_WallTorch);
    assert(p->x==28 && p->y==112 && p->priority==2 && p->width==16 && p->height==16);
    assert(p->owner_tag==(in_oam?1:128) && p->rows[1]==0x3ffc);
    assert(resolves==before_resolve+(in_oam?4:0));
    pixels=(const uint32_t *)f.voxel_sprite_atlas.data;
    assert(pixels[2*kVoxelSpriteAtlasWidth+3]==0xff000000);
    fail_raster=true;CaptureVoxelSprites(&f,&api,NULL,42);
    assert(!f.voxel_sprites.count && !f.voxel_sprite_atlas.data);fail_raster=false;
  }
  puts("Voxel sprite capture: ownership, sections, stable identity, solid/spike bands, joins, lighting and fallbacks passed");
}
