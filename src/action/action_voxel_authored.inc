/* Bounded, room-owned native artwork for editor-authored surfaces. This is
 * compiled after semantic classification; no camera-dependent donor sampling. */
static unsigned AuthoredPixel(const ActionRoomScene *s,unsigned word,int x,int y) {
  const unsigned tile = word & 1023;
  if (tile < 0x200) return Pixel(s, word, x, y);
  const unsigned extra = tile - 0x200;
  if (extra >= kActionVoxelExtraCharacters || !s->have_extra_characters) return 0;
  return ActionRoomScene_Decode4bppPixel(
      s->extra_characters + extra * kActionVoxelCharacterBytes, word, x, y);
}
static uint16_t AuthoredArtPixel(const ActionRoomScene *s,const uint16_t palette[128],
    const DioramaVoxelArt *art,int x,int y) {
  x=art->x+x%art->size;y=art->y+y%art->size;
  const unsigned word=art->words[(y/8)*2+x/8];
  const unsigned pixel=AuthoredPixel(s,word,x&7,y&7);
  return pixel?0x8000|palette[((word>>10)&7)*16+pixel]:0;
}
static uint16_t AuthoredRgb555(uint32_t argb) {
  return argb>>24 ? 0x8000|((argb>>19)&31)|((argb>>6)&992)|((argb<<7)&31744) : 0;
}
/* Bounded image interning keeps a whole-room rectangle proportional to unique
 * native artwork, not seven RGBA images per cell. */
enum { kAuthoredHashSlots=16384 };
static bool InternAuthoredArt(ActionVoxelGeometry *c,uint32_t *slots,
    const uint16_t pixels[256],uint32_t *id) {
  uint64_t hash=UINT64_C(14695981039346656037);
  for(unsigned i=0;i<256;++i)hash=(hash^pixels[i])*UINT64_C(1099511628211);
  unsigned slot=(unsigned)hash&(kAuthoredHashSlots-1);
  while(slots[slot]) {
    const unsigned candidate=slots[slot]-1;
    if(!memcmp(c->authored_art[candidate],pixels,256*sizeof(*pixels))){*id=candidate;return true;}
    slot=(slot+1)&(kAuthoredHashSlots-1);
  }
  if(c->authored_art_count==kVoxelAuthoredArtMax){c->build_error=2;return false;}
  if(c->authored_art_count==c->authored_art_capacity) {
    unsigned capacity=c->authored_art_capacity?c->authored_art_capacity*2:256;
    if(capacity>kVoxelAuthoredArtMax)capacity=kVoxelAuthoredArtMax;
    void *next=realloc(c->authored_art,(size_t)capacity*sizeof(*c->authored_art));
    if(!next){c->build_error=3;return false;}
    c->authored_art=next;c->authored_art_capacity=capacity;
  }
  *id=c->authored_art_count++;slots[slot]=*id+1;
  memcpy(c->authored_art[*id],pixels,256*sizeof(*pixels));return true;
}
static bool BuildAuthoredContours(ActionVoxelGeometry *c) {
  for(unsigned i=0;i<c->authored_count;++i) {
    VoxelAuthoredCell *a=&c->authored[i];a->contour_start=c->authored_contour_count;
    if(!a->surface)continue;
    for(unsigned side=0;side<4;++side)for(int row=0;row<16;++row)for(int col=0;col<16;) {
      const int x=side&1?row:col,y=side&1?col:row;
      if(!ActionVoxel_AuthoredContourExposed(c,a,x,y,side)){++col;continue;}
      int end=col+1;
      while(end<16 && end/8==col/8 &&
          ActionVoxel_AuthoredContourExposed(c,a,side&1?row:end,side&1?end:row,side))++end;
      if(c->authored_contour_count==kVoxelAuthoredContourMax){c->build_error=5;return false;}
      if(c->authored_contour_count==c->authored_contour_capacity) {
        unsigned capacity=c->authored_contour_capacity?c->authored_contour_capacity*2:1024;
        if(capacity>kVoxelAuthoredContourMax)capacity=kVoxelAuthoredContourMax;
        void *next=realloc(c->authored_contours,(size_t)capacity*sizeof(*c->authored_contours));
        if(!next){c->build_error=3;return false;}
        c->authored_contours=next;c->authored_contour_capacity=capacity;
      }
      c->authored_contours[c->authored_contour_count++]=(VoxelAuthoredContour){x,y,end-col,side};
      ++a->contour_count;col=end;
    }
  }
  return true;
}
static bool KeepOwnedArt(ActionVoxelGeometry *c,bool used[kVoxelAuthoredArtMax],unsigned id) {
  if(used[id])return true;
  if(!c->authored_cap_art) {
    c->authored_cap_art=malloc(kVoxelAuthoredArtMax*sizeof(*c->authored_cap_art));
    if(!c->authored_cap_art){c->build_error=3;return false;}
  }
  c->authored_cap_art[c->authored_cap_art_count++]=id;used[id]=true;return true;
}
static const VoxelAuthoredCell *AuthoredPixelOwner(const ActionVoxelGeometry *c,int x,int y) {
  return ActionVoxel_AuthoredCellAt(c,(int)floorf(x/16.f),(int)floorf(y/16.f));
}

