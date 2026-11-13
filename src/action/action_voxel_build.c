/* Compile native collision, priority and reviewed artwork into solid surfaces.
 * Tile/character addresses are deliberately excluded from material identity. */
#include "action_voxel_internal.h"
#include "action/action_bg_metatile.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
typedef struct VoxelMaterialProfile {
  uint64_t key;
  uint16_t rooms;
  uint8_t kind[4];
  uint32_t flags;
  uint16_t rear_colors[4];
  uint16_t gate_colors[4];
  uint16_t face_colors[4]; /* Native face-only ink; PixelRecess instead selects rear-mounted ink. */
  bool complete_rear_matte;
  bool portal_jamb;
} VoxelMaterialProfile;
/* Artwork-only policy is separate from structural flags: existing shape,
 * animation and rear-copy allowlists must not reinterpret a new top donor. */
enum { kVoxelTopSurface_Own16=1, kVoxelTopSurface_Donor16=2, kVoxelTopSurface_MasonryCap16=4,
       kVoxelTopSurface_TurfBand8=8, kVoxelTopSurface_TurfDonor8=16, kVoxelTopSurface_SoilBand8=32,
       kVoxelTopSurface_FamilyCap16=64, kVoxelTopSurface_FamilyDonor16=128,
       kVoxelTopSurface_OwnTopBand5=256, kVoxelTopSurface_OwnTopBand4=512 };
enum { kVoxelNativeSurfaceIceCrystal=5, kVoxelNativeSurfaceHeadCrown=7, kVoxelNativeSurfaceDesertSandLive=8,
       kVoxelNativeSurfaceLavaStoneCap=11 };
typedef struct VoxelNativeSurfaceProfile {
  uint64_t key;
  uint16_t rooms;
  uint8_t profile,faces,family,max_side_length;
} VoxelNativeSurfaceProfile;
/* Exact mixed ice/stone source ownership; never a receiver policy. */
typedef struct VoxelIceTipSourceProfile {
  uint64_t key;
  uint16_t rooms,ice_rows[16];
} VoxelIceTipSourceProfile;
typedef struct VoxelTopSurfaceProfile {
  uint64_t key;
  uint16_t rooms;
  uint16_t policy;
  uint8_t family; /* Turf family: plain, right-soil transition, left-soil transition. */
} VoxelTopSurfaceProfile;
/* Native collision contours are a separate reviewed geometry contract. The
 * artwork above a walking slope stays visible on the rear plane. */
typedef struct VoxelContourSurfaceProfile {
  uint64_t key;
  uint16_t rooms;
  uint8_t orientation; /* kVoxelContourSkinTop/Bottom; no geometry policy. */
} VoxelContourSurfaceProfile;
typedef struct VoxelContourProfile {
  uint64_t key;
  uint16_t rooms;
  uint8_t native_slope;
} VoxelContourProfile;
typedef struct VoxelRearInkProfile {
  uint64_t key;
  uint16_t rooms;
  uint8_t palette_index;
} VoxelRearInkProfile;
typedef struct VoxelRearDonorProfile {
  uint64_t key;
  uint16_t rooms;
} VoxelRearDonorProfile;
#include "action_voxel_castle_materials.inc"
#include "action_voxel_forest_materials.inc"
#include "action_voxel_cave_materials.inc"
#include "action_voxel_cave_water_materials.inc"
#include "action_voxel_temple_materials.inc"
#include "action_voxel_marsh_materials.inc"
#include "action_voxel_desert_materials.inc"
#include "action_voxel_tomb_materials.inc"
#include "action_voxel_mountains_materials.inc"
#include "action_voxel_caverns_materials.inc"
#include "action_voxel_jungle_materials.inc"
#include "action_voxel_marahna_temple_materials.inc"
#include "action_voxel_snow_materials.inc"
#include "action_voxel_ice_materials.inc"
#include "action_voxel_ice_water_materials.inc"
#include "action_voxel_death_heim_materials.inc"
#include "action_voxel_native_surfaces.inc"
#include "action_voxel_ice_tip_sources.inc"
typedef struct VoxelTransparencyRule {
  uint64_t key;
  uint8_t group, room;
  uint16_t colors[4];
} VoxelTransparencyRule;
#include "action_voxel_transparency.inc"
enum { kCastleRearMaterial = 1, kCastleBackingMaterial = 2, kVoxelTexturedTopMaterial = 4,
       kVoxelRearCopyMaterial = 8, kVoxelRearFillMaterial = 16, kVoxelGrowthMaterial = 32,
       kVoxelBranchMaterial = 64, kVoxelSideProfileMaterial = 128,
       kVoxelRecessedMaterial = 256, kVoxelSpikeMaterial = 512,
       kVoxelRearGrassMaterial = 1024, kVoxelInfillMaterial = 2048,
       kVoxelTerrainBodyMaterial = 4096, kVoxelWaterSurfaceMaterial = 8192,
       kVoxelSoftSurfaceMaterial = 16384, kVoxelRearMountedMaterial = 32768,
       kVoxelOpaqueSurfaceMaterial = 65536, kVoxelFullSurfaceTextureMaterial = 131072,
       kVoxelTreeBarkMaterial = 262144, kVoxelTreeRootMaterial = 524288,
       kVoxelNativePrioritySceneryMaterial = 1048576,
       kVoxelRearMatteMaterial = 2097152,
       kVoxelPixelRecessMaterial = 4194304,
       kVoxelOpaqueRearMaterial = 8388608,
       kVoxelContactSceneryMaterial = 268435456,
       kVoxelRaisedSceneryMaterial = 536870912,
       kVoxelPostShadowMaterial = 1073741824 };
#define kVoxelSnowRockMaterial UINT32_C(2147483648)
typedef struct CastleMaterial {
  uint16_t words[4];
  uint64_t shape;
  uint8_t kind[4], collision;
  uint16_t top_surface;
  uint8_t top_family, contour_surface;
  uint8_t native_surface_faces,native_surface_family,native_surface_side_limit,native_surface_flip;
  uint8_t native_ice_tip_source,native_ice_tip_flip;
  bool native_slope, native_slope_foot, native_slope_support;
  bool rear_backing_donor;
  bool complete_rear_matte;
  bool portal_jamb;
  uint8_t opaque_rear_ink; /* One-based native palette index; zero is legacy black. */
  uint32_t flags;
  uint16_t rear_rows[16]; /* Contextual native collision contour, MSB is left. */
  uint16_t rear_colors[4];
  uint16_t gate_colors[4];
  uint16_t face_colors[4];
  uint8_t rear_fold_pixels;
  uint16_t cutouts[16];
  uint8_t pixels[4][64];
} CastleMaterial;
static VoxelSemanticKind SemanticMaterialKind(const CastleMaterial *m,unsigned kind) {
  if(m->flags&kVoxelSideProfileMaterial)return kVoxelSemantic_Stump;
  if(m->flags&(kVoxelTreeBarkMaterial|kVoxelTreeRootMaterial))return kVoxelSemantic_TreeWood;
  if(m->flags&kVoxelBranchMaterial)return kVoxelSemantic_Branch;
  if(m->flags&(kVoxelGrowthMaterial|kVoxelSpikeMaterial))return kVoxelSemantic_Hazard;
  if(kind==kVoxelGate)return kVoxelSemantic_Gate;
  if(m->flags&(kVoxelWaterSurfaceMaterial|kVoxelOpaqueSurfaceMaterial))return kVoxelSemantic_Water;
  if(kind==kVoxelSolid)return kVoxelSemantic_Platform;
  if(kind==kVoxelThin || kind==kVoxelForeground)return kVoxelSemantic_Scenery;
  if(kind==kVoxelFoliage)return kVoxelSemantic_Foliage;
  if(kind==kVoxelRearOnly || kind==kVoxelFlatParapet)return kVoxelSemantic_Rear;
  return kVoxelSemantic_None;
}
static unsigned Pixel(const ActionRoomScene *s, unsigned word, int x, int y) {
  if (word & 0x4000) x = 7 - x;
  if (word & 0x8000) y = 7 - y;
  if ((word & 1023) >= kActionRoomSceneCharacterBytes / 32) return 0;
  const unsigned char *c = s->characters + (word & 1023) * 32;
  const unsigned shift = 7 - x;
  return ((c[y * 2] >> shift) & 1) | (((c[y * 2 + 1] >> shift) & 1) << 1) |
         (((c[y * 2 + 16] >> shift) & 1) << 2) | (((c[y * 2 + 17] >> shift) & 1) << 3);
}
static unsigned DepthRank(const ActionVoxelGeometry *c, size_t at) {
  if (c->branch_pixels && (c->branch_pixels[at >> 3] & (1u << (at & 7)))) return 3;
  const unsigned kind = c->front[at] & kVoxelKindMask;
  const bool rear_relief=kind==kVoxelThin && c->material_rear_mounted &&
      c->raised_scenery_cells && c->raised_scenery_cells[
          (at/(size_t)c->width/8)*(c->width/8)+(at%(size_t)c->width)/8];
  return kind == kVoxelSolid ? 2 : kind == kVoxelForeground || rear_relief ? 1 : 0;
}
/* Complete profile panels replace internal pixel walls, but a wet rock's
 * ragged exterior crown still needs the short joins between its top strips.
 * Admit only the complete one/two-pixel closure above two supported columns;
 * never reopen a hole, an underside, or the profile's full outer panel. */
