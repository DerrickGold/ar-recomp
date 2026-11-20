/* Snow over a rear-mounted canopy is a coating, not a square slab. Native
 * palette4 ink12..15 describes its reviewed snow fringe; source indices,
 * material roles and adjacency remain stable through fades and tile remaps. */
static bool SnowCanopyMaterial(const CastleMaterial *m) {
  const uint32_t flags=kVoxelRearMountedMaterial|kVoxelRearCopyMaterial;
  if(!m->leaf_canopy || (m->flags&flags)!=flags || (m->flags&(kVoxelTreeBarkMaterial|kVoxelSnowRockMaterial)))return false;
  for(unsigned q=0;q<4;++q)
    if(m->kind[q]!=kVoxelForeground || (m->words[q]&0x2000) || ((m->words[q]>>10)&7)!=4)return false;
  return true;
}
static bool SnowCanopySolid(const CastleMaterial *m) {
  if((m->collision!=15 && m->collision!=6 && m->collision!=9) || (m->flags&(kVoxelTreeBarkMaterial|kVoxelSnowRockMaterial)))return false;
  for(unsigned q=0;q<4;++q)
    if(m->kind[q]!=kVoxelSolid || (m->words[q]&0x2000) || ((m->words[q]>>10)&7)!=4)return false;
  return true;
}
/* The finish cannot move an exposed walking contact. A thin end keeps its
 * native front lip and uses only the remaining thickness for the return. */
static bool SnowCanopyStart(const ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaStampLayerOverride *stamps,const CastleMaterial native[256],int wx,int end,
    const uint16_t *tiles,size_t tile_pitch,int *start) {
  const int xx=wx-c->origin_x;if(xx<0 || xx>=c->width)return false;
  *start=end-32;bool body=false;int first=end;CastleMaterial m;int row=0;bool have_material=false;
  for(int y=end-1;y>=end-36 && y>=c->origin_y;--y) {
    const int cy=(int)floorf(y/16.f);uint8_t id;
    if(!have_material || row!=cy) {
      if(!LookupMaterial(s,stamps,native,(int)floorf(wx/16.f),cy,&m,&id,tiles,tile_pitch)) {
        if(body){if(first+4>*start)*start=first+4;break;}continue;
      }
      row=cy;have_material=true;
    }
    const unsigned px=(unsigned)wx&15,py=(unsigned)y&15,q=(py/8)*2+px/8,p=m.pixels[q][(py&7)*8+(px&7)];
    const bool solid=SnowCanopySolid(&m) && p && !(m.rear_rows[py]&(1u<<(15-px))) &&
        !(m.cutouts[py]&(1u<<(15-px))) && !(m.rear_colors[q]&(1u<<p));
    if(solid){body=true;first=y;}
    else if(body){if(first+4>*start)*start=first+4;break;}
  }
  if(!body)return false;
  if(first==c->origin_y && first+4>*start)*start=first+4;
  if(*start>=end)*start=end-1;
  return true;
}
/* Skip sky above thin/sloping tips. Opaque leaves prove that the snow fringe
 * has ended; their native alpha holes remain holes in the generated mesh. */
static bool SnowCanopyEnd(const ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaStampLayerOverride *stamps,const CastleMaterial native[256],int wx,int hint,
    const uint16_t *tiles,size_t tile_pitch,int *end,int *start) {
  int last=0;unsigned leaves=0;bool have_snow=false,leaf_witness=false;
  CastleMaterial m;int row=0;uint8_t id=0;bool have_material=false;
  for(int y=hint-32;y<hint+32;++y) {
    const int cy=(int)floorf(y/16.f),cx=(int)floorf(wx/16.f);
    if(!have_material || cy!=row) {
      if(!LookupMaterial(s,stamps,native,cx,cy,&m,&id,tiles,tile_pitch) ||
          (!SnowCanopySolid(&m) && !SnowCanopyMaterial(&m))) {
        if(have_snow) {if(!leaf_witness)return false;*end=last;return SnowCanopyStart(c,s,stamps,native,wx,last,tiles,tile_pitch,start);}
        have_material=false;continue;
      }
      row=cy;have_material=true;
    }
    const unsigned x=(unsigned)wx&15,py=(unsigned)y&15,q=(py/8)*2+x/8,p=m.pixels[q][(py&7)*8+(x&7)];
    if(!p) {
      if(!have_snow)continue;
      if(!SnowCanopyMaterial(&m))return false;
      if(leaf_witness){*end=last;return SnowCanopyStart(c,s,stamps,native,wx,last,tiles,tile_pitch,start);}
      continue;
    }
    if(p>=12){last=y+1;leaves=0;have_snow=true;}
    else {
      leaf_witness|=have_snow && SnowCanopyMaterial(&m);
      if(++leaves>=8 && have_snow && leaf_witness) {*end=last;return SnowCanopyStart(c,s,stamps,native,wx,last,tiles,tile_pitch,start);}
    }
  }
  return false;
}
/* A terminal snow cell has sky below it rather than a leaf donor. Carry a
 * physically touching coating to that end cap, preserving native opacity. */
