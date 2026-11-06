/* Owned native-packet cache: semantic relocation, real invalidations and
 * decoded/split pixel equivalence. No ROM, SDL or user settings are needed. */
#include "diorama/diorama_voxel_source_cache.h"
#include "action/action_voxel.h"
#include "support/test_assert.h"
#include <stdio.h>

enum { W=24, H=4, PITCH=29 };
static SrPpuBgPacket original, candidate;
static unsigned hits, misses, compared_pixels, row_plans, partial_spans, anchored_splits;

static void CopyPacket(SrPpuBgPacket *to,const SrPpuBgPacket *from) {
  memcpy(to,from,SrPpuBgPacket_Size(from));
}

static void Cell(uint32_t *cell,unsigned x,unsigned value,unsigned palette,unsigned flags) {
  const unsigned bit=x&7;
  for(unsigned plane=0;plane<4;++plane)if(value&(1u<<plane))cell[0]|=1u<<(bit+plane*8);
  for(unsigned plane=0;plane<3;++plane)if(palette&(1u<<plane))cell[1]|=1u<<(bit+plane*8);
  cell[2]|=flags<<bit;
}

static void Build(SrPpuBgPacket *p,unsigned format,bool relocated,bool full_add) {
  SrPpuBgPacket_Begin(p,W,H);
  p->request_flags=SR_PPU_BG_PACKET_TILES|(full_add?SR_PPU_BG_PACKET_FULL_ADD:0);
  p->owned_sources=3;p->words[2]=3;
  if(relocated) {
    memset(p->words+p->words[3],0x7b,37*sizeof(uint32_t));p->words[3]+=37;
  }
  for(unsigned order=0;order<2;++order) {
    const unsigned source=relocated?1-order:order;
    for(unsigned y=0;y<H;++y) {
      uint32_t palette[256];
      for(unsigned i=0;i<256;++i)palette[i]=0xff000000u|((i*17+source*31)&255)<<16|
          ((i*11+(y==2?53:0))&255)<<8|((i*7+source*19)&255);
      uint32_t *meta=SrPpuBgPacket_Meta(p,source,y);
      meta[0]=format;meta[1]=0xff172839;meta[2]=2;meta[3]=W-2;meta[4]=format>=3?3:0;
      assert(SrPpuBgPacket_SetPalette(p,source,y,palette));
      uint32_t *pixels=SrPpuBgPacket_AllocatePixels(p,source,y);assert(pixels);
      if(format==1) {
        for(unsigned x=0;x<W;++x)pixels[x]=x%7==0?0x80000000u:x%7==1?257:x%7==2?258:1+(x%14);
      } else if(format==2) {
        for(unsigned x=0;x<W;++x) {
          const unsigned value=x%7>=3?1+x%14:0;
          const unsigned flags=x%7==0?1u<<16:x%7==1?1:x%7==2?1u<<8:0;
          Cell(pixels+x/8*9,x,value,x/8,flags);
        }
      } else {
        for(unsigned column=0;column<W/8+1;++column) {
          uint32_t *tile=pixels+column*2;
          const bool flipped=(column&1)!=0;
          tile[1]=(column%3)|(flipped?32u:0)|(255u<<8)|((source?3u:4u)<<16);
          for(unsigned fine=0;fine<8;++fine) {
            const unsigned value=fine==0?0:1+(fine+column)%14,bit=flipped?fine:7-fine;
            for(unsigned plane=0;plane<4;++plane)if(value&(1u<<plane))tile[0]|=1u<<(bit+plane*8);
          }
        }
        if(format==4) {
          uint32_t *edits=SrPpuBgPacket_AllocateEdits(p,meta);assert(edits);
          Cell(edits,5,2,1,1u<<24);
          Cell(edits,7,0,0,(1u<<24)|(1u<<16));
        }
      }
    }
  }
  if(full_add)for(unsigned y=0;y<H;++y) {
    uint32_t *row=SrPpuBgPacket_AllocateFullAdd(p,y);assert(row);
    row[0]=1u|(1u<<(8+4)); /* BG1 full-add; native OBJ main enables math. */
    row[1]=0x7384;row[2]=W<<16;row[3]=0;
    for(unsigned x=0;x<W;++x)row[4+x]=((10u<<3)|4u)|(15u<<16);
  }
  assert(SrPpuBgPacket_Valid(p));
}