static bool KeepWetProfileCrownRiser(const ActionVoxelGeometry *c,
    const VoxelPatch *p, int x, int y, unsigned side) {
  if(c->profile!=kActionVoxelProfile_AitosMountains ||
      c->active_room<2 || c->active_room>3 || !c->soft_surfaces || !(side&1))return false;
  const int nx=x+(side==1?1:-1),left=p->x-c->origin_x,right=left+p->width;
  const int top=p->y-c->origin_y,bottom=top+p->height;
  if(left<0 || right>c->width || top<0 || bottom>c->height ||
      nx<left || nx>=right || x<left || x>=right)return false;
  const int columns[]={x,nx};int crown[2]={bottom,bottom};
  for(unsigned i=0;i<2;++i) {
    const int xx=columns[i];
    if(top>0 && DepthRank(c,(size_t)(top-1)*c->width+xx)==2)return false;
    for(int yy=top;yy<bottom;++yy)
      if(DepthRank(c,(size_t)yy*c->width+xx)==2) {crown[i]=yy;break;}
    if(crown[i]==bottom ||
        !c->soft_surfaces[(size_t)(crown[i]/16)*(c->width/16)+xx/16])return false;
  }
  const int height=crown[1]-crown[0];
  if(height<1 || height>2 || y<crown[0] || y>=crown[1])return false;
  for(int yy=crown[0];yy<=crown[1];++yy) {
    const size_t at=(size_t)yy*c->width+x,neighbor=(size_t)yy*c->width+nx;
    const unsigned owner_kind=c->front[at]&kVoxelKindMask;
    /* The reviewed wet profiles contain opaque face-only foam between rock
     * pixels. It remains on the original front, while the exterior cap join
     * closes behind it. Actual alpha holes and erased pixels remain open. */
    if((owner_kind!=kVoxelSolid && owner_kind!=kVoxelFoliage) ||
        !!(c->front[at]&kVoxelHighPriority)!=!!p->high_priority ||
        !c->soft_surfaces[(size_t)(yy/16)*(c->width/16)+x/16])return false;
    if(yy<crown[1]) {
      if((c->front[neighbor]&kVoxelKindMask)!=kVoxelFoliage ||
          !!(c->front[neighbor]&kVoxelHighPriority)!=!!p->high_priority)return false;
    } else if(DepthRank(c,neighbor)!=2 ||
        !!(c->front[neighbor]&kVoxelHighPriority)!=!!p->high_priority)return false;
  }
  return true;
}
static bool WetProfileFoamCrownRiser(const ActionVoxelGeometry *c,int x,int y,unsigned side) {
  if(!(side&1) || c->profile!=kActionVoxelProfile_AitosMountains ||
      c->active_room<2 || c->active_room>3 || !c->soft_surfaces ||
      !c->soft_surfaces[(size_t)(y/16)*(c->width/16)+x/16])return false;
  for(unsigned i=0;i<c->patch_count;++i) {
    const VoxelPatch *p=&c->patches[i];
    if(p->kind==kVoxelPatch_SideProfile && x+c->origin_x>=p->x &&
        x+c->origin_x<p->x+p->width && y+c->origin_y>=p->y && y+c->origin_y<p->y+p->height)
      return KeepWetProfileCrownRiser(c,p,x,y,side);
  }
  return false;
}
static bool KeepProfileContour(const ActionVoxelGeometry *c, int x, int y, unsigned side) {
  /* Forest stumps follow the native pixel silhouette, including bark steps. */
  if (c->profile == kActionVoxelProfile_FillmoreForest) return true;
  for (unsigned i = 0; i < c->patch_count; ++i) {
    const VoxelPatch *p = &c->patches[i];
    if (p->kind != kVoxelPatch_SideProfile || x+c->origin_x < p->x ||
        x+c->origin_x >= p->x+p->width || y+c->origin_y < p->y || y+c->origin_y >= p->y+p->height)
      continue;
    if (side & 1) return KeepWetProfileCrownRiser(c,p,x,y,side);
    /* Continuous walls hide the old extrusion's internal ledges. Only the
     * outer top and bottom silhouette still needs a horizontal cap. */
    const int first = side == 0 ? p->y-c->origin_y : y+1;
    const int end = side == 0 ? y : p->y+p->height-c->origin_y;
    for (int yy = first; yy < end; ++yy)
      if (DepthRank(c,(size_t)yy*c->width+x) == 2) return false;
    break;
  }
  return true;
}
static bool NativeActorPriorityAt(const ActionVoxelGeometry *c,int x,int y) {
  if(!c->native_priority_decks)return false;
  const unsigned quadrants=c->native_priority_decks[(size_t)(y/16)*(c->width/16)+x/16];
  /* Collision remains native, including pass-through platform behavior.
   * Every exposed contour of a reviewed beam follows its source priority. */
  return (quadrants&(1u<<(((y&15)/8)*2+(x&15)/8)))!=0;
}
static uint64_t Boundary(const ActionVoxelGeometry *c, int x, int y, unsigned side,
                         const float depths[4]) {
  const int dx[] = {0, 1, 0, -1}, dy[] = {-1, 0, 1, 0};
  const int xx = x + dx[side], yy = y + dy[side];
  const unsigned pixel = c->front[y * c->width + x];
  unsigned far_rank = DepthRank(c, y * c->width + x);
  if(!far_rank && (pixel&kVoxelKindMask)==kVoxelFoliage && WetProfileFoamCrownRiser(c,x,y,side))
    far_rank=2; /* Cap closure only: never change the native front/profile mask. */
  const unsigned near_rank = xx < 0 || yy < 0 || xx >= c->width || yy >= c->height
                                 ? 0
                                 : DepthRank(c, yy * c->width + xx);
  /* Thin foreground trim on a full floor exposes only the walkway behind it.
   * Shared solid interiors have no side, even across native priorities. */
  const bool rear=c->mixed_scenery && c->front_recess && far_rank==1 && c->front_recess[(y/8)*(c->width/8)+x/8];
  const bool neighbor_rear=c->mixed_scenery && c->front_recess && near_rank==1 &&
      c->front_recess[(yy/8)*(c->width/8)+xx/8];
  const bool thin=c->thin_trim_cells && far_rank==1 &&
      c->thin_trim_cells[(y/8)*(c->width/8)+x/8];
  const bool neighbor_thin=c->thin_trim_cells && near_rank==1 &&
      c->thin_trim_cells[(yy/8)*(c->width/8)+xx/8];
  const bool raised=c->raised_scenery_cells && far_rank==1 &&
      c->raised_scenery_cells[(y/8)*(c->width/8)+x/8];
  const bool neighbor_raised=c->raised_scenery_cells && near_rank==1 &&
      c->raised_scenery_cells[(yy/8)*(c->width/8)+xx/8];
  const unsigned column=c->column_cells && far_rank==2
      ? c->column_cells[(size_t)(y/8)*(c->width/8)+x/8] : 0;
  const unsigned neighbor_column=c->column_cells && near_rank==2
      ? c->column_cells[(size_t)(yy/8)*(c->width/8)+xx/8] : 0;
  const float world_y=c->origin_y+y+(side==2);
  bool exposed=depths[far_rank]>depths[near_rank];
  if(c->mixed_scenery || column || neighbor_column) {
    VoxelTrunkCurve section={0},neighbor_section={0};
    if(column)ActionVoxel_ColumnSection(&c->column_curves[column-1],c->origin_x+x+.5f,&section);
    if(neighbor_column)ActionVoxel_ColumnSection(&c->column_curves[neighbor_column-1],c->origin_x+xx+.5f,&neighbor_section);
    const float front=column?ActionVoxel_ColumnFrontDepth(c,&section,world_y):
        ActionVoxel_SceneryFront(c,rear,raised);
    const float back=far_rank==1?ActionVoxel_SceneryBack(c,front,thin,raised):depths[far_rank];
    const float neighbor_front=neighbor_column?ActionVoxel_ColumnFrontDepth(c,&neighbor_section,world_y):
        ActionVoxel_SceneryFront(c,neighbor_rear,neighbor_raised);
    const float neighbor_back=near_rank==1?ActionVoxel_SceneryBack(c,neighbor_front,neighbor_thin,neighbor_raised):depths[near_rank];
    exposed=back>front && !(neighbor_front<=front && neighbor_back>=back);
    if(!exposed && (side&1) && (column || neighbor_column)) {
      const float lower_front=column?ActionVoxel_ColumnFrontDepth(c,&section,world_y+1):front;
      const float lower_neighbor=neighbor_column?ActionVoxel_ColumnFrontDepth(c,&neighbor_section,world_y+1):neighbor_front;
      exposed=back>lower_front && !(lower_neighbor<=lower_front && neighbor_back>=back);
    }
  }
  return exposed && KeepProfileContour(c,x,y,side)
             ? far_rank | (near_rank << 2) | ((pixel & kVoxelHighPriority) ? 16 : 0) |
                   ((pixel & kVoxelRearTrim) ? 32 : 0) | ((pixel & kVoxelColliding) ? 64 : 0) |
                   ((pixel & kVoxelTexturedTop) ? 128 : 0) |
                   ((c->soft_surfaces && c->soft_surfaces[(y/16)*(c->width/16)+x/16]) ? 256 : 0) |
                   (rear?512:0) | (neighbor_rear?1024:0)
                   | ((c->trunk_cells && far_rank==1 ?
                       c->trunk_cells[(size_t)(y/8)*(c->width/8)+x/8] : 0u)<<11)
                   | ((uint64_t)column<<19) | ((uint64_t)neighbor_column<<27)
                   | ((uint64_t)thin<<35) | ((uint64_t)neighbor_thin<<36)
                   | ((uint64_t)raised<<37) | ((uint64_t)neighbor_raised<<38)
                   | ((uint64_t)(far_rank==2 && NativeActorPriorityAt(c,x,y))<<39)
             : 0;
}
static uint64_t HashByte(uint64_t hash, unsigned byte) {
  return (hash ^ (uint8_t)byte) * UINT64_C(1099511628211);
}
static const VoxelMaterialProfile *FindMaterial(unsigned group, unsigned room, uint64_t key) {
  const VoxelMaterialProfile *profiles;
  unsigned count;
  switch(ActionVoxel_ProfileForRoom(group,room)) {
    case kActionVoxelProfile_Castle:
      profiles=kCastleMaterials;count=sizeof(kCastleMaterials)/sizeof(*kCastleMaterials);break;
    case kActionVoxelProfile_FillmoreForest:
      profiles=kForestMaterials;count=sizeof(kForestMaterials)/sizeof(*kForestMaterials);break;
    case kActionVoxelProfile_FillmoreCave:
      profiles=kCaveMaterials;count=sizeof(kCaveMaterials)/sizeof(*kCaveMaterials);break;
    case kActionVoxelProfile_FillmoreTemple:
    case kActionVoxelProfile_FillmoreTower:
      profiles=kTempleMaterials;count=sizeof(kTempleMaterials)/sizeof(*kTempleMaterials);break;
    case kActionVoxelProfile_BloodpoolMarsh:
      profiles=kMarshMaterials;count=sizeof(kMarshMaterials)/sizeof(*kMarshMaterials);break;
    case kActionVoxelProfile_KassandoraDesert:
      profiles=kDesertMaterials;count=sizeof(kDesertMaterials)/sizeof(*kDesertMaterials);break;
    case kActionVoxelProfile_KassandoraTomb:
      profiles=kTombMaterials;count=sizeof(kTombMaterials)/sizeof(*kTombMaterials);break;
    case kActionVoxelProfile_AitosMountains:
      profiles=kMountainsMaterials;count=sizeof(kMountainsMaterials)/sizeof(*kMountainsMaterials);break;
    case kActionVoxelProfile_AitosCaverns:
      profiles=kCavernsMaterials;count=sizeof(kCavernsMaterials)/sizeof(*kCavernsMaterials);break;
    case kActionVoxelProfile_MarahnaJungle:
      profiles=kJungleMaterials;count=sizeof(kJungleMaterials)/sizeof(*kJungleMaterials);break;
    case kActionVoxelProfile_MarahnaTemple:
      profiles=kMarahnaTempleMaterials;count=sizeof(kMarahnaTempleMaterials)/sizeof(*kMarahnaTempleMaterials);break;
    case kActionVoxelProfile_NorthwallSnow:
      profiles=kSnowMaterials;count=sizeof(kSnowMaterials)/sizeof(*kSnowMaterials);break;
    case kActionVoxelProfile_NorthwallIce:
      profiles=kIceMaterials;count=sizeof(kIceMaterials)/sizeof(*kIceMaterials);break;
    case kActionVoxelProfile_DeathHeim:
      profiles=kDeathHeimMaterials;count=sizeof(kDeathHeimMaterials)/sizeof(*kDeathHeimMaterials);break;
    default:return NULL;
  }
  unsigned lo = 0, hi = count;
  while (lo < hi) {
    const unsigned mid = lo + (hi - lo) / 2;
    if (profiles[mid].key < key)
      lo = mid + 1;
    else
      hi = mid;
  }
  for (;
       lo < count && profiles[lo].key == key;
       ++lo)
    if (profiles[lo].rooms & (1u << room)) return &profiles[lo];
  return NULL;
}
static uint64_t MaterialKey(const CastleMaterial *m, unsigned flip) {
  uint64_t key = UINT64_C(14695981039346656037);
  for (unsigned q = 0; q < 4; ++q) {
    const unsigned source = q ^ flip, w = m->words[source];
    key = HashByte(key, (w >> 10) & 7);
    key = HashByte(key, (m->collision >> source) & 1);
    key = HashByte(key, !!(w & 0x2000));
    for (unsigned y = 0; y < 8; ++y)
      for (unsigned x = 0; x < 8; ++x)
        key = HashByte(key,
                       m->pixels[source][((flip & 2) ? 7 - y : y) * 8 + ((flip & 1) ? 7 - x : x)]);
  }
  return key;
}
static unsigned ContourSurfaceProfile(const ActionRoomScene *s,uint64_t key) {
  const VoxelContourSurfaceProfile *profiles=NULL;unsigned count=0;
  switch(ActionVoxel_ProfileForRoom(s->group,s->map)) {
    case kActionVoxelProfile_KassandoraTomb:
      profiles=kTombContourSurfaces;count=sizeof(kTombContourSurfaces)/sizeof(*profiles);break;
    case kActionVoxelProfile_NorthwallSnow:
      profiles=kSnowContourSurfaces;count=sizeof(kSnowContourSurfaces)/sizeof(*profiles);break;
    case kActionVoxelProfile_DeathHeim:
      profiles=kDeathHeimContourSurfaces;count=sizeof(kDeathHeimContourSurfaces)/sizeof(*profiles);break;
    default:break;
  }
  for(unsigned i=0;i<count;++i)
    if(profiles[i].key==key && (profiles[i].rooms&(1u<<s->map)))
      return profiles[i].orientation;
  return 0;
}
static unsigned NativeContourProfile(const ActionRoomScene *s,uint64_t key) {
  const VoxelContourProfile *profiles;unsigned count;
  switch(ActionVoxel_ProfileForRoom(s->group,s->map)) {
    case kActionVoxelProfile_AitosMountains:
      profiles=kMountainsContours;count=sizeof(kMountainsContours)/sizeof(*kMountainsContours);break;
    case kActionVoxelProfile_NorthwallSnow:
      profiles=kSnowContours;count=sizeof(kSnowContours)/sizeof(*kSnowContours);break;
    default:return 0;
  }
  for(unsigned i=0;i<count;++i)
    if(profiles[i].key==key && (profiles[i].rooms&(1u<<s->map)))return profiles[i].native_slope;
  return 0;
}
static unsigned NativeRearInkProfile(const ActionRoomScene *s,uint64_t key) {
  if(ActionVoxel_ProfileForRoom(s->group,s->map)!=kActionVoxelProfile_AitosMountains)return 0;
  for(unsigned i=0;i<sizeof(kMountainsRearInks)/sizeof(*kMountainsRearInks);++i)
    if(kMountainsRearInks[i].key==key && (kMountainsRearInks[i].rooms&(1u<<s->map)))
      return kMountainsRearInks[i].palette_index+1u;
  return 0;
}
static bool NativeRearDonorProfile(const ActionRoomScene *s,uint64_t key) {
  if(ActionVoxel_ProfileForRoom(s->group,s->map)!=kActionVoxelProfile_MarahnaJungle)return false;
  for(unsigned i=0;i<sizeof(kJungleRearDonors)/sizeof(*kJungleRearDonors);++i)
    if(kJungleRearDonors[i].key==key && (kJungleRearDonors[i].rooms&(1u<<s->map)))return true;
  return false;
}
static unsigned FindIceTipSourceProfile(const ActionRoomScene *s,uint64_t key) {
  if(ActionVoxel_ProfileForRoom(s->group,s->map)!=kActionVoxelProfile_NorthwallIce)return 0;
  for(unsigned i=0;i<sizeof(kVoxelIceTipSources)/sizeof(*kVoxelIceTipSources);++i)
    if(kVoxelIceTipSources[i].key==key && (kVoxelIceTipSources[i].rooms&(1u<<s->map)))return i+1;
  return 0;
}
static const VoxelNativeSurfaceProfile *FindNativeSurfaceProfile(const ActionRoomScene *s,uint64_t key) {
  const unsigned profile=ActionVoxel_ProfileForRoom(s->group,s->map);
  for(unsigned i=0;i<sizeof(kVoxelNativeSurfaces)/sizeof(*kVoxelNativeSurfaces);++i) {
    const VoxelNativeSurfaceProfile *p=&kVoxelNativeSurfaces[i];
    if(p->profile==profile && p->key==key && (p->rooms&(1u<<s->map)))return p;
  }
  return NULL;
}
static const VoxelTopSurfaceProfile *FindTopSurfaceProfile(const ActionRoomScene *s,uint64_t key) {
  const VoxelTopSurfaceProfile *profiles=NULL;unsigned count=0;
  switch(ActionVoxel_ProfileForRoom(s->group,s->map)) {
    case kActionVoxelProfile_BloodpoolMarsh:
      profiles=kMarshTopSurfaces;count=sizeof(kMarshTopSurfaces)/sizeof(*profiles);break;
    case kActionVoxelProfile_Castle:
      profiles=kCastleTopSurfaces;count=sizeof(kCastleTopSurfaces)/sizeof(*profiles);break;
    case kActionVoxelProfile_FillmoreTemple:case kActionVoxelProfile_FillmoreTower:
      profiles=kTempleTopSurfaces;count=sizeof(kTempleTopSurfaces)/sizeof(*profiles);break;
    case kActionVoxelProfile_FillmoreCave:
      profiles=kCaveTopSurfaces;count=sizeof(kCaveTopSurfaces)/sizeof(*profiles);break;
    case kActionVoxelProfile_DeathHeim:
      profiles=kDeathHeimTopSurfaces;count=sizeof(kDeathHeimTopSurfaces)/sizeof(*profiles);break;
    case kActionVoxelProfile_FillmoreForest:
      profiles=kForestTopSurfaces;count=sizeof(kForestTopSurfaces)/sizeof(*profiles);break;
    case kActionVoxelProfile_AitosMountains:
      profiles=kMountainsTopSurfaces;count=sizeof(kMountainsTopSurfaces)/sizeof(*profiles);break;
    case kActionVoxelProfile_NorthwallIce:
      profiles=kIceTopSurfaces;count=sizeof(kIceTopSurfaces)/sizeof(*profiles);break;
    case kActionVoxelProfile_MarahnaJungle:
      profiles=kJungleTopSurfaces;count=sizeof(kJungleTopSurfaces)/sizeof(*profiles);break;
    case kActionVoxelProfile_KassandoraTomb:
      profiles=kTombTopSurfaces;count=sizeof(kTombTopSurfaces)/sizeof(*profiles);break;
    default:return NULL;
  }
  unsigned lo=0,hi=count;
  while(lo<hi){const unsigned mid=lo+(hi-lo)/2;if(profiles[mid].key<key)lo=mid+1;else hi=mid;}
  for(;lo<count && profiles[lo].key==key;++lo)
    if(profiles[lo].rooms&(1u<<s->map))return &profiles[lo];
  return NULL;
}
static const VoxelTransparencyRule *FindTransparency(unsigned group, unsigned room, uint64_t key) {
  unsigned lo = 0, hi = sizeof(kVoxelTransparencyRules) / sizeof(*kVoxelTransparencyRules);
  const unsigned count = hi;
  while (lo < hi) {
    const unsigned mid = lo + (hi - lo) / 2;
    if (kVoxelTransparencyRules[mid].key < key) lo = mid + 1;
    else hi = mid;
  }
  for (; lo < count && kVoxelTransparencyRules[lo].key == key; ++lo)
    if (kVoxelTransparencyRules[lo].group == group && kVoxelTransparencyRules[lo].room == room)
      return &kVoxelTransparencyRules[lo];
  return NULL;
}
static void MaterialCutouts(CastleMaterial *m, const VoxelTransparencyRule *rule, unsigned flip) {
  /* Flood only the reviewed palette indices connected to the outer silhouette.
   * Enclosed shadows survive. Index zero is a passage, already transparent in
   * the PPU. Match indices before native fades/lighting, never displayed RGB. */
  uint8_t candidate[256], seen[256] = {0}, queue[256];
  unsigned head = 0, tail = 0;
  for (unsigned y = 0; y < 16; ++y)
    for (unsigned x = 0; x < 16; ++x) {
      const unsigned q = (y / 8) * 2 + x / 8, at = y * 16 + x;
      const unsigned pixel = m->pixels[q][(y & 7) * 8 + (x & 7)];
      candidate[at] = !pixel || (rule->colors[q ^ flip] & (1u << pixel));
      if (candidate[at] && (!x || x == 15 || !y || y == 15)) {
        seen[at] = 1;
        queue[tail++] = at;
      }
    }
  while (head < tail) {
    const unsigned at = queue[head++], x = at & 15, y = at / 16;
    const unsigned q = (y / 8) * 2 + x / 8;
    if (m->pixels[q][(y & 7) * 8 + (x & 7)]) m->cutouts[y] |= 1u << (15 - x);
    const int neighbours[] = {x ? (int)at - 1 : -1, x < 15 ? (int)at + 1 : -1,
                              y ? (int)at - 16 : -1, y < 15 ? (int)at + 16 : -1};
    for (unsigned i = 0; i < 4; ++i) {
      const int next = neighbours[i];
      if (next < 0 || !candidate[next] || seen[next]) continue;
      seen[next] = 1;
      queue[tail++] = next;
    }
  }
}
static bool MaterialTransparent(const CastleMaterial *m, const DioramaPixelEdit *edit,
                                 unsigned x, unsigned y) {
  const unsigned bit = 1u << (15 - x);
  return (edit && (edit->transparent[y] & bit)) ||
      (!(m->flags&kVoxelRearMatteMaterial) && (m->cutouts[y] & bit) &&
       !(edit && (edit->black[y] & bit)));
}
/* A water/post character is one native source but two semantic surfaces.
 * Only reviewed authored ink moves; water, high-priority wood and explicit
 * pixel edits retain their existing placement. Snow slope endpoints use the
 * same source partition: only their two contact pixels come forward while
 * the rest of the shallow detail stays recessed. Deduplicate small masks
 * and bound the split cells, rather than storing per-pixel depths. */
static bool BuildPixelRecess(ActionVoxelGeometry *c,const CastleMaterial *m,
    const DioramaPixelEdit *edit,unsigned quadrant,int tile_x,int tile_y,uint16_t slope_foot) {
  const bool foot=quadrant<2 && (slope_foot&(quadrant?0x00ff:0xff00)) &&
      m->kind[quadrant]==kVoxelForeground && (m->flags&kVoxelRearMountedMaterial);
  const bool ink=(m->flags&kVoxelPixelRecessMaterial) && m->face_colors[quadrant];
  if((!ink && !foot) || (m->words[quadrant]&0x2000))return true;
  uint8_t rows[8]={0};unsigned any=0;
  for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x) {
    const unsigned mx=(quadrant&1)*8+x,my=(quadrant>>1)*8+y;
    const unsigned p=m->pixels[quadrant][y*8+x];
    const bool rear=foot?!(my==0 && (slope_foot&(1u<<(15-mx)))):
        !!(m->face_colors[quadrant]&(1u<<p));
    if(!p || !rear || MaterialTransparent(m,edit,mx,my) ||
        (!foot && edit && (edit->black[my]&(1u<<(15-mx)))))continue;
    rows[y]|=1u<<(7-x);any=1;
  }
  if(!any)return true;
  if(c->pixel_recess_cell_count==kVoxelMaxPixelRecessCells)return false;
  if(!c->pixel_recess_cells) {
    c->pixel_recess_cells=calloc((size_t)(c->width/8),c->height/8);
    if(!c->pixel_recess_cells)return false;
  }
  unsigned owner=0;
  for(;owner<c->pixel_recess_mask_count;++owner)
    if(!memcmp(rows,c->pixel_recess_rows[owner],sizeof(rows)))break;
  if(owner==c->pixel_recess_mask_count) {
    memcpy(c->pixel_recess_rows[owner],rows,sizeof(rows));
    ++c->pixel_recess_mask_count;
  }
  c->pixel_recess_cells[(size_t)(tile_y-c->origin_y/8)*(c->width/8)+tile_x-c->origin_x/8]=owner+1;
  ++c->pixel_recess_cell_count;
  return true;
}
/* Decode each material once. Keys contain displayed art, palette slot,
 * priority and collision; metatile and character addresses are excluded.
 * Authored mirrors compare reflected quadrants and pixels, then transform
 * the semantic roles back. Arbitrary changed art still uses the fallback. */