static bool BuildSnowCanopyTerminals(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaRoomOverride *terrain,const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256],const uint16_t *tiles,size_t tile_pitch) {
  const int left=c->origin_x/16,top=c->origin_y/16,right=left+c->width/16,bottom=top+c->height/16;
  bool changed;
  do {
    changed=false;
    for(int cy=top;cy+1<bottom;++cy)for(int cx=left;cx<right;++cx) {
      CastleMaterial snow,below;uint8_t id;
      if(!LookupMaterial(s,stamps,native,cx,cy,&snow,&id,tiles,tile_pitch) || !SnowCanopySolid(&snow) ||
          !LookupMaterial(s,stamps,native,cx,cy+1,&below,&id,tiles,tile_pitch))continue;
      bool empty=true;
      for(unsigned q=0;q<4;++q)for(unsigned p=0;p<64;++p)empty&=below.pixels[q][p]==0;
      if(!empty)continue;
      bool owned=false;
      for(unsigned i=0;i<c->column_curve_count;++i) {
        const VoxelTrunkCurve *a=c->column_curves+i;
        owned|=a->native_lip && a->contact_x==cx*16 && abs(a->contact[4]+16-(cy+1)*16)<32;
      }
      if(owned)continue;
      unsigned support=0,edge=0;int direction=0;
      for(unsigned i=0;i<c->column_curve_count && !support;++i) {
        const VoxelTrunkCurve *a=c->column_curves+i;if(!a->native_lip)continue;
        const int side=a->contact_x+16==cx*16?-1:a->contact_x==cx*16+16?1:0;
        if(!side)continue;
        const unsigned knot=side<0?8:0;
        CastleMaterial neighbor;
        if(!LookupMaterial(s,stamps,native,cx+side,cy,&neighbor,&id,tiles,tile_pitch) || !SnowCanopySolid(&neighbor))continue;
        for(unsigned y=0;y<16;++y) {
          const unsigned x=side<0?0:15,nx=15-x,q=(y/8)*2+x/8,nq=(y/8)*2+nx/8;
          const int wy=cy*16+y;
          if(wy<a->canopy_top[knot] || wy>=a->contact[knot]+16 ||
              !snow.pixels[q][(y&7)*8+(x&7)] || !neighbor.pixels[nq][(y&7)*8+(nx&7)])continue;
          support=i+1;edge=knot;direction=side;break;
        }
      }
      if(!support)continue;
      const VoxelTrunkCurve *a=c->column_curves+support-1;
      VoxelTrunkCurve curve={.contact_x=cx*16,.contact_count=9,.native_lip=1};
      const int join_end=a->contact[edge]+16,join_top=a->canopy_top[edge];
      int first=cy*16+16,last=cy*16;bool safe=true;
      for(unsigned k=0;k<9;++k) {
        const float t=(direction<0?k:8-k)/8.f;
        int end=(int)lroundf(join_end+fmaxf(0,(cy+1)*16-join_end)*t),start=join_top,local;
        const int wx=cx*16+(k==8?15:(int)k*2);
        if(SnowCanopyStart(c,s,stamps,native,wx,end,tiles,tile_pitch,&local) && local>start) {
          if(((direction<0 && !k) || (direction>0 && k==8)) && local>join_top+4)safe=false;
          start=local;
        }
        if((direction<0 && !k) || (direction>0 && k==8))start=join_top;
        if(end<=start)end=start+1;
        curve.contact[k]=(short)(end-16);curve.canopy_top[k]=(short)start;
        if(start<first)first=start;if(end>last)last=end;
      }
      if(!safe)continue;
      bool available=true;const int y0=(int)floorf(first/8.f),y1=(int)ceilf(last/8.f);
      for(int y=y0;y<y1;++y) {
        CastleMaterial m;
        if(y<top*2 || y>=bottom*2 || !LookupMaterial(s,stamps,native,cx,(int)floorf(y/2.f),&m,&id,tiles,tile_pitch) ||
            !SnowCanopySolid(&m)){available=false;break;}
        for(unsigned x=0;x<2;++x)
          if(c->column_cells[(size_t)(y-top*2)*(c->width/8)+cx*2-left*2+x])available=false;
      }
      if(!available)continue;
      if(c->column_curve_count==kVoxelMaxCastleColumns)return false;
      const unsigned owner=++c->column_curve_count;c->column_curves[owner-1]=curve;
      for(int y=y0;y<y1;++y)for(unsigned x=0;x<2;++x)
        if(!NativeContourEdited(terrain,cx,(int)floorf(y/2.f)))
          c->column_cells[(size_t)(y-top*2)*(c->width/8)+cx*2-left*2+x]=(uint8_t)owner;
      changed=true;
    }
  }while(changed);
  return true;
}
static bool BuildSnowCanopyReturns(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaRoomOverride *terrain,const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256],const uint16_t *tiles,size_t tile_pitch) {
  if(c->profile!=kActionVoxelProfile_NorthwallSnow)return true;
  const int left=c->origin_x/16,top=c->origin_y/16,right=left+c->width/16,bottom=top+c->height/16;
  for(int cy=top+1;cy<bottom;++cy)for(int cx=left;cx<right;++cx) {
    CastleMaterial canopy,snow;uint8_t id;
    if(!LookupMaterial(s,stamps,native,cx,cy,&canopy,&id,tiles,tile_pitch) || !SnowCanopyMaterial(&canopy) ||
        !LookupMaterial(s,stamps,native,cx,cy-1,&snow,&id,tiles,tile_pitch) || !SnowCanopySolid(&snow))continue;
    /* Shared boundary knots and a four-pixel source average remove the
     * artwork's individual snow grains from the physical coating contour. */
    VoxelTrunkCurve curve={.contact_x=cx*16,.contact_count=9,.native_lip=1};
    bool intact=true;int first=cy*16+16,last=(cy-1)*16;
    for(unsigned column=0;column<9 && intact;++column) {
      int sum=0,limit=cy*16-64;
      for(int dx=-2;dx<2;++dx) {
        int end,start;
        const int wx=cx*16+column*2+dx;
        if(!SnowCanopyEnd(c,s,stamps,native,wx,cy*16,tiles,tile_pitch,&end,&start)) {
          bool found=false;
          for(int radius=1;radius<=8 && !found;++radius)for(int side=-1;side<=1;side+=2)
            if(SnowCanopyEnd(c,s,stamps,native,wx+side*radius,cy*16,tiles,tile_pitch,&end,&start)){found=true;break;}
          if(!found){intact=false;break;}
          int local_start;
          if(SnowCanopyStart(c,s,stamps,native,wx,end,tiles,tile_pitch,&local_start) && local_start>start)start=local_start;
        }
        sum+=end;if(start>limit)limit=start;
      }
      int end=(int)floorf(sum*.25f+.5f);
      if(end<=limit)end=limit+1;
      if(!intact || end<cy*16-20 || end>cy*16+16){intact=false;break;}
      curve.contact[column]=(short)(end-16);curve.canopy_top[column]=(short)limit;
      if(limit<first)first=limit;
      if(end>last)last=end;
    }
    if(!intact)continue;
    curve.top_y=curve.canopy_top[0];curve.base_y=curve.contact[0]+16;
    const int first_row=(int)floorf(first/8.f),last_row=(int)ceilf(last/8.f);
    bool available=true;
    for(int y=first_row;y<last_row;++y)for(unsigned x=0;x<2;++x) {
      CastleMaterial m;uint8_t provenance;
      if(y<top*2 || !LookupMaterial(s,stamps,native,cx,(int)floorf(y/2.f),&m,&provenance,tiles,tile_pitch) ||
          (!SnowCanopySolid(&m) && !SnowCanopyMaterial(&m))){available=false;continue;}
      const size_t at=(size_t)(y-top*2)*(c->width/8)+cx*2-left*2+x;
      if(c->column_cells && c->column_cells[at])available=false;
    }
    if(!available)continue;
    if(c->column_curve_count==kVoxelMaxCastleColumns)return false;
    if(!c->column_cells) {
      c->column_cells=calloc((size_t)c->width/8,c->height/8);
      if(!c->column_cells)return false;
    }
    const unsigned owner=++c->column_curve_count;c->column_curves[owner-1]=curve;
    for(int y=first_row;y<last_row;++y)for(unsigned x=0;x<2;++x)
      if(!NativeContourEdited(terrain,cx,(int)floorf(y/2.f)))
        c->column_cells[(size_t)(y-top*2)*(c->width/8)+cx*2-left*2+x]=(uint8_t)owner;
  }
  return !c->column_cells || BuildSnowCanopyTerminals(c,s,terrain,stamps,native,tiles,tile_pitch);
}
