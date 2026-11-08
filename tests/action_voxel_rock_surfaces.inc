/* Fixture IDs identify native art for the oracle only. Runtime contracts use
 * reviewed fingerprints and family membership, including edited stamp words. */
static bool RockFixtureFoam(unsigned id) {
  return id==0x4e || id==0x4f || id==0xf4 || id==0xf6 || id==0xfc || id==0xfe;
}
static bool RockFixtureDonor(const ActionRoomScene *s,int x,int y) {
  const unsigned donors[]={0x21,0x22,0x24,0x25};
  for(unsigned i=0;i<sizeof(donors)/sizeof(*donors);++i) {
    int tx,ty;if(!FixtureCell(s,donors[i],&tx,&ty))continue;
    for(unsigned flip=0;flip<4;++flip) {
      bool same=true;
      for(unsigned py=0;py<16 && same;++py)for(unsigned px=0;px<16 && same;++px) {
        const unsigned sx=(flip&1)?15-px:px,sy=(flip&2)?15-py:py;
        uint16_t actual,reference;uint8_t id;
        assert(ActionRoomScene_LookupTile(s,1,(x+px)/8,(y+py)/8,&actual,&id));
        assert(ActionRoomScene_LookupTile(s,1,tx+sx/8,ty+sy/8,&reference,NULL));
        const unsigned q=(py/8)*2+px/8,rq=(sy/8)*2+sx/8;
        const uint8_t *a=&s->bg[0].metatiles[id*8+q*2],*b=&s->bg[0].metatiles[donors[i]*8+rq*2];
        same=!((actual^reference)&0x3c00) && !!(a[0]&2)==!!(b[0]&2) &&
            FixturePixel(s,x+px,y+py)==FixturePixel(s,tx*8+sx,ty*8+sy);
      }
      if(same)return true;
    }
  }
  return false;
}
static unsigned RockFixtureWordPixel(const ActionRoomScene *s,unsigned word,unsigned x,unsigned y) {
  if(word&0x4000)x=7-x;if(word&0x8000)y=7-y;
  const uint8_t *c=s->characters+(word&1023)*32;const unsigned bit=7-x;
  return ((c[y*2]>>bit)&1)|(((c[y*2+1]>>bit)&1)<<1)|
      (((c[y*2+16]>>bit)&1)<<2)|(((c[y*2+17]>>bit)&1)<<3);
}
static void RockFixtureNegativeBoundary(const ActionRoomScene *source,ActionVoxelGeometry *g) {
  const unsigned crowns[]={0x1a,0x1b,0x1e,0x1f};unsigned short_risers=0;
  for(unsigned crown=0;crown<sizeof(crowns)/sizeof(*crowns);++crown) {
    DioramaTileStamp stamps[2]={{0}};static DioramaRoomOverride terrain;
    for(unsigned i=0;i<2;++i) {
      int tx,ty;assert(FixtureCell(source,i?crowns[crown]:0x21,&tx,&ty));
      stamps[i].metatile=i?crowns[crown]:0x21;
      for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(source,1,tx+(q&1),ty+(q>>1),&stamps[i].words[q],NULL));
    }
    VoxelEdge *positive=NULL;unsigned positive_count=0;
    for(unsigned negative=0;negative<2;++negative) {
      const int x=negative?-2:2,y=negative?-1:2;
      memset(&terrain,0,sizeof(terrain));
      terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,
          .x0=x,.y0=y,.x1=x+2,.y1=y+1,.cells=stamps,.count=2};
      for(unsigned i=0;i<2;++i){stamps[i].x=x+i;stamps[i].y=y;}
      assert(ActionVoxel_Build(g,source,&terrain,ActionVoxel_SettingsForRoom(4,1)));
      unsigned count;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
      if(!negative) {
        positive=malloc(count*sizeof(*positive));assert(positive);
        memcpy(positive,edges,count*sizeof(*positive));positive_count=count;continue;
      }
      assert(count==positive_count);
      for(unsigned i=0;i<count;++i) {
        const VoxelEdge *e=&edges[i],*match=NULL;
        for(unsigned n=0;n<positive_count;++n)if(positive[n].x==e->x+64 && positive[n].y==e->y+48 &&
            positive[n].side==e->side && positive[n].length==e->length && positive[n].near_rank==e->near_rank &&
            positive[n].far_rank==e->far_rank && positive[n].high_priority==e->high_priority) {match=&positive[n];break;}
        assert(match && e->surface_top_material==match->surface_top_material);
        if(e->surface_top_material) {
          assert(e->surface_texture && e->top_sample_size==16 && e->top_owned_art);
          assert(e->top_sample_x+64==match->top_sample_x && e->top_sample_y+48==match->top_sample_y);
          ++short_risers;
        }
      }
    }
    free(positive);
  }
  assert(short_risers); /* Exercise negative sub-cell pixels, not only aligned origins. */
}
static void RockFixtureShippedTerrain(const ActionRoomScene *source,unsigned region,ActionVoxelGeometry *g) {
  static DioramaRoomOverride manifest,resolved;char line[4096],section[40];
  snprintf(section,sizeof(section),"[layers:%02x:%02x]",source->group,source->map);
  FILE *file=fopen("diorama-layers.ini","r");assert(file);bool active=false,found=false;
  while(fgets(line,sizeof(line),file)) {
    line[strcspn(line,"\r\n")]=0;char *body=line;
    while(*body==' ' || *body=='\t')++body;
    size_t length=strlen(body);while(length && (body[length-1]==' ' || body[length-1]=='\t'))body[--length]=0;
    if(body[0]=='['){active=!strncmp(body,section,strlen(section));found|=active;continue;}
    if(!active || !body[0] || body[0]=='#' || body[0]==';')continue;
    const char *error=NULL;const bool ok=DioramaLayerOrder_ParseLine(&manifest,body,&error);
    if(!ok)fprintf(stderr,"shipped terrain parse: %s (%s)\n",body,error?error:"unknown");
    assert(ok);
  }
  assert(!ferror(file) && found);fclose(file);
  assert(DioramaLayerOrder_ForTerrain(&manifest,region,&resolved));
  assert(ActionVoxel_Build(g,source,&resolved,ActionVoxel_SettingsForRoom(source->group,source->map)));
  int cx=51,cy=47;
  if(source->map!=1) {int tx,ty;assert(FixtureCell(source,0x4e,&tx,&ty));cx=tx/2;cy=ty/2;}
  unsigned count,matched=0;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edges[i];
    if(e->side || e->x/16<cx || e->x/16>cx+1 || e->y/16!=cy)continue;
    assert(e->surface_texture && e->top_sample_size==16 && e->top_owned_art);++matched;
    if(source->map==3 && region<2) {
      const DioramaTileStamp *stamp=DioramaLayerOrder_StampAt(&resolved.stamp_layers[0],e->top_sample_x/16,e->top_sample_y/16);
      assert(stamp && stamp->x==1 && stamp->y==3);
      const uint16_t *pixels=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(pixels);
      for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
        const unsigned word=stamp->words[(y/8)*2+x/8],p=RockFixtureWordPixel(source,word,x&7,y&7);
        const unsigned at=(((word>>10)&7)*16+p)*2;
        assert(p && pixels[y*16+x]==(uint16_t)(0x8000|source->palette[at]|source->palette[at+1]<<8));
      }
    }
  }
  if(source->map==1 || source->map==3)assert(matched);
  if(source->map==3 && region<2) {
    const DioramaTileStamp *stamp=DioramaLayerOrder_StampAt(&resolved.stamp_layers[0],1,3);
    assert(stamp && stamp->metatile==0x25 && stamp->words[0]==0x505a);
  }
  DioramaLayerOrder_ClearRoom(&resolved);DioramaLayerOrder_ClearRoom(&manifest);
}
static void RockFixtureValidate(const ActionRoomScene *source,const ActionVoxelGeometry *g) {
  unsigned count,pinned_count,profile_count,art_count,cap_count=0,masked=0,foam=0;
  int donor_cache[256];for(unsigned i=0;i<256;++i)donor_cache[i]=-1;
  const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  const uint16_t *pinned=ActionVoxel_AuthoredCapArt(g,&pinned_count);
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edges[i];uint16_t word;uint8_t id,donor;
    assert(ActionRoomScene_LookupTile(source,1,e->x/8,e->y/8,&word,&id));
    if(e->side || e->far_rank!=2 || !(RockFixtureFoam(id) || id==0x1a || id==0x1b || id==0x1e || id==0x1f))continue;
    assert(e->surface_texture && e->top_sample_size==16 && e->top_owned_art);
    assert(ActionRoomScene_LookupTile(source,1,e->top_sample_x/8,e->top_sample_y/8,&word,&donor));
    if(donor_cache[donor]<0)donor_cache[donor]=RockFixtureDonor(source,e->top_sample_x,e->top_sample_y);
    assert(donor_cache[donor]);
    assert(abs(e->x/16-e->top_sample_x/16)+abs(e->y/16-e->top_sample_y/16)<=40);
    ++cap_count;
  }
  assert(cap_count);
  const VoxelPatch *profiles=ActionVoxel_Patches(g,&profile_count);
  const VoxelProfileSurface *art=ActionVoxel_ProfileSurfaces(g,&art_count);
  for(unsigned i=0;i<profile_count;++i) {
    const VoxelPatch *p=&profiles[i];if(!p->surface_count)continue;
    assert(p->kind==kVoxelPatch_SideProfile && p->surface_first<=art_count &&
        p->surface_count<=art_count-p->surface_first);
    for(unsigned n=0;n<p->surface_count;++n) {
      const VoxelProfileSurface a=art[p->surface_first+n];
      const uint16_t *pixels=ActionVoxel_AuthoredArt(g,a.art_id);assert(pixels);
      bool found=false;for(unsigned j=0;j<pinned_count;++j)found|=pinned[j]==a.art_id;
      assert(found && !(a.x&15) && !(a.y&15));
      for(int y=0;y<16;++y)for(int x=0;x<16;++x) {
        const int wx=a.x+x,wy=a.y+y;uint16_t word;uint8_t id;
        assert(ActionRoomScene_LookupTile(source,1,wx/8,wy/8,&word,&id));
        const unsigned native=FixturePixel(source,wx,wy);
        const bool inside=wx>=p->x && wx<p->x+p->width && wy>=p->y && wy<p->y+p->height;
        const bool structural=inside && native && !(RockFixtureFoam(id) && native==7);
        assert(!!(pixels[y*16+x]&0x8000)==structural);
        if(structural) {
          const unsigned at=(((word>>10)&7)*16+native)*2;
          assert(pixels[y*16+x]==(uint16_t)(0x8000|source->palette[at]|source->palette[at+1]<<8));++masked;
        } else if(inside && native==7)++foam;
      }
    }
  }
  if(source->map>=2)assert(masked && foam && art_count);
  else assert(!art_count);
}
static void RockSurfaceFixtures(const char *directory) {
  if(!directory)return;
  static ActionSceneSnapshot snapshot;static ActionRoomScene original,renamed;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region)for(unsigned room=1;room<=3;++room) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/04%02x.arscene",directory,region,room);
    FILE *file=fopen(path,"rb");assert(file);const size_t size=fread(bytes,1,sizeof(bytes),file);
    assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));original=snapshot.scene;
    ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(4,room);
    assert(ActionVoxel_Build(g,&original,NULL,settings));RockFixtureValidate(&original,g);
    renamed=original;Renumber(&renamed);assert(ActionVoxel_Build(g,&renamed,NULL,settings));RockFixtureValidate(&original,g);
    if(room==3) {
      int tx,ty;assert(FixtureCell(&original,0x4e,&tx,&ty));
      assert(ActionVoxel_Build(g,&original,NULL,settings));
      uint32_t native[256],rear[256],front[256];DesertNativeLow(&original,tx,ty,native);
      const ActionRoomSceneFrameState frame={.brightness=15};
      assert(ActionVoxel_Split(g,tx*8,ty*8,16,16,16,&frame,native,16,rear,front));
      for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
        if(FixturePixel(&original,tx*8+x,ty*8+y)==7)assert(front[y*16+x]==native[y*16+x]);
      unsigned native_count;const VoxelPatch *native_profiles=ActionVoxel_Patches(g,&native_count);
      VoxelPatch edited_profile={0};
      for(unsigned i=0;i<native_count;++i)if(native_profiles[i].kind==kVoxelPatch_SideProfile &&
          tx*8>=native_profiles[i].x && tx*8<native_profiles[i].x+native_profiles[i].width &&
          ty*8>=native_profiles[i].y && ty*8<native_profiles[i].y+native_profiles[i].height)edited_profile=native_profiles[i];
      assert(edited_profile.surface_compiled && edited_profile.surface_count);
      const unsigned fields[]={kDioramaVoxelCell_Width,kDioramaVoxelCell_Front,
          kDioramaVoxelCell_Back,kDioramaVoxelCell_Faces,kDioramaVoxelCell_SideArt};
      for(unsigned test=0;test<sizeof(fields)/sizeof(*fields);++test) {
        memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
        DioramaVoxelCell *edit=&terrain.voxel.cells[0];
        *edit=(DioramaVoxelCell){.x=tx/2,.y=ty/2,.fields=fields[test],.width=12,
            .front=.03f,.back=.15f,.faces=kDioramaVoxelFaceMask_Top};
        if(fields[test]==kDioramaVoxelCell_SideArt) {
          edit->side_art=(DioramaVoxelArt){.size=16,.set=true};
          for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(&original,1,tx+(q&1),ty+(q>>1),&edit->side_art.words[q],NULL));
        }
        assert(ActionVoxel_Build(g,&original,&terrain,settings));
        unsigned count;const VoxelPatch *patches=ActionVoxel_Patches(g,&count);bool found=false;
        for(unsigned i=0;i<count;++i)if(patches[i].kind==kVoxelPatch_SideProfile &&
            tx*8>=patches[i].x && tx*8<patches[i].x+patches[i].width && ty*8>=patches[i].y && ty*8<patches[i].y+patches[i].height) {
          assert(patches[i].surface_count);found=true;
          unsigned art_count;const VoxelProfileSurface *art=ActionVoxel_ProfileSurfaces(g,&art_count);
          assert(patches[i].surface_first+patches[i].surface_count<=art_count);
          for(unsigned n=0;n<patches[i].surface_count;++n) {
            const VoxelProfileSurface *cell=&art[patches[i].surface_first+n];
            assert(cell->x!=tx*8 || cell->y!=ty*8);
          }
        }
        assert(found);
        const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(g,tx/2,ty/2);assert(a && a->surface);
        if(fields[test]==kDioramaVoxelCell_SideArt)assert(!(a->automatic_art&((1u<<4)|(1u<<5))));
        RockFixtureValidate(&original,g); /* Unedited neighbor masks/colors stay exact. */
      }
      memset(&terrain,0,sizeof(terrain));
      for(int y=edited_profile.y/16;y<(edited_profile.y+edited_profile.height+15)/16;++y)
        for(int x=edited_profile.x/16;x<(edited_profile.x+edited_profile.width+15)/16;++x) {
          assert(terrain.voxel.cell_count<kDioramaVoxelCellMax);
          terrain.voxel.cells[terrain.voxel.cell_count++]=(DioramaVoxelCell){.x=x,.y=y,.fields=kDioramaVoxelCell_Faces,.faces=0};
        }
      assert(ActionVoxel_Build(g,&original,&terrain,settings));
      unsigned hidden_count;const VoxelPatch *hidden_profiles=ActionVoxel_Patches(g,&hidden_count);bool hidden=false;
      for(unsigned i=0;i<hidden_count;++i)if(hidden_profiles[i].x==edited_profile.x && hidden_profiles[i].y==edited_profile.y) {
        assert(hidden_profiles[i].surface_compiled && !hidden_profiles[i].surface_count);hidden=true;
      }
      assert(hidden);
      /* The room has one reviewed donor. H/V stamp transforms must retain
       * its family and publish the exact displayed donor orientation. */
      int dx,dy;assert(FixtureCell(&original,0x21,&dx,&dy));
      DioramaTileStamp stamp={.x=dx/2,.y=dy/2};uint16_t words[4],word;
      assert(ActionRoomScene_LookupTile(&original,1,dx,dy,&word,&stamp.metatile));
      for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(&original,1,dx+(q&1),dy+(q>>1),&words[q],NULL));
      for(unsigned flip=1;flip<=2;++flip) {
        memset(&terrain,0,sizeof(terrain));terrain.stamp_layers[0].count=1;terrain.stamp_layers[0].cells=&stamp;
        for(unsigned q=0;q<4;++q)stamp.words[q]=words[q^flip]^(flip==1?0x4000:0x8000);
        assert(ActionVoxel_Build(g,&original,&terrain,settings));unsigned count,matched=0;
        const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
        for(unsigned i=0;i<count;++i)if(!edges[i].side && edges[i].x/16==tx/2 && edges[i].y/16==ty/2) {
          const VoxelEdge *e=&edges[i];assert(e->surface_texture && e->top_sample_size==16);
          const uint16_t *pixels=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(pixels);
          for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
            const unsigned sx=flip==1?15-x:x,sy=flip==2?15-y:y;
            assert(ActionRoomScene_LookupTile(&original,1,dx+sx/8,dy+sy/8,&word,NULL));
            const unsigned p=FixturePixel(&original,dx*8+sx,dy*8+sy),at=(((word>>10)&7)*16+p)*2;
            assert(pixels[y*16+x]==(uint16_t)(0x8000|original.palette[at]|original.palette[at+1]<<8));
          }
          ++matched;
        }
        assert(matched);
      }
      /* Masking the sole donor must invalidate its cache and safely retain
       * the old cap sampling. Clearing edits restores the reviewed family. */
      memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
      terrain.pixel_layers[0].edits[0]=(DioramaPixelEdit){.by_cell=true,.x=dx/2,.y=dy/2};
      terrain.pixel_layers[0].edits[0].transparent[0]=0x8000;
      assert(ActionVoxel_Build(g,&original,&terrain,settings));unsigned count;
      const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
      for(unsigned i=0;i<count;++i)if(!edges[i].side && edges[i].x/16==tx/2 && edges[i].y/16==ty/2)
        assert(!edges[i].surface_texture);
      assert(ActionVoxel_Build(g,&original,NULL,settings));RockFixtureValidate(&original,g);
    }
    if(room==1)RockFixtureNegativeBoundary(&original,g);
    RockFixtureShippedTerrain(&original,region,g);
    assert(!memcmp(&original,&snapshot.scene,sizeof(original)));ActionVoxel_Release(g);
    printf("rock surfaces04/%02x terrain%u: reviewed caps, native masked sides, remapping and editor precedence passed\n",room,region);
  }
}