static void Material(const ActionRoomScene *s, unsigned bg, const uint16_t words[4], unsigned collision,
                     CastleMaterial *out) {
  memset(out, 0, sizeof(*out));
  out->collision = collision;
  uint64_t shape = UINT64_C(14695981039346656037);
  for (unsigned q = 0; q < 4; ++q) {
    out->words[q] = words[q];
    out->kind[q] = (collision >> q) & 1 ? kVoxelSolid : 0;
    for (unsigned y = 0; y < 8; ++y)
      for (unsigned x = 0; x < 8; ++x) {
        const unsigned pixel = Pixel(s, words[q], x, y);
        out->pixels[q][y * 8 + x] = pixel;
        shape = HashByte(shape, pixel);
      }
  }
  out->shape = shape;
  for(unsigned flip=0;flip<4 && !out->native_ice_tip_source;++flip) {
    const unsigned profile=FindIceTipSourceProfile(s,MaterialKey(out,flip));
    if(profile) {out->native_ice_tip_source=(uint8_t)profile;out->native_ice_tip_flip=(uint8_t)flip;}
  }
  for(unsigned flip=0;flip<4 && !out->native_surface_family;++flip) {
    const VoxelNativeSurfaceProfile *p=FindNativeSurfaceProfile(s,MaterialKey(out,flip));
    if(!p)continue;
    unsigned faces=p->faces;
    if(flip&1)faces=(faces&5)|((faces&2)<<2)|((faces&8)>>2);
    if(flip&2) {
      const unsigned reflected=(faces&10)|((faces&1)<<2)|((faces&4)>>2);
      /* A reflected native ceiling must not opt in through an upper-only
       * contract. The reviewed lava cap carries its bounded stone band with
       * a reflected source; other families keep explicit orientations. */
      if(reflected!=faces && p->family!=kVoxelNativeSurfaceLavaStoneCap)continue;
      faces=reflected;
    }
    out->native_surface_faces=(uint8_t)faces;
    out->native_surface_family=p->family;
    out->native_surface_side_limit=p->max_side_length;
    out->native_surface_flip=(uint8_t)flip;
  }
  for(unsigned flip=0;flip<4 && !out->contour_surface;++flip) {
    unsigned skin=ContourSurfaceProfile(s,MaterialKey(out,flip));
    /* Summit profiles are inward from a native upper crest. A reflected
     * underside is a different surface, not a reason to whiten its edge. */
    if((flip&2) && skin>=kVoxelContourSkinSnowCrest)continue;
    if(skin && (flip&2))skin=skin==kVoxelContourSkinTop?kVoxelContourSkinBottom:kVoxelContourSkinTop;
    out->contour_surface=(uint8_t)skin;
  }
  for(unsigned flip=0;flip<4 && !out->native_slope && !out->native_slope_foot &&
      !out->native_slope_support;++flip) {
    const unsigned contour=NativeContourProfile(s,MaterialKey(out,flip));
    out->native_slope=contour==1;out->native_slope_foot=contour==2;
    out->native_slope_support=contour==3;
  }
  for(unsigned flip=0;flip<4 && !out->opaque_rear_ink;++flip)
    out->opaque_rear_ink=NativeRearInkProfile(s,MaterialKey(out,flip));
  for(unsigned flip=0;flip<4 && !out->rear_backing_donor;++flip)
    out->rear_backing_donor=NativeRearDonorProfile(s,MaterialKey(out,flip));
  for(unsigned flip=0;flip<4 && !out->top_surface;++flip) {
    const VoxelTopSurfaceProfile *top=FindTopSurfaceProfile(s,MaterialKey(out,flip));
    if(!top)continue;
    /* A native lower/upper band is directional. Horizontal stamps exchange
     * the two soil transitions; vertically flipped bands need fresh review. */
    if((flip&2) && (top->policy&(kVoxelTopSurface_TurfBand8|
        kVoxelTopSurface_TurfDonor8|kVoxelTopSurface_SoilBand8|
        kVoxelTopSurface_OwnTopBand5|kVoxelTopSurface_OwnTopBand4)))continue;
    out->top_surface=top->policy;out->top_family=top->family;
    if((flip&1) && (out->top_surface&(kVoxelTopSurface_TurfBand8|kVoxelTopSurface_TurfDonor8)) &&
        (out->top_family==2 || out->top_family==3))out->top_family^=1;
  }
  for (unsigned flip = 0; flip < 4; ++flip) {
    const VoxelMaterialProfile *p = FindMaterial(s->group, s->map, MaterialKey(out, flip));
    if (!p) continue;
    for (unsigned q = 0; q < 4; ++q) {
      out->kind[q] = p->kind[q ^ flip];
      out->rear_colors[q] = p->rear_colors[q ^ flip];
      out->gate_colors[q] = p->gate_colors[q ^ flip];
      out->face_colors[q] = p->face_colors[q ^ flip];
    }
    /* Backing stays in its reviewed source orientation. */
    out->flags = flip ? p->flags & ~kCastleBackingMaterial : p->flags;
    out->complete_rear_matte = p->complete_rear_matte;
    out->portal_jamb = p->portal_jamb;
    out->rear_fold_pixels = (flip & 2) ? 0 : (p->flags >> 24) & 15;
    break;
  }
  for (unsigned flip = 0; bg == 0 && flip < 4; ++flip) {
    const VoxelTransparencyRule *rule = FindTransparency(s->group, s->map, MaterialKey(out, flip));
    if (!rule) continue;
    MaterialCutouts(out, rule, flip);
    break;
  }
}
static unsigned NativeWords(const ActionRoomScene *s, unsigned id, uint16_t words[4]) {
  unsigned collision = 0;
  for (unsigned q = 0; q < 4; ++q) {
    const uint8_t *d = s->bg[0].metatiles + id * 8 + q * 2;
    const unsigned raw = (d[0] << 8) | d[1];
    collision |= !!(raw & 0x200) << q;
    words[q] = ActionBg_ComposeTilemapWord(raw, kActionRoomSceneTileWordMask,
                                           ActionRoomScene_BgAttributes(s, 1));
  }
  return collision;
}
bool ActionVoxel_AnimationMatches(const ActionRoomScene *reference,
                                   const uint8_t characters[kActionRoomSceneCharacterBytes],
                                   size_t *offset, size_t *bytes) {
  if (!reference || !characters || !offset || !bytes ||
      !reference->have_character_bank[0] || !reference->have_character_bank[1] ||
      !ActionRoomScene_HasCharacterAnimation(reference)) return false;
  const size_t target = (size_t)ActionRoomScene_CharacterAnimationTarget(reference) * 2;
  const size_t stride = ActionRoomScene_CharacterAnimationStride(reference);
  const unsigned phases = ActionRoomScene_CharacterAnimationPhaseCount(reference);
  if ((target & 31) || (stride & 31) || target > kActionRoomSceneCharacterBytes ||
      phases * stride > kActionRoomSceneCharacterBytes - target) return false;
  for (unsigned phase = 0; phase < phases; ++phase)
    if (!memcmp(characters + target, reference->characters + target + phase * stride, stride)) {
      *offset = target; *bytes = stride;
      return true;
    }
  return false;
}
typedef struct AnimationMaterial {
  uint16_t words[4];
  uint8_t collision;
} AnimationMaterial;
enum { kAnimationMaterials = 256 };
static bool AddAnimationMaterial(AnimationMaterial materials[kAnimationMaterials], unsigned *count,
                                  const uint16_t words[4], unsigned collision,
                                  size_t offset, size_t bytes, bool allow_solid) {
  bool affected = false;
  for (unsigned q = 0; q < 4; ++q) {
    const size_t at = (words[q] & 1023) * 32;
    affected |= at >= offset && at < offset + bytes;
  }
  if (!affected) return true;
  if (collision && !allow_solid) return false;
  for (unsigned i = 0; i < *count; ++i)
    if (materials[i].collision == collision &&
        !memcmp(materials[i].words, words, sizeof(materials[i].words))) return true;
  if (*count == kAnimationMaterials) return false;
  AnimationMaterial *m = &materials[(*count)++];
  memcpy(m->words, words, sizeof(m->words));
  m->collision = collision;
  return true;
}
bool ActionVoxel_AnimationIsPresentationOnly(const ActionRoomScene *s,
                                   const ActionRoomScene *reference,
                                   const DioramaRoomOverride *terrain,
                                   const uint16_t *tiles, size_t tile_pitch) {
  size_t offset, bytes;
  /* Only opt in policies without baked neighbour samples or structural
   * inference from rear artwork. Other policies retain normal invalidation. */
  if (!s || !reference || s->group != reference->group || s->map != reference->map ||
      !ActionVoxel_AnimationCacheCandidate(ActionVoxel_ProfileForRoom(s->group,s->map)) ||
      !ActionRoomScene_HasBackground(s,1) ||
      !ActionVoxel_AnimationMatches(reference,s->characters,&offset,&bytes)) return false;
  const ActionVoxelProfile profile = ActionVoxel_ProfileForRoom(s->group,s->map);
  const bool desert = profile == kActionVoxelProfile_KassandoraDesert;
  const unsigned width = ActionRoomScene_TileWidth(s,1), height = ActionRoomScene_TileHeight(s,1);
  if (tiles && tile_pitch < width) return false;
  /* Owned front/backing and donor art need exact animation only when they
   * actually observe the animated CHR window. A static touch-up must not undo
   * whole-room animation reuse on heavy jungle and snow maps. */
  if(terrain && terrain->voxel.cell_count) {
    const DioramaStampLayerOverride *bounds=&terrain->stamp_layers[0];
    const int x0=bounds->set_bounds?bounds->x0:0,y0=bounds->set_bounds?bounds->y0:0;
    const int columns=bounds->set_bounds?bounds->x1-x0:(int)width/2;
    const int rows=bounds->set_bounds?bounds->y1-y0:(int)height/2;
    uint16_t *owners=ActionVoxel_RecordOwners(&terrain->voxel,x0,y0,columns,rows);
    if(!owners)return false;
    bool used[kDioramaVoxelCellMax]={0},safe=true;
    for(int yy=0;safe && yy<rows;++yy)for(int xx=0;safe && xx<columns;++xx) {
      const unsigned owner=owners[(size_t)yy*columns+xx];if(!owner)continue;
      const DioramaVoxelCell *cell=&terrain->voxel.cells[owner-1];
      if(!ActionVoxel_CellOwnsArtwork(cell))continue;
      used[owner-1]=true;
      const int cx=x0+xx,cy=y0+yy;
      /* A moved native cap owns a small neighbour patch for its reviewed
       * inset/fold sampling. Those source pixels must stay live as well as
       * the selected cell itself; the compiler's 16px patches reach at most
       * one adjacent cell in each direction. */
      const int reach=ActionVoxel_CellUsesNativeTransform(cell)?1:0;
      for(int dy=-reach;safe && dy<=reach;++dy)for(int dx=-reach;safe && dx<=reach;++dx) {
        const DioramaTileStamp *stamp=DioramaLayerOrder_StampAt(bounds,cx+dx,cy+dy);
        if(reach && stamp && stamp->blank)continue;
        for(unsigned q=0;q<4;++q) {
          uint16_t word=0;const int x=(cx+dx)*2+(q&1),y=(cy+dy)*2+(q>>1);
          if(stamp)word=stamp->blank?0:stamp->words[q];
          else if(x>=0 && y>=0 && (unsigned)x<width && (unsigned)y<height) {
            if(tiles)word=tiles[(size_t)y*tile_pitch+x];
            else if(!ActionRoomScene_LookupTile(s,1,x,y,&word,NULL)){safe=false;break;}
          } else if(reach)continue; /* Missing neighbours are transparent, not CHR 0. */
          const size_t at=(word&1023)*32;
          if(at>=offset && at<offset+bytes){safe=false;break;}
        }
      }
    }
    free(owners);if(!safe)return false;
    for(unsigned i=0;i<terrain->voxel.cell_count;++i)if(used[i]) {
      const DioramaVoxelCell *cell=&terrain->voxel.cells[i];
      const DioramaVoxelArt *arts[]={&cell->backing_art,&cell->top_art,&cell->side_art,
          &cell->front_art,&cell->left_art,&cell->right_art,&cell->bottom_art};
      const unsigned fields[]={kDioramaVoxelCell_BackArt,kDioramaVoxelCell_TopArt,kDioramaVoxelCell_SideArt,
          kDioramaVoxelCell_FrontArt,kDioramaVoxelCell_LeftArt,kDioramaVoxelCell_RightArt,kDioramaVoxelCell_BottomArt};
      for(unsigned a=0;a<7;++a)if((cell->fields&fields[a]) && arts[a]->set)
        for(unsigned q=0;q<4;++q) {
          const size_t donor=(arts[a]->words[q]&1023)*32;
          if(donor>=offset && donor<offset+bytes)return false;
        }
    }
  }
  AnimationMaterial materials[kAnimationMaterials];
  unsigned count = 0;
  /* Include unused definitions: a definition can supply the backing marker
   * without a placed instance. Live publications and authored stamps below
   * may also combine characters differently from their native definitions. */
  for (unsigned id = 0; id < 256; ++id) {
    uint16_t words[4];
    const unsigned collision = NativeWords(s,id,words);
    if (!AddAnimationMaterial(materials,&count,words,collision,offset,bytes,desert)) return false;
  }
  if (tiles) for (unsigned y = 0; y < height; y += 2)
    for (unsigned x = 0; x < width; x += 2) {
      uint16_t words[4], native[4], word; uint8_t id;
      if (!ActionRoomScene_LookupTile(s,1,x,y,&word,&id)) return false;
      const unsigned collision = NativeWords(s,id,native);
      for (unsigned q = 0; q < 4; ++q)
        words[q] = tiles[(size_t)(y+(q>>1))*tile_pitch+x+(q&1)];
      if (!AddAnimationMaterial(materials,&count,words,collision,offset,bytes,desert)) return false;
    }
  if (terrain) for (size_t i = 0; i < terrain->stamp_layers[0].count; ++i) {
    const DioramaTileStamp *stamp = &terrain->stamp_layers[0].cells[i];
    uint16_t words[4];
    if (!stamp->blank && !AddAnimationMaterial(materials,&count,stamp->words,
          NativeWords(s,stamp->metatile,words),offset,bytes,desert)) return false;
  }
  /* Jungle's newly tracked BG2 waves may reuse geometry only when that
   * exact native animation window has no BG1 material/live-word/stamp owner.
   * Its optional uniform-water eligibility accepts every reviewed phase;
   * no animated front, neighbour repair or authored donor is canonicalized. */
  if(profile==kActionVoxelProfile_MarahnaJungle) {
    if(count || memcmp(s->bg[1].metatiles,reference->bg[1].metatiles,
                      sizeof(s->bg[1].metatiles)))return false;
    if(terrain)for(size_t i=0;i<terrain->stamp_layers[1].count;++i)
      for(unsigned q=0;q<4;++q) {
        const size_t at=(terrain->stamp_layers[1].cells[i].words[q]&1023)*32;
        if(at>=offset && at<offset+bytes)return false;
      }
    return true;
  }
  ActionRoomScene *phase_scene = malloc(sizeof(*phase_scene));
  if (!phase_scene) return false;
  *phase_scene = *s;
  bool safe = true;
  uint32_t flags[kAnimationMaterials];
  uint8_t live_surfaces[kAnimationMaterials];
  uint8_t kinds[kAnimationMaterials][4];
  uint64_t opaque[kAnimationMaterials][4];
  const unsigned phases = ActionRoomScene_CharacterAnimationPhaseCount(reference);
  for (unsigned phase = 0; safe && phase < phases; ++phase) {
    memcpy(phase_scene->characters+offset,reference->characters+offset+phase*bytes,bytes);
    for (unsigned i = 0; safe && i < count; ++i) {
      CastleMaterial m;
      Material(phase_scene,0,materials[i].words,materials[i].collision,&m);
      /* A molten surface depends on its fixed placement/priority and whether
       * any source survives authored masking. Identical per-pixel opacity in
       * every phase makes even partially erased/stamped surfaces invariant. */
      const unsigned live_surface=m.native_surface_family==kVoxelNativeSurfaceDesertSandLive
          ?16u|m.native_surface_faces:0;
      if(!phase){flags[i]=m.flags;live_surfaces[i]=(uint8_t)live_surface;}
      /* Desert sand animation changes color indices inside a fixed solid
       * silhouette. Unlike inert rear artwork, colliding surfaces require
       * exact kinds, cap/soft-surface/terrain-body flags and opacity in every
       * native phase. The terrain-body allowance is limited to reviewed live
       * dunes; it preserves depth through noncolliding silhouette quadrants.
       * This excludes baked color samples, neighbour inference, growth and
       * all palette-index-selected faces. Authored erasures intersect the
       * same opacity mask; native priority/collision words never change here. */
      bool solid_surface = desert &&
          !(m.flags & ~(kVoxelTexturedTopMaterial | kVoxelSoftSurfaceMaterial |
                        kVoxelTerrainBodyMaterial)) &&
          (!(m.flags & kVoxelTerrainBodyMaterial) ||
           m.native_surface_family == kVoxelNativeSurfaceDesertSandLive);
      for (unsigned q = 0; q < 4; ++q)
        solid_surface &= m.kind[q] == 0 || m.kind[q] == kVoxelSolid;
      safe = m.flags==flags[i] && live_surface==live_surfaces[i] && (solid_surface || !m.flags ||
          (profile==kActionVoxelProfile_AitosCaverns &&
           (m.flags&kVoxelOpaqueSurfaceMaterial) &&
           !(m.flags&~(kVoxelOpaqueSurfaceMaterial|kVoxelFullSurfaceTextureMaterial))));
      for (unsigned q = 0; q < 4; ++q) {
        if (!phase) kinds[i][q] = m.kind[q];
        safe &= (!desert || m.kind[q] == kinds[i][q]) &&
            (m.kind[q] == 0 || m.kind[q] == kVoxelRearOnly ||
             (solid_surface && m.kind[q] == kVoxelSolid)) &&
            !m.rear_colors[q] && !m.gate_colors[q] && !m.face_colors[q];
      }
      if(safe && (m.flags || desert))for(unsigned q=0;q<4;++q) {
        uint64_t mask=0;
        for(unsigned pixel=0;pixel<64;++pixel)if(m.pixels[q][pixel])mask|=UINT64_C(1)<<pixel;
        if(!phase)opaque[i][q]=mask;
        safe &= mask==opaque[i][q];
      }
      for (unsigned y = 0; y < 16; ++y) safe &= !m.cutouts[y];
    }
  }
  free(phase_scene);
  return safe;
}
static bool LookupMaterialSource(const ActionRoomScene *s, const DioramaStampLayerOverride *stamps,
                           const CastleMaterial native[256], int cx, int cy, CastleMaterial *out,
                           uint8_t *metatile, const uint16_t *tiles, size_t tile_pitch) {
  const DioramaTileStamp *stamp = stamps ? DioramaLayerOrder_StampAt(stamps, cx, cy) : NULL;
  if (stamp) {
    if (stamp->blank) return false;
    *metatile = stamp->metatile;
    if (!memcmp(stamp->words, native[*metatile].words, sizeof(stamp->words)))
      *out = native[*metatile];
    else
      Material(s, 0, stamp->words, native[*metatile].collision, out);
    return true;
  }
  uint16_t word;
  if (cx < 0 || cy < 0 || !ActionRoomScene_LookupTile(s, 1, cx * 2, cy * 2, &word, metatile))
    return false;
  *out = native[*metatile];
  if (tiles) {
    uint16_t words[4];
    for (unsigned q = 0; q < 4; ++q)
      words[q] = tiles[(size_t)(cy * 2 + (q >> 1)) * tile_pitch + cx * 2 + (q & 1)];
    if (memcmp(words, out->words, sizeof(words))) Material(s, 0, words, out->collision, out);
  }
  return true;
}
static unsigned NativeContourCollision(const ActionRoomScene *s,
    const DioramaStampLayerOverride *stamps,const CastleMaterial native[256],int cx,int cy) {
  if(stamps && stamps->set_bounds &&
      (cx<stamps->x0 || cy<stamps->y0 || cx>=stamps->x1 || cy>=stamps->y1))return 0;
  const DioramaTileStamp *stamp=stamps?DioramaLayerOrder_StampAt(stamps,cx,cy):NULL;
  if(stamp)return stamp->blank?0:native[stamp->metatile].collision;
  uint16_t word;uint8_t id;
  return cx>=0 && cy>=0 && ActionRoomScene_LookupTile(s,1,cx*2,cy*2,&word,&id)
      ? native[id].collision : 0;
}
static bool LookupMaterial(const ActionRoomScene *s,const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256],int cx,int cy,CastleMaterial *out,
    uint8_t *metatile,const uint16_t *tiles,size_t tile_pitch) {
  if(!LookupMaterialSource(s,stamps,native,cx,cy,out,metatile,tiles,tile_pitch))return false;
  if(!out->native_slope || (out->collision!=6 && out->collision!=9))return true;
  const unsigned left=NativeContourCollision(s,stamps,native,cx-1,cy);
  const unsigned right=NativeContourCollision(s,stamps,native,cx+1,cy);
  /* The native $00:90A9/$9084 collision paths choose 1:2 paired ramps from
   * adjacent matching collision codes, otherwise a standalone 1:1 ramp.
   * Neighbour priority is intentional and matches the native two branches. */
  for(unsigned x=0;x<16;++x) {
    const unsigned phase=out->collision==6?(x&14):((~x)&14);
    const bool upper=out->collision==6?left==6:right==9;
    const bool lower=out->collision==6?right==6:left==9;
    const unsigned height=upper?8+phase/2:lower?phase/2:phase;
    for(unsigned y=0;y<16-height;++y)out->rear_rows[y]|=1u<<(15-x);
  }
  return true;
}
static bool PlatformCap(const CastleMaterial *cap, const CastleMaterial *below) {
  /* Whole capitals follow structurally solid matching shafts. Shape and
   * collision replace the old metatile+8 relationship, including stamps. */
  if (cap->collision != 3 || cap->shape != below->shape) return false;
  bool different_palette = false;
  for (unsigned q = 0; q < 4; ++q) {
    if (below->kind[q] != kVoxelSolid) return false;
    different_palette |= ((cap->words[q] ^ below->words[q]) & 0x1c00) != 0;
  }
  return different_palette;
}
static bool RearFillDonor(const CastleMaterial *target, const CastleMaterial *donor) {
  if (donor->collision != target->collision ||
      (donor->flags & (kVoxelRearFillMaterial | kVoxelRearCopyMaterial |
                       kVoxelBranchMaterial | kVoxelSideProfileMaterial))) return false;
  for (unsigned q = 0; q < 4; ++q)
    if (donor->kind[q] != (target->collision ? kVoxelSolid : kVoxelRearOnly) ||
        (donor->words[q] & 0x2000)) return false;
  return true;
}
static bool BorderPlant(const CastleMaterial *m) {
  if (m->collision || (m->flags & (kVoxelRearFillMaterial | kVoxelRearCopyMaterial))) return false;
  for (unsigned q = 0; q < 4; ++q)
    if (m->kind[q] != kVoxelRearOnly || (m->words[q] & 0x2000)) return false;
  return true;
}
static bool AppendRearFill(ActionVoxelGeometry *c, const VoxelRearFill *fill) {
  if (c->rear_fill_count == c->rear_fill_capacity) {
    if (c->rear_fill_count == kVoxelMaxRearFills) return false;
    const unsigned capacity = c->rear_fill_capacity ? c->rear_fill_capacity * 2 : 16;
    VoxelRearFill *fills = realloc(c->rear_fills, capacity * sizeof(*fills));
    if (!fills) return false;
    c->rear_fills = fills;
    c->rear_fill_capacity = capacity;
  }
  c->rear_fills[c->rear_fill_count++] = *fill;
  return true;
}
static bool MaterialVisible(const CastleMaterial *m, const DioramaPixelEdit *edit) {
  for (unsigned y = 0; y < 16; ++y)
    for (unsigned x = 0; x < 16; ++x) {
      const unsigned bit = 1u << (15-x), q = (y/8)*2+x/8;
      if (MaterialTransparent(m, edit, x, y)) continue;
      if ((edit && (edit->black[y] & bit)) || m->pixels[q][(y&7)*8+(x&7)]) return true;
    }
  return false;
}
static unsigned WaterMaterial(const ActionRoomScene *s, const uint16_t words[4],
                                 CastleMaterial *material) {
  Material(s,1,words,0,material);
  const bool ice=ActionVoxel_ProfileForRoom(s->group,s->map)==kActionVoxelProfile_NorthwallIce;
  const VoxelMaterialProfile *profiles=ice?kIceWaterMaterials:kCaveWaterMaterials;
  const unsigned count=ice?sizeof(kIceWaterMaterials)/sizeof(*kIceWaterMaterials):
      sizeof(kCaveWaterMaterials)/sizeof(*kCaveWaterMaterials);
  for (unsigned flip = 0; flip < 4; ++flip) {
    const uint64_t key = MaterialKey(material,flip);
    for (unsigned i = 0; i < count; ++i)
      if (key == profiles[i].key && (profiles[i].rooms & (1u<<s->map))) return profiles[i].kind[0];
  }
  return false;
}
static bool BuildWaterSurfaces(ActionVoxelGeometry *c, const ActionRoomScene *s,
                               const DioramaRoomOverride *terrain) {
  if (!ActionVoxel_BackgroundWater(c->profile)) return true;
  if (!ActionRoomScene_HasBackground(s,2) || !s->have_character_bank[1]) return false;
  const DioramaStampLayerOverride *stamps = terrain ? &terrain->stamp_layers[1] : NULL;
  int x0 = 0, y0 = 0, x1 = ActionRoomScene_TileWidth(s,2)*8, y1 = ActionRoomScene_TileHeight(s,2)*8;
  if (stamps && stamps->set_bounds) {
    x0 = stamps->x0*16; y0 = stamps->y0*16; x1 = stamps->x1*16; y1 = stamps->y1*16;
  }
  if (!ActionVoxel_ValidBounds(x0,y0,x1,y1)) return false;
  CastleMaterial materials[256]; unsigned char water[256];
  for (unsigned id = 0; id < 256; ++id) {
    uint16_t words[4];
    for (unsigned q = 0; q < 4; ++q) {
      const uint8_t *d = s->bg[1].metatiles+id*8+q*2;
      words[q] = ActionBg_ComposeTilemapWord((d[0]<<8)|d[1],kActionRoomSceneTileWordMask,
                                             ActionRoomScene_BgAttributes(s,2));
    }
    water[id] = WaterMaterial(s,words,&materials[id]);
  }
  for (int y = y0/16; y < y1/16; ++y) for (int x = x0/16; x < x1/16; ++x) {
    const DioramaTileStamp *stamp = stamps ? DioramaLayerOrder_StampAt(stamps,x,y) : NULL;
    CastleMaterial edited; const CastleMaterial *material;
    uint16_t word; uint8_t id;unsigned kind;
    if (stamp) {
      if (stamp->blank || !(kind=WaterMaterial(s,stamp->words,&edited))) continue;
      id = stamp->metatile; material = &edited;
    } else {
      if (x < 0 || y < 0 || !ActionRoomScene_LookupTile(s,2,x*2,y*2,&word,&id) || !water[id]) continue;
      material = &materials[id];
      kind=water[id];
    }
    const DioramaPixelEdit *edit = terrain ? DioramaLayerOrder_PixelEditAt(terrain,1,x,y,id) : NULL;
    if (!MaterialVisible(material,edit)) continue;
    const bool high = (material->words[0]&0x2000) != 0;
    if(kind==2) {
      unsigned i=0;
      for(;i<c->waterfall_count;++i) {
        VoxelWaterfall *fall=&c->waterfalls[i];
        if(fall->x==x*16 && fall->width==16 && fall->y+fall->height==y*16 && fall->high_priority==high) {
          fall->height+=16;break;
        }
      }
      if(i==c->waterfall_count) {
        if(i==kVoxelMaxWaterfalls)return false;
        c->waterfalls[c->waterfall_count++]=(VoxelWaterfall){x*16,y*16,16,16,high};
      }
      continue;
    }
    VoxelWaterSurface *last = c->water_surface_count ? &c->water_surfaces[c->water_surface_count-1] : NULL;
    if (last && last->y == y*16 && last->x+last->width == x*16 && last->high_priority == high)
      last->width += 16;
    else {
      if (c->water_surface_count == kVoxelMaxWaterSurfaces) return false;
      c->water_surfaces[c->water_surface_count++] = (VoxelWaterSurface){
          .x=x*16,.y=y*16,.width=16,.high_priority=high,.background=1,.sample_y=3,.sample_height=8};
    }
  }
  /* Join adjacent columns only when the complete native fall agrees. */
  for(unsigned i=0;i<c->waterfall_count;++i)for(unsigned j=i+1;j<c->waterfall_count;) {
    VoxelWaterfall *a=&c->waterfalls[i],*b=&c->waterfalls[j];
    if(a->x+a->width==b->x && a->y==b->y && a->height==b->height && a->high_priority==b->high_priority) {
      a->width+=b->width;memmove(b,b+1,(c->waterfall_count-j-1)*sizeof(*b));--c->waterfall_count;
    } else ++j;
  }
  return true;
}
/* A high-priority post has no low-priority pixels to copy at its old position.
 * Reconstruct only an unambiguous continuation between two native neighbours.
 * The three receiver families stay separate: foliage/backdrop, shallow rear
 * masonry, and a colliding masonry course. Water and open pixels never qualify. */
