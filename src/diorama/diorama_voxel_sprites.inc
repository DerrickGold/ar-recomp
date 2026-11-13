/* Dynamic native props share the modeled path's depth. Their captured native
 * winners are handed off only after the replacement atlas uploads successfully;
 * duplicate interior pixels must not reset depth through the actor-priority pass.
 * Top surfaces are behind actors; only the front shell can cover their feet. */
#include "action/action_voxel_sprites.h"
#include "actraiser/actraiser_sprite_ownership.h"

static bool VoxelSpriteFace(const DioramaCapture *capture,const DioramaViewGeometry *g,
    const ActionVoxelSprite *p,float ax,float ay,bool sample_alpha,unsigned *count,
    const float x[4],const float y[4],const float depth[4],
    const float u[4],const float v[4],float shade,float ambient) {
  if (*count == kVoxelMaxVisibleFaces) return false;
  VoxelSide face = {0};
  DioramaVoxelPickFace pick={.kind=kDioramaVoxelFace_Object,.priority=p->priority,
      .plane=-1,.cell_x=-1,.cell_y=-1,.owner=p->owner,.sample_alpha=sample_alpha};
  const float origin=p->x-p->component_x;
  const float width=(p->presentation_fields&kDioramaVoxelObject_Width)?p->width_scale:1;
  const float ox=capture->bg1_camera_x-(capture->width-256)*.5f-capture->obj_apron;
  const float oy=capture->camera_y-capture->authentic_y0;
  for (unsigned i=0; i<4; ++i) {
    const float distance=fmaxf(1,g->camera.distance);
    /* Ice keeps the native front sprite. Match the native OBJ projection
     * exactly at the lip so the top and sprite share their screen edge. */
    const float scale = ActionVoxelSprite_KeepsNativeFace(p) ?
        (distance+depth[i])/(distance-.01f) : 1+depth[i]/distance;
    const float px=origin+(x[i]-origin)*width;
    const float wx=(px/SR_PPU_SURFACE_MAX_WIDTH-g->u0)/(g->u1-g->u0)-.5f;
    const float wy=.5f-(y[i]/SR_PPU_SURFACE_MAX_HEIGHT-g->v0)/(g->v1-g->v0);
    if (!VoxelProjectWithW(g,wx*g->aspect_x*scale,wy*g->height_scale*scale,-depth[i],
                      &face.v[i].position, &face.distance, &face.depths[i],&face.clip_w[i])) return true;
    face.v[i].tex_coord=(ArRenderPointF){u[i]/kVoxelSpriteAtlasWidth,v[i]/kVoxelSpriteAtlasHeight};
    face.v[i].color=(ArRenderColorF){shade*ambient,shade*ambient,shade*ambient,1};
    pick.screen_xy[2*i]=face.v[i].position.x;pick.screen_xy[2*i+1]=face.v[i].position.y;
    pick.world_xy[2*i]=ox+p->x+u[i]-ax;pick.world_xy[2*i+1]=oy+p->y+v[i]-ay;
    pick.depth[i]=face.depths[i];pick.clip_w[i]=face.clip_w[i];
  }
  if(g->options->voxel_observe_face)
    g->options->voxel_observe_face(g->options->voxel_observer_userdata,&pick);
  s_voxel_sides[(*count)++]=face;
  return true;
}

