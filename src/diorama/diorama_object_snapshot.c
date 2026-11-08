#include "diorama_object_snapshot.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(float)==4 && FLT_RADIX==2 && FLT_MANT_DIG==24,
               "object snapshots require IEEE binary32");
enum { kMagic=0x424f5241u }; /* AROB */

typedef struct Codec {
  const uint8_t *input;
  uint8_t *output;
  size_t at, size;
  bool decode;
  unsigned version;
} Codec;

static uint32_t Read32(const uint8_t *p) {
  return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static void Write32(uint8_t *p,uint32_t v) {
  for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i));
}
static uint32_t Checksum(const uint8_t *p,size_t size) {
  uint32_t h=UINT32_C(2166136261);
  for(size_t i=0;i<size;++i)h=(h^(i>=12 && i<16?0:p[i]))*UINT32_C(16777619);
  return h;
}
static bool Word(Codec *c,uint32_t *v) {
  if(c->at>c->size || c->size-c->at<4)return false;
  if(c->decode)*v=Read32(c->input+c->at);
  else if(c->output)Write32(c->output+c->at,*v);
  c->at+=4;return true;
}

/* Range checks precede every narrowing assignment, including booleans. */
#define U(FIELD,LO,HI) do { \
  uint32_t value=(uint32_t)(FIELD); \
  if(!Word(c,&value) || (uint64_t)value<(uint64_t)(LO) || (uint64_t)value>(uint64_t)(HI))return false; \
  if(c->decode)(FIELD)=value; \
} while(0)
#define I(FIELD,LO,HI) do { \
  int32_t value=(FIELD); uint32_t bits;memcpy(&bits,&value,4); \
  if(!Word(c,&bits))return false;memcpy(&value,&bits,4); \
  if(value<(LO) || value>(HI))return false; \
  if(c->decode)(FIELD)=value; \
} while(0)
#define F(FIELD,LO,HI) do { \
  float value=(FIELD);uint32_t bits;memcpy(&bits,&value,4); \
  if(!Word(c,&bits))return false;memcpy(&value,&bits,4); \
  if(!isfinite(value) || value<(LO) || value>(HI))return false; \
  if(c->decode)(FIELD)=value; \
} while(0)

static bool Header(Codec *c,DioramaObjectSnapshotView *v) {
  U(v->map_group,1,7);U(v->map_number,1,8);U(v->terrain_profile,0,2);U(v->layer_section,0,2);
  U(v->game_frame,0,65535);
  I(v->world_x,-65536,65536);I(v->world_y,-65536,65536);
  I(v->camera_x,-32768,65535);I(v->camera_y,-32768,65535);
  U(v->extra_x,0,128);U(v->extra_top,0,128);U(v->obj_apron,0,64);
  U(v->width,1,kDioramaObjectSnapshotWidth);U(v->height,1,kDioramaObjectSnapshotHeight);
  U(v->owner_tags,0,1);U(v->additive_priority_mask,0,15);
  U(v->voxel_sprites.count,0,kVoxelSpriteLimit);U(v->actor_count,0,kActionEffectActorMax);
  U(v->atlas_height,0,kVoxelSpriteAtlasHeight);
  static const uint8_t rooms[]={0,4,8,6,7,8,8,8};
  if(v->map_number>rooms[v->map_group] ||
      v->world_x!=(int64_t)v->camera_x-v->extra_x-v->obj_apron ||
      v->world_y!=(int64_t)v->camera_y-v->extra_top ||
      v->extra_x*2u+v->obj_apron*2u>=v->width || v->extra_top>=v->height ||
      (v->voxel_sprites.count && !v->owner_tags))return false;
  return true;
}