static bool SurfaceStaticWord(const ActionRoomScene *s,unsigned word);
static unsigned SceneryBackingFamily(const ActionRoomScene *s,const CastleMaterial *m,
    const DioramaPixelEdit *edit) {
  if(m->flags&(kVoxelRearFillMaterial|kVoxelWaterSurfaceMaterial|kVoxelOpaqueSurfaceMaterial|
      kVoxelBranchMaterial|kVoxelSideProfileMaterial|kVoxelPixelRecessMaterial|
      kVoxelRearMatteMaterial|kVoxelOpaqueRearMaterial|kVoxelPostShadowMaterial))return 0;
  unsigned family=0;
  for(unsigned q=0;q<4;++q) {
    if((m->words[q]&0x2000) || !SurfaceStaticWord(s,m->words[q]) ||
        m->rear_colors[q] || m->gate_colors[q] || m->face_colors[q])return 0;
    const unsigned part=!m->collision && m->kind[q]==kVoxelRearOnly?1:
        !m->collision && m->kind[q]==kVoxelForeground &&
          (m->flags&(kVoxelRearMountedMaterial|kVoxelRearCopyMaterial))==
            (kVoxelRearMountedMaterial|kVoxelRearCopyMaterial)?2:
        m->collision==15 && m->kind[q]==kVoxelSolid?3:0;
    if(!part || (family && family!=part))return 0;
    family=part;
  }
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const unsigned q=(y/8)*2+x/8,bit=1u<<(15-x);
    if(MaterialTransparent(m,edit,x,y) || (m->cutouts[y]&bit) ||
        (!m->pixels[q][(y&7)*8+(x&7)] && !(edit && (edit->black[y]&bit))))return 0;
  }
  return family;
}
static uint16_t SceneryBackingColor(const CastleMaterial *m,const DioramaPixelEdit *edit,
    const uint16_t palette[128],unsigned x,unsigned y) {
  if(edit && (edit->black[y]&(1u<<(15-x))))return 0;
  const unsigned q=(y/8)*2+x/8,p=m->pixels[q][(y&7)*8+(x&7)];
  return palette[((m->words[q]>>10)&7)*16+p];
}
static unsigned SceneryBackingDifference(uint16_t a,uint16_t b) {
  unsigned difference=0;
  for(unsigned shift=0;shift<15;shift+=5)
    difference+=(unsigned)abs((int)((a>>shift)&31)-(int)((b>>shift)&31));
  return difference;
}
static bool SceneryBackingEdited(const DioramaRoomOverride *terrain,int x,int y) {
  const DioramaVoxelCell *a=terrain?DioramaVoxelEdits_Cell(&terrain->voxel,x,y):NULL;
  return a && (((a->fields&kDioramaVoxelCell_Backing) && a->backing!=kDioramaVoxelBacking_Auto) ||
      ((a->fields&kDioramaVoxelCell_BackArt) && a->backing_art.set) ||
      ((a->fields&kDioramaVoxelCell_Faces) && !(a->faces&kDioramaVoxelFaceMask_Back)) ||
      ((a->fields&kDioramaVoxelCell_Transform) && !ActionVoxel_CellKeepsNativeIdentity(a)) ||
      (a->fields&(kDioramaVoxelCell_Back|kDioramaVoxelCell_BackEnd|kDioramaVoxelCell_Width|
        kDioramaVoxelCell_Offset|kDioramaVoxelCell_Bend|kDioramaVoxelCell_Shape)));
}
static bool SceneryBackingPost(const ActionRoomScene *s,const CastleMaterial *m) {
  if(m->collision || !(m->flags&kVoxelRearFillMaterial))return false;
  for(unsigned q=0;q<4;++q)
    if(!(m->words[q]&0x2000) || !SurfaceStaticWord(s,m->words[q]) ||
        m->kind[q]!=kVoxelForeground || m->rear_colors[q] || m->gate_colors[q] || m->face_colors[q])return false;
  return true;
}
static bool BuildPostShadowRepair(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaRoomOverride *terrain,const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256],const CastleMaterial *target,int cx,int cy,
    const DioramaPixelEdit *target_edit,const uint16_t palette[128],
    const uint16_t *tiles,size_t tile_pitch) {
  if(c->profile!=kActionVoxelProfile_MarahnaJungle || !(target->flags&kVoxelPostShadowMaterial) ||
      target->collision)return true;
  unsigned uniform=0,shadow_index=0;
  for(unsigned q=0;q<4;++q) {
    if((target->words[q]&0x2000) || !SurfaceStaticWord(s,target->words[q]) ||
        target->kind[q]!=kVoxelForeground)return true;
    bool same=target->pixels[q][0]!=0;
    for(unsigned i=1;i<64;++i)same&=target->pixels[q][i]==target->pixels[q][0];
    if(same) {
      uniform|=1u<<q;
      if(shadow_index && shadow_index!=target->pixels[q][0])return true;
      shadow_index=target->pixels[q][0];
    }
  }
  if(uniform!=5 && uniform!=10)return true; /* Only the reviewed half-cell matte. */
  const int post_x=cx+(uniform==10?1:-1);
  if(post_x*16<c->origin_x || (post_x+1)*16>c->origin_x+c->width)return true;
  CastleMaterial post;uint8_t post_id;
  if(!LookupMaterial(s,stamps,native,post_x,cy,&post,&post_id,tiles,tile_pitch) ||
      !SceneryBackingPost(s,&post))return true;
  const DioramaPixelEdit *post_edit=terrain?DioramaLayerOrder_PixelEditAt(terrain,0,post_x,cy,post_id):NULL;
  CastleMaterial donor;const DioramaPixelEdit *donor_edit=NULL;bool found=false;
  /* A complete native wall must match every unshadowed source pixel. This
   * rejects the nearby window pieces instead of copying their openings into
   * the pillar footprint. Search only this course, with a fixed small bound. */
  for(int distance=1;distance<=4 && !found;++distance)
    for(int direction=-1;direction<=1 && !found;direction+=2) {
      const int sx=cx+distance*direction;uint8_t id;
      if(sx*16<c->origin_x || (sx+1)*16>c->origin_x+c->width ||
          SceneryBackingEdited(terrain,sx,cy) ||
          !LookupMaterial(s,stamps,native,sx,cy,&donor,&id,tiles,tile_pitch))continue;
      donor_edit=terrain?DioramaLayerOrder_PixelEditAt(terrain,0,sx,cy,id):NULL;
      if(SceneryBackingFamily(s,&donor,donor_edit)!=2)continue;
      bool match=true;
      for(unsigned q=0;q<4;++q) {
        match&=(donor.words[q]&0x1c00)==(target->words[q]&0x1c00);
        for(unsigned i=0;i<64;++i)
          match&=(uniform&(1u<<q))?donor.pixels[q][i]!=shadow_index:
              donor.pixels[q][i]==target->pixels[q][i];
      }
      if(donor_edit)for(unsigned y=0;y<16;++y)
        match&=!(donor_edit->transparent[y]|donor_edit->black[y]);
      found=match;
    }
  if(!found)return true;
  VoxelRearFill shadow={.x=cx*16,.y=cy*16,.replace_front=true};
  VoxelRearFill backing={.x=post_x*16,.y=cy*16,.preserve_existing=true};
  bool shadow_visible=false,backing_visible=false;
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const unsigned q=(y/8)*2+x/8,bit=1u<<(15-x);
    const uint16_t color=0x8000|SceneryBackingColor(&donor,donor_edit,palette,x,y);
    if((uniform&(1u<<q)) && !MaterialTransparent(target,target_edit,x,y) &&
        !(target->cutouts[y]&bit) && !(target_edit && (target_edit->black[y]&bit))) {
      shadow.colors[y*16+x]=color;shadow_visible=true;
    }
    if(!MaterialTransparent(&post,post_edit,x,y) && !(post.cutouts[y]&bit) &&
        (post.pixels[q][(y&7)*8+(x&7)] || (post_edit && (post_edit->black[y]&bit)))) {
      backing.colors[y*16+x]=color;backing_visible=true;
    }
  }
  return (!shadow_visible || AppendRearFill(c,&shadow)) &&
      (!backing_visible || AppendRearFill(c,&backing));
}
/* A terminal doorway can end with two foreground posts at the finite map
 * edge. It has no second rear neighbour; repeat only explicitly reviewed
 * plain masonry within the same bounded architectural run. Doorway and
 * carved-post artwork are deliberately not eligible donors. */