static unsigned PixelDifference(const SrPpuBgPacket *a,const SrPpuBgPacket *b) {
  unsigned count=0;
  for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x)
    count+=SrPpuBgPacket_Color(a,0,0,x,y)!=SrPpuBgPacket_Color(b,0,0,x,y);
  return count;
}

static void EqualPixelsAndSplit(const DioramaVoxelSourceCache *cache,const SrPpuBgPacket *p) {
  uint32_t a[PITCH*H],b[PITCH*H],rear_a[PITCH*H],rear_b[PITCH*H],front_a[PITCH*H],front_b[PITCH*H];
  memset(a,0x69,sizeof(a));memset(b,0x69,sizeof(b));
  memset(rear_a,0x53,sizeof(rear_a));memset(rear_b,0x53,sizeof(rear_b));
  memset(front_a,0xc7,sizeof(front_a));memset(front_b,0xc7,sizeof(front_b));
  for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x) {
    a[y*PITCH+x]=SrPpuBgPacket_Color(cache->packet,0,0,x,y);
    b[y*PITCH+x]=SrPpuBgPacket_Color(p,0,0,x,y);
    assert(a[y*PITCH+x]==b[y*PITCH+x]);++compared_pixels;
  }
  const DioramaVoxelSourceKey *k=&cache->key;
  const ActionRoomSceneFrameState frame={.brightness=k->brightness,.cgwsel=k->cgwsel,
    .cgadsub=k->cgadsub,.fixed_color=k->fixed_color};
  assert(ActionVoxel_Split(k->geometry,k->world_x,k->world_y,W,H,PITCH,&frame,a,PITCH,rear_a,front_a));
  assert(ActionVoxel_Split(k->geometry,k->world_x,k->world_y,W,H,PITCH,&frame,b,PITCH,rear_b,front_b));
  assert(!memcmp(rear_a,rear_b,sizeof(rear_a))&&!memcmp(front_a,front_b,sizeof(front_a)));
  unsigned foreground=0,rear=0;
  for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){foreground+=front_a[y*PITCH+x]!=0;rear+=rear_a[y*PITCH+x]!=0;}
  assert(foreground&&rear);
  /* The splitter must leave padding alone, even when the output stride differs
   * from the packet width. This catches a false cache-key pitch equivalence. */
  for(unsigned y=0;y<H;++y)for(unsigned x=W;x<PITCH;++x) {
    assert(rear_a[y*PITCH+x]==0x53535353u&&front_a[y*PITCH+x]==0xc7c7c7c7u);
  }
}

static void Expect(const DioramaVoxelSourceCache *cache,const SrPpuBgPacket *p,
    const DioramaVoxelSourceKey *key,bool match) {
  assert(DioramaVoxelSourceCache_Matches(cache,p,key)==match);
  if(match){++hits;EqualPixelsAndSplit(cache,p);}else ++misses;
}

static void Retain(DioramaVoxelSourceCache *cache,const SrPpuBgPacket *p,const DioramaVoxelSourceKey *key) {
  DioramaVoxelSourceCache_Retain(cache,p,key);
  assert(cache->valid&&cache->packet&&cache->packet!=p);
  Expect(cache,p,key,true);
}

