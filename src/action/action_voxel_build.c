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
} VoxelMaterialProfile;
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
       kVoxelOpaqueRearMaterial = 8388608 };
typedef struct CastleMaterial {
  uint16_t words[4];
  uint64_t shape;
  uint8_t kind[4], collision;
  uint32_t flags;
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
  return kind == kVoxelSolid ? 2 : kind == kVoxelForeground ? 1 : 0;
}
static bool KeepProfileContour(const ActionVoxelGeometry *c, int x, int y, unsigned side) {
  /* Forest stumps follow the native pixel silhouette, including bark steps. */
  if (c->profile == kActionVoxelProfile_FillmoreForest) return true;
  for (unsigned i = 0; i < c->patch_count; ++i) {
    const VoxelPatch *p = &c->patches[i];
    if (p->kind != kVoxelPatch_SideProfile || x+c->origin_x < p->x ||
        x+c->origin_x >= p->x+p->width || y+c->origin_y < p->y || y+c->origin_y >= p->y+p->height)
      continue;
    if (side & 1) return false;
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
static uint64_t Boundary(const ActionVoxelGeometry *c, int x, int y, unsigned side,
                         const float depths[4]) {
  const int dx[] = {0, 1, 0, -1}, dy[] = {-1, 0, 1, 0};
  const int xx = x + dx[side], yy = y + dy[side];
  const unsigned pixel = c->front[y * c->width + x], far_rank = DepthRank(c, y * c->width + x);
  const unsigned near_rank = xx < 0 || yy < 0 || xx >= c->width || yy >= c->height
                                 ? 0
                                 : DepthRank(c, yy * c->width + xx);
  /* Thin foreground trim on a full floor exposes only the walkway behind it.
   * Shared solid interiors have no side, even across native priorities. */
  const bool rear=c->mixed_scenery && far_rank==1 && c->front_recess[(y/8)*(c->width/8)+x/8];
  const bool neighbor_rear=c->mixed_scenery && near_rank==1 &&
      c->front_recess[(yy/8)*(c->width/8)+xx/8];
  const bool thin=c->thin_trim_cells && far_rank==1 &&
      c->thin_trim_cells[(y/8)*(c->width/8)+x/8];
  const bool neighbor_thin=c->thin_trim_cells && near_rank==1 &&
      c->thin_trim_cells[(yy/8)*(c->width/8)+xx/8];
  const unsigned column=c->column_cells && far_rank==2
      ? c->column_cells[(size_t)(y/8)*(c->width/8)+x/8] : 0;
  const unsigned neighbor_column=c->column_cells && near_rank==2
      ? c->column_cells[(size_t)(yy/8)*(c->width/8)+xx/8] : 0;
  const float world_y=c->origin_y+y+(side==2);
  bool exposed=depths[far_rank]>depths[near_rank];
  if(c->mixed_scenery || column || neighbor_column) {
    const float front=column?ActionVoxel_ColumnFrontDepth(c,&c->column_curves[column-1],world_y):
        rear?ActionVoxel_SceneryRecess(c):0;
    const float back=far_rank==1?front+ActionVoxel_LocalTrimDepth(c,thin):depths[far_rank];
    const float neighbor_front=neighbor_column?ActionVoxel_ColumnFrontDepth(c,&c->column_curves[neighbor_column-1],world_y):
        neighbor_rear?ActionVoxel_SceneryRecess(c):0;
    const float neighbor_back=near_rank==1?neighbor_front+ActionVoxel_LocalTrimDepth(c,neighbor_thin):depths[near_rank];
    exposed=back>front && !(neighbor_front<=front && neighbor_back>=back);
    if(!exposed && (side&1) && (column || neighbor_column)) {
      const float lower_front=column?ActionVoxel_ColumnFrontDepth(c,&c->column_curves[column-1],world_y+1):front;
      const float lower_neighbor=neighbor_column?ActionVoxel_ColumnFrontDepth(c,&c->column_curves[neighbor_column-1],world_y+1):neighbor_front;
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
 * pixel edits retain their existing placement. Deduplicate small masks and
 * bound the number of split cells, rather than storing per-pixel depths. */
static bool BuildPixelRecess(ActionVoxelGeometry *c,const CastleMaterial *m,
    const DioramaPixelEdit *edit,unsigned quadrant,int tile_x,int tile_y) {
  if(!(m->flags&kVoxelPixelRecessMaterial) || (m->words[quadrant]&0x2000) ||
      !m->face_colors[quadrant])return true;
  uint8_t rows[8]={0};unsigned any=0;
  for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x) {
    const unsigned mx=(quadrant&1)*8+x,my=(quadrant>>1)*8+y;
    const unsigned p=m->pixels[quadrant][y*8+x];
    if(!p || !(m->face_colors[quadrant]&(1u<<p)) ||
        MaterialTransparent(m,edit,mx,my) || (edit && (edit->black[my]&(1u<<(15-mx)))))continue;
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
  ActionRoomScene *phase_scene = malloc(sizeof(*phase_scene));
  if (!phase_scene) return false;
  *phase_scene = *s;
  bool safe = true;
  uint32_t flags[kAnimationMaterials];
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
      if(!phase)flags[i]=m.flags;
      /* Desert sand animation changes color indices inside a fixed solid
       * silhouette. Unlike inert rear artwork, colliding surfaces require
       * exact kinds, cap/soft-surface flags and opacity in every native phase.
       * This excludes baked color samples, neighbour inference, growth and
       * all palette-index-selected faces. Authored erasures intersect the
       * same opacity mask; native priority/collision words never change here. */
      bool solid_surface = desert &&
          !(m.flags & ~(kVoxelTexturedTopMaterial | kVoxelSoftSurfaceMaterial));
      for (unsigned q = 0; q < 4; ++q)
        solid_surface &= m.kind[q] == 0 || m.kind[q] == kVoxelSolid;
      safe = m.flags==flags[i] && (solid_surface || !m.flags ||
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
static bool LookupMaterial(const ActionRoomScene *s, const DioramaStampLayerOverride *stamps,
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
static bool BuildRearFill(ActionVoxelGeometry *c, const ActionRoomScene *s,
    const DioramaRoomOverride *terrain, const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256], const CastleMaterial *target, int cx, int cy,
    const DioramaPixelEdit *target_edit, const uint16_t palette[128],
    const uint16_t *tiles, size_t tile_pitch) {
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
/* The forest's seven-pixel painted ground band belongs on the deck. Compile
 * its fold only for full solid ground supported by reviewed rear vegetation;
 * branches, stump profiles and hazards retain their original surfaces. The
 * metadata leaves source pixels, collision and native object anchors intact. */
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
  for(unsigned r=0;r<run_count;++r)if(runs[r].supported)
    for(unsigned n=0;n<runs[r].count;++n)c->edges[top[runs[r].first+n]].rear_fold_top=kForestGroundFoldPixels;
  for(unsigned i=0;i<c->edge_count;++i) {
    VoxelEdge *e=&c->edges[i];
    if(!(e->side&1) || !ForestGroundEdge(c,cells,e))continue;
    const int boundary_x=e->x+(e->side==1);
    e->rear_fold_top=ForestFoldEndpoint(runs,run_count,boundary_x,e->y)?kForestGroundFoldPixels:0;
    e->rear_fold_bottom=ForestFoldEndpoint(runs,run_count,boundary_x,e->y+e->length)?kForestGroundFoldPixels:0;
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
#include "action_voxel_marsh_fold.inc"
#include "action_voxel_castle_columns.inc"
#include "action_voxel_authored.inc"
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
  CastleMaterial native[256];
  unsigned backing_material = 256;
  for (unsigned m = 0; m < 256; ++m) {
    uint16_t words[4];
    const unsigned collision = NativeWords(s,m,words);
    Material(s, 0, words, collision, &native[m]);
    if (native[m].flags & kCastleBackingMaterial) backing_material = m;
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
      const bool rear_foliage = foliage && have_below && !below.collision &&
          below.kind[0] == kVoxelRearOnly && below.kind[1] == kVoxelRearOnly;
      const bool growth_crown = foliage && have_below && (below.flags & kVoxelGrowthMaterial);
      const DioramaPixelEdit *edit =
          terrain ? DioramaLayerOrder_PixelEditAt(terrain, 0, cx, cy, metatile) : NULL;
      if (material.flags & kVoxelSoftSurfaceMaterial) {
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
        if (last && !last->background && last->y==cy*16 && last->x+last->width==cx*16 &&
            last->opaque==!!(material.flags & kVoxelOpaqueSurfaceMaterial) &&
            last->sample_y==sample_y && last->sample_height==sample_height)
          last->width+=16;
        else {
          if (c->water_surface_count==kVoxelMaxWaterSurfaces) return false;
          c->water_surfaces[c->water_surface_count++]=(VoxelWaterSurface){
              .x=cx*16,.y=cy*16,.width=16,.high_priority=true,.background=0,
              .sample_y=sample_y,.sample_height=sample_height,
              .opaque=!!(material.flags & kVoxelOpaqueSurfaceMaterial)};
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
      if ((material.flags & kVoxelOpaqueRearMaterial) &&
          !BuildOpaquePortalBacking(c,&material,cx,cy,edit))return false;
      for (unsigned quadrant = 0; quadrant < 4; ++quadrant) {
        const int tx = cx * 2 + (quadrant & 1), ty = cy * 2 + (quadrant >> 1);
        const unsigned w = material.words[quadrant];
        const bool solid = (material.collision >> quadrant) & 1;
        unsigned kind = cap ? kVoxelSolid : material.kind[quadrant];
        if (rear_foliage && kind == kVoxelFoliage) kind = kVoxelRearOnly;
        if (!kind) continue;
        const bool high_priority = (w & 0x2000) != 0;
        /* Reviewed raised crenellations need matching rear trim. Portals
         * with authored rear jambs do not request a second copy. */
        const bool rear_trim = high_priority && (material.flags & kCastleRearMaterial);
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
        const unsigned role=SemanticMaterialKind(&material,kind);
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
        /* Noncolliding front posts are scenery outside the walking lane.
         * Keep their native front face/priority, but do not give them the
         * quarter-deck thickness used by rocks and rear-mounted supports. */
        if(priority_scenery && high_priority && !solid && kind==kVoxelForeground) {
          if(!c->thin_trim_cells) {
            c->thin_trim_cells=calloc((size_t)c->width/8,c->height/8);
            if(!c->thin_trim_cells)return false;
          }
          c->thin_trim_cells[(ty-y0/8)*(c->width/8)+tx-x0/8]=1;
        }
        if((ActionVoxel_RearMountedScenery(c) || (material.flags&kVoxelRearMountedMaterial) ||
            (priority_scenery && !high_priority)) &&
            kind==kVoxelForeground) {
          if(!c->front_recess)return false;
          c->front_recess[(ty-y0/8)*(c->width/8)+tx-x0/8]=2;
        }
        c->mixed_scenery |= (material.flags&kVoxelRearMountedMaterial)!=0 || priority_scenery;
        if(!BuildPixelRecess(c,&material,edit,quadrant,tx,ty))return false;
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
            c->patches[c->patch_count++] = (VoxelPatch){tx*8,ty*8,8,8,high_priority,patch_kind};
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
            const unsigned pixel_kind = rear_matte || (material.rear_colors[quadrant] & (1u << p))
                ? kVoxelRearOnly : material.gate_colors[quadrant] & (1u << p) ? kVoxelGate
                : !(material.flags&kVoxelPixelRecessMaterial) &&
                    (material.face_colors[quadrant] & (1u << p)) ? kVoxelFoliage : kind;
            c->front[(ty * 8 + y - y0) * c->width + tx * 8 + x - x0] =
                (kind == kVoxelFlatParapet ||
                 (p && (!silhouette || palette[((w >> 10) & 7) * 16 + p])))
                    ? (pixel_kind | (high_priority ? kVoxelHighPriority : 0) |
                       (rear_trim ? kVoxelRearTrim : 0) | (solid ? kVoxelColliding : 0) |
                       ((material.flags & kVoxelRearCopyMaterial) ? kVoxelRearCopy : 0) |
                       ((material.flags & kVoxelTexturedTopMaterial) ? kVoxelTexturedTop : 0))
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
        if(xx>=0 && yy>=0 && xx<width && yy<height && !c->front_recess[yy*width+xx])
          c->front_recess[yy*width+xx]=3;
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
  if(!BuildCastleColumns(c,s,stamps,native,tiles,tile_pitch))return false;
  if(c->column_cells)for(int cy=0;cy<c->height/16;++cy)for(int cx=0;cx<c->width/16;++cx) {
    bool column=false;
    for(unsigned q=0;q<4;++q)column|=c->column_cells[(size_t)(cy*2+(q>>1))*(c->width/8)+cx*2+(q&1)]!=0;
    if(column)c->semantic_cells[(size_t)cy*(c->width/16)+cx]=kVoxelSemantic_Column;
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
  return BuildAuthoredCells(c,s,terrain,stamps,native,palette,tiles,tile_pitch);
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
  free(c->cutouts);
  free(c->branch_pixels);
  free(c->front_recess);
  free(c->thin_trim_cells);
  free(c->pixel_recess_cells);
  free(c->trunk_cells);
  free(c->column_cells);
  free(c->rear_relief);
  free(c->soft_surfaces);
  free(c->rear_fills);
  free(c->authored);
  free(c->authored_at);
  free(c->authored_art);
  free(c->authored_cap_art);
  free(c->authored_contours);
  *c = *next;
  atomic_init(&c->references, 1);
  free(next);
  return true;
}

bool ActionVoxel_Build(ActionVoxelGeometry *c, const ActionRoomScene *s,
                        const DioramaRoomOverride *terrain, ActionVoxelSettings settings) {
  return ActionVoxel_BuildWithTiles(c, s, terrain, settings, NULL, 0);
}