static bool VoxelSpriteSpikes(const DioramaCapture *capture,const DioramaViewGeometry *g,unsigned *count,
    const ActionVoxelSprite *p,float ax,float ay,unsigned y0,unsigned y1,
    float start,float full,float recess,bool front) {
  if (y1<=y0) return true;
  const float pixel_depth=g->aspect_x/((g->u1-g->u0)*SR_PPU_SURFACE_MAX_WIDTH);
  /* Match the static spike treatment: three rows of crossed native profiles.
   * Clip each diagonal at the actor plane so its rear half cannot cover a
   * higher-priority actor. Texture coordinates follow that same clipping. */
  for (unsigned column=0; column<p->width; column+=16) {
    const unsigned width=p->width-column<16?p->width-column:16;
    const float half_x=width*.35355339f, half_z=fminf((full-start)/6,half_x*pixel_depth);
    if (half_z<=0) continue;
    for (unsigned row=0; row<3; ++row) for (unsigned diagonal=0; diagonal<2; ++diagonal) {
      const float middle=start+(full-start)*(row+.5f)/3;
      const float z0=middle+(diagonal?half_z:-half_z), z1=middle+(diagonal?-half_z:half_z);
      const float near=front?start:fmaxf(start,recess), far=front?fminf(full,recess):full;
      if(far<=near)continue;
      const float a=(near-z0)/(z1-z0), b=(far-z0)/(z1-z0);
      const float t0=fmaxf(0,fminf(a,b)), t1=fminf(1,fmaxf(a,b));
      if (t1<=t0) continue;
      const float x0=p->x+column+width*.5f-half_x+2*half_x*t0;
      const float x1=p->x+column+width*.5f-half_x+2*half_x*t1;
      const float u0=ax+column+.5f+(width-1)*t0, u1=ax+column+.5f+(width-1)*t1;
      const float d0=z0+(z1-z0)*t0, d1=z0+(z1-z0)*t1;
      if (!VoxelSpriteFace(capture,g,p,ax,ay,true,count,(float[]){x0,x1,x1,x0},
          (float[]){p->y+y0,p->y+y0,p->y+y1,p->y+y1},
          (float[]){d0,d1,d1,d0},(float[]){u0,u1,u1,u0},
          (float[]){ay+y0+.5f,ay+y0+.5f,ay+y1-.5f,ay+y1-.5f},
          front?.98f:.90f,p->ambient)) return false;
    }
  }
  return true;
}

