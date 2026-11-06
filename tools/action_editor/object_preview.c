#include "object_preview.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { kWidth=640, kHeight=352, kArea=kWidth*kHeight };
bool EditorObjectPreview_Load(EditorObjectPreview *p,const uint8_t *bytes,size_t size,
    unsigned group,unsigned room,unsigned terrain) {
  if(!p)return false;
  DioramaObjectSnapshot next={0};
  if(!DioramaObjectSnapshot_Decode(bytes,size,&next))return false;
  if(next.view.map_group!=group || next.view.map_number!=room || next.view.terrain_profile!=terrain) {
    DioramaObjectSnapshot_Release(&next);return false;
  }
  const bool ok=EditorObjectPreview_Adopt(p,&next);
  DioramaObjectSnapshot_Release(&next);return ok;
}
bool EditorObjectPreview_Adopt(EditorObjectPreview *p,DioramaObjectSnapshot *next) {
  if(!p || !next || !next->storage)return false;
  uint32_t *working=p->pixels?p->pixels:calloc(8*kArea,sizeof(uint32_t));
  if(!working)return false;
  DioramaObjectSnapshot_Release(&p->snapshot);
  p->snapshot=*next;memset(next,0,sizeof(*next));
  p->pixels=working;p->atlas_dirty=true;
  return true;
}
void EditorObjectPreview_Reset(EditorObjectPreview *p,ArRenderDevice *device) {
  if(!p)return;
  DioramaObjectSnapshot_Release(&p->snapshot);
  ArRenderDevice_DestroyTexture(device,p->atlas);
  free(p->pixels);memset(p,0,sizeof(*p));
}
/* Shared projection math reports what this view can actually see, including
 * drawable OBJ apron columns. Require only that footprint, not every unused
 * guard pixel. Voxel play recess contracts UVs toward the capture centre;
 * including that centre also conservatively covers the contracted range. */
