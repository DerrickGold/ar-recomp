#include "action/action_voxel.h"
#include "action/action_effect_receivers.h"
#include "actraiser/actraiser_sprite_ownership.h"
#include <string.h>

/* Reviewed native compositions, relative to the origin recorded by their
 * emitter. Their visible bounds may shrink at the viewport; section anchors
 * and the solid band must not move with that clipping. */
static bool VoxelSpriteLayout(unsigned shape, const SrPpuObjResolveResult *b,
    int *origin_x, int *origin_y, int *solid_y, int *solid_height) {
  int width=32, height=16, sy=0, sh=16;
  switch (shape) {
    case kActRaiserSpriteProp_Log:
    case kActRaiserSpriteProp_RockPlatform:
    case kActRaiserSpriteProp_WallTorch:
      if (b->x1-b->x0>kVoxelSpriteWidth || b->y1-b->y0>kVoxelSpriteHeight) return false;
      *solid_y=b->y0; *solid_height=b->y1-b->y0;
      return true;
    case kActRaiserSpriteProp_TempleBridge: width=192; height=sh=32; break;
    case kActRaiserSpriteProp_TemplePlatform: break;
    case kActRaiserSpriteProp_TempleCrusher: height=48; sy=16; break;
    case kActRaiserSpriteProp_TempleSpikes: sh=0; break;
    case kActRaiserSpriteProp_TempleSpikeRail: sh=3; break;
    default: return false;
  }
  if (b->x0<*origin_x || b->x1>*origin_x+width ||
      b->y0<*origin_y || b->y1>*origin_y+height) return false;
  *solid_y=*origin_y+sy; *solid_height=sh;
  return true;
}

/* An independent native composition atlas avoids sampling another sprite
 * through an overlapped OBJ plane. Public runner services own tile decoding,
 * flips, size, palette and master fade; no renderer reads live WRAM/VRAM. */