static void Formats(DioramaVoxelSourceCache *cache,const DioramaVoxelSourceKey *key) {
  for(unsigned format=1;format<=4;++format) {
    Build(&original,format,false,false);Build(&candidate,format,true,false);
    Retain(cache,&original,key);
    DioramaVoxelSourceRowPlan rows;
    assert(!DioramaVoxelSourceCache_PlanRows(cache,&candidate,key,&rows));
    assert(SrPpuBgPacket_Meta(&original,0,0)[6]!=SrPpuBgPacket_Meta(&candidate,0,0)[6]);
    assert(SrPpuBgPacket_Meta(&original,0,0)[7]!=SrPpuBgPacket_Meta(&candidate,0,0)[7]);
    if(format==4)assert(SrPpuBgPacket_Meta(&original,0,0)[5]!=SrPpuBgPacket_Meta(&candidate,0,0)[5]);
    Expect(cache,&candidate,key,true);
    /* Producer allocation/palette hints and unrelated BG2 artwork do not
     * change the immutable BG1-low source used by ActionVoxel_Split. */
    candidate.request_flags^=SR_PPU_BG_PACKET_VALIDATE_TILES;candidate.last_palette[0]=0;
    uint32_t *bg2=SrPpuBgPacket_Meta(&candidate,1,1);
    bg2[1]^=0x00800000;candidate.words[bg2[6]+1]^=0x00008000;candidate.words[bg2[7]]^=1;
    Expect(cache,&candidate,key,true);
    for(unsigned mutation=0;mutation<6;++mutation) {
      CopyPacket(&candidate,&original);
      uint32_t *row=SrPpuBgPacket_Meta(&candidate,0,mutation==1?2:0);
      if(mutation<=1) {
        for(unsigned i=0;i<256;++i)candidate.words[row[6]+i]^=0x00808000;
      } else if(mutation==2)row[1]^=0x00008000;
      else if(mutation==3) {
        if(format>=3)row[4]^=1;
        else row[3]--;
      } else if(mutation==4) {
        if(format==1)candidate.words[row[7]+5]=2;
        else if(format==2)candidate.words[row[7]+2]|=1u<<5;
        else candidate.words[row[7]]^=1u<<4;
      } else {
        if(format==4)candidate.words[row[5]+2]|=1u<<7; /* transparent edited matte becomes opaque black */
        else if(format==3) {
          row[0]=4;uint32_t *edit=SrPpuBgPacket_AllocateEdits(&candidate,row);assert(edit);
          Cell(edit,5,0,0,(1u<<24)|1u);
        } else if(format==2)candidate.words[row[7]+2]^=1u<<1;
        else candidate.words[row[7]+1]=0x80000000u;
      }
      assert(SrPpuBgPacket_Valid(&candidate));Expect(cache,&candidate,key,false);
      /* Metadata-only backing-width changes can be conservative misses;
       * art, palette, phase and matte mutations visibly alter the source. */
      if(mutation!=2&&!(mutation==3&&format<3))assert(PixelDifference(&original,&candidate));
    }
    /* Retained storage is owned: subsequent producer overwrite cannot alter
     * the reference packet or turn changed artwork into a false hit. */
    CopyPacket(&candidate,&original);
    uint32_t *row=SrPpuBgPacket_Meta(&original,0,0);
    original.words[row[6]+2]^=0x00101010;
    Expect(cache,&candidate,key,true);Expect(cache,&original,key,false);
  }
}

static void FullAdd(DioramaVoxelSourceCache *cache,const DioramaVoxelSourceKey *key) {
  for(unsigned format=3;format<=4;++format) {
    Build(&original,format,false,true);Build(&candidate,format,true,true);
    Retain(cache,&original,key);Expect(cache,&candidate,key,true);
    assert(original.words[7]!=candidate.words[7]);
    for(unsigned mutation=0;mutation<7;++mutation) {
      CopyPacket(&candidate,&original);
      uint32_t *row=SrPpuBgPacket_FullAddRow(&candidate,1);
      uint32_t *bg2=SrPpuBgPacket_Meta(&candidate,1,1);
      if(mutation==0) { /* Native BG2 rank overtakes BG1 on the subscreen. */
        for(unsigned i=0;i<W/8+1;++i)candidate.words[bg2[7]+i*2+1]+=3u<<16;
      } else if(mutation==1) {
        bg2[0]=4;uint32_t *edit=SrPpuBgPacket_AllocateEdits(&candidate,bg2);assert(edit);
        Cell(edit+3,6,0,0,(1u<<24)|1u); /* Authored high band, rank 7. */
      } else if(mutation==2)row[4+6]=(row[4+6]&~255u)|((10u<<3)|2u); /* non-math native BG3 main winner */
      else if(mutation==3)row[0]^=1u<<(8+4);
      else if(mutation==4)row[3]^=1u;
      else if(mutation==5)candidate.words[candidate.words[7]+1]=0;
      else candidate.words[7]=0;
      assert(SrPpuBgPacket_Valid(&candidate));Expect(cache,&candidate,key,false);
      if(mutation<=3)assert(PixelDifference(&original,&candidate));
    }
  }
}