static bool TerminalBackingCourse(const ActionRoomScene *s,const CastleMaterial *m) {
  if(m->collision!=15)return false;
  for(unsigned q=0;q<4;++q)
    if(m->kind[q]!=kVoxelSolid || !SurfaceStaticWord(s,m->words[q]) ||
        m->rear_colors[q] || m->gate_colors[q] || m->face_colors[q])return false;
  return true;
}
static void BuildTerminalPostBacking(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaRoomOverride *terrain,const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256],const CastleMaterial *target,int cx,int cy,
    const DioramaPixelEdit *target_edit,const uint16_t palette[128],
    const uint16_t *tiles,size_t tile_pitch) {
  if(!SceneryBackingPost(s,target))return;
  const int x0=c->origin_x/16,x1=(c->origin_x+c->width)/16;
  const int y0=c->origin_y/16,y1=(c->origin_y+c->height)/16;
  for(unsigned side=0;side<2;++side) {
    const int edge=side?x1-1:x0,inward=side?-1:1,distance=(cx-edge)*inward;
    if(distance<0 || distance>1)continue;
    CastleMaterial material;uint8_t id;bool joined=true;
    for(int n=0;n<=distance;++n)
      joined&=LookupMaterial(s,stamps,native,edge+n*inward,cy,&material,&id,tiles,tile_pitch) &&
          SceneryBackingPost(s,&material);
    if(!joined)continue;
    int top=cy,bottom=cy;
    while(top>y0 && cy-top<8 && LookupMaterial(s,stamps,native,edge,top-1,&material,&id,tiles,tile_pitch) &&
        SceneryBackingPost(s,&material))--top;
    while(bottom+1<y1 && bottom-cy<8 && LookupMaterial(s,stamps,native,edge,bottom+1,&material,&id,tiles,tile_pitch) &&
        SceneryBackingPost(s,&material))++bottom;
    if(top==y0 || bottom+1==y1 || bottom-top>=8 ||
        !LookupMaterial(s,stamps,native,edge,top-1,&material,&id,tiles,tile_pitch) || !TerminalBackingCourse(s,&material) ||
        !LookupMaterial(s,stamps,native,edge,bottom+1,&material,&id,tiles,tile_pitch) || !TerminalBackingCourse(s,&material))continue;
    CastleMaterial donor;bool found=false;int best=1000;
    for(int row=top;row<=bottom;++row)for(int n=1;n<=4;++n) {
      const int sx=edge+n*inward;if(sx<x0 || sx>=x1 || SceneryBackingEdited(terrain,sx,row))continue;
      const int score=abs(row-cy)*16+abs(sx-cx);if(score>=best)continue;
      CastleMaterial candidate;
      if(!LookupMaterial(s,stamps,native,sx,row,&candidate,&id,tiles,tile_pitch) || !candidate.rear_backing_donor)continue;
      const DioramaPixelEdit *edit=terrain?DioramaLayerOrder_PixelEditAt(terrain,0,sx,row,id):NULL;
      bool changed=false;if(edit)for(unsigned y=0;y<16;++y)changed|=(edit->transparent[y]|edit->black[y])!=0;
      if(changed || SceneryBackingFamily(s,&candidate,edit)!=2)continue;
      donor=candidate;best=score;found=true;
    }
    if(!found)continue;
    VoxelRearFill fill={.x=cx*16,.y=cy*16,.preserve_existing=true};bool visible=false;
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      const unsigned q=(y/8)*2+x/8,bit=1u<<(15-x);
      if(MaterialTransparent(target,target_edit,x,y) || (target->cutouts[y]&bit) ||
          (!target->pixels[q][(y&7)*8+(x&7)] && !(target_edit && (target_edit->black[y]&bit))))continue;
      fill.colors[y*16+x]=0x8000|SceneryBackingColor(&donor,NULL,palette,x,y);visible=true;
    }
    /* This optional extension runs after required rear repairs. Exhaustion
     * or allocation failure keeps the native room rather than rejecting it. */
    if(visible)(void)AppendRearFill(c,&fill);
    return;
  }
  return;
}
static void BuildTerminalPostBackings(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaRoomOverride *terrain,const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256],const uint16_t palette[128],
    const uint16_t *tiles,size_t tile_pitch) {
  if(c->profile!=kActionVoxelProfile_MarahnaJungle)return;
  for(unsigned side=0;side<2;++side)for(int offset=0;offset<2;++offset) {
    const int x=side?(c->origin_x+c->width)/16-1-offset:c->origin_x/16+offset;
    if(x<c->origin_x/16 || x>=(c->origin_x+c->width)/16)continue;
    for(int y=c->origin_y/16;y<(c->origin_y+c->height)/16;++y) {
      if(c->rear_fill_count==kVoxelMaxRearFills)return;
      bool filled=false;for(unsigned i=0;i<c->rear_fill_count;++i)
        filled|=c->rear_fills[i].x==x*16 && c->rear_fills[i].y==y*16;
      if(filled)continue;
      CastleMaterial material;uint8_t id;
      if(!LookupMaterial(s,stamps,native,x,y,&material,&id,tiles,tile_pitch) || !SceneryBackingPost(s,&material))continue;
      const DioramaPixelEdit *edit=terrain?DioramaLayerOrder_PixelEditAt(terrain,0,x,y,id):NULL;
      BuildTerminalPostBacking(c,s,terrain,stamps,native,&material,x,y,edit,palette,tiles,tile_pitch);
    }
  }
}
static bool BuildSceneryRearFill(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaRoomOverride *terrain,const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256],const CastleMaterial *target,int cx,int cy,
    const DioramaPixelEdit *target_edit,const uint16_t palette[128],
    const uint16_t *tiles,size_t tile_pitch) {
  if(target->collision!=0 && target->collision!=15)return true;
  for(unsigned q=0;q<4;++q)
    if(!(target->words[q]&0x2000) || !SurfaceStaticWord(s,target->words[q]) ||
        target->rear_colors[q] || target->gate_colors[q] ||
        target->face_colors[q] || target->kind[q]!=(target->collision?kVoxelSolid:kVoxelForeground))return true;
  for(unsigned i=0;i<c->rear_fill_count;++i)
    if(c->rear_fills[i].x==cx*16 && c->rear_fills[i].y==cy*16 && c->rear_fills[i].preserve_existing)return true;
  if((cx-1)*16<c->origin_x || (cx+2)*16>c->origin_x+c->width)return true;
  CastleMaterial donors[2];uint8_t ids[2];const DioramaPixelEdit *edits[2];unsigned family=0;
  for(unsigned side=0;side<2;++side) {
    const int sx=cx+(side?1:-1);
    if(SceneryBackingEdited(terrain,sx,cy))return true;
    if(!LookupMaterial(s,stamps,native,sx,cy,&donors[side],&ids[side],tiles,tile_pitch))return true;
    edits[side]=terrain?DioramaLayerOrder_PixelEditAt(terrain,0,sx,cy,ids[side]):NULL;
    const unsigned candidate=SceneryBackingFamily(s,&donors[side],edits[side]);
    if(!candidate || (side && family!=candidate) ||
        (target->collision?(candidate!=3):(candidate==3)))return true;
    family=candidate;
  }
  for(unsigned q=0;q<4;++q)
    if((donors[0].words[q]&0x1c00)!=(donors[1].words[q]&0x1c00))return true;
  /* Select one complete native tile, never blend colours or repeat a narrow
   * strip. Compare both joins so a nearby plain matte does not automatically
   * replace the textured continuation on the other side. */
  unsigned scores[2]={0,0};
  for(unsigned side=0;side<2;++side)for(unsigned y=0;y<16;++y) {
    scores[side]+=SceneryBackingDifference(SceneryBackingColor(&donors[0],edits[0],palette,15,y),
        SceneryBackingColor(&donors[side],edits[side],palette,0,y));
    scores[side]+=SceneryBackingDifference(SceneryBackingColor(&donors[side],edits[side],palette,15,y),
        SceneryBackingColor(&donors[1],edits[1],palette,0,y));
  }
  const unsigned best=scores[1]<scores[0];
  VoxelRearFill fill={.x=cx*16,.y=cy*16,.preserve_existing=true};bool visible=false;
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const unsigned q=(y/8)*2+x/8,bit=1u<<(15-x);
    if(MaterialTransparent(target,target_edit,x,y) || (target->cutouts[y]&bit) ||
        (!target->pixels[q][(y&7)*8+(x&7)] && !(target_edit && (target_edit->black[y]&bit))))continue;
    fill.colors[y*16+x]=0x8000|SceneryBackingColor(&donors[best],edits[best],palette,x,y);
    visible=true;
  }
  return !visible || AppendRearFill(c,&fill);
}
static bool BuildRearFill(ActionVoxelGeometry *c, const ActionRoomScene *s,
    const DioramaRoomOverride *terrain, const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256], const CastleMaterial *target, int cx, int cy,
    const DioramaPixelEdit *target_edit, const uint16_t palette[128],
    const uint16_t *tiles, size_t tile_pitch) {
  if(c->profile==kActionVoxelProfile_MarahnaJungle)
    return BuildSceneryRearFill(c,s,terrain,stamps,native,target,cx,cy,target_edit,palette,tiles,tile_pitch);
  /* Only reviewed foreground decorations request a repair. Search along the
   * same world row, never up/down across a floor or an intentional pit. A
   * matching collision/material class prevents borrowing soil for a bush. */
  CastleMaterial support; uint8_t support_id;
  if (!target->collision &&
      (!LookupMaterial(s,stamps,native,cx,cy+1,&support,&support_id,tiles,tile_pitch) ||
       !support.collision)) return true;
  /* Prefer a complete tall border column over a closer short bush. Its
   * support must match the thorn's floor; no search crosses into a pit. */
  int best_x = 0, best_height = -1;
  for (int distance = 1; distance <= 4; ++distance)
    for (int direction = -1; direction <= 1; direction += 2) {
      const int sx = cx + direction * distance;
      if (sx * 16 < c->origin_x || (sx + 1) * 16 > c->origin_x + c->width) continue;
      CastleMaterial donor; uint8_t id;
      if (!LookupMaterial(s, stamps, native, sx, cy, &donor, &id, tiles, tile_pitch) ||
          !RearFillDonor(target, &donor)) continue;
      const DioramaPixelEdit *donor_edit = terrain ? DioramaLayerOrder_PixelEditAt(terrain,0,sx,cy,id) : NULL;
      if (!MaterialVisible(&donor,donor_edit)) continue;
      if (!target->collision) {
        CastleMaterial donor_support; uint8_t donor_support_id;
        if (!LookupMaterial(s,stamps,native,sx,cy+1,&donor_support,&donor_support_id,tiles,tile_pitch) ||
            donor_support.collision != support.collision) continue;
      }
      int height = 0;
      if (!target->collision)
        for (int above = 1; above <= 2 && (cy-above)*16 >= c->origin_y; ++above) {
          CastleMaterial plant; uint8_t plant_id;
          if (!LookupMaterial(s,stamps,native,sx,cy-above,&plant,&plant_id,tiles,tile_pitch) ||
              !BorderPlant(&plant)) break;
          const DioramaPixelEdit *plant_edit = terrain
              ? DioramaLayerOrder_PixelEditAt(terrain,0,sx,cy-above,plant_id) : NULL;
          if (!MaterialVisible(&plant,plant_edit)) break;
          ++height;
        }
      if (height > best_height) { best_x = sx; best_height = height; }
    }
  if (best_height < 0) return true;
  for (int above = 0; above <= best_height; ++above) {
      const int row = cy - above;
      CastleMaterial donor, destination; uint8_t id, destination_id;
      if (!LookupMaterial(s,stamps,native,best_x,row,&donor,&id,tiles,tile_pitch) ||
          !LookupMaterial(s,stamps,native,cx,row,&destination,&destination_id,tiles,tile_pitch)) continue;
      if (above && destination.collision) break;
      const DioramaPixelEdit *edit = terrain ? DioramaLayerOrder_PixelEditAt(terrain,0,best_x,row,id) : NULL;
      const DioramaPixelEdit *destination_edit = !above ? target_edit :
          terrain ? DioramaLayerOrder_PixelEditAt(terrain,0,cx,row,destination_id) : NULL;
      VoxelRearFill fill = {.x = cx * 16, .y = row * 16, .preserve_existing = above != 0};
      bool opaque = false;
      for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x) {
          const unsigned bit = 1u << (15 - x), q = (y / 8) * 2 + x / 8;
          if ((destination_edit && (destination_edit->transparent[y] & bit)) ||
              MaterialTransparent(&donor, edit, x, y)) continue;
          if ((destination_edit && (destination_edit->black[y] & bit)) ||
              (edit && (edit->black[y] & bit))) {
            fill.colors[y*16+x] = 0x8000; opaque = true; continue;
          }
          const unsigned p = donor.pixels[q][(y & 7) * 8 + (x & 7)];
          if (!p) continue;
          fill.colors[y * 16 + x] = palette[((donor.words[q] >> 10) & 7) * 16 + p] | 0x8000;
          opaque = true;
        }
      if (!opaque) continue;
      if (!AppendRearFill(c,&fill)) return false;
    }
  return true;
}
/* Thorn caps can share a shrub crown with a neighboring tree root. The
 * joining pixels identify the rear shrub independently of the hazard below. */
static bool ForestCrownJoinsRoot(const ActionRoomScene *s,
    const DioramaStampLayerOverride *stamps, const CastleMaterial native[256],
    const CastleMaterial *crown, int cx, int cy, const uint16_t *tiles, size_t tile_pitch) {
  for (int dx = -1; dx <= 1; dx += 2) {
    CastleMaterial root;
    uint8_t id;
    if (!LookupMaterial(s, stamps, native, cx + dx, cy, &root, &id, tiles, tile_pitch) ||
        !(root.flags & kVoxelTreeRootMaterial)) continue;
    const unsigned root_x = dx < 0 ? 7 : 0;
    const unsigned crown_x = 7 - root_x;
    for (unsigned y = 0; y < 16; ++y) {
      const unsigned root_q = (y / 8) * 2 + (dx < 0);
      const unsigned crown_q = root_q ^ 1;
      const unsigned row = (y & 7) * 8;
      const unsigned ink = root.pixels[root_q][row + root_x];
      if (ink && (root.rear_colors[root_q] & (1u << ink)) &&
          crown->kind[crown_q] == kVoxelFoliage && crown->pixels[crown_q][row + crown_x])
        return true;
    }
  }
  return false;
}

/* A grounded mixed root already contains a complete native foliage patch.
 * Continue that local plant only into the old curved-bark footprint. Keeping
 * a second bark image at the vegetation plane would restore the old depth
 * fight; borrowing a whole shrub crown can instead copy its empty sky. */
static bool BuildForestRootBacking(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaRoomOverride *terrain,const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256],const uint16_t palette[128],
    const uint16_t *tiles,size_t tile_pitch) {
  if(c->profile!=kActionVoxelProfile_FillmoreForest || !c->trunk_cells)return true;
  const int columns=c->width/8;
  for(int cy=c->origin_y/16;cy<(c->origin_y+c->height)/16;++cy)
    for(int cx=c->origin_x/16;cx<(c->origin_x+c->width)/16;++cx) {
      const int bx=cx*16-c->origin_x,by=cy*16-c->origin_y;
      bool curved=false;
      for(unsigned q=0;q<4;++q)
        curved|=c->trunk_cells[(size_t)(by/8+(q>>1))*columns+bx/8+(q&1)]!=0;
      if(!curved || SceneryBackingEdited(terrain,cx,cy))continue;
      CastleMaterial material;uint8_t id;
      if(!LookupMaterial(s,stamps,native,cx,cy,&material,&id,tiles,tile_pitch) ||
          material.collision || (material.flags&(kVoxelTreeRootMaterial|kVoxelRearCopyMaterial))!=
              (kVoxelTreeRootMaterial|kVoxelRearCopyMaterial))continue;
      bool source_valid=true;
      for(unsigned q=0;q<4;++q)
        source_valid&=material.kind[q]==kVoxelForeground && !(material.words[q]&0x2000) &&
            SurfaceStaticWord(s,material.words[q]);
      if(!source_valid)continue;
      const DioramaPixelEdit *edit=terrain?DioramaLayerOrder_PixelEditAt(terrain,0,cx,cy,id):NULL;
      unsigned donor=4;
      for(unsigned q=0;q<4 && donor==4;++q) {
        bool complete=true;
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x) {
          const unsigned px=(q&1)*8+x,py=(q>>1)*8+y,p=material.pixels[q][y*8+x];
          complete&=p && (material.rear_colors[q]&(1u<<p)) &&
              !MaterialTransparent(&material,edit,px,py) &&
              !(edit && (edit->black[py]&(1u<<(15-px))));
        }
        if(complete)donor=q;
      }
      if(donor==4)continue;
      VoxelRearFill fill={.x=cx*16,.y=cy*16,.preserve_existing=true};bool visible=false;
      for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
        const unsigned pixel=c->front[(size_t)(by+y)*c->width+bx+x];
        if((pixel&kVoxelKindMask)!=kVoxelForeground || !(pixel&kVoxelRearCopy) ||
            (pixel&kVoxelHighPriority) ||
            !c->trunk_cells[(size_t)(by/8+y/8)*columns+bx/8+x/8] ||
            MaterialTransparent(&material,edit,x,y) ||
            (edit && (edit->black[y]&(1u<<(15-x)))))continue;
        const unsigned ink=material.pixels[donor][(y&7)*8+(x&7)];
        fill.colors[y*16+x]=0x8000|palette[((material.words[donor]>>10)&7)*16+ink];
        visible=true;
      }
      /* This cosmetic repair cannot reject a larger edited level when its
       * optional sparse budget is exhausted; native tests prove room headroom. */
      if(visible && c->rear_fill_count<kVoxelMaxRearFills && !AppendRearFill(c,&fill))return false;
    }
  return true;
}