static uint16_t NativeArtColor(const ActionRoomScene *scene, const uint16_t palette[128],
    const CastleMaterial *material, const DioramaPixelEdit *edit, unsigned x, unsigned y) {
  const unsigned bit = 1u << (15 - x);
  if (edit && (edit->transparent[y] & bit)) return 0;
  if (edit && (edit->black[y] & bit)) return 0x8000;
  if (material->cutouts[y] & bit) return 0;
  const unsigned word = material->words[(y / 8) * 2 + x / 8];
  const unsigned pixel = AuthoredPixel(scene, word, x & 7, y & 7);
  return pixel ? 0x8000 | palette[((word >> 10) & 7) * 16 + pixel] : 0;
}

/* A patch covers at most four metatiles. Resolve each material and its paint
 * masks once, even when the patch crosses a tile boundary. */
static bool SampleNativeArtPatch(const ActionRoomScene *scene, const DioramaRoomOverride *terrain,
    const DioramaStampLayerOverride *stamps, const CastleMaterial native[256],
    const uint16_t palette[128], const uint16_t *tiles, size_t pitch,
    int ox, int oy, int size, bool require_full_patch, uint16_t pixels[256], bool *priority) {
  memset(pixels, 0, 256 * sizeof(*pixels));
  if (priority) memset(priority, 0, 256 * sizeof(*priority));
  for (int y0 = 0; y0 < size;) {
    const int wy = oy + y0, cy = (int)floorf(wy / 16.f);
    int y1 = y0 + 16 - (wy & 15);
    if (y1 > size) y1 = size;
    for (int x0 = 0; x0 < size;) {
      const int wx = ox + x0, cx = (int)floorf(wx / 16.f);
      int x1 = x0 + 16 - (wx & 15);
      if (x1 > size) x1 = size;
      CastleMaterial material;
      uint8_t metatile;
      if (LookupMaterial(scene, stamps, native, cx, cy, &material, &metatile, tiles, pitch)) {
        const DioramaPixelEdit *edit = DioramaLayerOrder_PixelEditAt(terrain, 0, cx, cy, metatile);
        for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
          const unsigned mx = (ox + x) & 15, my = (oy + y) & 15;
          const unsigned at = y * 16 + x;
          pixels[at] = NativeArtColor(scene, palette, &material, edit, mx, my);
          if (priority) priority[at] = (material.words[(my / 8) * 2 + mx / 8] & 0x2000) != 0;
        }
      } else if (require_full_patch) return false;
      x0 = x1;
    }
    y0 = y1;
  }
  return true;
}

/* Native affine faces keep the reviewed split and its source priority, but
 * own those pixels so moving them never samples outside the captured view. */