static void CheckRows(const DioramaVoxelSourceCache *cache,const SrPpuBgPacket *packet,
    const DioramaVoxelSourceKey *key,unsigned expected) {
  DioramaVoxelSourceRowPlan plan;memset(&plan,0xa5,sizeof(plan));
  assert(DioramaVoxelSourceCache_PlanRows(cache,packet,key,&plan));
  unsigned count=0;
  for(unsigned y=0;y<H;++y) {
    assert(!!plan.changed[y]==!!(expected&(1u<<y)));count+=!!plan.changed[y];
  }
  assert(plan.count==count);++row_plans;
  uint32_t previous[W*H],current[W*H];
  uint32_t rear[PITCH*H],front[PITCH*H],full_rear[PITCH*H],full_front[PITCH*H];
  memset(previous,0x69,sizeof(previous));memset(current,0x69,sizeof(current));
  memset(rear,0x53,sizeof(rear));memset(full_rear,0x53,sizeof(full_rear));
  memset(front,0xc7,sizeof(front));memset(full_front,0xc7,sizeof(full_front));
  for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x) {
    previous[y*W+x]=SrPpuBgPacket_Color(cache->packet,0,0,x,y);
    current[y*W+x]=SrPpuBgPacket_Color(packet,0,0,x,y);
    if(!plan.changed[y])assert(previous[y*W+x]==current[y*W+x]);
  }
  const ActionRoomSceneFrameState frame={.brightness=key->brightness,.cgwsel=key->cgwsel,
    .cgadsub=key->cgadsub,.fixed_color=key->fixed_color};
  assert(ActionVoxel_Split(key->geometry,key->world_x,key->world_y,W,H,PITCH,
      &frame,previous,W,rear,front));
  assert(ActionVoxel_Split(key->geometry,key->world_x,key->world_y,W,H,PITCH,
      &frame,current,W,full_rear,full_front));
  for(unsigned y=0;y<H;) {
    if(!plan.changed[y]){++y;continue;}
    const unsigned first=y;
    do {
      for(unsigned x=0;x<W;++x)previous[y*W+x]=SrPpuBgPacket_Color(packet,0,0,x,y);
      ++y;
    } while(y<H&&plan.changed[y]);
    assert(ActionVoxel_Split(key->geometry,key->world_x,key->world_y+first,W,y-first,
        PITCH,&frame,previous+first*W,W,rear+first*PITCH,front+first*PITCH));
    ++partial_spans;
  }
  /* Includes unchanged gaps and padding, not just dirty samples. */
  assert(!memcmp(previous,current,sizeof(previous)));
  assert(!memcmp(rear,full_rear,sizeof(rear))&&!memcmp(front,full_front,sizeof(front)));
}