static bool AppendGate(ActionVoxelGeometry *c,VoxelGate gate) {
  for(unsigned i=0;i<c->gate_count;++i) {
    VoxelGate *p=&c->gates[i];
    if(p->x==gate.x && p->y+p->height==gate.y && p->width==gate.width &&
        p->high_priority==gate.high_priority && p->to_right==gate.to_right && p->centered==gate.centered) {
      p->height+=gate.height;return true;
    }
  }
  if(c->gate_count==kVoxelMaxGates)return false;
  c->gates[c->gate_count++]=gate;return true;
}
/* The forest walking cap stays flat at native contact. Keep the original
 * rear plant and brown ground artwork intact; repeating the plant into that
 * ground band creates a second line of stems. Restrict the cap treatment to
 * supported full ground, preserving branches, stumps and hazards. */
enum { kForestGroundFoldPixels = 7 };
typedef struct ForestFoldRun {
  int x0,x1,y;
  unsigned first,count;
  bool supported;
} ForestFoldRun;
static unsigned ForestFoldCell(const ActionVoxelGeometry *c,const uint8_t *cells,int x,int y) {
  x-=c->origin_x;y-=c->origin_y;
  if(x<0 || y<0 || x>=c->width || y>=c->height)return 0;
  return cells[(size_t)(y/16)*(c->width/16)+x/16];
}
static bool ForestGroundEdge(const ActionVoxelGeometry *c,const uint8_t *cells,const VoxelEdge *e) {
  if(e->far_rank!=2 || !e->colliding)return false;
  for(int at=0;at<e->length;++at)
    if(!(ForestFoldCell(c,cells,e->x+((e->side&1)?0:at),
                           e->y+((e->side&1)?at:0))&1))return false;
  return true;
}
static bool ForestFoldEndpoint(const ForestFoldRun *runs,unsigned count,int x,int y) {
  unsigned lo=0,hi=count;
  while(lo<hi) {const unsigned mid=lo+(hi-lo)/2;if(runs[mid].y<y)lo=mid+1;else hi=mid;}
  for(;lo<count && runs[lo].y==y;++lo)
    if(runs[lo].supported && x>=runs[lo].x0 && x<=runs[lo].x1)return true;
  return false;
}
static bool BuildForestFoldEdges(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaStampLayerOverride *stamps,const CastleMaterial native[256],
    const uint16_t *tiles,size_t tile_pitch) {
  if(c->profile!=kActionVoxelProfile_FillmoreForest || !c->edge_count)return true;
  const int width=c->width/16,height=c->height/16;
  uint8_t *cells=calloc((size_t)width,height);
  uint16_t *top=malloc((size_t)c->edge_count*sizeof(*top));
  ForestFoldRun *runs=calloc(c->edge_count,sizeof(*runs));
  if(!cells || !top || !runs) {free(cells);free(top);free(runs);return false;}
  const unsigned excluded=kVoxelBranchMaterial|kVoxelSideProfileMaterial|
      kVoxelGrowthMaterial|kVoxelSpikeMaterial|kVoxelRearFillMaterial;
  for(int cy=0;cy<height;++cy)for(int cx=0;cx<width;++cx) {
    CastleMaterial material;uint8_t provenance;
    if(!LookupMaterial(s,stamps,native,cx+c->origin_x/16,cy+c->origin_y/16,
                       &material,&provenance,tiles,tile_pitch) || (material.flags&excluded))continue;
    bool ground=material.collision==15,rear=!material.collision;
    for(unsigned q=0;q<4;++q) {
      /* The folded band is the native-low rear source. Preserve high-source
       * architecture on its established geometry and priority path. */
      ground &= material.kind[q]==kVoxelSolid && !(material.words[q]&0x2000);
      rear &= material.kind[q]==kVoxelRearOnly && !(material.words[q]&0x2000);
    }
    cells[(size_t)cy*width+cx]=(ground?1:0)|(rear?2:0);
  }
  unsigned run_count=0,top_count=0;
  /* Compiler top edges are already ordered by y then x. Propagate across
   * touching grass/dirt/root-adjacent runs, never across a pit or stump. */
  for(unsigned i=0;i<c->edge_count;++i) {
    VoxelEdge *e=&c->edges[i];
    if(e->side!=0 || !ForestGroundEdge(c,cells,e))continue;
    /* Rounded native turf contours remain in the same supporting ground cell. */
    bool supported=false;
    for(int x=e->x;x<e->x+e->length && !supported;++x)
      supported=(ForestFoldCell(c,cells,x,c->origin_y+((e->y-c->origin_y)/16)*16-1)&2)!=0;
    ForestFoldRun *run=run_count?&runs[run_count-1]:NULL;
    if(!run || run->y!=e->y || run->x1!=e->x) {
      run=&runs[run_count++];
      *run=(ForestFoldRun){.x0=e->x,.x1=e->x,.y=e->y,.first=top_count};
    }
    run->x1=e->x+e->length;run->supported|=supported;++run->count;
    top[top_count++]=(uint16_t)i;
  }
  for(unsigned r=0;r<run_count;++r)if(runs[r].supported) {
    for(unsigned n=0;n<runs[r].count;++n) {
      VoxelEdge *edge=&c->edges[top[runs[r].first+n]];
      edge->rear_fold_top=kForestGroundFoldPixels;
      edge->flat_rear_join=1;
    }
  }
  for(unsigned i=0;i<c->edge_count;++i) {
    VoxelEdge *e=&c->edges[i];
    if(!(e->side&1) || !ForestGroundEdge(c,cells,e))continue;
    const int boundary_x=e->x+(e->side==1);
    e->rear_fold_top=ForestFoldEndpoint(runs,run_count,boundary_x,e->y)?kForestGroundFoldPixels:0;
    e->rear_fold_bottom=ForestFoldEndpoint(runs,run_count,boundary_x,e->y+e->length)?kForestGroundFoldPixels:0;
    e->flat_rear_join=e->rear_fold_top || e->rear_fold_bottom;
  }
  free(cells);free(top);free(runs);return true;
}
/* Reviewed forest stumps retain their native silhouette and stepped sides.
 * Only the outer cap uses the clean central native top-art square;
 * an edited/partial profile or transparent sample keeps ordinary sampling. */
static void BuildForestStumpCapSamples(ActionVoxelGeometry *c) {
  if(c->profile!=kActionVoxelProfile_FillmoreForest)return;
  for(unsigned i=0;i<c->patch_count;++i) {
    const VoxelPatch *p=&c->patches[i];
    if(p->kind!=kVoxelPatch_SideProfile || p->width!=32 || p->height!=56)continue;
    const int sx=p->x+9,sy=p->y+7,size=4;
    if(sx<c->origin_x || sy<c->origin_y || sx+size>c->origin_x+c->width ||
        sy+size>c->origin_y+c->height)continue;
    bool opaque=true;
    for(int y=0;y<size;++y)for(int x=0;x<size;++x) {
      const unsigned pixel=c->front[(size_t)(sy+y-c->origin_y)*c->width+sx+x-c->origin_x];
      opaque &= (pixel&kVoxelKindMask)==kVoxelSolid &&
          !!(pixel&kVoxelHighPriority)==p->high_priority;
    }
    if(!opaque)continue;
    for(unsigned j=0;j<c->edge_count;++j) {
      VoxelEdge *e=&c->edges[j];
      if(e->side || e->high_priority!=p->high_priority || e->x<p->x ||
          e->x+e->length>p->x+p->width || e->y<p->y || e->y>=p->y+p->height)continue;
      /* Bark notches expose small horizontal ledges below the cut top. Keep
       * their native edge texture instead of painting them with cut wood. */
      bool outer=true;
      for(int x=e->x;x<e->x+e->length && outer;++x)
        for(int y=p->y;y<e->y;++y)
          if(DepthRank(c,(size_t)(y-c->origin_y)*c->width+x-c->origin_x)==2) {
            outer=false;break;
          }
      if(!outer)continue;
      e->top_sample_x=(short)sx;e->top_sample_y=(short)sy;e->top_sample_size=size;
    }
  }
}
#include "action_voxel_forest_trunks.inc"
#include "action_voxel_portal_backing.inc"
static bool NativeContourEdited(const DioramaRoomOverride *terrain,int x,int y) {
  const DioramaVoxelCell *cell=terrain?DioramaVoxelEdits_Cell(&terrain->voxel,x,y):NULL;
  const unsigned geometry=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back|
      kDioramaVoxelCell_Width|kDioramaVoxelCell_Offset|kDioramaVoxelCell_Faces|
      kDioramaVoxelCell_Shape|kDioramaVoxelCell_FrontEnd|kDioramaVoxelCell_BackEnd|
      kDioramaVoxelCell_Bend|kDioramaVoxelCell_Transform;
  return ActionVoxel_CellOwnsArtwork(cell) && (cell->fields&geometry);
}
static bool SnowRockReturnsToBank(const CastleMaterial *material,const ActionRoomScene *s,
    const DioramaStampLayerOverride *stamps,const CastleMaterial native[256],
    int cx,int cy,const uint16_t *tiles,size_t tile_pitch) {
  if(material->flags&kVoxelRearMountedMaterial)return true;
  if(!material->native_slope)return false;
  /* A slope can be the right edge of the lower rock course. Match the
   * adjacent base art, through at most one intervening rock slope. */
  for(int distance=1;distance<=2;++distance) {
    CastleMaterial left;uint8_t id;
    if(!LookupMaterial(s,stamps,native,cx-distance,cy,&left,&id,tiles,tile_pitch) ||
        !(left.flags&kVoxelSnowRockMaterial))break;
    if(left.flags&kVoxelRearMountedMaterial)return true;
    if(!left.native_slope)break;
  }
  return false;
}
static uint16_t NativeSlopeFootRow(const ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaRoomOverride *terrain,const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256],const CastleMaterial *foot,int cx,int cy,
    const DioramaPixelEdit *edit,const uint16_t *tiles,size_t tile_pitch) {
  if(!foot->native_slope_foot || foot->collision || cy*16<=c->origin_y ||
      NativeContourEdited(terrain,cx,cy) || NativeContourEdited(terrain,cx,cy-1))return 0;
  CastleMaterial above;uint8_t id;
  if(!LookupMaterial(s,stamps,native,cx,cy-1,&above,&id,tiles,tile_pitch) ||
      !above.native_slope || (above.collision!=6 && above.collision!=9))return 0;
  const DioramaPixelEdit *above_edit=terrain?DioramaLayerOrder_PixelEditAt(terrain,0,cx,cy-1,id):NULL;
  uint16_t row=0;
  /* A zero-height native ramp still supports the player: the native probe
   * starts one row above (foot+8)/16, then sets foot=(tileY+1)*16-height.
   * Its exact contact lies on the next source row. Carry only that opaque
   * rock pixel, not the decorative underface or an invented vertical slab. */
  for(unsigned x=0;x<16;++x) {
    const unsigned bit=1u<<(15-x),q=x/8;
    const unsigned above_pixel=above.pixels[q+2][56+(x&7)];
    const unsigned foot_pixel=foot->pixels[q][x&7];
    const bool endpoint=(above.rear_rows[15]&bit)!=0;
    /* Two reviewed snow outlines have a native alpha pixel at contact15.
     * Their first actual ink is the following row, not an invented contact
     * texel. Only this one-row native outline offset may continue the floor;
     * authored erasure/painting must never be mistaken for that source gap. */
    const bool alpha_outline=!endpoint && (above.rear_rows[14]&bit) && !above_pixel &&
        !(above_edit && ((above_edit->transparent[15]|above_edit->black[15])&bit));
    const bool rear=foot->kind[q]==kVoxelRearOnly ||
        (foot->kind[q]==kVoxelForeground && (foot->flags&kVoxelRearMountedMaterial));
    if((!endpoint && !alpha_outline) || !rear ||
        ((above.words[q+2]^foot->words[q])&0x3c00) ||
        !SurfaceStaticWord(s,above.words[q+2]) || !SurfaceStaticWord(s,foot->words[q]) ||
        !foot_pixel || (!above_pixel && above_edit && (above_edit->black[15]&bit)) ||
        ((foot->rear_colors[q]|foot->gate_colors[q]|foot->face_colors[q])&(1u<<foot_pixel)) ||
        MaterialTransparent(&above,above_edit,x,15) || MaterialTransparent(foot,edit,x,0))continue;
    row|=bit;
  }
  return row;
}
/* A mountain ramp is one rock ledge, not a new rectangular wedge at every
 * metatile boundary. Continue its exact native contact contour one tile down
 * through reviewed rear rock. The resulting body is16px thick; everything
 * below it remains on the back plane. This never changes native collision or
 * erases the original rear artwork, and does not apply to snow endpoints. */
static bool NativeMountainSlopeSupport(const ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaRoomOverride *terrain,const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256],const CastleMaterial *support,int cx,int cy,
    const DioramaPixelEdit *edit,const uint16_t *tiles,size_t tile_pitch,uint16_t rows[16]) {
  if(c->profile!=kActionVoxelProfile_AitosMountains || s->map!=1 ||
      (!support->native_slope_foot && !support->native_slope_support) || support->collision ||
      cy*16<=c->origin_y || NativeContourEdited(terrain,cx,cy) ||
      NativeContourEdited(terrain,cx,cy-1))return false;
  CastleMaterial above;uint8_t id;
  if(!LookupMaterial(s,stamps,native,cx,cy-1,&above,&id,tiles,tile_pitch) ||
      !above.native_slope || (above.collision!=6 && above.collision!=9))return false;
  const DioramaPixelEdit *above_edit=terrain?DioramaLayerOrder_PixelEditAt(terrain,0,cx,cy-1,id):NULL;
  bool any=false;
  for(unsigned x=0;x<16;++x) {
    const unsigned bit=1u<<(15-x),aq=2+x/8,aw=above.words[aq];
    bool intact=SurfaceStaticWord(s,aw);
    for(unsigned y=0;y<16 && intact;++y)
      intact=!(above_edit && ((above_edit->transparent[y]|above_edit->black[y])&bit));
    if(!intact)continue;
    for(unsigned y=0;y<16 && (above.rear_rows[y]&bit);++y) {
      const unsigned q=(y/8)*2+x/8,p=support->pixels[q][(y&7)*8+(x&7)];
      /* Reviewed snow-to-rock underfaces change palettes inside the same
       * continuous ledge. Their exact material key owns that color change;
       * native sprite priority must still agree across the attachment. */
      if(support->kind[q]!=kVoxelRearOnly || ((aw^support->words[q])&0x2000) ||
          !SurfaceStaticWord(s,support->words[q]) || !p ||
          ((support->rear_colors[q]|support->gate_colors[q]|support->face_colors[q])&(1u<<p)) ||
          MaterialTransparent(support,edit,x,y) ||
          (edit && (edit->black[y]&bit)))break;
      rows[y]|=bit;any=true;
    }
  }
  return any;
}
/* Retain a short native front lip, then return the bounded ramp body to its
 * original wall. Per2px contact columns follow the collision contour; using
 * one curve per metatile height would recreate the staircase at its joins. */