static bool NativeSourcePatch(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaRoomOverride *terrain,const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256],const uint16_t palette[128],const uint16_t *tiles,
    size_t pitch,int ox,int oy,bool high,bool rear_source,uint16_t pixels[256]) {
  uint32_t source[256],front[256],rear[256];uint16_t original[256];bool priority[256];
  SampleNativeArtPatch(s, terrain, stamps, native, palette, tiles, pitch, ox, oy, 16,
      false, original, priority);
  for (unsigned i = 0; i < 256; ++i)
    source[i] = priority[i] ? 0 : ActionVoxel_AuthoredColor(original[i], NULL);
  /* This private build has not published pixel pointers yet. Request only
   * its native split, never a partially compiled authored replacement. */
  const unsigned authored_count=c->authored_count;c->authored_count=0;
  const ActionRoomSceneFrameState frame={.brightness=15};
  const bool ok=ActionVoxel_Split(c,ox,oy,16,16,16,&frame,source,16,rear,front);
  c->authored_count=authored_count;if(!ok)return false;
  for(unsigned i=0;i<256;++i)pixels[i]=rear_source?AuthoredRgb555(rear[i]):
      high?(priority[i]?original[i]:0):AuthoredRgb555(front[i]);
  return true;
}
static bool PartitionAuthoredEdges(ActionVoxelGeometry *c) {
  VoxelEdge *edges=malloc(sizeof(c->edges));if(!edges){c->build_error=3;return false;}
  unsigned n=0;
  for(unsigned i=0;i<c->edge_count;++i) {
    const VoxelEdge e=c->edges[i];
    for(int start=0;start<e.length;) {
      const VoxelAuthoredCell *a=AuthoredPixelOwner(c,e.x+((e.side&1)?0:start),e.y+((e.side&1)?start:0));
      const bool owned=a && a->surface,transformed=a && a->native_transform;
      int end=start+1;
      while(end<e.length) {
        const VoxelAuthoredCell *b=AuthoredPixelOwner(c,e.x+((e.side&1)?0:end),e.y+((e.side&1)?end:0));
        if((b && b->surface)!=owned || (b && b->native_transform)!=transformed ||
            (transformed && (b!=a || end-start==8)))break;
        ++end;
      }
      if(!owned) {
        if(n==kVoxelMaxEdges){free(edges);c->build_error=5;return false;}
        edges[n]=e;edges[n].x+=(e.side&1)?0:start;edges[n].y+=(e.side&1)?start:0;
        if(transformed && (e.side&1) && (start || end<e.length)) {
          edges[n].fold_origin_y=e.y;edges[n].fold_length=e.length;
        }
        edges[n++].length=end-start;
      }
      start=end;
    }
  }
  memcpy(c->edges,edges,n*sizeof(*edges));free(edges);c->edge_count=n;
  memset(c->pass_counts,0,sizeof(c->pass_counts));
  for(unsigned i=0;i<n;++i) {
    const VoxelEdge *e=&c->edges[i];const unsigned list=e->high_priority?1:0;
    c->pass_edges[list][c->pass_counts[list]++]=i;
    if(e->rear_trim)c->pass_edges[2][c->pass_counts[2]++]=i;
    if(e->high_priority && e->colliding && e->side==0 && e->far_rank>=2)c->pass_edges[3][c->pass_counts[3]++]=i;
  }
  return true;
}
static bool SameGroupTransform(const DioramaVoxelTransform *a,const DioramaVoxelTransform *b) {
  return a->px==b->px && a->py==b->py && a->pz==b->pz &&
      a->dx==b->dx && a->dy==b->dy && a->dz==b->dz &&
      a->sx==b->sx && a->sy==b->sy && a->sz==b->sz;
}
/* Native faces can only move as one complete semantic object. Validate final
 * covering ownership after every member has compiled, so a local authored
 * face still participates and record order cannot conceal a missing member. */
static bool ValidateNativeTransformGroups(ActionVoxelGeometry *c) {
  bool requested=false;
  for(unsigned i=0;i<c->authored_count && !requested;++i)requested=c->authored[i].native_transform_requested;
  if(!requested)return true;
  const int columns=c->width/16,rows=c->height/16,ox=c->origin_x/16,oy=c->origin_y/16;
  unsigned char *checked=calloc((size_t)columns*rows,1);
  VoxelGroupCell *members=malloc(kVoxelSemanticGroupMaxCells*sizeof(*members));
  if(!checked || !members){free(checked);free(members);c->build_error=3;return false;}
  bool ok=true;
  for(unsigned i=0;ok && i<c->authored_count;++i) {
    const VoxelAuthoredCell *a=&c->authored[i];if(!a->native_transform_requested)continue;
    const int x=a->x/16,y=a->y/16;
    if(checked[(size_t)(y-oy)*columns+x-ox])continue;
    VoxelSemanticGroup group;
    if(ActionVoxel_SemanticGroup(c,x,y,&group,members,kVoxelSemanticGroupMaxCells)!=kActionVoxelGroup_Complete) {
      c->build_error=9;ok=false;break;
    }
    for(unsigned j=0;j<group.count;++j) {
      const VoxelGroupCell cell=members[j];
      checked[(size_t)(cell.y-oy)*columns+cell.x-ox]=1;
      const VoxelAuthoredCell *b=ActionVoxel_AuthoredCellAt(c,cell.x,cell.y);
      if(!b || !(b->native_transform_requested || b->surface) ||
          !SameGroupTransform(&a->transform,&b->transform)) {c->build_error=9;ok=false;break;}
      if(b->native_transform_requested && ActionVoxel_TransformUnsupportedReason(c,cell.x,cell.y)) {
        c->build_error=11;ok=false;break;
      }
    }
  }
  free(checked);free(members);return ok;
}
/* A semantic cap can sample another tile's native artwork (the two halves
 * of a stump share one clean grain square). Keep that donor alive when its
 * original flat tile is hidden by an authored replacement. This does not
 * copy the edited front artwork onto neighbouring automatic geometry. */