static void FullAddRows(DioramaVoxelSourceCache *cache,const DioramaVoxelSourceKey *key) {
  for(unsigned format=3;format<=4;++format) {
    Build(&original,format,false,true);Build(&candidate,format,true,true);
    Retain(cache,&original,key);CheckRows(cache,&candidate,key,0);
    for(unsigned mutation=0;mutation<13;++mutation) {
      CopyPacket(&candidate,&original);unsigned mask=0;
      if(mutation==0) {
        SrPpuBgPacket_Meta(&candidate,0,0)[1]^=0x8000;mask=1;
      } else if(mutation==1) {
        uint32_t *row=SrPpuBgPacket_Meta(&candidate,0,H-1);
        candidate.words[row[7]]^=1;mask=1u<<(H-1);
      } else if(mutation==2) {
        for(unsigned y=1;y<=2;++y)SrPpuBgPacket_FullAddRow(&candidate,y)[0]^=1u<<(8+4);
        mask=6;
      } else if(mutation==3) {
        for(unsigned y=0;y<H;y+=H-1) {
          uint32_t *row=SrPpuBgPacket_Meta(&candidate,1,y);candidate.words[row[7]+1]^=1u<<16;
        }
        mask=9;
      } else if(mutation==4) {
        for(unsigned y=0;y<H;++y)SrPpuBgPacket_Meta(&candidate,0,y)[4]^=1;
        mask=15;
      } else if(mutation==5||mutation==6) {
        const unsigned source=mutation-5;uint32_t *row=SrPpuBgPacket_Meta(&candidate,source,0);
        assert(row[6]==SrPpuBgPacket_Meta(&candidate,source,1)[6]);
        /* A failed palette comparison must dirty every dependent row. */
        for(unsigned i=0;i<256;++i)candidate.words[row[6]+i]^=0x00808000;
        mask=3;
      } else if(mutation==7) {
        uint32_t *row=SrPpuBgPacket_Meta(&candidate,1,2);row[0]=4;
        uint32_t *edits=SrPpuBgPacket_AllocateEdits(&candidate,row);assert(edits);
        Cell(edits,7,3,1,(1u<<24)|1u);mask=4;
      } else if(mutation==8||mutation==9) {
        const unsigned y=mutation-7,x=mutation==8?0:W-1;
        SrPpuBgPacket_FullAddRow(&candidate,y)[4+x]^=1u<<18;mask=1u<<y;
      } else if(mutation==10) {
        candidate.words[candidate.words[7]]=0;mask=1;
      } else if(mutation==11) {
        SrPpuBgPacket_FullAddRow(&candidate,H-1)[2]^=1;mask=1u<<(H-1);
      } else {
        candidate.words[candidate.words[7]+2]=0;
        SrPpuBgPacket_Meta(&candidate,0,2)[7]=0;mask=4;
      }
      assert(SrPpuBgPacket_Valid(&candidate));CheckRows(cache,&candidate,key,mask);
    }
    /* Palette sharing can be nonadjacent and arena offsets can differ. */
    CopyPacket(&candidate,&original);
    SrPpuBgPacket_Meta(&candidate,0,3)[6]=SrPpuBgPacket_Meta(&candidate,0,0)[6];
    candidate.words[SrPpuBgPacket_Meta(&candidate,0,0)[6]+3]^=0x1000;
    CheckRows(cache,&candidate,key,11);
    /* A -> B -> B -> C compares with the last fully retained packet. */
    CopyPacket(&candidate,&original);SrPpuBgPacket_FullAddRow(&candidate,0)[3]^=1;
    CheckRows(cache,&candidate,key,1);Retain(cache,&candidate,key);CheckRows(cache,&candidate,key,0);
    SrPpuBgPacket_FullAddRow(&candidate,3)[3]^=1;
    CheckRows(cache,&candidate,key,8);Retain(cache,&candidate,key);CheckRows(cache,&candidate,key,0);
    for(unsigned mutation=0;mutation<3;++mutation) {
      CopyPacket(&candidate,&original);
      if(mutation==0)candidate.words[7]=0;
      else if(mutation==1)candidate.words[2]^=2;
      else candidate.owned_sources&=~1u;
      DioramaVoxelSourceRowPlan plan;
      assert(!DioramaVoxelSourceCache_PlanRows(cache,&candidate,key,&plan));
    }
  }
}