static bool Prop(Codec *c,ActionVoxelSprite *p,unsigned width,unsigned height) {
  U(p->owner,1,65535);U(p->shape,1,c->version>=2?9:8);U(p->priority,0,3);
  U(p->width,1,kVoxelSpriteWidth);U(p->height,1,kVoxelSpriteHeight);U(p->owner_tag,1,128);
  I(p->component_x,-32768,32767);I(p->component_y,-32768,32767);
  U(p->solid_y,0,kVoxelSpriteHeight);U(p->solid_height,0,kVoxelSpriteHeight);
  U(p->join_left,0,UINT32_MAX);U(p->join_right,0,UINT32_MAX);
  F(p->x,-kVoxelSpriteWidth,kDioramaObjectSnapshotWidth);
  F(p->y,-kVoxelSpriteHeight,kDioramaObjectSnapshotHeight);F(p->ambient,0,4);
  if(c->version>=2) {
    U(p->source,0,65535);U(p->parent_source,0,65535);U(p->animation,0,65535);
    U(p->presentation_mode,0,kDioramaVoxelObject_Extruded);U(p->presentation_fields,0,15);
    F(p->depth_offset,-.4f,.8f);F(p->thickness,0,.8f);
    /* Unauthored legacy/synthetic props may be zero-initialized. Resolve()
     * supplies 1 before drawing; an explicitly authored width is never zero. */
    F(p->width_scale,0,4);
    if((p->presentation_fields&kDioramaVoxelObject_Width) && p->width_scale<.125f)return false;
    uint32_t tlo=(uint32_t)p->join_top,thi=(uint32_t)(p->join_top>>32);
    uint32_t blo=(uint32_t)p->join_bottom,bhi=(uint32_t)(p->join_bottom>>32);
    if(!Word(c,&tlo) || !Word(c,&thi) || !Word(c,&blo) || !Word(c,&bhi))return false;
    if(c->decode){p->join_top=(uint64_t)thi<<32|tlo;p->join_bottom=(uint64_t)bhi<<32|blo;}
    U(p->generation,0,UINT32_MAX);
  } else if(c->decode)p->width_scale=1;
  uint64_t occupied=0;
  const uint64_t columns=p->width==64?UINT64_MAX:(UINT64_C(1)<<p->width)-1;
  for(unsigned y=0;y<kVoxelSpriteHeight;++y) {
    uint32_t lo=(uint32_t)p->rows[y],hi=(uint32_t)(p->rows[y]>>32);
    if(!Word(c,&lo) || !Word(c,&hi))return false;
    if(c->decode)p->rows[y]=(uint64_t)hi<<32|lo;
    if((y>=p->height && p->rows[y]) || (p->rows[y]&~columns))return false;
    occupied|=p->rows[y];
  }
  const uint32_t rows=p->height==32?UINT32_MAX:(UINT32_C(1)<<p->height)-1;
  return occupied && p->solid_y<=p->height && p->solid_height<=p->height-p->solid_y &&
      !(p->join_left&~rows) && !(p->join_right&~rows) &&
      !(p->join_top&~columns) && !(p->join_bottom&~columns) &&
      p->x==floorf(p->x) && p->y==floorf(p->y) &&
      p->x>=-(float)p->width && p->x<(float)width &&
      p->y>=-(float)p->height && p->y<(float)height;
}

static bool Actor(Codec *c,ActionEffectActor *a) {
  U(a->generation,0,UINT32_MAX);
  U(a->source,0,65535);U(a->parent_source,0,65535);U(a->animation,0,65535);
  U(a->state,0,65535);U(a->visual,0,65535);U(a->handler,0,65535);U(a->resume,0,65535);
  U(a->age,0,65535);U(a->phase_ticks,0,65535);U(a->address,0,65535);
  I(a->x,-32768,32767);I(a->y,-32768,32767);I(a->vx,-32768,32767);I(a->vy,-32768,32767);
  U(a->bank,0,255);U(a->priority,0,3);U(a->visible,0,1);U(a->flip,0,255);U(a->player,0,1);
  I(a->feet_y,-32768,32767);U(a->body_radius,0,255);
  F(a->wake.x,-65536,65536);F(a->wake.y,-65536,65536);F(a->wake.dx,-65536,65536);
  F(a->wake.strength,0,1);U(a->wake.age,0,kActionActorWakeTicks);
  return true;
}
#undef F
#undef I
#undef U

/* Each row: run count, then (start:u16,length:u16) and literal nonzero words.
 * Runs are maximal and strictly separated by zero words. Worst-case row size
 * is4*width+8: between successive runs at least one zero pays for its header. */
