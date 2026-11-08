/* Waterfall rock cap seams: derive the missing wall from the two already
 * emitted horizontal cap strips, independently of the compiler's contour
 * admission helper. Raw IDs are fixture locations, never runtime policy. */
static uint64_t WetCrownHash(uint64_t h,const void *data,size_t size) {
  const unsigned char *p=data;for(size_t i=0;i<size;++i)h=(h^p[i])*UINT64_C(1099511628211);return h;
}
static unsigned WetCrownPixel(const ActionVoxelGeometry *g,int x,int y) {
  x-=g->origin_x;y-=g->origin_y;
  return x>=0 && y>=0 && x<g->width && y<g->height?g->front[(size_t)y*g->width+x]:0;
}
static bool WetCrownSoft(const ActionVoxelGeometry *g,int x,int y) {
  x-=g->origin_x;y-=g->origin_y;
  return g->soft_surfaces && x>=0 && y>=0 && x<g->width && y<g->height &&
      g->soft_surfaces[(size_t)(y/16)*(g->width/16)+x/16];
}
static void WetCrownTerrain(unsigned room,unsigned region,DioramaRoomOverride *terrain) {
  static DioramaRoomOverride manifest;DioramaLayerOrder_ClearRoom(&manifest);
  DioramaLayerOrder_ClearRoom(terrain);char section[32],line[4096];bool active=false,found=false;
  snprintf(section,sizeof(section),"[layers:04:%02x]",room);
  FILE *f=fopen("diorama-layers.ini","r");assert(f);
  while(fgets(line,sizeof(line),f)) {
    line[strcspn(line,"\r\n")]=0;char *p=line;while(*p==' ' || *p=='\t')++p;
    size_t n=strlen(p);while(n && (p[n-1]==' ' || p[n-1]=='\t'))p[--n]=0;
    if(*p=='['){active=!strcmp(p,section);found|=active;continue;}
    if(!active || !*p || *p=='#' || *p==';')continue;
    const char *error=NULL;assert(DioramaLayerOrder_ParseLine(&manifest,p,&error));
  }
  assert(found && !ferror(f));fclose(f);
  assert(DioramaLayerOrder_ForTerrain(&manifest,region,terrain));
  DioramaLayerOrder_ClearRoom(&manifest);
}
static unsigned WetCrownEdgesAt(const ActionVoxelGeometry *g,int x,int y,unsigned side,
    const VoxelEdge **found) {
  unsigned count,matched=0;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edges[i];
    if(e->side==side && e->x==x && y>=e->y && y<e->y+e->length && e->far_rank==2 && !e->near_rank) {
      ++matched;if(found)*found=e;
    }
  }
  return matched;
}
static unsigned WetCrownValidate(const ActionVoxelGeometry *g,unsigned region,unsigned room,
    const char *label,bool baseline) {
  unsigned np,ne;const VoxelPatch *profiles=ActionVoxel_Patches(g,&np);
  const VoxelEdge *edges=ActionVoxel_Edges(g,&ne);unsigned expected=0,pixels=0,missing=0,cross_cell=0;
  uint64_t front_hash=WetCrownHash(UINT64_C(14695981039346656037),g->front,(size_t)g->width*g->height);
  uint64_t top_hash=UINT64_C(14695981039346656037),profile_hash=top_hash;
  for(unsigned i=0;i<ne;++i)if(!(edges[i].side&1)) {
    VoxelEdge e=edges[i];e.top_owned_art=0;top_hash=WetCrownHash(top_hash,&e,sizeof(e));
  }
  for(unsigned i=0;i<np;++i) {
    VoxelPatch p=profiles[i];p.surface_first=0;
    profile_hash=WetCrownHash(profile_hash,&p,sizeof(p));
  }
  unsigned surface_count;const VoxelProfileSurface *surfaces=ActionVoxel_ProfileSurfaces(g,&surface_count);
  for(unsigned i=0;i<surface_count;++i) {
    const int position[]={surfaces[i].x,surfaces[i].y};
    profile_hash=WetCrownHash(profile_hash,position,sizeof(position));
    const uint16_t *art=ActionVoxel_AuthoredArt(g,surfaces[i].art_id);assert(art);
    profile_hash=WetCrownHash(profile_hash,art,256*sizeof(*art));
  }
  for(unsigned pi=0;pi<np;++pi) {
    const VoxelPatch *p=&profiles[pi];if(p->kind!=kVoxelPatch_SideProfile)continue;
    const VoxelEdge **tops=calloc(p->width,sizeof(*tops));assert(tops);
    for(unsigned i=0;i<ne;++i) {
      const VoxelEdge *e=&edges[i];
      if(e->side || e->far_rank!=2 || e->near_rank || e->high_priority!=p->high_priority ||
          e->y<p->y || e->y>=p->y+p->height)continue;
      for(int x=e->x;x<e->x+e->length;++x)
        if(x>=p->x && x<p->x+p->width && (!tops[x-p->x] || e->y<tops[x-p->x]->y))tops[x-p->x]=e;
    }
    for(int column=1;column<p->width;++column) {
      const VoxelEdge *left=tops[column-1],*right=tops[column];if(!left || !right)continue;
      const int delta=left->y-right->y;if(!delta || abs(delta)>2)continue;
      const unsigned side=delta<0?1:3;
      const VoxelEdge *cap=delta<0?left:right;
      const int x=p->x+column-(side==1),nx=x+(side==1?1:-1),top=cap->y,bottom=top+abs(delta);
      if(!WetCrownSoft(g,x,top) || !WetCrownSoft(g,nx,bottom))continue;
      /* The native wet face is opaque rock plus a separate foam fringe.
       * Follow the two emitted cap strips through that original foam ink,
       * but never bridge an actual alpha hole or a shallow scene object. */
      const unsigned lower_owner=WetCrownPixel(g,x,bottom)&kVoxelKindMask;
      bool supported=(lower_owner==kVoxelSolid || lower_owner==kVoxelFoliage) &&
          (WetCrownPixel(g,nx,bottom)&kVoxelKindMask)==kVoxelSolid;
      for(int y=top;y<bottom;++y) {
        const unsigned a=WetCrownPixel(g,x,y),b=WetCrownPixel(g,nx,y),kind=a&kVoxelKindMask;
        supported&=(kind==kVoxelSolid || kind==kVoxelFoliage) &&
            (b&kVoxelKindMask)==kVoxelFoliage &&
            !!(a&kVoxelHighPriority)==!!p->high_priority &&
            !!(b&kVoxelHighPriority)==!!p->high_priority;
      }
      if(!supported)continue;
      ++expected;cross_cell+=(x/16)!=(nx/16);
      if(!region && !strcmp(label,"native") &&
          ((room==3 && p->x==80 && p->y==128) || (room==2 && p->x==896 && p->y==496)))
        printf("wet-crown witness room=04%02u owner=%d,%d side=%u height=%d donor=%d,%d\n",
            room,x,top,side,bottom-top,cap->top_sample_x,cap->top_sample_y);
      for(int y=top;y<bottom;++y) {
        const VoxelEdge *e=NULL;const unsigned found=WetCrownEdgesAt(g,x,y,side,&e);++pixels;
        assert(found<=1);if(!found){++missing;continue;}
        assert(e->high_priority==cap->high_priority && e->soft_surface && e->surface_top_material);
        assert(e->length<=2 && (!e->fold_length || e->fold_length<=2));
        assert(e->surface_texture && e->top_sample_size==16 && e->top_owned_art);
        assert(cap->surface_texture && cap->top_sample_size==16 && cap->top_owned_art);
        assert(e->top_sample_x==cap->top_sample_x && e->top_sample_y==cap->top_sample_y);
        assert(!memcmp(ActionVoxel_AuthoredArt(g,e->top_owned_art-1),
            ActionVoxel_AuthoredArt(g,cap->top_owned_art-1),256*sizeof(uint16_t)));
        VoxelEdgeSpan span;assert(ActionVoxel_ResolveEdgeSpan(g,e,kVoxelEdgeNative,0,ActionVoxel_Depth(g),&span));
        assert(fabsf(span.near_depth)<.000001f && fabsf(span.far_depth-ActionVoxel_Depth(g))<.000001f);
      }
    }
    /* Every retained interior profile wall must be part of an exterior cap
     * join. In particular no full panel, bottom scallop or internal hole is
     * re-extruded. This also bounds any unexpectedly broad implementation. */
    for(unsigned i=0;i<ne;++i) {
      const VoxelEdge *e=&edges[i];if(!(e->side&1) || e->far_rank!=2 ||
          e->x<p->x || e->x>=p->x+p->width || e->y<p->y || e->y>=p->y+p->height)continue;
      const int column=e->x-p->x,neighbor=column+(e->side==1?1:-1);
      assert(neighbor>=0 && neighbor<p->width && tops[column] && tops[neighbor]);
      assert(e->y==tops[column]->y && tops[neighbor]->y>e->y &&
          tops[neighbor]->y-e->y<=2 && e->y+e->length<=tops[neighbor]->y);
    }
    free(tops);
  }
  assert(ne<kVoxelMaxEdges && !g->surface_stats.edge_budget_fallbacks && !g->surface_stats.art_budget_fallbacks);
  printf("wet-crown region=%u room=04%02u mode=%s edges=%u expected=%u pixels=%u missing=%u cross_cell=%u front=%016llx horizontal=%016llx profiles=%016llx\n",
      region,room,label,ne,expected,pixels,missing,cross_cell,(unsigned long long)front_hash,
      (unsigned long long)top_hash,(unsigned long long)profile_hash);
  fflush(stdout);
  if(baseline)assert(missing>0);else assert(!missing);
  return expected;
}
static void WetCrownStampCases(const ActionRoomScene *s,ActionVoxelGeometry *g) {
  static DioramaRoomOverride terrain;DioramaTileStamp stamps[3]={{0}};
  uint16_t source_words[2][4];const unsigned ids[]={0x4e,0x4f,0x21};
  for(unsigned i=0;i<3;++i) {
    int tx,ty;assert(FixtureCell(s,ids[i],&tx,&ty));stamps[i].metatile=ids[i];
    for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(s,1,tx+(q&1),ty+(q>>1),&stamps[i].words[q],NULL));
    if(i<2)memcpy(source_words[i],stamps[i].words,sizeof(stamps[i].words));
  }
  ActionVoxelGeometry *plain=ActionVoxel_Create();assert(plain);
  for(unsigned flipped=0;flipped<2;++flipped)for(unsigned negative=0;negative<2;++negative) {
    memset(&terrain,0,sizeof(terrain));const int x=negative?-2:2,y=negative?-3:2;
    terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,.x0=x,.y0=y,.x1=x+3,.y1=y+1,.count=3,.cells=stamps};
    for(unsigned i=0;i<3;++i){stamps[i].x=x+i;stamps[i].y=y;}
    for(unsigned i=0;i<2;++i)for(unsigned q=0;q<4;++q)
      stamps[i].words[q]=source_words[flipped?1-i:i][flipped?q^1:q]^(flipped?0x4000:0);
    assert(ActionVoxel_Build(g,s,&terrain,ActionVoxel_SettingsForRoom(4,3)));
    assert(WetCrownValidate(g,0,3,flipped?"hflip-stamp":"negative-stamp",false));
    if(!flipped && !negative)assert(ActionVoxel_Build(plain,s,&terrain,ActionVoxel_SettingsForRoom(4,3)));
    if(!flipped && negative) {
      assert(g->edge_count==plain->edge_count);
      for(unsigned i=0;i<g->edge_count;++i) {
        VoxelEdge a=g->edges[i],b=plain->edges[i];a.x+=64;a.y+=80;
        if(a.surface_texture){a.top_sample_x+=64;a.top_sample_y+=80;}
        if(a.fold_length)a.fold_origin_y+=80;a.top_owned_art=b.top_owned_art=0;
        assert(!memcmp(&a,&b,sizeof(a)));
      }
    }
  }
  /* The native crown stays unchanged when a hole is erased below it. A
   * whole-column erase or a deliberately deep notch must not be bridged. */
  memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
  DioramaPixelEdit *pixel=&terrain.pixel_layers[0].edits[0];pixel->x=5;pixel->y=8;pixel->by_cell=true;
  pixel->transparent[8]=1u<<(15-5);
  assert(ActionVoxel_Build(g,s,&terrain,ActionVoxel_SettingsForRoom(4,3)));
  assert(WetCrownValidate(g,0,3,"internal-hole",false));
  assert(!WetCrownEdgesAt(g,84,136,1,NULL) && !WetCrownEdgesAt(g,86,136,3,NULL));
  memset(pixel->transparent,0,sizeof(pixel->transparent));
  for(unsigned y=0;y<16;++y)pixel->transparent[y]=1u<<(15-1);
  assert(ActionVoxel_Build(g,s,&terrain,ActionVoxel_SettingsForRoom(4,3)));
  assert(!WetCrownEdgesAt(g,80,129,1,NULL) && !WetCrownEdgesAt(g,82,129,3,NULL));
  memset(pixel->transparent,0,sizeof(pixel->transparent));
  for(unsigned y=0;y<5;++y)pixel->transparent[y]=1u<<(15-1);
  assert(ActionVoxel_Build(g,s,&terrain,ActionVoxel_SettingsForRoom(4,3)));
  assert(!WetCrownEdgesAt(g,82,129,3,NULL)); /* This edge exists before the deep notch. */
  /* Actual remaining two-pixel slit: native4E atx89 is rock/foam/rock,
   * while its next column starts rock aty130. Keep all original face foam,
   * and close the cap behind it; explicit deletion still opens the gap. */
  memset(&terrain,0,sizeof(terrain));
  assert(ActionVoxel_Build(g,s,&terrain,ActionVoxel_SettingsForRoom(4,3)));
  assert((WetCrownPixel(g,89,129)&kVoxelKindMask)==kVoxelFoliage);
  const VoxelEdge *foam_edge=NULL;assert(WetCrownEdgesAt(g,89,128,1,&foam_edge)==1);
  assert(foam_edge->y==128 && foam_edge->length==2 && foam_edge->surface_top_material);
  uint16_t foam_art[256];memcpy(foam_art,ActionVoxel_AuthoredArt(g,foam_edge->top_owned_art-1),sizeof(foam_art));
  assert(WetCrownEdgesAt(g,89,129,1,NULL)==1);
  terrain.pixel_layers[0].count=1;pixel=&terrain.pixel_layers[0].edits[0];
  *pixel=(DioramaPixelEdit){.by_cell=true,.x=5,.y=8};pixel->transparent[1]=1u<<(15-9);
  assert(ActionVoxel_Build(g,s,&terrain,ActionVoxel_SettingsForRoom(4,3)));
  assert(!WetCrownEdgesAt(g,89,128,1,NULL) && !WetCrownEdgesAt(g,89,129,1,NULL));
  pixel->transparent[1]=0;pixel->black[1]=1u<<(15-9);
  assert(ActionVoxel_Build(g,s,&terrain,ActionVoxel_SettingsForRoom(4,3)));
  /* Black is opaque recoloring, not erasure. Preserve that native front,
   * while its cap still uses the same reviewed rock and original mask. */
  assert((WetCrownPixel(g,89,129)&kVoxelKindMask)==kVoxelFoliage);
  assert(WetCrownEdgesAt(g,89,128,1,&foam_edge)==1 && foam_edge->length==2);
  assert(!memcmp(foam_art,ActionVoxel_AuthoredArt(g,foam_edge->top_owned_art-1),sizeof(foam_art)));
  const ActionRoomSceneFrameState frame={.brightness=15};uint32_t low[256],rear[256],front[256];
  DesertNativeLow(s,10,16,low);low[25]=UINT32_C(0xff000000);
  assert(ActionVoxel_Split(g,80,128,16,16,16,&frame,low,16,rear,front) && front[25]==low[25]);
  unsigned ns;bool masked=false;const VoxelProfileSurface *skins=ActionVoxel_ProfileSurfaces(g,&ns);
  for(unsigned i=0;i<ns;++i)if(skins[i].x==80 && skins[i].y==128) {
    assert(!ActionVoxel_AuthoredArt(g,skins[i].art_id)[25]);masked=true;
  }
  assert(masked);
  /* Existing edited ownership remains responsible for moved/hidden/painted
   * cells; no native helper may change those records or their neighbor. */
  memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
  DioramaVoxelCell *cell=&terrain.voxel.cells[0];
  *cell=(DioramaVoxelCell){.x=5,.y=8,.fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back|kDioramaVoxelCell_Width,
      .front=.03f,.back=.15f,.width=12};
  assert(ActionVoxel_Build(g,s,&terrain,ActionVoxel_SettingsForRoom(4,3)));
  const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(g,5,8);assert(a && a->surface && a->width==12);
  for(unsigned q=0;q<4;++q)assert(a->front[q]==.03f && a->back[q]==.15f);
  *cell=(DioramaVoxelCell){.x=5,.y=8,.fields=kDioramaVoxelCell_Faces,.faces=0};
  assert(ActionVoxel_Build(g,s,&terrain,ActionVoxel_SettingsForRoom(4,3)));
  a=ActionVoxel_AuthoredCellAt(g,5,8);assert(a && !a->faces);
  cell->fields=0;assert(ActionVoxel_Build(g,s,&terrain,ActionVoxel_SettingsForRoom(4,3)));
  assert(WetCrownValidate(g,0,3,"reset",false));
  ActionVoxel_Release(plain);
}
static void WetCrownRiserFixtures(const char *directory) {
  if(!directory)return;const bool baseline=getenv("AR_VOXEL_WET_CROWN_BASELINE")!=NULL;
  static ActionSceneSnapshot snapshot;static ActionRoomScene source,renumbered;
  static unsigned char bytes[kActionSceneSnapshotMaxBytes];static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region)for(unsigned room=2;room<=3;++room) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/04%02x.arscene",directory,region,room);
    FILE *f=fopen(path,"rb");assert(f);const size_t size=fread(bytes,1,sizeof(bytes),f);assert(!ferror(f));fclose(f);
    assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));source=snapshot.scene;
    ActionVoxelGeometry *g=ActionVoxel_Create(),*copy=ActionVoxel_Create();assert(g && copy);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(4,room);
    assert(ActionVoxel_Build(g,&source,NULL,settings));
    assert(WetCrownValidate(g,region,room,"native",baseline));RockFixtureValidate(&source,g);
    assert(!memcmp(&source,&snapshot.scene,sizeof(source)));
    if(!baseline) {
      renumbered=source;Renumber(&renumbered);assert(ActionVoxel_Build(copy,&renumbered,NULL,settings));SameGeometry(g,copy);
      if(!region && room==3)WetCrownStampCases(&source,copy);
    }
    WetCrownTerrain(room,region,&terrain);assert(ActionVoxel_Build(copy,&source,&terrain,settings));
    WetCrownValidate(copy,region,room,"shipped",baseline);DioramaLayerOrder_ClearRoom(&terrain);
    ActionVoxel_Release(g);ActionVoxel_Release(copy);
  }
}