static void ChangeSharedPalette(SrPpuBgPacket *packet,unsigned source,
    unsigned first,unsigned end) {
  unsigned seen[H],count=0;
  for(unsigned y=0;y<H;++y) {
    const unsigned at=SrPpuBgPacket_Meta(packet,source,y)[6];
    bool prior=false;
    for(unsigned i=0;i<count;++i)prior|=seen[i]==at;
    if(prior)continue;
    seen[count++]=at;
    for(unsigned i=first;i<end;++i)packet->words[at+i]^=0x00808000;
  }
}

static void PaletteSpans(DioramaVoxelSourceCache *cache,const DioramaVoxelSourceKey *key) {
  for(unsigned format=2;format<=4;++format) {
    Build(&original,format==2?3:format,false,true);
    if(format==2)for(unsigned y=0;y<2;++y) {
      /* Cell rows may coexist with full-add tile rows elsewhere. Their own
       * full-add record is absent, as required by packet validation. */
      original.words[original.words[7]+y]=0;
      uint32_t *meta=SrPpuBgPacket_Meta(&original,0,y);
      meta[0]=2;meta[4]=meta[5]=meta[7]=0;
      uint32_t *cells=SrPpuBgPacket_AllocatePixels(&original,0,y);assert(cells);
      for(unsigned x=0;x<W;++x)Cell(cells+x/8*9,x,15,7,0);
    }
    /* Force a visible reference to the highest reachable palette entry. */
    uint32_t *meta=SrPpuBgPacket_Meta(&original,0,0);
    if(format>=3) {
      original.words[meta[7]]=UINT32_MAX;
      original.words[meta[7]+1]|=7u;
      if(format==4)Cell(original.words+meta[5],5,15,7,1u<<24);
    }
    assert(SrPpuBgPacket_Valid(&original));Retain(cache,&original,key);
    for(unsigned source=0;source<2;++source) {
      CopyPacket(&candidate,&original);ChangeSharedPalette(&candidate,source,128,256);
      /* Ordinary whole-source matching remains conservative. Only the
       * full-add row plan uses the exact format-specific reachable span. */
      Expect(cache,&candidate,key,false);assert(!PixelDifference(&original,&candidate));
      CheckRows(cache,&candidate,key,0);
      SrPpuBgPacket_FullAddRow(&candidate,2)[4+3]^=1u<<18;
      CheckRows(cache,&candidate,key,4); /* Native winner changes still count. */
    }
    CopyPacket(&candidate,&original);
    candidate.words[SrPpuBgPacket_Meta(&candidate,0,0)[6]+127]^=0x00808000;
    assert(PixelDifference(&original,&candidate));CheckRows(cache,&candidate,key,3);
  }
  for(unsigned source=0;source<2;++source) {
    Build(&original,3,false,true);
    const unsigned palette=SrPpuBgPacket_Meta(&original,source,0)[6];
    for(unsigned y=0;y<H;++y)SrPpuBgPacket_Meta(&original,source,y)[6]=palette;
    uint32_t *meta=SrPpuBgPacket_Meta(&original,source,1);
    meta[0]=1;meta[4]=meta[5]=meta[7]=0;
    uint32_t *pixels=SrPpuBgPacket_AllocatePixels(&original,source,1);assert(pixels);
    for(unsigned x=0;x<W;++x)pixels[x]=256; /* Format1 can read entry255. */
    original.words[original.words[7]+1]=0;
    assert(SrPpuBgPacket_Valid(&original));Retain(cache,&original,key);
    CopyPacket(&candidate,&original);candidate.words[palette+255]^=0x00808000;
    /* Shared offsets traverse128(true)->256(false)->128(true). Memoization
     * must neither conceal the middle mismatch nor dirty later tile rows. */
    Expect(cache,&candidate,key,false);CheckRows(cache,&candidate,key,2);
    if(!source)assert(PixelDifference(&original,&candidate));
  }
}

/* Optional real-art check of Castle backing and forest rear fills crossing
 * span boundaries. No renderer or native input is involved. */