static void CaptureVoxelSprites(FrameSlot *f, const SnesRunnerApi *api,
    SrRunnerHandle *runner, uint64_t lifetime) {
  f->voxel_sprites.count = 0;
  f->voxel_sprite_atlas = (SrPpuSurfaceView){0};
  if (!f->diorama_active || !f->diorama_voxel ||
      !f->diorama_obj_owner_tags ||
      !ActionVoxel_Active(f->diorama_voxel,f->diorama_map_group,f->diorama_map_number) ||
      !(f->diorama_plane_request_mask & (1u << SR_PPU_OVERLAY_OBJ)) ||
      !api || api->struct_size < SNES_RUNNER_API_PPU_OBJ_PARTS_SIZE ||
      !api->resolve_ppu_obj_range || !api->rasterize_ppu_obj_parts) return;
  const ActRaiserSpriteOwnership ownership = ActRaiserSpriteOwnership_Presented(
      f->diorama_map_group, f->diorama_map_number);
  if (!ownership.valid) return;
  bool any=false;
  for (unsigned slot=0;slot<128;++slot) any |= ownership.prop_owner[slot]!=0;
  for (unsigned i=0;i<ownership.wall_torch_part_count;++i)
    any |= ownership.wall_torch_parts[i].owner_tag!=0;
  if (!any) return;
  static uint32_t atlas[kVoxelSpriteAtlasWidth * kVoxelSpriteAtlasHeight];
  /* One neighbour column on either side suppresses internal bridge walls.
   * The bounded atlas and upload dimensions remain unchanged. */
  static uint32_t pixels[(kVoxelSpriteWidth+2) * kVoxelSpriteHeight];
  static ActionReceiverLighting lighting;
  const bool lit = f->action_environmental_effects && ActionEffectReceivers_Prepare(
      &f->action_scene_effects, f->diorama_map_group, f->diorama_map_number,
      f->bg1_camera_x, f->bg1_camera_y, f->bg2_camera_x, f->bg2_camera_y,
      false, &lighting);
  for (unsigned first = 0; first < 128;) {
    const uint16_t owner = ownership.prop_owner[first];
    if (!owner) { ++first; continue; }
    unsigned end = first + 1;
    while (end < 128 && ownership.prop_owner[end] == owner) ++end;
    const unsigned shape=ownership.prop_shape[first], owner_start=f->voxel_sprites.count;
    const int origin_x=ownership.prop_origin_x[first], origin_y=ownership.prop_origin_y[first];
    const float ambient=lit ? ActionEffectReceivers_Ambient(&lighting, kActionReceiver_Enemies,
        ownership.world_x[first], ownership.world_y[first]) : 1;
    bool failed=false;
    for (unsigned priority = 0; priority < 4 && !failed; ++priority) {
      SrPpuObjPart parts[128];
      SrPpuObjResolveResult bounds = {.struct_size = sizeof(bounds)};
      const SrPpuObjResolveRequest request = {.struct_size = sizeof(request),
          .lifetime_generation = lifetime, .first_sprite = first,
          .sprite_count = end-first, .priority = priority,
          .part_capacity = 128, .parts = parts};
      if (api->resolve_ppu_obj_range(runner, &request, &bounds) != SR_RESULT_OK) {
        failed=true; break;
      }
      if (!bounds.part_count) continue;
      if (bounds.x1<=bounds.x0 || bounds.y1<=bounds.y0) continue;
      int sx=origin_x, sy=origin_y, solid_y=0, solid_height=0;
      if (!VoxelSpriteLayout(shape,&bounds,&sx,&sy,&solid_y,&solid_height)) {
        failed=true; break;
      }
      /* The older small props retain their exact single rectangle. Larger
       * reviewed compositions use a grid anchored before native clipping. */
      if (shape==kActRaiserSpriteProp_Log || shape==kActRaiserSpriteProp_RockPlatform ||
          shape==kActRaiserSpriteProp_WallTorch) {
        sx=bounds.x0; sy=bounds.y0;
      }
      for (int gy=sy; gy<bounds.y1 && !failed; gy+=kVoxelSpriteHeight)
      for (int gx=sx; gx<bounds.x1 && !failed; gx+=kVoxelSpriteWidth) {
        const int x0=gx>bounds.x0?gx:bounds.x0, y0=gy>bounds.y0?gy:bounds.y0;
        const int x1=gx+kVoxelSpriteWidth<bounds.x1?gx+kVoxelSpriteWidth:bounds.x1;
        const int y1=gy+kVoxelSpriteHeight<bounds.y1?gy+kVoxelSpriteHeight:bounds.y1;
        const int width=x1-x0, height=y1-y0;
        if (width<=0 || height<=0) continue;
        const int x=x0+f->ws_extra+f->obj_apron, y=y0+f->ws_extra_top;
        if (x+width<=0 || y+height<=0 || x>=f->snes_width+2*f->obj_apron ||
            y>=f->snes_height+f->ws_extra_top+f->ws_extra_bottom) continue;
        if (f->voxel_sprites.count==kVoxelSpriteLimit) {
          f->voxel_sprites.count=0;
          return;
        }
        SrPpuObjRasterResult result = {.struct_size = sizeof(result)};
        const SrPpuObjPartsRasterRequest raster = {.struct_size = sizeof(raster),
            .lifetime_generation = lifetime, .parts = parts, .part_count = bounds.part_count,
            .x0=x0-1, .y0=y0, .x1=x1+1, .y1=y1,
            .pixel_format = SR_PPU_OBJ_PIXEL_FORMAT_ARGB8888_U32,
            .pixels = pixels, .pixel_byte_size = sizeof(pixels),
            .pitch_bytes = (kVoxelSpriteWidth+2)*sizeof(uint32_t)};
        if (api->rasterize_ppu_obj_parts(runner,&raster,&result)!=SR_RESULT_OK) {
          failed=true; break;
        }
        const unsigned index=f->voxel_sprites.count;
        const int ax=(index%4)*kVoxelSpriteCellWidth, ay=(index/4)*kVoxelSpriteCellHeight;
        const int body0=solid_y>y0?solid_y:y0;
        const int body1=solid_y+solid_height<y1?solid_y+solid_height:y1;
        ActionVoxelSprite prop={.owner=owner,.shape=shape,.priority=priority,
            .owner_tag=ActRaiserSpriteOwnership_PropTag(&ownership,owner),
            .width=width,.height=height,.x=x,.y=y,.ambient=ambient,
            .component_x=x0-origin_x,.component_y=y0-origin_y,
            .solid_y=body1>body0?body0-y0:0,.solid_height=body1>body0?body1-body0:0};
        uint64_t occupied=0;
        for (unsigned cy=0; cy<kVoxelSpriteCellHeight; ++cy) {
          uint32_t *row=atlas+(ay+cy)*kVoxelSpriteAtlasWidth+ax;
          memset(row,0,kVoxelSpriteCellWidth*sizeof(*row));
          if (!cy || cy>(unsigned)height) continue;
          const uint32_t *source=pixels+(cy-1)*(kVoxelSpriteWidth+2);
          if (source[0]>>24) prop.join_left|=UINT32_C(1)<<(cy-1);
          if (source[width+1]>>24) prop.join_right|=UINT32_C(1)<<(cy-1);
          for (int cx=0; cx<width; ++cx) {
            const uint32_t color=source[cx+1];
            if (!(color>>24)) continue;
            prop.rows[cy-1]|=UINT64_C(1)<<cx;
            row[cx+1]=color;
          }
          occupied|=prop.rows[cy-1];
        }
        if (occupied) f->voxel_sprites.sprites[f->voxel_sprites.count++]=prop;
      }
    }
    /* Never publish only half a prop after a failed capture. The native OBJ
     * remains intact, including any unrelated sprite overlapping this owner. */
    if (failed) f->voxel_sprites.count=owner_start;
    first=end;
  }
  /* A native part wholly in the capture guard has no OAM slot to resolve.
   * Its completed-build record supplies the exact same part and owner used
   * by the apron rasterizer. Edge-straddling OAM owners were already captured
   * above, including their outer pixels, and must not gain a second face. */
  for(unsigned i=0;i<ownership.wall_torch_part_count;++i) {
    const ActRaiserWallTorchPart *native=&ownership.wall_torch_parts[i];
    if(!native->owner_tag)continue;
    bool in_oam=false;
    for(unsigned slot=0;slot<128;++slot)in_oam |= ownership.prop_owner[slot]==native->owner;
    if(in_oam)continue;
    const int x=native->x+f->ws_extra+f->obj_apron,y=native->y+f->ws_extra_top;
    const unsigned size=native->size;
    if(size!=16 || x+(int)size<=0 || y+(int)size<=0 ||
        x>=f->snes_width+2*f->obj_apron ||
        y>=f->snes_height+f->ws_extra_top+f->ws_extra_bottom)continue;
    if(f->voxel_sprites.count==kVoxelSpriteLimit) {f->voxel_sprites.count=0;return;}
    const SrPpuObjPart part={.x=native->x,.y=native->y,
        .tile_attr=native->tile_attr,.size=native->size};
    SrPpuObjRasterResult result={.struct_size=sizeof(result)};
    const SrPpuObjPartsRasterRequest raster={.struct_size=sizeof(raster),
        .lifetime_generation=lifetime,.parts=&part,.part_count=1,
        .x0=native->x,.y0=native->y,.x1=native->x+size,.y1=native->y+size,
        .pixel_format=SR_PPU_OBJ_PIXEL_FORMAT_ARGB8888_U32,
        .pixels=pixels,.pixel_byte_size=sizeof(pixels),
        .pitch_bytes=(kVoxelSpriteWidth+2)*sizeof(uint32_t)};
    if(api->rasterize_ppu_obj_parts(runner,&raster,&result)!=SR_RESULT_OK)continue;
    const unsigned index=f->voxel_sprites.count;
    const int ax=(index%4)*kVoxelSpriteCellWidth,ay=(index/4)*kVoxelSpriteCellHeight;
    ActionVoxelSprite prop={.owner=native->owner,.shape=kActRaiserSpriteProp_WallTorch,
        .priority=(native->tile_attr>>12)&3,.owner_tag=native->owner_tag,
        .width=size,.height=size,.x=x,.y=y,
        .component_x=native->x-native->origin_x,.component_y=native->y-native->origin_y,
        .solid_height=size,.ambient=lit?ActionEffectReceivers_Ambient(&lighting,
            kActionReceiver_Enemies,native->world_x,native->world_y):1};
    uint64_t occupied=0;
    for(unsigned cy=0;cy<kVoxelSpriteCellHeight;++cy) {
      uint32_t *row=atlas+(ay+cy)*kVoxelSpriteAtlasWidth+ax;
      memset(row,0,kVoxelSpriteCellWidth*sizeof(*row));
      if(!cy || cy>size)continue;
      const uint32_t *source=pixels+(cy-1)*(kVoxelSpriteWidth+2);
      for(unsigned cx=0;cx<size;++cx)if(source[cx]>>24) {
        row[cx+1]=source[cx];prop.rows[cy-1]|=UINT64_C(1)<<cx;
      }
      occupied|=prop.rows[cy-1];
    }
    if(occupied)f->voxel_sprites.sprites[f->voxel_sprites.count++]=prop;
  }
  if (!f->voxel_sprites.count) return;
  const unsigned height=((f->voxel_sprites.count+3)/4)*kVoxelSpriteCellHeight;
  f->voxel_sprite_atlas=(SrPpuSurfaceView){
      .flags=SR_PPU_SURFACE_BOUND|SR_PPU_SURFACE_HAS_CONTENT,
      .pixel_format=SR_PPU_OBJ_PIXEL_FORMAT_ARGB8888_U32,
      .data=(const uint8_t *)atlas,.byte_size=height*kVoxelSpriteAtlasWidth*4u,
      .pitch_bytes=kVoxelSpriteAtlasWidth*4u,
      .width_pixels=kVoxelSpriteAtlasWidth,.height_pixels=height,.scale=1};
}
