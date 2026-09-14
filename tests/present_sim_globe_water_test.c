/* CPU source oracle: semantic coverage, diagonal conformity and publication
 * lifetime. Only the resource sink and art providers are substituted. The
 * water planner and radial encoding are production code. */
#include "present_sim_globe_water.h"
#include "sim/sim3d_mesh_set.h"
#include "sim/sim_town_ground_art.h"
#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Sim3DDepthSurfaceVertex *captured;
static ArRenderPointF *coordinates;
static size_t captured_count;
static bool reject_upload;
static unsigned pattern;

bool Sim3DMeshSet_Ready(const Sim3DMeshSet *set) { return set->valid; }
void Sim3DMeshSet_Destroy(Sim3DMeshSet *set) {
  free(captured); free(coordinates); captured=NULL; coordinates=NULL;
  captured_count=0; memset(set,0,sizeof(*set));
}
bool Sim3DMeshSet_UpdateSurface(Sim3DMeshSet *set,
    const Sim3DDepthSurfaceVertex *vertices, const ArRenderPointF *mask, size_t count) {
  Sim3DMeshSet_Destroy(set);
  if (reject_upload) return false;
  if (count) {
    captured=malloc(count*4*sizeof(*captured)); coordinates=malloc(count*4*sizeof(*coordinates));
    assert(captured && coordinates);
    memcpy(captured,vertices,count*4*sizeof(*captured));
    memcpy(coordinates,mask,count*4*sizeof(*coordinates));
  }
  captured_count=count; set->valid=true; set->quads=count;
  return true;
}
bool Sim3DMeshSet_AppendSurface(const Sim3DMeshSet *set,
    const Sim3DDepthSurfaceBatch *batch, size_t count) {
  assert(set->valid && count==1 && batch->range.quad_count==captured_count);
  assert(batch->texture.value==123 && batch->transform.ambient==1);
  return true;
}

bool SimWorldMap_OriginForTown(uint8_t town, int *x, int *y) {
  static const int origin[6][2]={{80,48},{48,48},{16,64},{16,32},{64,96},{32,0}};
  if (town<1 || town>6) return false;
  if (x) *x=origin[town-1][0]; if (y) *y=origin[town-1][1];
  return true;
}
static bool WaterPixel(int x,int y) {
  return !pattern || !((x%43)<5 && (y%37)<17);
}
bool SimWorldMap_OpenWaterMask(int x,int y,uint8_t mask[64]) {
  assert(x>=0 && y>=0 && x<128 && y<128);
  for (int p=0;p<64;++p) mask[p]=WaterPixel(x*8+p%8,y*8+p/8);
  return true;
}
bool SimTownGroundArt_IsOpenWater(uint8_t town,uint8_t tier,uint8_t tile) {
  (void)town; (void)tier; return tile==1;
}
bool SimTownGroundArt_OpenWaterMask(uint8_t town,uint8_t tier,uint8_t tile,uint8_t mask[256]) {
  (void)town; (void)tier;
  for (int p=0;p<256;++p) mask[p]=tile==1 || (tile==2 && p%16<9);
  return true;
}

static bool SafePixel(const SimWorldNavigationTownGround *ground, int x,int y) {
  if (x<0 || y<0 || x>=128*16 || y>=128*16) return true;
  if (!WaterPixel(x/2,y/2)) return false;
  for (uint8_t town=1;town<=6;++town) {
    if (!(ground->enabled_town_mask & (1u<<(town-1)))) continue;
    int ox,oy; assert(SimWorldMap_OriginForTown(town,&ox,&oy));
    const int cx=x/16-ox,cy=y/16-oy;
    if (cx<0 || cy<0 || cx>=32 || cy>=32) continue;
    if (ground->object_rows[town-1][cy] & (UINT32_C(1)<<cx)) return false;
    const uint8_t tile=ground->terrain[town-1][cy*32+cx];
    if (tile!=1 && (tile!=2 || x%16>=9)) return false;
  }
  return true;
}

static float Cross(ArRenderPointF a,ArRenderPointF b,ArRenderPointF c) {
  return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
}