static bool DrawVoxelSprites(ArRenderDevice *device,const DioramaCapture *capture,const DioramaViewGeometry *g,
    const DioramaScene *scene,const ArRenderOutputFrame *output,unsigned priority,bool front) {
  const ActionVoxelSpriteFrame *frame=scene->voxel_sprites;
  if (!frame || !frame->count || !ArRenderTexture_IsValid(scene->voxel_sprite_atlas) ||
      !ActionVoxel_Active(scene->voxel_geometry,scene->map_group,scene->map_number)) return true;
  unsigned count=0;
  const float default_depth=ActionVoxel_Depth(scene->voxel_geometry);
  const float recess=ActionVoxel_PlayRecess(scene->voxel_geometry);
  for (unsigned i=0; i<frame->count && i<kVoxelSpriteLimit; ++i) {
    const ActionVoxelSprite *p=&frame->sprites[i];
    if (!ActionVoxelSprite_IsModeled(p) || p->priority!=priority || !p->width || p->width>kVoxelSpriteWidth ||
        !p->height || p->height>kVoxelSpriteHeight ||
        p->solid_y+p->solid_height>p->height) continue;
    const float ax=(i%4)*kVoxelSpriteCellWidth+1, ay=(i/4)*kVoxelSpriteCellHeight+1;
    const bool attached_torch=p->shape==kActRaiserSpriteProp_WallTorch &&
        p->presentation_mode!=kDioramaVoxelObject_Extruded;
    const bool wall_torch=attached_torch &&
        (!(p->presentation_fields&kDioramaVoxelObject_Thickness) || p->thickness==0);
    const float start=ActionVoxelSprite_DepthOffset(p,recess)+(attached_torch?default_depth:0);
    const float full=start+ActionVoxelSprite_Thickness(p,default_depth);
    /* Native flame artwork shares the sconce/glow's BG1 plane. Keep the same
     * native priority slot and exact-owner handoff, but emit only one flat
     * rear face; a front cap would recreate the floating duplicate. */
    const unsigned caps=wall_torch || full==start?1:2;
    for(unsigned end=0;end<caps;++end) {
    if(!end && ActionVoxelSprite_KeepsNativeFace(p))continue;
    const float cap=wall_torch?start:end?full:start;
    if(front!=(cap<recess))continue;
    if (!VoxelSpriteFace(capture,g,p,ax,ay,true,&count,
        (float[]){p->x,p->x+p->width,p->x+p->width,p->x},
        (float[]){p->y,p->y,p->y+p->height,p->y+p->height},
        (float[]){cap,cap,cap,cap},
        (float[]){ax,ax+p->width,ax+p->width,ax},
        (float[]){ay,ay,ay+p->height,ay+p->height},wall_torch||front?1:.88f,p->ambient)) return false;
    }
    if (wall_torch) continue;
    if (!VoxelSpriteSpikes(capture,g,&count,p,ax,ay,0,p->solid_y,start,full,recess,front) ||
        !VoxelSpriteSpikes(capture,g,&count,p,ax,ay,p->solid_y+p->solid_height,p->height,
            start,full,recess,front)) return false;
    if (!p->solid_height) continue;
    for (unsigned axis=0; axis<2; ++axis) for (unsigned side=0; side<2; ++side) {
      /* Walking surfaces stay below the player even if their front rim is
       * higher priority. Other sides split at the native actor plane. */
      const bool top=axis==0 && side==0;
      if (front && top) continue;
      const bool native_ice_lip=top && ActionVoxelSprite_KeepsNativeFace(p);
      /* The native sprite is a sampled rectangle, while the cap is a
       * perspective triangle. Overlap their join by a subpixel in depth
       * so independent edge rasterization cannot expose the wall behind. */
      const float near=top?start-(native_ice_lip?.004f:0):front?start:fmaxf(start,recess);
      const float far=top?full:front?fminf(full,recess):full;
      if(far<=near)continue;
      const unsigned first_row=axis?0:p->solid_y, rows=axis?p->width:p->solid_y+p->solid_height;
      const unsigned first_col=axis?p->solid_y:0, cols=axis?p->solid_y+p->solid_height:p->width;
      for (unsigned row=first_row; row<rows; ++row) for (unsigned col=first_col; col<cols;) {
        unsigned begin=col;
        while (col<cols) {
          const unsigned x=axis?row:col, y=axis?col:row;
          const int nx=(int)x+(axis?(side?1:-1):0);
          const int ny=(int)y+(axis?0:(side?1:-1));
          const bool filled=y>=p->solid_y && y<p->solid_y+p->solid_height &&
              (p->rows[y]&(UINT64_C(1)<<x))!=0;
          const bool neighbor=ny<0 ? (p->join_top&(UINT64_C(1)<<x)) :
              ny>=p->height ? (p->join_bottom&(UINT64_C(1)<<x)) :
              ny>=p->solid_y && ny<p->solid_y+p->solid_height &&
              (nx<0 ? (p->join_left&(UINT32_C(1)<<ny)) :
               nx>=p->width ? (p->join_right&(UINT32_C(1)<<ny)) :
               (p->rows[ny]&(UINT64_C(1)<<nx))!=0);
          if (!filled || neighbor) break;
          ++col;
        }
        if (begin==col) {++col;continue;}
        const float edge=row+side;
        const float x0=axis?edge:begin, x1=axis?edge:col;
        const float y0=axis?begin:edge, y1=axis?col:edge;
        const float u0=ax+(axis?row+.5f:begin), u1=ax+(axis?row+.5f:col);
        const float v0=ay+(axis?begin:row+.5f), v1=ay+(axis?col:row+.5f);
        if (!VoxelSpriteFace(capture,g,p,ax,ay,false,&count,
            (float[]){p->x+x0,p->x+x1,p->x+x1,p->x+x0},
            (float[]){p->y+y0+(native_ice_lip?.25f:0),p->y+y1+(native_ice_lip?.25f:0),
                p->y+y1,p->y+y0},
            (float[]){near,near,far,far},(float[]){u0,u1,u1,u0},
            (float[]){v0,v1,v1,v0},top?.96f:axis?.80f:.65f,p->ambient)) return false;
      }
    }
  }
  if (!count) return true;
  count=SortVoxelSides(g,count);
  if(!count)return true;
  for (unsigned i=0;i<count;++i) {
    const VoxelSide *face=&s_voxel_sides[s_voxel_order[i]];
    memcpy(s_voxel_vertices+i*4,face->v,sizeof(face->v));
    memcpy(s_voxel_clip_w+i*4,face->clip_w,sizeof(face->clip_w));
    if(g->depth_sink)memcpy(s_voxel_depths+i*4,face->depths,sizeof(face->depths));
  }
  /* The existing screen-space receiver map works with arbitrary atlas UVs.
   * Reuse it for these faces instead of rebuilding/sampling light on the CPU. */
  const DioramaSceneryLightEffectParams light={.illumination=scene->actor_lightmaps[1],
      .shade={1,1,1,1},.light_uv={(float)output->viewport.x/output->output_width,
          (float)output->viewport.y/output->output_height,
          (float)output->viewport.w/output->output_width,
          (float)output->viewport.h/output->output_height}};
  return RenderShadedDioramaDepthGeometryWithW(device,scene->voxel_sprite_atlas,
      s_voxel_vertices,s_voxel_depths,s_voxel_clip_w,count*4,VoxelQuadIndices(),count*6,kArRenderBlendMode_Alpha,
      NULL,(ArRenderRectF){0,0,1,1},&light,g->depth_sink,DioramaPlaneForObjectPriority(priority),false);
}