static bool Covered(const DioramaObjectSnapshotView *source,const DioramaCapture *capture,
    const DioramaView *view,const DioramaScene *scene,int origin_x,int origin_y) {
  for(unsigned band=0;band<4;++band) {
    ArRenderRectF footprint;
    if(!Diorama_ResolveCaptureFootprint(capture,view,scene,
        DioramaPlaneForObjectPriority(band),&footprint))continue;
    const float x0=fmaxf(-capture->obj_apron,fminf(capture->width*.5f,footprint.x));
    const float y0=fmaxf(0,fminf(capture->height*.5f,footprint.y));
    const float x1=fminf(capture->width+capture->obj_apron,fmaxf(capture->width*.5f,footprint.x+footprint.w));
    const float y1=fminf(capture->height,fmaxf(capture->height*.5f,footprint.y+footprint.h));
    if(x1<=x0 || y1<=y0)continue;
    const float left=origin_x+capture->obj_apron+x0,top=origin_y+y0;
    const float right=origin_x+capture->obj_apron+x1,bottom=origin_y+y1;
    if(left<source->world_x-0.01f || top<source->world_y-0.01f ||
        right>source->world_x+source->width+0.01f ||
        bottom>source->world_y+source->height+0.01f)return false;
  }
  return true;
}
static bool Texture(ArRenderDevice *device,ArRenderTexture *texture,int width,int height) {
  if(ArRenderTexture_IsValid(*texture))return true;
  const ArRenderTextureDesc desc={.width=width,.height=height,
    .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming,
    .filter=kArRenderFilter_Nearest,.blend=kArRenderBlendMode_Alpha};
  return ArRenderDevice_CreateTexture(device,&desc,texture);
}
bool EditorObjectPreview_Capture(EditorObjectPreview *p,ArRenderDevice *device,
    DioramaCapture *capture,const DioramaView *view,DioramaScene *scene,bool voxel,
    ArRenderTexture textures[kDioramaPlane_Count],const uint8_t *pixels[kDioramaPlane_Count],
    unsigned *uploads) {
  if(!p || !p->snapshot.storage || !p->pixels)return false;
  const DioramaObjectSnapshotView *source=&p->snapshot.view;
  if(source->map_group!=scene->map_group || source->map_number!=scene->map_number ||
      source->layer_section!=scene->layer_section)return false;
  const int width=capture->width+2*capture->obj_apron,height=capture->height;
  if(width<=0 || width>kWidth || height<=0 || height>kHeight)return false;
  const int origin_x=capture->bg1_camera_x-(capture->width-256)/2-capture->obj_apron;
  const int origin_y=capture->camera_y-capture->authentic_y0;
  scene->effect_obj_priority_mask|=15;
  if(!Covered(source,capture,view,scene,origin_x,origin_y))return false;
  const int dx=source->world_x-origin_x,dy=source->world_y-origin_y;
  const int sx=dx<0?-dx:0,sy=dy<0?-dy:0;
  const int tx=dx>0?dx:0,ty=dy>0?dy:0;
  int copy_width=(int)source->width-sx,copy_height=(int)source->height-sy;
  if(copy_width>width-tx)copy_width=width-tx;
  if(copy_height>height-ty)copy_height=height-ty;
  memset(p->pixels,0,8*kArea*sizeof(uint32_t));
  for(unsigned band=0;band<4;++band) {
    uint32_t *color=p->pixels+(2*band)*kArea,*attr=color+kArea;
    for(int y=0;y<copy_height && copy_width>0;++y) {
      if(source->obj[band])memcpy(color+(ty+y)*kWidth+tx,
          source->obj[band]+(sy+y)*source->obj_pitch[band]+sx,copy_width*4);
      if(source->attributes[band])memcpy(attr+(ty+y)*kWidth+tx,
          source->attributes[band]+(sy+y)*source->attribute_pitch[band]+sx,copy_width*4);
    }
  }
  p->props=(ActionVoxelSpriteFrame){0};
  scene->voxel_sprites=NULL;scene->voxel_sprite_atlas=ArRenderTexture_Invalid();
  bool modeled[4]={false};
  /* Atlas handoff must succeed before any native winner is removed. */
  if(voxel && source->voxel_sprites.count) {
    if(!Texture(device,&p->atlas,kVoxelSpriteAtlasWidth,kVoxelSpriteAtlasHeight))return false;
    if(p->atlas_dirty) {
      if(!ArRenderDevice_UpdateTexture(device,p->atlas,NULL,source->atlas,source->atlas_pitch*4))return false;
      p->atlas_dirty=false;if(uploads)++*uploads;
    }
    p->props=source->voxel_sprites;
    for(unsigned i=0;i<p->props.count;++i) {
      ActionVoxelSprite *prop=&p->props.sprites[i];prop->x+=dx;prop->y+=dy;
      modeled[prop->priority]=true;
      uint32_t *color=p->pixels+2*prop->priority*kArea;
      ActionVoxelSprite_RemoveNative(prop,width,height,color,kWidth,color+kArea,kWidth);
    }
    scene->voxel_sprites=&p->props;scene->voxel_sprite_atlas=p->atlas;
  }
  for(unsigned band=0;band<4;++band) {
    uint32_t *color=p->pixels+2*band*kArea,*attr=color+kArea;
    if(source->owner_tags)for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
      const unsigned offset=y*kWidth+x;const uint32_t a=attr[offset];
      attr[offset]=(a>>24) && !(modeled[band]&&!color[offset]) ? a|UINT32_C(0xff000000) : 0;
    }
    const unsigned plane=DioramaPlaneForObjectPriority(band),attribute=kDioramaPlane_ObjAttributes0+band;
    if(!Texture(device,&textures[plane],kWidth,kHeight) ||
        !Texture(device,&textures[attribute],kWidth,kHeight) ||
        !ArRenderDevice_UpdateTexture(device,textures[plane],NULL,color,kWidth*4) ||
        !ArRenderDevice_UpdateTexture(device,textures[attribute],NULL,attr,kWidth*4))return false;
    pixels[plane]=(const uint8_t *)color;pixels[attribute]=(const uint8_t *)attr;
    if(uploads)*uploads+=2;
    scene->additive_plane_mask&=~(1u<<plane);
    if(source->additive_priority_mask&(1u<<band))scene->additive_plane_mask|=1u<<plane;
  }
  return true;
}
