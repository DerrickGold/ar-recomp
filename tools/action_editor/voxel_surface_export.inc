/* Read-only surface inventory. Include the production compiler in this audit
 * translation unit so material lookup is authoritative, not reimplemented.
 * These records describe source/compiled ownership; they are never visual
 * acceptance, and absent generic edges do not make a specialized route pass. */
#include <assert.h>
static void AuditHex8(const uint8_t *p,size_t n) {
  putchar('"');for(size_t i=0;i<n;++i)printf("%02x",p[i]);putchar('"');
}
static void AuditHex16(const uint16_t *p,size_t n) {
  putchar('"');for(size_t i=0;i<n;++i)printf("%04x",p?p[i]:0);putchar('"');
}
static void AuditHex32(const uint32_t *p,size_t n) {
  putchar('"');for(size_t i=0;i<n;++i)printf("%08x",p[i]);putchar('"');
}
static unsigned AuditMaterialId(CastleMaterial *bank,unsigned *count,const CastleMaterial *m) {
  for(unsigned i=0;i<*count;++i)if(!memcmp(bank+i,m,sizeof(*m)))return i;
  bank[*count]=*m;return (*count)++;
}
static void AuditMaterialJson(const ActionRoomScene *s,const CastleMaterial *m) {
  printf("{\"key\":\"%016llx\",\"words\":[%u,%u,%u,%u],\"kinds\":[%u,%u,%u,%u],\"collision\":%u,\"flags\":%u,\"top_policy\":%u,\"top_family\":%u,\"native_surface_family\":%u,\"native_surface_faces\":%u,\"native_surface_flip\":%u,\"contour_surface\":%u,\"rear_rows\":[",
      (unsigned long long)MaterialKey(m,0),m->words[0],m->words[1],m->words[2],m->words[3],
      m->kind[0],m->kind[1],m->kind[2],m->kind[3],m->collision,m->flags,m->top_surface,m->top_family,
      m->native_surface_family,m->native_surface_faces,m->native_surface_flip,m->contour_surface);
  for(unsigned i=0;i<16;++i)printf("%s%u",i?",":"",m->rear_rows[i]);
  printf("],\"cutouts\":[");for(unsigned i=0;i<16;++i)printf("%s%u",i?",":"",m->cutouts[i]);
  printf("],\"pixels_hex\":\"");
  uint16_t colors[256];
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    unsigned q=y/8*2+x/8,ink=m->pixels[q][(y&7)*8+(x&7)],at=(((m->words[q]>>10)&7)*16+ink)*2;
    printf("%x",ink);colors[y*16+x]=ink?(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8):0;
  }
  printf("\",\"rgba555_hex\":");AuditHex16(colors,256);putchar('}');
}
static void AuditSourceCells(const ActionRoomScene *s,const ActionVoxelGeometry *g,const DioramaRoomOverride *terrain) {
  int x0=g->ready?g->origin_x/16:0,y0=g->ready?g->origin_y/16:0;
  int x1=g->ready?(g->origin_x+g->width)/16:s->bg[0].pages_wide*16;
  int y1=g->ready?(g->origin_y+g->height)/16:s->bg[0].pages_high*16;
  const size_t capacity=(size_t)(x1-x0)*(y1-y0)+257;
  CastleMaterial *bank=calloc(capacity,sizeof(*bank)),native[256];assert(bank);
  unsigned count=0;
  for(unsigned id=0;id<256;++id) {
    uint16_t words[4];unsigned collision=0;
    for(unsigned q=0;q<4;++q) {
      const uint8_t *p=s->bg[0].metatiles+id*8+q*2;unsigned raw=p[0]<<8|p[1];
      collision|=!!(raw&0x200)<<q;
      words[q]=ActionBg_ComposeTilemapWord(raw,kActionRoomSceneTileWordMask,ActionRoomScene_BgAttributes(s,1));
    }
    Material(s,0,words,collision,native+id);
    /* The native table is stable, including unused entries; source cells
     * point at id0..255 before any editor/context-dependent variants. */
    bank[count++]=native[id];
  }
  printf(",\"cell_fields\":[\"x\",\"y\",\"native_id\",\"native_material_id\",\"material_id\",\"edit_id\",\"authored_index\"],\"source_cells\":[");
  bool comma=false;
  for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x) {
    uint16_t word;uint8_t native_id=0,id=0;
    bool exists=x>=0&&y>=0&&ActionRoomScene_LookupTile(s,1,x*2,y*2,&word,&native_id);
    CastleMaterial material;memset(&material,0,sizeof(material));
    LookupMaterial(s,&terrain->stamp_layers[0],native,x,y,&material,&id,NULL,0);
    unsigned material_id=AuditMaterialId(bank,&count,&material);assert(count<=capacity);
    const DioramaPixelEdit *edit=DioramaLayerOrder_PixelEditAt(terrain,0,x,y,id);
    int edit_id=edit?(int)(edit-terrain->pixel_layers[0].edits):-1;
    const VoxelAuthoredCell *authored=ActionVoxel_AuthoredCellAt(g,x,y);
    printf("%s[%d,%d,%d,%d,%u,%d,%d]",comma?",":"",x,y,exists?(int)native_id:-1,
        exists?(int)native_id:-1,material_id,edit_id,authored?(int)(authored-g->authored):-1);comma=true;
  }
  printf("],\"materials\":[");for(unsigned i=0;i<count;++i){if(i)putchar(',');AuditMaterialJson(s,bank+i);}free(bank);
  printf("],\"pixel_edits\":[");
  for(unsigned i=0;i<terrain->pixel_layers[0].count;++i) {
    const DioramaPixelEdit *e=terrain->pixel_layers[0].edits+i;
    printf("%s{\"transparent\":[",i?",":"");for(unsigned j=0;j<16;++j)printf("%s%u",j?",":"",e->transparent[j]);
    printf("],\"black\":[");for(unsigned j=0;j<16;++j)printf("%s%u",j?",":"",e->black[j]);printf("]}");
  }
  printf("]");
}
static void AuditEdges(const ActionVoxelGeometry *g) {
  printf(",\"edge_fields\":[\"x\",\"y\",\"length\",\"side\",\"near_rank\",\"far_rank\",\"priority\",\"colliding\",\"sample_x\",\"sample_y\",\"sample_size\",\"surface_texture\",\"owned_art\",\"derived\",\"native_x\",\"soft\",\"column\",\"trunk\",\"contour_skin\",\"surface_top_material\",\"fold_top\",\"fold_bottom\",\"flat_rear_join\",\"native_actor_priority\",\"rear_trim\",\"rear_mounted\",\"thin_trim\",\"raised_scenery\",\"native_near\",\"native_far\",\"native_span_valid\",\"behind_near\",\"behind_far\",\"behind_span_valid\",\"rear_near\",\"rear_far\",\"rear_span_valid\",\"physical_near\",\"physical_far\",\"physical_span_valid\"],\"edges\":[");
  for(unsigned i=0;i<g->edge_count;++i) {
    const VoxelEdge *e=g->edges+i;VoxelEdgeSpan span={0};
    bool valid=ActionVoxel_ResolveEdgeSpan(g,e,kVoxelEdgeNative,0,ActionVoxel_Depth(g),&span);
    VoxelEdgeSpan behind={0},rear={0};
    bool behind_valid=ActionVoxel_ResolveEdgeSpan(g,e,kVoxelEdgeHighDeckBehind,0,ActionVoxel_Depth(g),&behind);
    bool rear_valid=ActionVoxel_ResolveEdgeSpan(g,e,kVoxelEdgeRearTrim,0,ActionVoxel_TrimDepth(g),&rear);
    float physical_near=ActionVoxel_Depth(g),physical_far=0;
    if(valid){physical_near=fminf(physical_near,span.near_depth);physical_far=fmaxf(physical_far,span.far_depth);}
    if(behind_valid){physical_near=fminf(physical_near,behind.near_depth);physical_far=fmaxf(physical_far,behind.far_depth);}
    if(rear_valid){physical_near=fminf(physical_near,rear.near_depth);physical_far=fmaxf(physical_far,rear.far_depth);}
    const bool physical_valid=valid||behind_valid||rear_valid;
    if(!physical_valid)physical_near=0;
    printf("%s[%d,%d,%d,%u,%u,%u,%u,%u,%d,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%.7g,%.7g,%s,%.7g,%.7g,%s,%.7g,%.7g,%s,%.7g,%.7g,%s]",i?",":"",
        e->x,e->y,e->length,e->side,e->near_rank,e->far_rank,e->high_priority,e->colliding,
        e->top_sample_x,e->top_sample_y,e->top_sample_size,e->surface_texture,e->top_owned_art,
        e->surface_derived,e->surface_native_x,e->soft_surface,e->column_curve,e->trunk_curve,
        e->contour_skin,e->surface_top_material,e->rear_fold_top,e->rear_fold_bottom,e->flat_rear_join,
        e->native_actor_priority,e->rear_trim,e->rear_mounted,e->thin_trim,e->raised_scenery,
        span.near_depth,span.far_depth,valid?"true":"false",behind.near_depth,behind.far_depth,behind_valid?"true":"false",
        rear.near_depth,rear.far_depth,rear_valid?"true":"false",physical_near,physical_far,physical_valid?"true":"false");
  }
  printf("],\"owned_art\":[");
  for(unsigned i=0;i<g->authored_art_count;++i){if(i)putchar(',');AuditHex16(g->authored_art[i],256);}printf("]");
}
static void AuditAuthored(const ActionVoxelGeometry *g) {
  printf(",\"authored\":[");
  for(unsigned i=0;i<g->authored_count;++i) {
    const VoxelAuthoredCell *a=g->authored+i;
    printf("%s{\"x\":%d,\"y\":%d,\"width\":%.7g,\"offset_x\":%.7g,\"faces\":%u,\"shape\":%u,\"surface\":%s,\"native_transform\":%s,\"front_hidden\":%s,\"automatic_art\":%u,\"priority\":[%u,%u,%u,%u],\"front\":[%.7g,%.7g,%.7g,%.7g],\"back\":[%.7g,%.7g,%.7g,%.7g],\"art_ids\":[",i?",":"",a->x,a->y,a->width,a->offset_x,a->faces,a->shape,a->surface?"true":"false",a->native_transform?"true":"false",a->native_front_hidden?"true":"false",a->automatic_art,
        a->priority[0],a->priority[1],a->priority[2],a->priority[3],a->front[0],a->front[1],a->front[2],a->front[3],a->back[0],a->back[1],a->back[2],a->back[3]);
    for(unsigned j=0;j<7;++j)printf("%s%u",j?",":"",a->art_id[j]);
    printf("],\"samples\":[");for(unsigned j=0;j<7;++j)printf("%s[%d,%d,%u,%u,%u]",j?",":"",a->surface_sample_x[j],a->surface_sample_y[j],a->surface_sample_size[j],a->surface_derived[j],a->surface_native_x[j]);
    printf("],\"contours\":[");for(unsigned j=0;j<a->contour_count;++j) {
      const VoxelAuthoredContour *c=g->authored_contours+a->contour_start+j;
      printf("%s[%u,%u,%u,%u]",j?",":"",c->x,c->y,c->length,c->side);
    }
    printf("],\"shape_domain\":[%d,%u],\"front_end\":[%.7g,%.7g,%.7g,%.7g],\"back_end\":[%.7g,%.7g,%.7g,%.7g],\"bend\":[%u,%d,%d,%.7g,%.7g,%.7g],\"transform\":[%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g]}",
        a->shape_y,a->shape_h,a->front_end[0],a->front_end[1],a->front_end[2],a->front_end[3],
        a->back_end[0],a->back_end[1],a->back_end[2],a->back_end[3],
        a->bend.axis,a->bend.anchor,a->bend.tip,a->bend.dx,a->bend.dy,a->bend.dz,
        a->transform.px,a->transform.py,a->transform.pz,a->transform.dx,a->transform.dy,a->transform.dz,
        a->transform.sx,a->transform.sy,a->transform.sz);
  }printf("]");
}
static void AuditSpecialized(const ActionRoomScene *s,const ActionVoxelGeometry *g,const DioramaRoomOverride *terrain,bool completion) {
  printf(",\"patches\":[");
  for(unsigned i=0;i<g->patch_count;++i){const VoxelPatch *p=g->patches+i;printf("%s{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d,\"kind\":%u,\"priority\":%u,\"surface_compiled\":%u,\"surface_first\":%u,\"surface_count\":%u}",i?",":"",p->x,p->y,p->width,p->height,p->kind,p->high_priority,p->surface_compiled,p->surface_first,p->surface_count);}
  printf("],\"profile_surfaces\":[");for(unsigned i=0;i<g->profile_surface_count;++i){const VoxelProfileSurface *p=g->profile_surfaces+i;printf("%s[%d,%d,%u]",i?",":"",p->x,p->y,p->art_id);}
  printf("],\"water\":[");for(unsigned i=0;i<g->water_surface_count;++i){const VoxelWaterSurface *p=g->water_surfaces+i;printf("%s{\"x\":%d,\"y\":%d,\"width\":%d,\"priority\":%u,\"background\":%u,\"sample_y\":%u,\"sample_height\":%u,\"opaque\":%u,\"uniform_color\":[%u,%u]}",i?",":"",p->x,p->y,p->width,p->high_priority,p->background,p->sample_y,p->sample_height,p->opaque,p->uniform_color[0],p->uniform_color[1]);}
  printf("],\"waterfalls\":[");for(unsigned i=0;i<g->waterfall_count;++i){const VoxelWaterfall *p=g->waterfalls+i;printf("%s[%d,%d,%d,%d,%u]",i?",":"",p->x,p->y,p->width,p->height,p->high_priority);}
  printf("],\"gates\":[");for(unsigned i=0;i<g->gate_count;++i){const VoxelGate *p=g->gates+i;printf("%s[%d,%d,%d,%u,%u,%u,%u,%u]",i?",":"",p->x,p->y,p->height,p->high_priority,p->to_right,p->width,p->centered,p->solid_face);}
  printf("],\"rear_trims\":[");for(unsigned i=0;i<g->rear_trim_count;++i){const VoxelRearTrim *p=g->rear_trims+i;printf("%s[%d,%d,%d]",i?",":"",p->x,p->y,p->width);}
  printf("],\"lintel_returns\":[");
  for(unsigned i=0;i<g->lintel_return_count;++i) {
    const VoxelLintelReturn *lip=&g->lintel_returns[i];
    printf("%s{\"x\":%d,\"y\":%d,\"rear_depth\":%.7g,\"priority\":%u,\"owned_art\":%u}",
        i?",":"",lip->x,lip->y,lip->rear_depth,lip->high_priority,lip->owned_art);
  }
  printf("],\"rear_fills\":[");for(unsigned i=0;i<g->rear_fill_count;++i){const VoxelRearFill *p=g->rear_fills+i;printf("%s{\"x\":%d,\"y\":%d,\"preserve_existing\":%s,\"replace_front\":%s,\"art\":",i?",":"",p->x,p->y,p->preserve_existing?"true":"false",p->replace_front?"true":"false");AuditHex16(p->colors,256);putchar('}');}
  printf("],\"trunk_curves\":[");for(unsigned i=0;i<g->trunk_curve_count;++i)printf("%s[%d,%d]",i?",":"",g->trunk_curves[i].top_y,g->trunk_curves[i].base_y);
  printf("],\"column_curves\":[");for(unsigned i=0;i<g->column_curve_count;++i)printf("%s[%d,%d]",i?",":"",g->column_curves[i].top_y,g->column_curves[i].base_y);printf("]");
  printf(",\"column_contacts\":[");for(unsigned i=0;i<g->column_curve_count;++i) {
    const VoxelTrunkCurve *curve=g->column_curves+i;
    printf("%s{\"owner\":%u,\"linear\":%s,\"contact_x\":%d,\"contact\":[",i?",":"",i+1,
        curve->linear?"true":"false",curve->contact_x);
    for(unsigned j=0;j<curve->contact_count && j<8;++j)printf("%s%d",j?",":"",curve->contact[j]);
    printf("]}");
  }printf("]");
  const bool hollow=DioramaTrunk_Room(s->group,s->map);
  printf(",\"trunk\":");
  if(hollow) {
    ActionTrunkAssets *a=calloc(1,sizeof(*a));assert(a);bool built=ActionTrunk_Build(a,s,terrain);
    printf("{\"built\":%s,\"height\":%u,\"core_x\":320,\"core_width\":384,\"side_width\":%u,\"core_masks\":[",built?"true":"false",a->height,a->side_width);
    for(unsigned i=0;i<3;++i){if(i)putchar(',');AuditHex8(a->masks[i],a->height*kActionTrunkMaskPitch);}
    printf("],\"side_masks\":[");for(unsigned i=0;i<kActionTrunkSideMaskCount;++i){if(i)putchar(',');AuditHex8(a->side_masks[i],a->height*kActionTrunkSideMaskPitch);}
    printf("],\"wall_rgba\":");AuditHex32(a->wall,kActionTrunkWallWidth*kActionTrunkWallHeight);
    printf(",\"bark_rgba\":");AuditHex32(a->bark,32*32);printf(",\"snow_rgba\":");AuditHex32(a->side_snow,16*16);putchar('}');free(a);
  }else printf("null");
  printf(",\"routes\":[{\"route\":\"generic_edges\",\"items\":%u,\"status\":\"inventoried_not_visually_accepted\"},{\"route\":\"authored\",\"items\":%u,\"status\":\"inventoried_not_visually_accepted\"},{\"route\":\"profiles\",\"items\":%u,\"status\":\"inventoried_not_visually_accepted\"},{\"route\":\"water\",\"items\":%u,\"status\":\"inventoried_not_visually_accepted\"},{\"route\":\"trunk\",\"applicable\":%s,\"status\":\"%s\"},{\"route\":\"ground_cover_and_effect_receivers\",\"status\":\"renderer_inventory_required\"},{\"route\":\"runtime_objects\",\"status\":\"snapshot_objects_not_in_static_map_inventory\"},{\"route\":\"desert_columns_and_live_sand\",\"applicable\":%s,\"status\":\"%s\"},{\"route\":\"completion\",\"applicable\":%s,\"status\":\"%s\"}]",
      g->edge_count,g->authored_count,g->patch_count,g->water_surface_count,hollow?"true":"false",hollow?"mask_inventory_render_review_required":"not_applicable",
      s->group==3&&s->map<=2?"true":"false",s->group==3&&s->map<=2?"renderer_inventory_required":"not_applicable",completion?"true":"false",completion?"separate_scene_review_required":"not_applicable");
}
static void AuditSurfaces(const ActionRoomScene *s,const ActionVoxelGeometry *g,const DioramaRoomOverride *terrain,bool completion) {
  printf(",\"surface_audit\":{\"schema\":1,\"scope\":\"all_compiled_routes_and_native_source_cells; emitted_geometry_is_not_expected_geometry_or_visual_acceptance\",\"origin\":[%d,%d],\"size\":[%d,%d]",g->origin_x,g->origin_y,g->width,g->height);
  AuditSourceCells(s,g,terrain);AuditEdges(g);AuditAuthored(g);AuditSpecialized(s,g,terrain,completion);
  printf(",\"front_rows_hex\":[");if(g->front)for(int y=0;y<g->height;++y){if(y)putchar(',');AuditHex8(g->front+(size_t)y*g->width,g->width);}printf("]");
  const VoxelSurfaceStats *p=&g->surface_stats;
  printf(",\"budgets\":{\"edges\":%u,\"edge_limit\":%u,\"art\":%u,\"art_limit\":%u,\"edge_fallbacks\":%u,\"art_fallbacks\":%u},\"complete_visual_review\":false}",g->edge_count,kVoxelMaxEdges,g->authored_art_count,kVoxelAuthoredArtMax,p->edge_budget_fallbacks,p->art_budget_fallbacks);
}