static bool BuildMountainRampReturns(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaRoomOverride *terrain,const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256],const uint16_t *tiles,size_t tile_pitch) {
  if(c->profile!=kActionVoxelProfile_AitosMountains || s->map!=1)return true;
  const int x0=c->origin_x/16,y0=c->origin_y/16;
  for(int cy=y0;cy<y0+c->height/16-1;++cy)for(int cx=x0;cx<x0+c->width/16;++cx) {
    CastleMaterial ramp,support;uint8_t id;
    if(!LookupMaterial(s,stamps,native,cx,cy,&ramp,&id,tiles,tile_pitch) ||
        !ramp.native_slope || (ramp.collision!=6 && ramp.collision!=9) ||
        !LookupMaterial(s,stamps,native,cx,cy+1,&support,&id,tiles,tile_pitch) ||
        (!support.native_slope_foot && !support.native_slope_support) || support.collision ||
        NativeContourEdited(terrain,cx,cy) || NativeContourEdited(terrain,cx,cy+1))continue;
    VoxelTrunkCurve curve={.contact_x=(short)(cx*16),.contact_count=8,.linear=1};
    bool complete=true;
    for(unsigned x=0;x<16 && complete;++x) {
      unsigned contact=0;while(contact<16 && (ramp.rear_rows[contact]&(1u<<(15-x))))++contact;
      const short world_contact=(short)(cy*16+contact);
      if(!(x&1))curve.contact[x/2]=world_contact;
      else if(curve.contact[x/2]!=world_contact){complete=false;break;}
      /* A missing/edited source pixel breaks automatic ownership, rather
       * than stretching its neighbours across that authored opening. */
      for(unsigned y=0;y<16;++y) {
        const size_t at=(size_t)(world_contact+(int)y-c->origin_y)*c->width+cx*16+x-c->origin_x;
        if((c->front[at]&kVoxelKindMask)!=kVoxelSolid){complete=false;break;}
      }
    }
    if(!complete)continue;
    curve.top_y=curve.contact[0]+4;curve.base_y=curve.contact[0]+16;
    if(c->column_curve_count==kVoxelMaxCastleColumns)return false;
    const unsigned owner=++c->column_curve_count;c->column_curves[owner-1]=curve;
    if(!c->column_cells) {
      c->column_cells=calloc((size_t)c->width/8,c->height/8);
      if(!c->column_cells)return false;
    }
    for(int ty=cy*2;ty<cy*2+4;++ty)for(int tx=cx*2;tx<cx*2+2;++tx) {
      bool occupied=false;
      for(unsigned y=0;y<8 && !occupied;++y)for(unsigned x=0;x<8;++x) {
        const int wx=tx*8+x,wy=ty*8+y,contact=curve.contact[(wx-cx*16)/2];
        if(wy>=contact && wy<contact+16){occupied=true;break;}
      }
      if(!occupied)continue;
      const size_t at=(size_t)(ty-c->origin_y/8)*(c->width/8)+tx-c->origin_x/8;
      if(c->column_cells[at])return false;
      c->column_cells[at]=(uint8_t)owner;
    }
  }
  return true;
}
#include "action_voxel_marsh_fold.inc"
#include "action_voxel_castle_columns.inc"
#include "action_voxel_desert_columns.inc"
#include "action_voxel_authored.inc"
#include "action_voxel_surfaces.inc"
#include "action_voxel_jungle_water.inc"
#include "action_voxel_castle_matte.inc"
#include "action_voxel_jungle_terminal.inc"
static bool BuildGeometry(ActionVoxelGeometry *c, const ActionRoomScene *s,
                          const DioramaRoomOverride *terrain, const uint16_t *tiles, size_t tile_pitch) {
  if (!ActionVoxel_Room(s->group, s->map)) return true;
  c->profile = ActionVoxel_ProfileForRoom(s->group, s->map);
  if (terrain) c->edits = terrain->voxel;
  if (!ActionRoomScene_HasBackground(s, 1) || !s->have_palette || !s->have_character_bank[0])
    return false;
  const int native_w = ActionRoomScene_TileWidth(s, 1), native_h = ActionRoomScene_TileHeight(s, 1);
  const DioramaStampLayerOverride *stamps = terrain ? &terrain->stamp_layers[0] : NULL;
  int x0 = 0, y0 = 0, x1 = native_w * 8, y1 = native_h * 8;
  if (stamps && stamps->set_bounds) {
    x0 = stamps->x0 * 16;
    y0 = stamps->y0 * 16;
    x1 = stamps->x1 * 16;
    y1 = stamps->y1 * 16;
  }
  if (!ActionVoxel_ValidBounds(x0, y0, x1, y1)) return false;
  c->origin_x = x0;
  c->origin_y = y0;
  c->width = x1 - x0;
  c->height = y1 - y0;
  c->front = calloc((size_t)c->width, c->height);
  if (!c->front) return false;
  uint16_t palette[128];
  for (unsigned i = 0; i < 128; ++i)
    palette[i] = s->palette[i * 2] | (s->palette[i * 2 + 1] << 8);
  const uint16_t jungle_water_main = JungleWaterMainColor(s, terrain, palette);
  CastleMaterial native[256];
  unsigned backing_material = 256;
  bool contact_materials=false,rear_materials=false;
  for (unsigned m = 0; m < 256; ++m) {
    uint16_t words[4];
    const unsigned collision = NativeWords(s,m,words);
    Material(s, 0, words, collision, &native[m]);
    if (native[m].flags & kCastleBackingMaterial) backing_material = m;
    contact_materials|=(native[m].flags&kVoxelContactSceneryMaterial)!=0;
    rear_materials|=c->profile==kActionVoxelProfile_Castle &&
        (native[m].flags&kVoxelRearMountedMaterial)!=0;
  }
  /* Unknown c->backing art must not silently sample whatever now occupies $4C. */
  if (backing_material == 256) {
    /* Unsupported source art uses ordinary Diorama. Never fabricate a wall
     * from an unrelated character or prevent the host from loading a room. */
    free(c->front);
    c->front = NULL;
    c->width = c->height = 0;
    return true;
  }
  /* Contact-mounted objects and rear relief keep their complete native
   * silhouette on a shallow plane. Only mapped reviewed artwork enables
   * either attachment; other trim keeps its front plane and thickness. */
  for(int cy=y0/16;(contact_materials || rear_materials || (stamps && stamps->count)) &&
      cy<y1/16;++cy)
    for(int cx=x0/16;cx<x1/16;++cx) {
      CastleMaterial material;uint8_t provenance;
      if(LookupMaterial(s,stamps,native,cx,cy,&material,&provenance,tiles,tile_pitch)) {
        c->contact_scenery|=(material.flags&kVoxelContactSceneryMaterial)!=0;
        c->material_rear_mounted|=c->profile==kActionVoxelProfile_Castle &&
            (material.flags&kVoxelRearMountedMaterial)!=0;
      }
    }
  c->mixed_scenery|=c->contact_scenery;
  if(ActionVoxel_SceneryRecess(c)>0) {
    c->front_recess=calloc((size_t)(c->width/8),c->height/8);
    if(!c->front_recess)return false;
  }
  c->semantic_cells=calloc((size_t)c->width/16,c->height/16);
  if(!c->semantic_cells)return false;
  for (int cy = y0 / 16; cy < y1 / 16; ++cy)
    for (int cx = x0 / 16; cx < x1 / 16; ++cx) {
      CastleMaterial material, below;
      uint8_t metatile, below_id;
      if (!LookupMaterial(s, stamps, native, cx, cy, &material, &metatile, tiles, tile_pitch)) continue;
      /* The same opaque black art fills both islands and shoreline rock. Only
       * an uninterrupted column below reviewed earth belongs to the solid
       * island. A scenic rock interior stays with its shallow silhouette. */
      if (material.flags & kVoxelInfillMaterial) {
        CastleMaterial above; uint8_t above_id;
        for (int y=cy-1; y>=y0/16; --y) {
          if (!LookupMaterial(s,stamps,native,cx,y,&above,&above_id,tiles,tile_pitch)) break;
          if (above.flags & kVoxelInfillMaterial) continue;
          if (above.flags & kVoxelTerrainBodyMaterial)
            for (unsigned q=0; q<4; ++q) material.kind[q]=kVoxelSolid;
          break;
        }
      }
      bool foliage = false;
      if (c->profile == kActionVoxelProfile_FillmoreForest && !material.collision)
        for (unsigned q = 0; q < 4; ++q)
          foliage |= material.kind[q] == kVoxelFoliage;
      const bool have_below = (c->profile == kActionVoxelProfile_Castle || foliage) &&
          LookupMaterial(s, stamps, native, cx, cy + 1, &below, &below_id, tiles, tile_pitch);
      const bool cap = c->profile == kActionVoxelProfile_Castle && have_below &&
                       PlatformCap(&material, &below);
      /* A shrub crown is also reused above solid thorn caps. Follow its reviewed
       * border base when present so the whole plant stays on one plane;
       * identical artwork elsewhere retains its own placement. */
      const bool growth_crown = foliage && have_below && (below.flags & kVoxelGrowthMaterial);
      const bool rear_foliage = foliage && have_below &&
          ((!below.collision && below.kind[0] == kVoxelRearOnly && below.kind[1] == kVoxelRearOnly) ||
           (growth_crown && ForestCrownJoinsRoot(s, stamps, native, &material, cx, cy, tiles, tile_pitch)));
      /* A raised bank's original shrub footprint continues through its crown.
       * Crown artwork is also used on foreground thorn platforms, so inherit
       * only a reviewed plain foliage base's backing, never its geometry. */
      bool backed_foliage=foliage && have_below && !material.flags && !below.collision &&
          below.flags==kVoxelRearCopyMaterial;
      for(unsigned q=0;backed_foliage && q<4;++q)
        backed_foliage=material.kind[q]==kVoxelFoliage && below.kind[q]==kVoxelFoliage;
      if(backed_foliage)material.flags|=kVoxelRearCopyMaterial;
      const DioramaPixelEdit *edit =
          terrain ? DioramaLayerOrder_PixelEditAt(terrain, 0, cx, cy, metatile) : NULL;
      const uint16_t slope_foot=NativeSlopeFootRow(c,s,terrain,stamps,native,&material,cx,cy,edit,tiles,tile_pitch);
      uint16_t slope_support[16]={0};
      const bool have_slope_support=NativeMountainSlopeSupport(c,s,terrain,stamps,native,
          &material,cx,cy,edit,tiles,tile_pitch,slope_support);
      const bool snow_rock=(material.flags&kVoxelSnowRockMaterial) &&
          !NativeContourEdited(terrain,cx,cy);
      const bool snow_rock_base=snow_rock &&
          SnowRockReturnsToBank(&material,s,stamps,native,cx,cy,tiles,tile_pitch);
      if ((material.flags & kVoxelSoftSurfaceMaterial) || slope_foot || have_slope_support) {
        if (!c->soft_surfaces) {
          c->soft_surfaces=calloc((size_t)c->width/16,c->height/16);
          if (!c->soft_surfaces) return false;
        }
        c->soft_surfaces[(cy-y0/16)*(c->width/16)+cx-x0/16]=1;
      }
      if ((material.flags & (kVoxelWaterSurfaceMaterial | kVoxelOpaqueSurfaceMaterial)) &&
          (material.words[2]&material.words[3]&0x2000) && MaterialVisible(&material,edit)) {
        /* Reviewed water and lava share a submerged high-priority band.
         * Sample below pilings, lips and bubbles so extending the surface
         * cannot stretch those details across the walking depth. A
         * vertically reflected stamp has no submerged band in this position
         * and retains its native face without an invented surface. */
        VoxelWaterSurface *last=c->water_surface_count?&c->water_surfaces[c->water_surface_count-1]:NULL;
        const bool jungle=c->profile==kActionVoxelProfile_MarahnaJungle;
        const unsigned sample_y=material.flags&kVoxelFullSurfaceTextureMaterial?0:jungle?13:12;
        const unsigned sample_height=material.flags&kVoxelFullSurfaceTextureMaterial?16:jungle?3:4;
        const uint16_t water_sub = jungle && sample_y==13 && sample_height==3
            ? JungleWaterSubColor(s, terrain, &material, edit, palette, cx, cy, jungle_water_main) : 0;
        const uint16_t water_main = water_sub ? jungle_water_main : 0;
        if (last && !last->background && last->y==cy*16 && last->x+last->width==cx*16 &&
            last->opaque==!!(material.flags & kVoxelOpaqueSurfaceMaterial) &&
            last->sample_y==sample_y && last->sample_height==sample_height &&
            last->uniform_color[0]==water_sub && last->uniform_color[1]==water_main)
          last->width+=16;
        else {
          if (c->water_surface_count==kVoxelMaxWaterSurfaces) return false;
          c->water_surfaces[c->water_surface_count++]=(VoxelWaterSurface){
              .x=cx*16,.y=cy*16,.width=16,.high_priority=true,.background=0,
              .sample_y=sample_y,.sample_height=sample_height,
              .opaque=!!(material.flags & kVoxelOpaqueSurfaceMaterial),
              .uniform_color={water_sub,water_main}};
        }
      }
      for (unsigned y = 0; y < 16; ++y) {
        const uint16_t mask = (material.flags&kVoxelRearMatteMaterial) ? 0 :
            material.cutouts[y] & ~(edit ? edit->black[y] : 0);
        if (!mask) continue;
        if (!c->cutouts) {
          c->cutouts = calloc((size_t)c->width / 8, c->height);
          if (!c->cutouts) return false;
        }
        const size_t at = (size_t)(cy * 16 + y - y0) * (c->width / 8) + cx * 2 - x0 / 8;
        c->cutouts[at] = mask >> 8;
        c->cutouts[at + 1] = mask;
      }
      if((material.flags & kVoxelRecessedMaterial) && !material.collision && MaterialVisible(&material,edit)) {
        const int width=(c->width+15)/16,height=(c->height+15)/16;
        if(!c->rear_relief) {
          c->rear_relief=calloc((size_t)width*height,1);
          if(!c->rear_relief)return false;
        }
        c->rear_relief[(cy-y0/16)*width+cx-x0/16]=255;
      }
      if ((material.flags & kVoxelRearFillMaterial) && !rear_foliage &&
          !BuildRearFill(c,s,terrain,stamps,native,&material,cx,cy,edit,palette,tiles,tile_pitch))
        return false;
      if(!BuildPostShadowRepair(c,s,terrain,stamps,native,&material,cx,cy,edit,palette,tiles,tile_pitch))
        return false;
      if ((material.flags & kVoxelOpaqueRearMaterial) &&
          !BuildOpaqueRearBacking(c,&material,cx,cy,edit,palette))return false;
      for (unsigned quadrant = 0; quadrant < 4; ++quadrant) {
        const int tx = cx * 2 + (quadrant & 1), ty = cy * 2 + (quadrant >> 1);
        const unsigned w = material.words[quadrant];
        const bool solid = (material.collision >> quadrant) & 1;
        unsigned kind = cap ? kVoxelSolid : material.kind[quadrant];
        if (rear_foliage && kind == kVoxelFoliage) kind = kVoxelRearOnly;
        if (!kind) continue;
        const bool high_priority = (w & 0x2000) != 0;
        /* Door art contains separate native foreground and rear strips.
         * Keep the high strip ahead of actors and let low trim sit against
         * the wall. Native colliding masonry and grille ink are independent. */
        const bool portal_jamb=material.portal_jamb && !solid &&
            (kind==kVoxelSolid || kind==kVoxelForeground || kind==kVoxelThin);
        if(portal_jamb)
          kind=high_priority?kVoxelForeground:c->front_recess?kVoxelThin:kVoxelRearOnly;
        /* Reviewed raised crenellations need matching rear trim. Portals
         * with authored rear jambs do not request a second copy. */
        const bool rear_trim = high_priority && !portal_jamb && (material.flags & kCastleRearMaterial);
        if (rear_trim && !(tx & 1) && !(ty & 1)) {
          VoxelRearTrim *last = c->rear_trim_count ? &c->rear_trims[c->rear_trim_count - 1] : NULL;
          if (last && last->y == ty * 8 && last->x + last->width == tx * 8)
            last->width += 16;
          else {
            if (c->rear_trim_count == kVoxelMaxRearTrims) return false;
            c->rear_trims[c->rear_trim_count++] = (VoxelRearTrim){tx * 8, ty * 8, 16};
          }
        }
        /* Reviewed continuous terrain can cross a noncolliding quadrant at
         * a slope's silhouette. Its physical cap must stay connected; native
         * collision and foreground priority remain separate flags. */
        if (high_priority && kind == kVoxelSolid && !solid &&
            !(material.flags & kVoxelTerrainBodyMaterial)) kind = kVoxelForeground;
        if(snow_rock && !high_priority && kind==kVoxelForeground)kind=kVoxelSolid;
        bool colored = false;
        for (int y = 0; y < 8; ++y)
          for (int x = 0; x < 8; ++x) {
            const unsigned mx=(tx&1)*8+x,my=(ty&1)*8+y,bit=1u<<(15-mx);
            /* Retained rear matte is a source/backing pixel, never evidence
             * that a carved foreground quadrant has a physical contour. */
            if (MaterialTransparent(&material,edit,mx,my) ||
                ((material.cutouts[my]&bit) && !(edit && (edit->black[my]&bit))))continue;
            const unsigned p = material.pixels[quadrant][y * 8 + x];
            colored |= p && (c->profile != kActionVoxelProfile_Castle ||
                            palette[((w >> 10) & 7) * 16 + p]);
          }
        if (!colored) continue;
        if(snow_rock && !high_priority && kind==kVoxelSolid) {
          /* Crown and base use the same actor-clear front and share the
           * existing rear endpoint. This changes no native collision bits. */
          if(!c->column_cells) {
            c->column_cells=calloc((size_t)c->width/8,c->height/8);
            if(!c->column_cells)return false;
          }
          /* A snow-rock base that meets a separate bank returns to the
           * shallow relief plane by the bottom of its native metatile. */
          const short top=snow_rock_base?cy*16+8:0, bottom=snow_rock_base?(cy+1)*16:0;
          unsigned owner=0;
          while(owner<c->column_curve_count &&
              (c->column_curves[owner].top_y!=top || c->column_curves[owner].base_y!=bottom))++owner;
          if(owner==c->column_curve_count) {
            if(owner==kVoxelMaxCastleColumns)return false;
            c->column_curves[c->column_curve_count++]=(VoxelTrunkCurve){.top_y=top,.base_y=bottom};
          }
          c->column_cells[(size_t)(ty-y0/8)*(c->width/8)+tx-x0/8]=(uint8_t)(owner+1);
        }
        bool foot_quadrant=quadrant<2 && (slope_foot&(quadrant?0x00ff:0xff00));
        for(unsigned y=0;y<8 && !foot_quadrant;++y)
          foot_quadrant=!!(slope_support[(quadrant/2)*8+y]&((quadrant&1)?0x00ff:0xff00));
        const unsigned role=SemanticMaterialKind(&material,foot_quadrant?kVoxelSolid:kind);
        unsigned char *semantic=&c->semantic_cells[(size_t)(cy-y0/16)*(c->width/16)+cx-x0/16];
        if(role>*semantic)*semantic=role;
        if(c->front_recess && kind!=kVoxelRearOnly)
          c->front_recess[(ty-y0/8)*(c->width/8)+tx-x0/8]=1;
        if(c->profile==kActionVoxelProfile_FillmoreForest &&
            (kind==kVoxelThin || kind==kVoxelForeground || (material.flags & kVoxelBranchMaterial) ||
             (kind==kVoxelFoliage && !growth_crown && !(material.flags & kVoxelGrowthMaterial)))) {
          c->front_recess[(ty-y0/8)*(c->width/8)+tx-x0/8]=2;
        }
        const bool priority_scenery=(material.flags&kVoxelNativePrioritySceneryMaterial)!=0;
        if(priority_scenery && solid && kind==kVoxelSolid) {
          if(!c->native_priority_decks) {
            c->native_priority_decks=calloc((size_t)c->width/16,c->height/16);
            if(!c->native_priority_decks)return false;
          }
          c->native_priority_decks[(size_t)(cy-y0/16)*(c->width/16)+cx-x0/16]|=1u<<quadrant;
        }
        /* Noncolliding front posts are scenery outside the walking lane.
         * Keep their native front face/priority, but do not give them the
         * quarter-deck thickness used by rocks and rear-mounted supports. */
        if((priority_scenery || portal_jamb) && high_priority && !solid && kind==kVoxelForeground) {
          if(!c->thin_trim_cells) {
            c->thin_trim_cells=calloc((size_t)c->width/8,c->height/8);
            if(!c->thin_trim_cells)return false;
          }
          c->thin_trim_cells[(ty-y0/8)*(c->width/8)+tx-x0/8]=1;
        }
        if((ActionVoxel_RearMountedScenery(c) ||
            (material.flags&(kVoxelRearMountedMaterial|kVoxelContactSceneryMaterial)) ||
            ((priority_scenery || portal_jamb) && !high_priority)) &&
            (kind==kVoxelForeground ||
             (kind==kVoxelThin && ((material.flags&kVoxelRearMountedMaterial) || portal_jamb)))) {
          if(!c->front_recess)return false;
          c->front_recess[(ty-y0/8)*(c->width/8)+tx-x0/8]=2;
        }
        /* Whole reviewed rear columns get a restrained intermediate plane.
         * Keep the original rear endpoint, collision and native priority;
         * front-facing art, sides and editor defaults share this cached tag. */
        if((material.flags&kVoxelRaisedSceneryMaterial) &&
            (ActionVoxel_RearMountedScenery(c) || (material.flags&kVoxelRearMountedMaterial)) &&
            (kind==kVoxelForeground || kind==kVoxelThin) && !solid && !high_priority) {
          if(!c->raised_scenery_cells) {
            c->raised_scenery_cells=calloc((size_t)c->width/8,c->height/8);
            if(!c->raised_scenery_cells)return false;
          }
          c->raised_scenery_cells[(ty-y0/8)*(c->width/8)+tx-x0/8]=1;
          c->mixed_scenery=true;
        }
        c->mixed_scenery |= (material.flags&kVoxelRearMountedMaterial)!=0 || priority_scenery || portal_jamb;
        if(!snow_rock && !BuildPixelRecess(c,&material,edit,quadrant,tx,ty,slope_foot))return false;
        if (material.flags & (kVoxelGrowthMaterial | kVoxelBranchMaterial | kVoxelSideProfileMaterial |
                              kVoxelSpikeMaterial | kVoxelRearGrassMaterial)) {
          const unsigned patch_kind = material.flags & kVoxelRearGrassMaterial ? kVoxelPatch_RearGrass
              : material.flags & kVoxelSpikeMaterial ? kVoxelPatch_Spikes
              : material.flags & kVoxelBranchMaterial ? kVoxelPatch_BranchRear
              : material.flags & kVoxelSideProfileMaterial ? kVoxelPatch_SideProfile : kVoxelPatch_Thorns;
          unsigned i = 0;
          for (; i < c->patch_count; ++i) {
            VoxelPatch *g = &c->patches[i];
            if (g->y == ty * 8 && g->x + g->width == tx * 8 &&
                g->high_priority == high_priority && g->kind == patch_kind) { g->width += 8; break; }
          }
          if (i == c->patch_count) {
            if (i == kVoxelMaxPatches) return false;
            c->patches[c->patch_count++] = (VoxelPatch){.x=tx*8,.y=ty*8,.width=8,.height=8,
                .high_priority=high_priority,.kind=patch_kind};
          }
        }
        if (kind == kVoxelGate) {
          const unsigned to_right = !!(w & 0x4000);
          if(!AppendGate(c,(VoxelGate){.x=tx*8,.y=ty*8,.height=8,
              .high_priority=high_priority,.to_right=to_right}))return false;
        } else if(material.gate_colors[quadrant]) {
          /* Native blue grille colours occupy only part of a gold jamb.
           * Compile their source width; stretching the whole character would
           * turn the openable doorway into a gold wall. No live RGB matching. */
          int left=8,right=0,top=8,bottom=0;
          for(int y=0;y<8;++y)for(int x=0;x<8;++x)
            if((material.gate_colors[quadrant]&(1u<<material.pixels[quadrant][y*8+x])) &&
                !MaterialTransparent(&material,edit,(tx&1)*8+x,(ty&1)*8+y)) {
              if(x<left)left=x;
              if(x+1>right)right=x+1;
              if(y<top)top=y;
              if(y+1>bottom)bottom=y+1;
            }
          if(right>left && !AppendGate(c,(VoxelGate){.x=tx*8+left,.y=ty*8+top,.width=right-left,.height=bottom-top,
              .high_priority=high_priority,.centered=true}))return false;
        }
        const bool silhouette = kind == kVoxelThin || kind == kVoxelGate;
        for (int y = 0; y < 8; ++y)
          for (int x = 0; x < 8; ++x) {
            const unsigned p = material.pixels[quadrant][y * 8 + x];
            if (MaterialTransparent(&material, edit, (tx & 1) * 8 + x, (ty & 1) * 8 + y)) continue;
            /* Reviewed mixed root art contains both bark and border plants.
             * Split by authored palette-index membership, never live RGB:
             * native fades/lighting must not change the geometry. Its rear
             * backing keeps the bush continuous behind the forward trunk. */
            const int mx=(tx&1)*8+x,my=(ty&1)*8+y;
            const bool rear_matte=(material.flags&kVoxelRearMatteMaterial) &&
                (material.cutouts[my]&(1u<<(15-mx))) && !(edit && (edit->black[my]&(1u<<(15-mx))));
            const bool foot_pixel=(my==0 && (slope_foot&(1u<<(15-mx)))) ||
                (slope_support[my]&(1u<<(15-mx)));
            const unsigned pixel_kind = foot_pixel ? kVoxelSolid : rear_matte || (material.rear_rows[my]&(1u<<(15-mx))) ||
                (material.rear_colors[quadrant] & (1u << p))
                ? kVoxelRearOnly : material.gate_colors[quadrant] & (1u << p) ? kVoxelGate
                : !(material.flags&kVoxelPixelRecessMaterial) &&
                    (material.face_colors[quadrant] & (1u << p)) ? kVoxelFoliage : kind;
            c->front[(ty * 8 + y - y0) * c->width + tx * 8 + x - x0] =
                (kind == kVoxelFlatParapet ||
                 (p && (!silhouette || palette[((w >> 10) & 7) * 16 + p])))
                    ? (pixel_kind | (high_priority ? kVoxelHighPriority : 0) |
                       (rear_trim ? kVoxelRearTrim : 0) | (solid ? kVoxelColliding : 0) |
                       (((material.flags & kVoxelRearCopyMaterial) && !portal_jamb) ? kVoxelRearCopy : 0) |
                       (((material.flags & kVoxelTexturedTopMaterial) || foot_pixel) ? kVoxelTexturedTop : 0))
                    : 0;
            if ((material.flags & kVoxelBranchMaterial) && p && pixel_kind != kVoxelRearOnly) {
              if (!c->branch_pixels) {
                c->branch_pixels = calloc(((size_t)c->width * c->height + 7) / 8, 1);
                if (!c->branch_pixels) return false;
              }
              const size_t at = (size_t)(ty * 8 + y - y0) * c->width + tx * 8 + x - x0;
              c->branch_pixels[at >> 3] |= 1u << (at & 7);
            }
          }
      }
    }
  /* Filtering can sample the last opaque texel from an adjacent empty cell.
   * Carry that empty footprint with its tree instead of leaving a detached
   * outline on the front plane. Occupied decks and hazards never move.
   * The temporary tag 3 prevents this one-cell halo from propagating again. */
  if(c->front_recess) {
    const int width=c->width/8,height=c->height/8;
    for(int y=0;y<height;++y)for(int x=0;x<width;++x)if(c->front_recess[y*width+x]==2)
      for(int yy=y-1;yy<=y+1;++yy)for(int xx=x-1;xx<=x+1;++xx)
        if(xx>=0 && yy>=0 && xx<width && yy<height && !c->front_recess[yy*width+xx]) {
          c->front_recess[yy*width+xx]=3;
          if(c->raised_scenery_cells)
            c->raised_scenery_cells[yy*width+xx]=c->raised_scenery_cells[y*width+x];
        }
    for(int i=0;i<width*height;++i)c->front_recess[i]=c->front_recess[i]>=2;
  }
  /* Two staggered rows of recognized native rear masonry. Colour math is
   * updated with the captured frame, so the ivy room's native cycle survives. */
  for (int y = 0; y < 32; ++y)
    for (int x = 0; x < 16; ++x) {
      const int xx = (x + (y >= 16 ? 8 : 0)) & 15, yy = y & 15;
      const unsigned char *p =
          s->bg[0].metatiles + backing_material * 8 + ((yy / 8) * 2 + xx / 8) * 2;
      const unsigned w = ActionBg_ComposeTilemapWord(
          p[1] | (p[0] << 8), kActionRoomSceneTileWordMask, ActionRoomScene_BgAttributes(s, 1));
      c->backing_color[y * 16 + x] = palette[((w >> 10) & 7) * 16 + Pixel(s, w, xx & 7, yy & 7)];
    }
  c->active_group = s->group;
  c->active_room = s->map;
  BuildTerminalPostBackings(c,s,terrain,stamps,native,palette,tiles,tile_pitch);
  /* Adjacent reviewed grille strips share one continuous side panel. */
  for(unsigned i=0;i<c->gate_count;++i)for(unsigned j=i+1;j<c->gate_count;) {
    VoxelGate *a=&c->gates[i],*b=&c->gates[j];
    if(a->centered && b->centered && a->y==b->y && a->height==b->height &&
        a->high_priority==b->high_priority && a->x+a->width==b->x) {
      a->width+=b->width;memmove(b,b+1,(c->gate_count-j-1)*sizeof(*b));--c->gate_count;
    } else ++j;
  }
  /* Join complete profile faces; a thorn side uses the whole native profile,
   * not an independently repeated strip for each character row. */
  for (unsigned i = 0; i < c->patch_count; ++i)
    for (unsigned j = 0; j < c->patch_count;) {
      VoxelPatch *a = &c->patches[i], *b = &c->patches[j];
      if (j > i && a->x == b->x && a->width == b->width && a->y + a->height == b->y &&
          a->high_priority == b->high_priority && a->kind == b->kind) {
        a->height += b->height;
        memmove(b,b+1,(c->patch_count-j-1)*sizeof(*b));
        --c->patch_count; j = i + 1;
      } else ++j;
    }
  /* Rear-mounted architecture leaves the front of a neighbouring full-depth
   * solid exposed. Rank it below that solid even when their backs coincide;
   * ResolveEdgeSpan supplies the corresponding front interval. */
  const bool rear_column=ActionVoxel_RearMountedScenery(c);
  const float depths[] = {0,rear_column ? ActionVoxel_SceneryRecess(c) : ActionVoxel_TrimBackDepth(c),
      ActionVoxel_Depth(c),ActionVoxel_BranchBackDepth(c)};
  for (unsigned i = 0; i < c->patch_count; ++i)
    c->high_deck_count += c->patches[i].high_priority && c->patches[i].kind == kVoxelPatch_BranchRear;
  /* Native silhouette runs are merged once, with bounded storage. */
  if(!BuildForestTrunkCurves(c,s,stamps,native,tiles,tile_pitch))return false;
  if(!BuildForestRootBacking(c,s,terrain,stamps,native,palette,tiles,tile_pitch))return false;
  if(!BuildCastleColumns(c,s,stamps,native,tiles,tile_pitch))return false;
  if(!BuildDesertGroundedColumns(c,s,stamps,native,tiles,tile_pitch))return false;
  if(!BuildMountainRampReturns(c,s,terrain,stamps,native,tiles,tile_pitch))return false;
  if(c->column_cells)for(int cy=0;cy<c->height/16;++cy)for(int cx=0;cx<c->width/16;++cx) {
    bool column=false,ramp=false;
    for(unsigned q=0;q<4;++q) {
      const unsigned owner=c->column_cells[(size_t)(cy*2+(q>>1))*(c->width/8)+cx*2+(q&1)];
      if(owner) {
        ramp|=c->column_curves[owner-1].contact_count!=0;
        column|=c->column_curves[owner-1].contact_count==0;
      }
    }
    if(column)c->semantic_cells[(size_t)cy*(c->width/16)+cx]=kVoxelSemantic_Column;
    else if(ramp)c->semantic_cells[(size_t)cy*(c->width/16)+cx]=kVoxelSemantic_Platform;
  }
  for (unsigned side = 0; side < 4; ++side) {
    const int rows = (side & 1) ? c->width : c->height, cols = (side & 1) ? c->height : c->width;
    for (int row = 0; row < rows; ++row)
      for (int col = 0; col < cols;) {
        const int x = (side & 1) ? row : col, y = (side & 1) ? col : row;
        const uint64_t boundary = Boundary(c, x, y, side, depths);
        if (!boundary) {
          ++col;
          continue;
        }
        int end = col + 1;
        while (end < cols &&
               Boundary(c, (side & 1) ? row : end, (side & 1) ? end : row, side, depths) == boundary)
          ++end;
        if (c->edge_count == kVoxelMaxEdges) {
          c->ready = false;
          return false;
        }
        c->edges[c->edge_count] = (VoxelEdge){.x = x + x0,
                                               .y = y + y0,
                                               .length = end - col,
                                               .side = (unsigned char)side,
                                               .near_rank = (boundary >> 2) & 3,
                                               .far_rank = boundary & 3,
                                               .high_priority = (boundary >> 4) & 1,
                                               .rear_trim = (boundary >> 5) & 1,
                                               .colliding = (boundary >> 6) & 1,
                                               .textured_top = (boundary >> 7) & 1,
                                               .soft_surface = (boundary >> 8) & 1,
                                               .rear_mounted = (boundary >> 9) & 1,
                                               .neighbor_rear_mounted = (boundary >> 10) & 1};
        const VoxelEdge *e = &c->edges[c->edge_count];
        c->edges[c->edge_count].trunk_curve=(boundary>>11)&255;
        c->edges[c->edge_count].column_curve=(boundary>>19)&255;
        c->edges[c->edge_count].neighbor_column_curve=(boundary>>27)&255;
        c->edges[c->edge_count].thin_trim=(boundary>>35)&1;
        c->edges[c->edge_count].neighbor_thin_trim=(boundary>>36)&1;
        c->edges[c->edge_count].raised_scenery=(boundary>>37)&1;
        c->edges[c->edge_count].neighbor_raised_scenery=(boundary>>38)&1;
        c->edges[c->edge_count].native_actor_priority=(boundary>>39)&1;
        const unsigned list = e->high_priority ? 1 : 0;
        c->pass_edges[list][c->pass_counts[list]++] = c->edge_count;
        if (e->rear_trim) c->pass_edges[2][c->pass_counts[2]++] = c->edge_count;
        if (e->high_priority && e->colliding && e->side == 0 && e->far_rank >= 2) c->pass_edges[3][c->pass_counts[3]++] = c->edge_count;
        c->high_deck_count += e->high_priority && e->colliding && e->side == 0 && e->far_rank >= 2;
        ++c->edge_count;
        col = end;
      }
  }
  if(!BuildForestFoldEdges(c,s,stamps,native,tiles,tile_pitch))return false;
  if(!BuildMarshFoldEdges(c,s,terrain,stamps,native,palette,tiles,tile_pitch))return false;
  BuildForestStumpCapSamples(c);
  /* Smooth once at publication. Sampling this world field never follows the
   * viewport or opens cracks between adjacent native tiles. */
  if(c->rear_relief) {
    const int width=(c->width+15)/16,height=(c->height+15)/16;
    unsigned char *smooth=calloc((size_t)width*height,1);
    if(!smooth)return false;
    for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
      unsigned sum=0;
      for(int yy=-1;yy<=1;++yy)for(int xx=-1;xx<=1;++xx)
        if(x+xx>=0 && x+xx<width && y+yy>=0 && y+yy<height)
          sum+=c->rear_relief[(y+yy)*width+x+xx]*(xx?1:2)*(yy?1:2);
      smooth[y*width+x]=(sum+8)/16;
    }
    free(c->rear_relief);c->rear_relief=smooth;
  }
  if (!BuildWaterSurfaces(c,s,terrain)) return false;
  c->ready = true;
  if(!BuildAuthoredCells(c,s,terrain,stamps,native,palette,tiles,tile_pitch) ||
      !BuildAutomaticSurfaceArt(c,s,terrain,stamps,native,palette,tiles,tile_pitch))return false;
  BuildCastleTerminalMatte(c,s,terrain,stamps,native,palette,tiles,tile_pitch);
  BuildJungleTerminal(c,s,terrain,stamps,native,palette,tiles,tile_pitch);
  return true;
}