static void CheckSurface(const SimGlobeMapping *map,
    const SimWorldNavigationTownGround *ground, const Sim3DDepthSurfaceVertex *grid) {
  uint8_t *seen=calloc(640*640,1); assert(seen);
  const int ox=(int)map->origin_x-4,oy=(int)map->origin_y-4;
  for (size_t q=0;q<captured_count;++q) {
    ArRenderPointF uv[4];
    float cx=0,cy=0;
    for (int p=0;p<4;++p) {
      uv[p]=(ArRenderPointF){coordinates[q*4+p].x*128,coordinates[q*4+p].y*128};
      cx+=uv[p].x*.25f; cy+=uv[p].y*.25f;
      assert(captured[q*4+p].color.a>=0 && captured[q*4+p].color.a<=1);
      assert(captured[q*4+p].uv.x>=.5f/512 && captured[q*4+p].uv.x<=511.5f/512);
      assert(captured[q*4+p].uv.y>=.5f/512 && captured[q*4+p].uv.y<=511.5f/512);
    }
    const int gx=(int)floorf(cx),gy=(int)floorf(cy);
    assert(!(gx>=map->origin_x && gx<map->origin_x+32 && gy>=map->origin_y && gy<map->origin_y+32));
    if (gx>=0 && gy>=0 && gx<128 && gy<128) for (int p=0;p<4;++p) {
      const float u=uv[p].x-gx,v=uv[p].y-gy;
      const float weights[4]={1-fmaxf(u,v),fmaxf(0,u-v),fminf(u,v),fmaxf(0,v-u)};
      const int indices[4]={gy*129+gx,gy*129+gx+1,(gy+1)*129+gx+1,(gy+1)*129+gx};
      for (int a=0;a<3;++a) {
        float expected=0;
        for (int i=0;i<4;++i) expected+=weights[i]*grid[indices[i]].normal[a]*(map->radius+grid[indices[i]].elevation[0]+.0005f);
        const Sim3DDepthSurfaceVertex *out=&captured[q*4+p];
        const float actual=out->normal[a]*(map->radius+out->elevation[0]);
        assert(fabsf(expected-actual)<.0002f);
      }
    }
    /* Rasterize in source chart space, independent of the rectangle planner.
     * Triangle centroids also prove each emitted face stays on one parent
     * plane rather than bridging the coastal quad's diagonal. */
    const int triangle[2][3]={{0,1,2},{0,2,3}};
    for (int tri=0;tri<2;++tri) {
      const ArRenderPointF a=uv[triangle[tri][0]],b=uv[triangle[tri][1]],c=uv[triangle[tri][2]];
      if (fabsf(Cross(a,b,c))<1e-8f) continue;
      const float sides[3]={a.x-gx-a.y+gy,b.x-gx-b.y+gy,c.x-gx-c.y+gy};
      assert(fminf(sides[0],fminf(sides[1],sides[2]))>=0 ||
             fmaxf(sides[0],fmaxf(sides[1],sides[2]))<=0);
      for (int py=0;py<16;++py) for (int px=0;px<16;++px) {
        const ArRenderPointF point={gx+(px+.5f)/16,gy+(py+.5f)/16};
        if (Cross(a,b,point)<-1e-7f || Cross(b,c,point)<-1e-7f || Cross(c,a,point)<-1e-7f) continue;
        const int x=(gx-ox)*16+px,y=(gy-oy)*16+py;
        assert(x>=0 && y>=0 && x<640 && y<640); seen[y*640+x]=1;
      }
    }
  }
  size_t protected=0,covered=0;
  for (int y=0;y<640;++y) for (int x=0;x<640;++x) {
    const int cx=x/16-4,cy=y/16-4;
    const float dx=fmaxf(0,fmaxf(-cx-1,cx-32)),dy=fmaxf(0,fmaxf(-cy-1,cy-32));
    bool expected=!(cx>=0 && cy>=0 && cx<32 && cy<32) && hypotf(dx,dy)<4;
    if (x==0 || y==0 || x==639 || y==639) expected=false;
    for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx)
      expected &= SafePixel(ground,ox*16+x+dx,oy*16+y+dy);
    assert(seen[y*640+x]==expected);
    covered+=expected; protected+=!expected;
  }
  printf("water town=%u pattern=%u quads=%zu covered=%zu protected=%zu: exact coverage and triangle conformity\n",
      map->town,pattern,captured_count,covered,protected);
  free(seen);
}

int main(void) {
  SimWorldNavigationTownGround ground={.enabled_town_mask=0x3f};
  memset(ground.terrain,1,sizeof(ground.terrain));
  ground.terrain[1][18*32+31]=2;
  ground.object_rows[1][20]=UINT32_C(1)<<31;
  Sim3DDepthSurfaceVertex *grid=calloc(129*129,sizeof(*grid)); assert(grid);
  for (uint8_t town=1;town<=5;town+=4) for (pattern=0;pattern<2;++pattern) {
    int ox,oy; assert(SimWorldMap_OriginForTown(town,&ox,&oy));
    SimGlobeMapping map; assert(SimGlobeMapping_Build(town,ox,oy,288,0,.4f,&map));
    for (int y=0;y<=128;++y) for (int x=0;x<=128;++x) {
      Sim3DDepthSurfaceVertex *v=&grid[y*129+x];
      /* Non-coplanar checkerboard makes a different diagonal measurable. */
      assert(SimGlobeMapping_Encode(&map,x,y,((x+y)&1)?2:0,0,v->normal,v->elevation));
    }
    assert(!PresentSimGlobeWater_Matches(&map,&ground));
    assert(PresentSimGlobeWater_Prepare(&map,&ground,grid));
    assert(PresentSimGlobeWater_Matches(&map,&ground));
    assert(PresentSimGlobeWater_QuadCount()==captured_count && captured_count>0);
    CheckSurface(&map,&ground,grid);
    const float matrix[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    const Sim3DDepthSurfaceFocus focus={0};
    assert(PresentSimGlobeWater_Append(matrix,map.radius,(ArRenderTexture){123},&focus));
    SimWorldNavigationTownGround changed=ground; changed.terrain[town-1][0]=3;
    assert(!PresentSimGlobeWater_Matches(&map,&changed));
    reject_upload=true;
    assert(!PresentSimGlobeWater_Prepare(&map,&ground,grid));
    assert(!PresentSimGlobeWater_Matches(&map,&ground));
    assert(!PresentSimGlobeWater_Append(matrix,map.radius,(ArRenderTexture){123},&focus));
    reject_upload=false;
    PresentSimGlobeWater_Reset();
  }
  assert(!PresentSimGlobeWater_Prepare(NULL,&ground,grid));
  free(grid);
  puts("present_sim_globe_water_test: PASS");
  return 0;
}