static void AnchoredSplitRows(const char *rom_path) {
  FILE *file=fopen(rom_path,"rb");assert(file);
  assert(!fseek(file,0,SEEK_END));const long bytes=ftell(file);assert(bytes>0);rewind(file);
  uint8_t *rom=malloc((size_t)bytes);assert(rom);
  assert(fread(rom,1,(size_t)bytes,file)==(size_t)bytes);fclose(file);
  ActionRoomScene *scene=malloc(sizeof(*scene));assert(scene);
  enum {width=37,height=33,input_pitch=41,output_pitch=43};
  uint32_t low[input_pitch*height],rear[output_pitch*height],front[output_pitch*height];
  uint32_t sliced_rear[output_pitch*height],sliced_front[output_pitch*height];
  for(unsigned which=0;which<2;++which) {
    const unsigned group=which?1:2,room=which?1:2;
    assert(ActionRoomScene_Load(scene,rom,(size_t)bytes,group,room));
    ActionVoxelGeometry *geometry=ActionVoxel_Create();assert(geometry);
    assert(ActionVoxel_Build(geometry,scene,NULL,ActionVoxel_SettingsForRoom(group,room)));
    const int ox=which?1344:24,oy=which?624:464;
    const ActionRoomSceneFrameState frame={.brightness=11,.cgadsub=0x81,.fixed_color=0x421};
    for(unsigned source=0;source<2;++source)for(unsigned partition=0;partition<2;++partition) {
      for(unsigned i=0;i<input_pitch*height;++i)low[i]=source&&i%3?0xff030507:0;
      memset(rear,0x53,sizeof(rear));memset(sliced_rear,0x53,sizeof(sliced_rear));
      memset(front,0xc7,sizeof(front));memset(sliced_front,0xc7,sizeof(sliced_front));
      assert(ActionVoxel_Split(geometry,ox,oy,width,height,output_pitch,&frame,low,input_pitch,rear,front));
      if(which&&!source) {
        unsigned filled=0;
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)filled+=rear[y*output_pitch+x]!=0;
        assert(filled>128); /* Exercise actual rear fills, not two empty buffers. */
      }
      for(unsigned y=0;y<height;) {
        const unsigned length=partition?1:(y==0?15:y==15?2:height-y);
        assert(ActionVoxel_Split(geometry,ox,oy+y,width,length,output_pitch,&frame,
            low+y*input_pitch,input_pitch,sliced_rear+y*output_pitch,sliced_front+y*output_pitch));
        y+=length;
      }
      assert(!memcmp(rear,sliced_rear,sizeof(rear))&&!memcmp(front,sliced_front,sizeof(front)));
      ++anchored_splits;
    }
    ActionVoxel_Release(geometry);
  }
  free(scene);free(rom);
}