bool ActionVoxel_BuildWithTiles(ActionVoxelGeometry *c, const ActionRoomScene *s,
                           const DioramaRoomOverride *terrain, ActionVoxelSettings settings,
                           const uint16_t *tiles, size_t tile_pitch) {
  if (!c || atomic_load_explicit(&c->references, memory_order_relaxed) != 1 || !s || !isfinite(settings.depth) || !isfinite(settings.play_recess) ||
      settings.depth < 0 || settings.depth > .4f || settings.play_recess < 0 ||
      settings.play_recess > settings.depth * .7f)
    return false;
  if (tiles && tile_pitch < ActionRoomScene_TileWidth(s, 1)) return false;
  ActionVoxelGeometry *next = ActionVoxel_Create();
  if (!next) return false;
  next->settings = settings;
  if (!BuildGeometry(next, s, terrain, tiles, tile_pitch)) {
    c->build_error=next->build_error;
    ActionVoxel_Release(next);
    return false;
  }
  free(c->front);
  free(c->semantic_cells);
  free(c->native_priority_decks);
  free(c->cutouts);
  free(c->branch_pixels);
  free(c->front_recess);
  free(c->thin_trim_cells);
  free(c->raised_scenery_cells);
  free(c->pixel_recess_cells);
  free(c->trunk_cells);
  free(c->column_cells);
  free(c->rear_relief);
  free(c->soft_surfaces);
  free(c->rear_fills);
  free(c->jungle_terminal);
  free(c->authored);
  free(c->authored_at);
  free(c->authored_art);
  free(c->authored_cap_art);
  free(c->authored_contours);
  free(c->profile_surfaces);
  *c = *next;
  atomic_init(&c->references, 1);
  free(next);
  return true;
}

bool ActionVoxel_Build(ActionVoxelGeometry *c, const ActionRoomScene *s,
                        const DioramaRoomOverride *terrain, ActionVoxelSettings settings) {
  return ActionVoxel_BuildWithTiles(c, s, terrain, settings, NULL, 0);
}
