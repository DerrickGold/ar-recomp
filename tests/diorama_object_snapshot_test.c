#include "diorama/diorama_object_snapshot.h"
#include "support/test_assert.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t packet[kDioramaObjectSnapshotCapacity],original[kDioramaObjectSnapshotCapacity];
static uint32_t color[4][7][17],attributes[4][7][19];
static uint32_t atlas[kVoxelSpriteAtlasWidth*kVoxelSpriteAtlasHeight];
static uint32_t dense[kDioramaObjectSnapshotWidth*kDioramaObjectSnapshotHeight];

static void Put(size_t offset,uint32_t n) {
  for(unsigned i=0;i<4;++i)packet[offset+i]=(uint8_t)(n>>(8*i));
}
static void Rehash(size_t size) {
  uint32_t hash=2166136261u;
  for(size_t i=0;i<size;++i)hash=(hash^(i>=12 && i<16?0:packet[i]))*16777619u;
  Put(12,hash);
}
static void Rejected(size_t size,DioramaObjectSnapshot *owned) {
  const DioramaObjectSnapshot old=*owned;
  const uint32_t pixel=owned->view.obj[0][0];
  assert(!DioramaObjectSnapshot_Decode(packet,size,owned));
  assert(memcmp(&old,owned,sizeof(old))==0 && owned->view.obj[0][0]==pixel);
}
static void BadWord(size_t size,size_t offset,uint32_t word,DioramaObjectSnapshot *owned) {
  memcpy(packet,original,size);Put(offset,word);Rehash(size);Rejected(size,owned);
}

static DioramaObjectSnapshotView Fixture(void) {
  DioramaObjectSnapshotView v={.map_group=5,.map_number=6,.terrain_profile=2,.game_frame=2304,
    .world_x=400,.world_y=200,.camera_x=400,.camera_y=200,.width=13,.height=7,
    .owner_tags=true,.additive_priority_mask=15,.actor_count=2,
    .atlas=atlas,.atlas_pitch=kVoxelSpriteAtlasWidth,.atlas_height=kVoxelSpriteCellHeight};
  for(unsigned p=0;p<4;++p) {
    v.obj[p]=&color[p][0][0];v.obj_pitch[p]=17;
    v.attributes[p]=&attributes[p][0][0];v.attribute_pitch[p]=19;
    for(unsigned y=0;y<7;++y)for(unsigned x=0;x<13;++x)if((x+y+p)%2==0) {
      color[p][y][x]=0xff000000u|(p<<20)|(y<<8)|x;
      attributes[p][y][x]=((5u+p)<<24)|0x00010203u;
    }
  }
  /* Preserve nonzero RGB below zero alpha too; this is a lossless transport. */
  color[0][1][2]=0x00123456u;
  v.voxel_sprites.count=2;
  v.voxel_sprites.sprites[0]=(ActionVoxelSprite){.owner=0x09a0,.shape=4,.priority=2,
    .width=3,.height=2,.owner_tag=5,.component_x=-4,.component_y=1,
    .solid_y=1,.solid_height=1,.join_left=2,.join_right=1,.x=2,.y=2,.ambient=.625f,
    .source=0xe2d1,.parent_source=0xa686,.animation=0x4000,.generation=0xfedcba98,
    .presentation_mode=kDioramaVoxelObject_Extruded,.presentation_fields=15,
    .depth_offset=-.1f,.thickness=.3f,.width_scale=.5f,.join_top=5,.join_bottom=7,
    .rows={5,7}};
  v.voxel_sprites.sprites[1]=(ActionVoxelSprite){.owner=0x0aa0,.shape=3,.priority=0,
    .width=64,.height=32,.owner_tag=128,.component_x=64,.component_y=0,
    .solid_y=0,.solid_height=32,.join_left=UINT32_MAX,.join_right=UINT32_MAX,
    .x=-10,.y=-8,.ambient=1};
  for(unsigned y=0;y<32;++y)v.voxel_sprites.sprites[1].rows[y]=UINT64_MAX;
  for(unsigned i=0;i<2;++i) {
    const ActionVoxelSprite *p=&v.voxel_sprites.sprites[i];
    const unsigned ax=i*kVoxelSpriteCellWidth+1;
    for(unsigned y=0;y<p->height;++y)for(unsigned x=0;x<p->width;++x)
      if(p->rows[y]&(UINT64_C(1)<<x))atlas[(y+1)*kVoxelSpriteAtlasWidth+ax+x]=0xff808060u+i+x;
  }
  v.actors[0]=(ActionEffectActor){.generation=0xabcdef01,.source=0x9876,.parent_source=0x1234,
    .animation=0xffff,.state=20,.visual=5,.handler=0xaabb,.resume=0xccdd,.age=300,.phase_ticks=12,
    .address=0x08a0,.x=430,.y=250,.vx=-256,.vy=512,.bank=1,.priority=2,.visible=1,
    .flip=0x80,.player=1,.feet_y=268,.body_radius=12,.wake={430.5f,268,-1.5f,.75f,5}};
  v.actors[1]=(ActionEffectActor){.generation=3,.source=1,.x=-300,.y=-1,.vx=-32768,.vy=32767,
    .priority=3,.flip=255,.feet_y=-2,.wake={-300,-1,0,0,90}};
  return v;
}