static void KeysAndFailures(DioramaVoxelSourceCache *cache,const DioramaVoxelSourceKey *key) {
  Build(&original,4,false,true);Retain(cache,&original,key);
  for(unsigned mutation=0;mutation<12;++mutation) {
    DioramaVoxelSourceKey changed=*key;
    switch(mutation) {
      case 0:changed.geometry=&candidate;break;
      case 1:++changed.revision;break;
      case 2:++changed.world_x;break;
      case 3:++changed.world_y;break;
      case 4:--changed.width;break;
      case 5:--changed.height;break;
      case 6:++changed.output_pitch;break;
      case 7:changed.brightness^=1;break;
      case 8:changed.cgwsel^=1;break;
      case 9:changed.cgadsub^=1;break;
      case 10:changed.fixed_color^=1;break;
      default:changed.revision=0;break;
    }
    Expect(cache,&original,&changed,false);
    DioramaVoxelSourceRowPlan plan;
    assert(!DioramaVoxelSourceCache_PlanRows(cache,&original,&changed,&plan));
  }
  for(unsigned mutation=0;mutation<9;++mutation) {
    CopyPacket(&candidate,&original);DioramaVoxelSourceKey changed=*key;
    if(mutation==0)candidate.owned_sources&=~1u;
    else if(mutation==1)candidate.words[2]&=~1u;
    else if(mutation==2)candidate.words[3]=SR_PPU_BG_PACKET_ARENA_BASE-1;
    else if(mutation==3)SrPpuBgPacket_Meta(&candidate,0,1)[6]=candidate.words[3];
    else if(mutation==4)SrPpuBgPacket_Meta(&candidate,0,1)[0]=0;
    else if(mutation==5)SrPpuBgPacket_Meta(&candidate,0,1)[5]=candidate.words[3];
    else if(mutation==6)changed.geometry=NULL;
    else if(mutation==7)changed.output_pitch=W-1;
    else changed.height=0;
    Expect(cache,&candidate,&changed,false);
    DioramaVoxelSourceRowPlan plan;
    assert(!DioramaVoxelSourceCache_PlanRows(cache,&candidate,&changed,&plan));
    DioramaVoxelSourceCache_Retain(cache,&candidate,&changed);assert(!cache->valid);
    Retain(cache,&original,key);
  }
  Expect(cache,NULL,key,false);
  DioramaVoxelSourceRowPlan plan;
  assert(!DioramaVoxelSourceCache_PlanRows(cache,NULL,key,&plan));
  DioramaVoxelSourceCache_Reset(cache);assert(!cache->valid&&!cache->packet&&!cache->key.geometry);
  assert(!DioramaVoxelSourceCache_PlanRows(cache,&original,key,&plan));
  Expect(cache,&original,key,false);DioramaVoxelSourceCache_Reset(cache);
}

int main(int argc,char **argv) {
  DioramaVoxelSourceCache cache={0};
  ActionVoxelGeometry *geometry=ActionVoxel_Create();assert(geometry);
  /* Build only through the public material contract. Alternating native
   * collision columns produce occupied foreground and open rear pixels. */
  static ActionRoomScene room;
  room.group=7;room.map=8;room.have_palette=true;room.have_character_bank[0]=true;
  room.bg[0].have_map=room.bg[0].have_metatiles=true;
  room.bg[0].pages_wide=room.bg[0].pages_high=1;room.bg[0].map_size=256;
  for(unsigned i=0;i<256;++i)room.bg[0].map[i]=(i+i/16)&1;
  for(unsigned quadrant=0;quadrant<4;++quadrant) {
    room.bg[0].metatiles[quadrant*2]=2;room.bg[0].metatiles[quadrant*2+1]=1;
  }
  for(unsigned y=0;y<8;++y)room.characters[32+y*2]=0xff;
  for(unsigned i=0;i<128;++i){room.palette[i*2]=0x1f;room.palette[i*2+1]=0x42;}
  /* The final arena authorizes transparent palette-4 backing, which can be
   * represented by all-zero character data without any ROM-derived artwork. */
  assert(ActionVoxel_Build(geometry,&room,NULL,ActionVoxel_SettingsForRoom(7,8)));
  assert(ActionVoxel_Active(geometry,7,8));
  const DioramaVoxelSourceKey key={.geometry=geometry,.revision=ActionVoxel_Revision(geometry),
    .world_x=17,.world_y=29,.width=W,.height=H,.output_pitch=PITCH,.brightness=13,
    .cgwsel=2,.cgadsub=0x41,.fixed_color=0x3946};
  Expect(&cache,&original,&key,false);
  Formats(&cache,&key);FullAdd(&cache,&key);FullAddRows(&cache,&key);
  PaletteSpans(&cache,&key);KeysAndFailures(&cache,&key);
  ActionVoxel_Release(geometry);
  if(argc==2)AnchoredSplitRows(argv[1]);
  printf("Voxel source cache: %u semantic hits, %u invalidations, %u decoded pixels plus rear/front split equivalence; formats 1–4, relocated arenas, HDMA, phase, mattes, full-add ownership, key fields and owned lifetime passed\n",hits,misses,compared_pixels);
  printf("Full-add rows: %u exact row plans, %u partial spans, %u world-anchored real-art splits passed\n",
      row_plans,partial_spans,anchored_splits);
  return 0;
}