static bool EncodePlane(Codec *c,const uint32_t *pixels,size_t pitch,
                        unsigned width,unsigned height,unsigned available_height) {
  for(unsigned y=0;y<height;++y) {
    const uint32_t *row=pixels && y<available_height?pixels+(size_t)y*pitch:NULL;
    uint32_t runs=0;
    if(row)for(unsigned x=0;x<width;) {
      if(!row[x]){++x;continue;}
      ++runs;while(x<width && row[x])++x;
    }
    if(!Word(c,&runs))return false;
    if(!row)continue;
    for(unsigned x=0;x<width;) {
      if(!row[x]){++x;continue;}
      const unsigned start=x;while(x<width && row[x])++x;
      uint32_t range=((x-start)<<16)|start;
      if(!Word(c,&range))return false;
      for(unsigned k=start;k<x;++k){uint32_t p=row[k];if(!Word(c,&p))return false;}
    }
  }
  return true;
}
static bool DecodePlane(Codec *c,uint32_t *pixels,unsigned width,unsigned height) {
  for(unsigned y=0;y<height;++y) {
    uint32_t runs=0;
    if(!Word(c,&runs) || runs>(width+1)/2)return false;
    unsigned previous_end=0;
    for(unsigned run=0;run<runs;++run) {
      uint32_t range=0;
      if(!Word(c,&range))return false;
      const unsigned start=range&65535,length=range>>16;
      if(!length || start>=width || length>width-start || (run && start<=previous_end))return false;
      for(unsigned x=start;x<start+length;++x) {
        uint32_t value=0;
        if(!Word(c,&value) || !value)return false;
        pixels[(size_t)y*width+x]=value;
      }
      previous_end=start+length;
    }
  }
  return true;
}

static bool SourcePitch(const uint32_t *pixels,size_t pitch,unsigned width,unsigned height) {
  return !pixels || (pitch>=width && pitch<=SIZE_MAX/sizeof(uint32_t)/height);
}
static bool AtlasValid(const DioramaObjectSnapshotView *v) {
  if(!v->voxel_sprites.count)return !v->atlas_height && !v->atlas;
  const unsigned required=((v->voxel_sprites.count+3)/4)*kVoxelSpriteCellHeight;
  if(!v->atlas || v->atlas_height<required || v->atlas_height>kVoxelSpriteAtlasHeight ||
      !SourcePitch(v->atlas,v->atlas_pitch,kVoxelSpriteAtlasWidth,v->atlas_height))return false;
  for(unsigned i=0;i<v->voxel_sprites.count;++i) {
    const ActionVoxelSprite *p=&v->voxel_sprites.sprites[i];
    const unsigned ax=(i%4)*kVoxelSpriteCellWidth+1,ay=(i/4)*kVoxelSpriteCellHeight+1;
    for(unsigned y=0;y<p->height;++y)for(unsigned x=0;x<p->width;++x)
      if(!!(p->rows[y]&(UINT64_C(1)<<x)) !=
          !!(v->atlas[(size_t)(ay+y)*v->atlas_pitch+ax+x]>>24))return false;
  }
  return true;
}
static bool Values(Codec *c,DioramaObjectSnapshotView *v) {
  for(unsigned i=0;i<v->voxel_sprites.count;++i)
    if(!Prop(c,&v->voxel_sprites.sprites[i],v->width,v->height))return false;
  for(unsigned i=0;i<v->actor_count;++i)if(!Actor(c,&v->actors[i]))return false;
  return true;
}
static bool EncodeBody(Codec *c,const DioramaObjectSnapshotView *source) {
  DioramaObjectSnapshotView v=*source;
  c->at=20;
  v.atlas_height=v.voxel_sprites.count?kVoxelSpriteAtlasHeight:0;
  if(!Header(c,&v))return false;
  c->at=kDioramaObjectSnapshotHeaderBytes;
  if(!Values(c,&v))return false;
  for(unsigned plane=0;plane<4;++plane)
    if(!EncodePlane(c,source->obj[plane],source->obj_pitch[plane],v.width,v.height,v.height))return false;
  for(unsigned plane=0;plane<4;++plane)
    if(!EncodePlane(c,source->attributes[plane],source->attribute_pitch[plane],v.width,v.height,v.height))return false;
  return !v.voxel_sprites.count || EncodePlane(c,source->atlas,source->atlas_pitch,
      kVoxelSpriteAtlasWidth,kVoxelSpriteAtlasHeight,source->atlas_height);
}