int main(void) {
  DioramaObjectSnapshotView source=Fixture();
  size_t size=1;
  assert(DioramaObjectSnapshot_Encode(&source,packet,sizeof(packet),&size));
  assert(size<20000 && !memcmp(packet,"AROB",4));
  memcpy(original,packet,size);
  DioramaObjectSnapshot owned={0};
  assert(DioramaObjectSnapshot_Decode(packet,size,&owned));
  const DioramaObjectSnapshotView *v=&owned.view;
  assert(v->map_group==5 && v->map_number==6 && v->terrain_profile==2 && v->game_frame==2304);
  assert(v->world_x==400 && v->world_y==200 && v->camera_x==400 && v->camera_y==200);
  assert(v->owner_tags && v->additive_priority_mask==15 && v->width==13 && v->height==7);
  for(unsigned p=0;p<4;++p) {
    assert(v->obj_pitch[p]==13 && v->attribute_pitch[p]==13);
    for(unsigned y=0;y<7;++y)for(unsigned x=0;x<13;++x) {
      assert(v->obj[p][y*13+x]==color[p][y][x]);
      assert(v->attributes[p][y*13+x]==attributes[p][y][x]);
    }
  }
  assert(v->voxel_sprites.count==2 && v->voxel_sprites.sprites[1].rows[31]==UINT64_MAX);
  assert(v->voxel_sprites.sprites[1].owner_tag==128 && v->voxel_sprites.sprites[1].x==-10);
  assert(v->voxel_sprites.sprites[0].source==0xe2d1 && v->voxel_sprites.sprites[0].parent_source==0xa686);
  assert(v->voxel_sprites.sprites[0].depth_offset==-.1f && v->voxel_sprites.sprites[0].width_scale==.5f);
  assert(v->voxel_sprites.sprites[0].join_top==5 && v->voxel_sprites.sprites[0].join_bottom==7);
  assert(v->voxel_sprites.sprites[0].generation==0xfedcba98);
  assert(v->atlas_height==136 && v->atlas_pitch==264);
  assert(v->actor_count==2 && v->actors[0].generation==0xabcdef01 && v->actors[0].wake.x==430.5f);
  assert(v->actors[0].player && v->actors[1].vx==-32768 && v->actors[1].flip==255);
  size_t again=0;
  assert(DioramaObjectSnapshot_Encode(v,packet,sizeof(packet),&again) && again==size);
  assert(!memcmp(packet,original,size));
  /* Successful replacement owns its pixels independently of input and packet. */
  color[0][0][0]^=123;packet[size-1]^=1;
  assert(v->obj[0][0]!=color[0][0][0]);color[0][0][0]^=123;
  Rejected(size,&owned);
  memcpy(packet,original,size);
  for(size_t n=0;n<size;n+=113)Rejected(n,&owned);
  Rejected(size+1,&owned);
  const struct {size_t offset;uint32_t value;} bad[]={
    {4,3},{16,64},{20,8},{24,9},{28,3},{32,3},{36,65536},{40,399},
    {68,641},{72,353},{76,2},{80,16},{84,17},{88,81},{92,34},{120,1},
    {128,0},{132,10},{136,4},{140,65},{144,33},{148,129},
    {176,0x7fc00000},{180,0x3f000000},{184,0x7f800000},
    {200,3},{204,16},{216,0}, /* mode, flags and explicit zero width */
    {220,8},{240,8}, /* p0 width3 cannot own bit3 in joins or rows. */
    {128+2*368+16*4,4}, /* native actor priority */
    {128+2*368+17*4,2}, /* native actor visible */
    {128+2*368+25*4,0x7fc00000}, /* native actor wake strength */
    {128+2*368+2*108,8}, /* too many runs for13pixels */
    {128+2*368+2*108+4,0}, /* zero-length run */
  };
  for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i)BadWord(size,bad[i].offset,bad[i].value,&owned);
  memcpy(packet,original,size);
  assert(DioramaObjectSnapshot_Decode(packet,size,&owned));
  /* The rear-wall flame adds a shape value, without changing the lossless
   * transport or silently rewriting old native-object captures. */
  DioramaObjectSnapshotView torch=source;
  torch.voxel_sprites.sprites[0].shape=8;
  size_t torch_size=0;
  assert(DioramaObjectSnapshot_Encode(&torch,packet,sizeof(packet),&torch_size));
  assert(torch_size==size && DioramaObjectSnapshot_Decode(packet,torch_size,&owned));
  assert(owned.view.voxel_sprites.sprites[0].shape==8);
  torch.voxel_sprites.sprites[0].shape=kActionVoxelSpriteGenericShape;
  assert(DioramaObjectSnapshot_Encode(&torch,packet,sizeof(packet),&torch_size));
  assert(DioramaObjectSnapshot_Decode(packet,torch_size,&owned));
  assert(owned.view.voxel_sprites.sprites[0].shape==kActionVoxelSpriteGenericShape);
  /* Version 1's fixed 316-byte prop contract remains readable: its first
   * 60 bytes are followed directly by 32 silhouette rows. No authored facts
   * or joins existed there. Keep original actor/plane payloads byte-exact. */
  memcpy(packet,original,128);
  for(unsigned i=0;i<2;++i) {
    memcpy(packet+128+i*316,original+128+i*368,60);
    memcpy(packet+128+i*316+60,original+128+i*368+112,256);
  }
  memcpy(packet+128+2*316,original+128+2*368,size-128-2*368);
  const size_t legacy_size=size-104;
  Put(4,1);Put(8,legacy_size);Rehash(legacy_size);
  assert(DioramaObjectSnapshot_Decode(packet,legacy_size,&owned));
  assert(!owned.view.voxel_sprites.sprites[0].source && owned.view.voxel_sprites.sprites[0].width_scale==1);
  assert(!owned.view.voxel_sprites.sprites[0].generation);
  assert(owned.view.voxel_sprites.sprites[1].rows[31]==UINT64_MAX);
  assert(owned.view.actors[0].generation==source.actors[0].generation);

  /* All validation failures are transactional on the encoder too. */
  memset(packet,0xa5,32);again=9;
  assert(!DioramaObjectSnapshot_Encode(&source,packet,32,&again) && !again);
  for(unsigned i=0;i<32;++i)assert(packet[i]==0xa5);
  DioramaObjectSnapshotView bad_source=source;
  bad_source.width=0;assert(!DioramaObjectSnapshot_Encode(&bad_source,packet,sizeof(packet),&again));
  bad_source=source;bad_source.obj_pitch[0]=12;
  assert(!DioramaObjectSnapshot_Encode(&bad_source,packet,sizeof(packet),&again));
  bad_source=source;bad_source.voxel_sprites.sprites[0].rows[0]|=8;
  assert(!DioramaObjectSnapshot_Encode(&bad_source,packet,sizeof(packet),&again));
  bad_source=source;bad_source.actors[0].wake.strength=NAN;
  assert(!DioramaObjectSnapshot_Encode(&bad_source,packet,sizeof(packet),&again));
  atlas[kVoxelSpriteAtlasWidth+1]=0;
  assert(!DioramaObjectSnapshot_Encode(&source,packet,sizeof(packet),&again));
  for(unsigned i=0;i<32;++i)assert(packet[i]==0xa5);

  /* Zero-prop ordinary capture and absent bands remain valid with owner tags. */
  DioramaObjectSnapshotView empty={.map_group=1,.map_number=1,.owner_tags=true,
    .camera_x=10,.camera_y=20,.world_x=10,.world_y=20,.width=8,.height=4};
  assert(DioramaObjectSnapshot_Encode(&empty,packet,sizeof(packet),&again));
  assert(again==128+8*4*4);
  assert(DioramaObjectSnapshot_Decode(packet,again,&owned));
  assert(!owned.view.voxel_sprites.count && !owned.view.atlas && !owned.view.actor_count);
  for(unsigned p=0;p<4;++p)for(unsigned i=0;i<32;++i)
    assert(!owned.view.obj[p][i] && !owned.view.attributes[p][i]);

  /* Fully dense surfaces exercise the declared worst-case packet/storage bound. */
  DioramaObjectSnapshotView full={.map_group=7,.map_number=8,.terrain_profile=1,.game_frame=65535,
    .camera_x=1000,.camera_y=300,.extra_x=128,.extra_top=96,.obj_apron=64,
    .world_x=808,.world_y=204,.width=640,.height=352};
  for(unsigned i=0;i<640*352;++i)dense[i]=0xff000001u+i;
  for(unsigned p=0;p<4;++p){full.obj[p]=full.attributes[p]=dense;full.obj_pitch[p]=full.attribute_pitch[p]=640;}
  assert(DioramaObjectSnapshot_Encode(&full,packet,sizeof(packet),&again));
  assert(again==128+8*352*(640*4+8) && again<kDioramaObjectSnapshotCapacity);
  assert(DioramaObjectSnapshot_Decode(packet,again,&owned));
  assert(owned.view.obj[3][640*352-1]==dense[640*352-1]);
  DioramaObjectSnapshot_Release(&owned);assert(!owned.storage && !owned.view.width);
  DioramaObjectSnapshot_Release(&owned);
  puts("object snapshots: sparse/dense lossless roundtrip, native actors/props, bounds and transactional rejection passed");
  return 0;
}