static bool BuildOwnedCapArt(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaRoomOverride *terrain,const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256],const uint16_t palette[128],
    const uint16_t *tiles,size_t pitch,uint32_t *slots) {
  bool used[kVoxelAuthoredArtMax]={0};
  for(unsigned i=0;i<c->edge_count;++i) {
    VoxelEdge *e=&c->edges[i];const int size=e->top_sample_size;
    const VoxelAuthoredCell *owner=AuthoredPixelOwner(c,e->x,e->y);
    const bool transformed=owner && owner->native_transform;
    if(transformed)for(unsigned rear=0;rear<2;++rear) {
      e->source_x[rear]=e->x-(e->side==1?4:0);
      e->source_y[rear]=e->y-(e->side==2?4:0);
      if(rear && !e->side && e->rear_fold_top)e->source_y[rear]=e->y-8;
      uint16_t pixels[256];uint32_t id;
      if(!NativeSourcePatch(c,s,terrain,stamps,native,palette,tiles,pitch,
          e->source_x[rear],e->source_y[rear],e->high_priority,rear,pixels) ||
          !InternAuthoredArt(c,slots,pixels,&id) || !KeepOwnedArt(c,used,id))return false;
      e->source_owned_art[rear]=id+1;
    }
    if(e->side || size<2 || size>16)continue;
    bool hidden=transformed,survives=false;
    for(int y=0;y<size && !hidden;++y)for(int x=0;x<size;++x)
      hidden|=ActionVoxel_AuthoredSurfaceAt(c,e->top_sample_x+x,e->top_sample_y+y);
    if(!hidden)continue;
    for(int x=0;x<e->length && !survives;++x)
      survives=!ActionVoxel_AuthoredSurfaceAt(c,e->x+x,e->y);
    if(!survives)continue;
    uint16_t pixels[256];
    if (!SampleNativeArtPatch(s, terrain, stamps, native, palette, tiles, pitch,
        e->top_sample_x, e->top_sample_y, size, true, pixels, NULL)) {
      c->build_error = 4;
      return false;
    }
    if (size < 16) for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x)
      pixels[y * 16 + x] = pixels[(y % size) * 16 + x % size];
    uint32_t id;if(!InternAuthoredArt(c,slots,pixels,&id))return false;
    e->top_owned_art=id+1;
    if(!KeepOwnedArt(c,used,id))return false;
  }
  return true;
}
static bool BuildAuthoredCells(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaRoomOverride *terrain,const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256],const uint16_t palette[128],
    const uint16_t *tiles,size_t pitch,const ActionVoxelAuthoredLayout *layout) {
  if (!layout || !layout->count) return true;
  const int columns = layout->columns, rows = layout->rows;
  c->authored_at = calloc((size_t)columns * rows, sizeof(*c->authored_at));
  if (!c->authored_at) { c->build_error = 3; return false; }
  c->authored=calloc(layout->count,sizeof(*c->authored));
  uint32_t *slots=calloc(kAuthoredHashSlots,sizeof(*slots));
  /* One resolved donor image per record/face. UINT32_MAX means automatic. */
  uint32_t (*donors)[kDioramaVoxelArtSlotCount]=malloc((size_t)terrain->voxel.cell_count*sizeof(*donors));
  if(!c->authored || !slots || !donors){free(slots);free(donors);c->build_error=3;return false;}
  bool ok=true;
  for(unsigned i=0;ok && i<terrain->voxel.cell_count;++i) {
    const DioramaVoxelCell *e=&terrain->voxel.cells[i];
    for (unsigned face = 0; face < kDioramaVoxelArtSlotCount; ++face) {
      donors[i][face]=UINT32_MAX;
      const DioramaVoxelArt *art = layout->records[i].used ? AuthoredFaceArt(e, face) : NULL;
      if (!art) continue;
      if((art->size!=8 && art->size!=16) || art->x+art->size>16 ||
          art->y+art->size>16){c->build_error=4;ok=false;break;}
      uint16_t pixels[256];for(int y=0;y<16;++y)for(int x=0;x<16;++x)
        pixels[y*16+x]=AuthoredArtPixel(s,palette,art,x,y);
      if(!InternAuthoredArt(c,slots,pixels,&donors[i][face])){ok=false;break;}
    }
  }
  unsigned count = 0;
  for(int row=0;ok && row<rows;++row)for(int col=0;ok && col<columns;++col) {
    const unsigned record = layout->owners[row * columns + col];
    if(!record)continue;
    const DioramaVoxelCell *edit=&terrain->voxel.cells[record-1];
    const VoxelAuthoredRecord *resolved = &layout->records[record - 1];
    const unsigned fields = resolved->fields;
    if(!fields)continue;
    if((fields&kDioramaVoxelCell_Transform) && (fields&(kDioramaVoxelCell_Backing|kDioramaVoxelCell_BackArt)) &&
        !(fields&~(kDioramaVoxelCell_Transform|kDioramaVoxelCell_Faces|kDioramaVoxelCell_Backing|kDioramaVoxelCell_BackArt))) {
      c->build_error=10;ok=false;break;
    }
    const int cx = layout->x + col, cy = layout->y + row;
    VoxelCellGeometry shape;CastleMaterial m;uint8_t metatile;
    if(!ActionVoxel_CellGeometry(c,cx,cy,&shape) ||
        !LookupMaterial(s,stamps,native,cx,cy,&m,&metatile,tiles,pitch)) {
      c->build_error=4;ok=false;break;
    }
    VoxelAuthoredCell *a=&c->authored[count];a->x=cx*16;a->y=cy*16;
    a->width=edit->fields&kDioramaVoxelCell_Width?edit->width:shape.width;
    a->offset_x=edit->fields&kDioramaVoxelCell_Offset?edit->offset_x:shape.offset_x;
    a->surface=!!(fields&~(kDioramaVoxelCell_Backing|kDioramaVoxelCell_BackArt));
    a->faces=edit->fields&kDioramaVoxelCell_Faces?edit->faces:kDioramaVoxelFaceMask_All;
    /* Hiding only the original front must not remodel native stepped sides,
     * curved trunks or caps. Keep their captured artwork available and omit
     * the front's projected cells instead of erasing the shared texture. */
    a->native_front_hidden = !resolved->owns_artwork &&
        (!resolved->native_identity || !(a->faces & kDioramaVoxelFaceMask_Front));
    a->native_transform_requested = resolved->native_transform;
    a->native_transform = resolved->native_transform && !resolved->native_identity;
    if(a->native_transform_requested)a->surface=false;
    if(a->native_transform){a->surface=false;a->native_front_hidden=true;++c->native_transform_count;}
    if(a->native_front_hidden){a->surface=false;++c->hidden_native_front_count;}
    a->shape=edit->fields&kDioramaVoxelCell_Shape?edit->shape:kDioramaVoxelShape_Box;
    a->shape_y=edit->shape_h?edit->shape_y:edit->y*16;
    a->shape_h=edit->shape_h?edit->shape_h:DioramaVoxelCell_Height(edit)*16;
    if(edit->fields&kDioramaVoxelCell_Bend) {
      a->bend=edit->bend;
      if((a->bend.axis!=kDioramaVoxelBend_X && a->bend.axis!=kDioramaVoxelBend_Y) ||
          a->bend.anchor==a->bend.tip || a->bend.anchor < -8192 || a->bend.anchor > 8192 ||
          a->bend.tip < -8192 || a->bend.tip > 8192 ||
          !isfinite(a->bend.dx) || !isfinite(a->bend.dy) || !isfinite(a->bend.dz) ||
          fabsf(a->bend.dx)>256 || fabsf(a->bend.dy)>256 || fabsf(a->bend.dz)>1.2f) {
        c->build_error=4;ok=false;break;
      }
    }
    if(edit->fields&kDioramaVoxelCell_Transform) {
      if(!ActionVoxel_TransformValid(&edit->transform)){c->build_error=4;ok=false;break;}
      a->transform=edit->transform;
    }
    for(unsigned q=0;q<4;++q) {
      a->front[q]=edit->fields&kDioramaVoxelCell_Front?edit->front:shape.front[q];
      a->back[q]=edit->fields&kDioramaVoxelCell_Back?edit->back:shape.back[q];
      a->front_end[q]=edit->fields&kDioramaVoxelCell_FrontEnd?edit->front_end:a->back[q];
      a->back_end[q]=edit->fields&kDioramaVoxelCell_BackEnd?edit->back_end:a->back[q];
      a->priority[q]=!!(m.words[q]&0x2000);
      a->native_actor_priority[q]=c->native_priority_decks &&
          (c->native_priority_decks[row*columns+col]&(1u<<q));
      if(!isfinite(a->front[q]) || !isfinite(a->back[q]) || !isfinite(a->front_end[q]) ||
          !isfinite(a->back_end[q]) || a->shape>kDioramaVoxelShape_Curve ||
          !(a->width>=1 && a->width<=64) || !isfinite(a->offset_x)) {ok=false;c->build_error=4;break;}
      a->back[q]=fmaxf(a->front[q],a->back[q]);
      a->back_end[q]=fmaxf(a->front_end[q],a->back_end[q]);
    }
    if(!ok)break;
    const DioramaPixelEdit *pixel_edit=DioramaLayerOrder_PixelEditAt(terrain,0,cx,cy,metatile);
    uint16_t face_pixels[256],back_pixels[256];uint32_t source[256],rear[256],front[256];
    const uint32_t front_donor = donors[record - 1][0];
    for(int y=0;y<16;++y)for(int x=0;x<16;++x) {
      const unsigned at=y*16+x,q=(y/8)*2+x/8;
      const uint16_t original = NativeArtColor(s, palette, &m, pixel_edit, x, y);
      source[at]=a->priority[q]?0:ActionVoxel_AuthoredColor(original,NULL);
      /* Explicit front artwork owns its silhouette. Native authored pixel
       * masks still apply, while an automatic matte must not erase new art. */
      face_pixels[at] = original;
      if (front_donor != UINT32_MAX) {
        const unsigned bit = 1u << (15 - x);
        uint16_t color = c->authored_art[front_donor][at];
        if (pixel_edit && (pixel_edit->black[y] & bit)) color = 0x8000;
        if (pixel_edit && (pixel_edit->transparent[y] & bit)) color = 0;
        face_pixels[at] = color;
      }
    }
    const ActionRoomSceneFrameState frame={.brightness=15};
    if(!ActionVoxel_Split(c,a->x,a->y,16,16,16,&frame,source,16,rear,front)){ok=false;break;}
    /* A native face with a reviewed baked-shadow repair must retain those
     * repaired pixels when it becomes editor-owned. Explicit face painting
     * still supplies its own complete artwork and bypasses this fallback. */
    if(donors[record-1][0]==UINT32_MAX)
      for(unsigned i=0;i<c->rear_fill_count;++i) {
        const VoxelRearFill *repair=&c->rear_fills[i];
        if(!repair->replace_front || repair->x!=a->x || repair->y!=a->y)continue;
        for(unsigned at=0;at<256;++at)
          if(repair->colors[at]&0x8000)face_pixels[at]=AuthoredRgb555(front[at]);
      }
    if(a->native_transform)for(unsigned at=0;at<256;++at)
      if(!a->priority[(at/16/8)*2+(at%16/8)])face_pixels[at]=AuthoredRgb555(front[at]);
    if(!InternAuthoredArt(c,slots,face_pixels,&a->art_id[0])){ok=false;break;}
    a->native_front_art=donors[record-1][0]==UINT32_MAX;
    for(unsigned face=1;face<7;++face)if(face!=3) {
      a->art_id[face]=donors[record-1][face];
      /* Auto and an omitted directional donor both inherit shared sides. */
      if(a->art_id[face]==UINT32_MAX && face>=4)a->art_id[face]=donors[record-1][2];
      if(a->art_id[face]==UINT32_MAX) {
        a->art_id[face]=a->art_id[0];a->automatic_art|=1u<<face;
      }
    }
    for(int y=0;y<16;++y)for(int x=0;x<16;++x) {
      const unsigned at=y*16+x,q=(y/8)*2+x/8;
      bool owned_rear=false;
      if((edit->fields&kDioramaVoxelCell_Transform) && !resolved->native_identity &&
          !(fields&(kDioramaVoxelCell_Backing|kDioramaVoxelCell_BackArt))) {
        const int mx=a->x+x-c->origin_x,my=a->y+y-c->origin_y;
        const size_t world_at=(size_t)my*c->width+mx;
        const unsigned material=c->front[world_at],kind=material&kVoxelKindMask;
        const bool branch=c->branch_pixels && (c->branch_pixels[world_at>>3]&(1u<<(world_at&7)));
        const bool curved_bark=kind==kVoxelForeground && (material&kVoxelRearCopy) && c->trunk_cells &&
            c->trunk_cells[(size_t)(my/8)*(c->width/8)+mx/8];
        const bool modeled=kind==kVoxelSolid || kind==kVoxelThin || kind==kVoxelFoliage || kind==kVoxelForeground;
        owned_rear=c->profile!=kActionVoxelProfile_Castle && !a->priority[q] && modeled &&
            !branch && !curved_bark && (kind==kVoxelSolid || (material&kVoxelRearCopy));
        if(owned_rear)a->native_rear_rows[y]|=1u<<(15-x);
      }
      uint16_t backing=owned_rear?AuthoredRgb555(source[at]):a->native_transform?0:AuthoredRgb555(rear[at]);
      const unsigned mode=edit->fields&kDioramaVoxelCell_Backing?edit->backing:kDioramaVoxelBacking_Auto;
      const uint16_t replacement=donors[record-1][3]!=UINT32_MAX?c->authored_art[donors[record-1][3]][at]:
          0x8000|c->backing_color[((a->y+y)&31)*16+((a->x+x)&15)];
      if(mode==kDioramaVoxelBacking_None || !(a->faces&kDioramaVoxelFaceMask_Back))backing=0;
      else if(mode==kDioramaVoxelBacking_Replace || (mode==kDioramaVoxelBacking_Fill && !(backing&0x8000)))backing=replacement;
      float depth,back;ActionVoxel_AuthoredDepths(a,q,a->y+y+.5f,&depth,&back);
      if(a->surface && (a->faces&kDioramaVoxelFaceMask_Front) && fabsf(depth-ActionVoxel_Depth(c))<.00001f &&
          (face_pixels[at]&0x8000))backing=0;
      back_pixels[at]=backing;
    }
    for(unsigned y=0;y<16;++y)if(a->native_rear_rows[y]){++c->transformed_rear_count;break;}
    if(!InternAuthoredArt(c,slots,back_pixels,&a->art_id[3])){ok=false;break;}
    c->authored_at[row*columns+col]=++count;
  }
  c->authored_count=count;
  if(ok)ok=ValidateNativeTransformGroups(c);
  if(ok)ok=PartitionAuthoredEdges(c);
  if(ok)ok=BuildOwnedCapArt(c,s,terrain,stamps,native,palette,tiles,pitch,slots);
  free(slots);free(donors);if(!ok)return false;
  /* Pool reallocations have finished; publish stable borrowed pixel pointers. */
  for(unsigned i=0;i<count;++i)for(unsigned face=0;face<7;++face)
    c->authored[i].pixels[face]=c->authored_art[c->authored[i].art_id[face]];
  if(!BuildAuthoredContours(c))return false;
  bool surfaces=false;for(unsigned i=0;i<count;++i)surfaces|=c->authored[i].surface;
  if(surfaces && !c->cutouts) {
    c->cutouts=calloc((size_t)c->width/8,c->height);
    if(!c->cutouts){c->build_error=3;return false;}
  }
  for(unsigned i=0;i<count;++i)if(c->authored[i].surface) {
    const VoxelAuthoredCell *a=&c->authored[i];
    for(int y=0;y<16;++y) {
      const size_t at=(size_t)(a->y+y-c->origin_y)*(c->width/8)+(a->x-c->origin_x)/8;
      c->cutouts[at]=c->cutouts[at+1]=255;
    }
  }
  return true;
}