bool DioramaObjectSnapshot_Encode(const DioramaObjectSnapshotView *view,
    uint8_t *packet,size_t capacity,size_t *size) {
  if(size)*size=0;
  if(!view || !packet || !size)return false;
  /* Validate dimensions before pitch arithmetic or touching input pixels. */
  DioramaObjectSnapshotView checked=*view;
  Codec header={.at=20,.size=kDioramaObjectSnapshotHeaderBytes,.version=kDioramaObjectSnapshotVersion};
  if(!Header(&header,&checked))return false;
  for(unsigned i=0;i<4;++i)
    if(!SourcePitch(view->obj[i],view->obj_pitch[i],view->width,view->height) ||
       !SourcePitch(view->attributes[i],view->attribute_pitch[i],view->width,view->height))return false;
  Codec values={.size=kDioramaObjectSnapshotCapacity,.version=kDioramaObjectSnapshotVersion};
  if(!Values(&values,&checked) || !AtlasValid(view))return false;
  Codec measure={.size=kDioramaObjectSnapshotCapacity,.version=kDioramaObjectSnapshotVersion};
  if(!EncodeBody(&measure,view) || measure.at>capacity)return false;
  uint8_t *owned=malloc(measure.at);
  if(!owned)return false;
  memset(owned,0,kDioramaObjectSnapshotHeaderBytes);
  Write32(owned,kMagic);Write32(owned+4,kDioramaObjectSnapshotVersion);
  Write32(owned+8,(uint32_t)measure.at);Write32(owned+16,kDioramaObjectSnapshotHeaderBytes);
  Codec encode={.output=owned,.size=measure.at,.version=kDioramaObjectSnapshotVersion};
  const bool ok=EncodeBody(&encode,view) && encode.at==measure.at;
  if(ok) {
    Write32(owned+12,Checksum(owned,measure.at));
    memcpy(packet,owned,measure.at);*size=measure.at;
  }
  free(owned);return ok;
}

void DioramaObjectSnapshot_Release(DioramaObjectSnapshot *snapshot) {
  if(!snapshot)return;
  free(snapshot->storage);memset(snapshot,0,sizeof(*snapshot));
}
bool DioramaObjectSnapshot_Decode(const uint8_t *packet,size_t size,
                                 DioramaObjectSnapshot *out) {
  if(!packet || !out || size<kDioramaObjectSnapshotHeaderBytes || size>kDioramaObjectSnapshotCapacity ||
      Read32(packet)!=kMagic || Read32(packet+4)<1 || Read32(packet+4)>kDioramaObjectSnapshotVersion ||
      Read32(packet+8)!=size || Read32(packet+16)!=kDioramaObjectSnapshotHeaderBytes ||
      Read32(packet+12)!=Checksum(packet,size))return false;
  DioramaObjectSnapshot next={0};
  DioramaObjectSnapshotView *v=&next.view;
  Codec decode={.input=packet,.at=20,.size=size,.decode=true,.version=Read32(packet+4)};
  if(!Header(&decode,v) || v->atlas_height!=(v->voxel_sprites.count?kVoxelSpriteAtlasHeight:0))return false;
  for(size_t i=decode.at;i<kDioramaObjectSnapshotHeaderBytes;++i)if(packet[i])return false;
  decode.at=kDioramaObjectSnapshotHeaderBytes;
  if(!Values(&decode,v))return false;
  const size_t plane_words=(size_t)v->width*v->height;
  const size_t atlas_words=v->voxel_sprites.count?kVoxelSpriteAtlasWidth*kVoxelSpriteAtlasHeight:0;
  next.storage=calloc(plane_words*8+atlas_words,sizeof(uint32_t));
  if(!next.storage)return false;
  bool ok=true;
  for(unsigned i=0;i<8 && ok;++i) {
    uint32_t *pixels=next.storage+plane_words*i;
    if(i<4){v->obj[i]=pixels;v->obj_pitch[i]=v->width;}
    else {v->attributes[i-4]=pixels;v->attribute_pitch[i-4]=v->width;}
    ok=DecodePlane(&decode,pixels,v->width,v->height);
  }
  if(ok && atlas_words) {
    uint32_t *pixels=next.storage+plane_words*8;
    v->atlas=pixels;v->atlas_pitch=kVoxelSpriteAtlasWidth;
    ok=DecodePlane(&decode,pixels,kVoxelSpriteAtlasWidth,kVoxelSpriteAtlasHeight);
  }
  ok=ok && decode.at==size && AtlasValid(v);
  if(!ok){DioramaObjectSnapshot_Release(&next);return false;}
  DioramaObjectSnapshot_Release(out);*out=next;return true;
}
