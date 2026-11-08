/* Compare the optimized split against the previous per-pixel rules, without
 * sharing its lookup table or clipped row traversal. Optional exported rooms
 * exercise the actual regional material maps and sparse backing contracts. */
#include "support/test_assert.h"
#include "action/action_voxel_internal.h"
#include "action/action_scene_snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t ReferenceColor(unsigned color, const ActionRoomSceneFrameState *f) {
  const unsigned subtract=f->fixed_color && !f->cgwsel && (f->cgadsub&1) &&
      (f->cgadsub&0xc0)==0x80 ? f->fixed_color : 0;
  uint32_t result=0xff000000u;
  for(unsigned channel=0;channel<3;++channel) {
    int value=((color>>(5*channel))&31)-((subtract>>(5*channel))&31);
    if(value<0)value=0;
    result|=(uint32_t)(((value*8+(value>>2))*f->brightness)/15)<<(16-8*channel);
  }
  return result;
}
static void ReferenceSplit(const ActionVoxelGeometry *c,int ox,int oy,int width,int height,
    size_t pitch,const ActionRoomSceneFrameState *f,const uint32_t *low,size_t low_pitch,
    uint32_t *rear,uint32_t *front) {
  const bool castle=c->profile==kActionVoxelProfile_Castle;
  for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
    const int wx=ox+x,wy=oy+y,mx=wx-c->origin_x,my=wy-c->origin_y;
    const bool inside=mx>=0 && mx<c->width && my>=0 && my<c->height;
    const size_t world_at=inside ? (size_t)my*c->width+mx : 0;
    const unsigned material=inside ? c->front[world_at] : 0;
    const unsigned kind=material&kVoxelKindMask;
    const uint32_t source=low ? low[(size_t)y*low_pitch+x] : 0;
    const bool branch=material && c->branch_pixels &&
        (c->branch_pixels[world_at>>3]&(1u<<(world_at&7)));
    const bool bark=kind==kVoxelForeground && (material&kVoxelRearCopy) && c->trunk_cells &&
        c->trunk_cells[(size_t)(my/8)*(c->width/8)+mx/8];
    const unsigned column=kind==kVoxelSolid && c->column_cells ?
        c->column_cells[(size_t)(my/8)*(c->width/8)+mx/8] : 0;
    int column_base=0;
    if(column && column<=c->column_curve_count) {
      const VoxelTrunkCurve *curve=c->column_curves+column-1;
      column_base=curve->base_y;
      if(curve->contact_count==8) {
        const int offset=wx-curve->contact_x;
        int contact=offset<0?-((-offset+1)/2):offset/2;
        if(contact<0)contact=0;else if(contact>7)contact=7;
        column_base=curve->contact[contact]+16;
      }
    }
    const bool rear_column=column && column<=c->column_curve_count && wy>=column_base;
    uint32_t r;
    if(branch || bark)r=0;
    else if(!castle)r=!(material&kVoxelRearCopy) &&
        (kind==kVoxelThin || kind==kVoxelFoliage || kind==kVoxelForeground) ? 0 : source;
    else if(kind==kVoxelFlatParapet)r=0;
    else if(kind==kVoxelSolid)r=ReferenceColor(c->backing_color[((unsigned)wy&31)*16+((unsigned)wx&15)],f);
    else if(kind==kVoxelThin || kind==kVoxelGate || kind==kVoxelFoliage)r=0xff000000u;
    else r=source;
    const size_t at=(size_t)y*pitch+x;
    rear[at]=r;
    front[at]=kind && kind!=kVoxelFlatParapet && kind!=kVoxelRearOnly ? source : 0;
    if(rear_column && (source>>24)==255)rear[at]=0;
  }
  /* Iterate the viewport, rather than the production fill clipping loop. */
  for(unsigned i=0;i<c->rear_fill_count;++i)for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
    const VoxelRearFill *fill=&c->rear_fills[i];
    const int fx=ox+x-fill->x,fy=oy+y-fill->y;
    if(fx<0 || fx>=16 || fy<0 || fy>=16)continue;
    const unsigned color=fill->colors[fy*16+fx];
    const size_t at=(size_t)y*pitch+x;
    if((color&0x8000) && (!fill->preserve_existing || !(rear[at]>>24)))
      rear[at]=ReferenceColor(color,f);
  }
}
static unsigned long long s_pixels;
static uint32_t Pixel(size_t i,unsigned alpha_case) {
  static const unsigned alphas[]={0,1,0x7f,0xfe,0xff};
  const unsigned alpha=alphas[alpha_case<5 ? alpha_case : i%5];
  const uint32_t rgb=((uint32_t)i*0x5d3b19u+0x010307u)&0xffffff;
  return (alpha<<24)|rgb;
}
static void Compare(const ActionVoxelGeometry *c,int ox,int oy,int width,int height,
    size_t pitch,size_t low_pitch,const ActionRoomSceneFrameState *f,
    unsigned alpha_case,unsigned alias,bool no_source) {
  assert(!alias || pitch==low_pitch);
  const size_t count=((size_t)height+1)*(pitch>low_pitch ? pitch : low_pitch)+8;
  uint32_t *a=malloc(count*6*sizeof(*a));assert(a);
  uint32_t *rear=a,*front=a+count,*source=a+count*2;
  uint32_t *expected=a+count*3,*expected_front=a+count*4,*expected_source=a+count*5;
  for(size_t i=0;i<count;++i) {
    rear[i]=expected[i]=0x92bead61u;
    front[i]=expected_front[i]=0x47cafed2u;
    source[i]=expected_source[i]=Pixel(i,alpha_case);
  }
  if(alias==1 || alias==3) {
    memcpy(rear,source,count*sizeof(*a));memcpy(expected,source,count*sizeof(*a));
    source=rear;expected_source=expected;
  } else if(alias==2) {
    memcpy(front,source,count*sizeof(*a));memcpy(expected_front,source,count*sizeof(*a));
    source=front;expected_source=expected_front;
  }
  if(alias==3 || alias==4) {front=rear;expected_front=expected;}
  ReferenceSplit(c,ox,oy,width,height,pitch,f,no_source ? NULL : expected_source,
      low_pitch,expected,expected_front);
  assert(ActionVoxel_Split(c,ox,oy,width,height,pitch,f,no_source ? NULL : source,
      low_pitch,rear,front));
  assert(!memcmp(rear,expected,count*sizeof(*a)));
  assert(!memcmp(front,expected_front,count*sizeof(*a)));
  assert(!memcmp(source,expected_source,count*sizeof(*a)));
  s_pixels+=(unsigned long long)width*height;
  free(a);
}
static void Synthetic(void) {
  static ActionVoxelGeometry c;
  unsigned char materials[256],branches[32],trunks[4]={1,0,1,1},columns[4]={0,1,1,1};
  VoxelRearFill fills[2]={0};
  for(unsigned i=0;i<256;++i)materials[i]=(unsigned char)i;
  memset(branches,0xa5,sizeof(branches));
  c.ready=true;c.front=materials;c.width=c.height=16;c.origin_x=-8;c.origin_y=16;
  c.profile=kActionVoxelProfile_BloodpoolMarsh;
  c.column_curve_count=1;c.column_curves[0].base_y=23;
  for(unsigned i=0;i<512;++i)c.backing_color[i]=(i*73)&0x7fff;
  for(unsigned j=0;j<2;++j) {
    fills[j].x=-11+j*11;fills[j].y=11+j*13;fills[j].preserve_existing=j!=0;
    for(unsigned i=0;i<256;++i)fills[j].colors[i]=(i%3 ? 0x8000 : 0)|((i*137)&0x7fff);
  }
  const int views[][4]={{-8,16,16,16},{-13,11,27,27},{-8,17,16,15},{-30,16,11,16},
    {8,16,17,16},{-8,-1,16,12},{-8,32,16,12},{-9,15,1,1},{7,31,2,2}};
  ActionRoomSceneFrameState f={.brightness=15};
  for(unsigned feature=0;feature<6;++feature) {
    c.branch_pixels=feature==1 ? branches : NULL;
    c.trunk_cells=feature==2 ? trunks : NULL;
    c.column_cells=feature==3 || feature==5 ? columns : NULL;
    c.column_curves[0].contact_count=feature==5?8:0;
    c.column_curves[0].contact_x=-8;c.column_curves[0].linear=feature==5;
    for(unsigned i=0;i<8;++i)c.column_curves[0].contact[i]=8+i;
    c.profile=feature==4 ? kActionVoxelProfile_Castle : kActionVoxelProfile_BloodpoolMarsh;
    for(unsigned v=0;v<sizeof(views)/sizeof(views[0]);++v) {
      const int *view=views[v];
      for(unsigned alpha=0;alpha<6;++alpha) {
        Compare(&c,view[0],view[1],view[2],view[3],view[2]+3,view[2]+5,&f,alpha,0,false);
        for(unsigned alias=1;alias<=4;++alias)
          Compare(&c,view[0],view[1],view[2],view[3],view[2]+3,view[2]+3,&f,alpha,alias,false);
      }
      Compare(&c,view[0],view[1],view[2],view[3],view[2]+2,0,&f,0,0,true);
      Compare(&c,view[0],view[1],view[2],view[3],view[2],view[2],&f,5,0,false);
    }
    c.rear_fills=fills;c.rear_fill_count=2;
    for(unsigned brightness=0;brightness<16;brightness+=3)for(unsigned math=0;math<3;++math) {
      f.brightness=brightness;f.fixed_color=0x21a5;f.cgadsub=0x81;f.cgwsel=math==1 ? 2 : 0;
      if(math==2)f.cgadsub|=0x40;
      for(unsigned alias=0;alias<5;++alias)
        Compare(&c,-13,11,32,37,35,35,&f,5,alias,false);
      Compare(&c,-13,11,32,37,35,0,&f,0,0,true);
    }
    c.rear_fill_count=0;f=(ActionRoomSceneFrameState){.brightness=15};
  }
  puts("split synthetic: 256 material bytes, alpha/RGB, clipping, pitches, aliases, sparse fills and complex fallback passed");
}
static void RegionalMarsh(const char *directory) {
  static ActionSceneSnapshot snapshot;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];
  for(unsigned region=0;region<3;++region) {
    char path[2048];assert(snprintf(path,sizeof(path),"%s/0201-%u.arscene",directory,region)>0);
    FILE *file=fopen(path,"rb");assert(file);
    const size_t size=fread(bytes,1,sizeof(bytes),file);assert(!ferror(file));assert(!fclose(file));
    assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
    ActionVoxelGeometry *c=ActionVoxel_Create();assert(c);
    assert(ActionVoxel_Build(c,&snapshot.scene,NULL,ActionVoxel_SettingsForRoom(2,1)));
    assert(ActionVoxel_Active(c,2,1) && c->profile==kActionVoxelProfile_BloodpoolMarsh);
    assert(!c->branch_pixels && !c->trunk_cells && !c->column_cells);
    const ActionRoomSceneFrameState frames[]={{.brightness=15},
      {.brightness=9,.cgadsub=0x81,.fixed_color=0x21a5}};
    for(unsigned frame=0;frame<2;++frame)
      for(int y=c->origin_y-31;y<c->origin_y+c->height+31;y+=47)
        for(int x=c->origin_x-39;x<c->origin_x+c->width+39;x+=73)
          Compare(c,x,y,97,61,101,103,&frames[frame],5,0,false);
    printf("split regional 0201-%u: %dx%d, %u sparse fills passed\n",region,c->width,c->height,c->rear_fill_count);
    ActionVoxel_Release(c);
  }
}
int main(int argc,char **argv) {
  assert(argc==1 || argc==2);
  Synthetic();
  if(argc==2)RegionalMarsh(argv[1]);
  printf("split exact legacy comparison: %llu pixels passed\n",s_pixels);
  return 0;
}
