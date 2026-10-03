/* Bloodpool castle: world-attached window light, dust and supported floor haze.
 * Pure bounded geometry; shares the existing BG1/BG2 batches and direct masks.
 * No native slots, texture resolves, backend calls or persistent particles. */
#include "action/action_effect_render_internal.h"
#include "action_effect_members.h"
#include "action_castle_sources.h"

enum {
  kCastleSoftFanColumns = 5, kCastleBossFanColumns = 9, kCastleFanRows = 4,
  kCastleOpeningColumns = 5, kCastleOpeningRows = 4,
  kCastleLedgeColumns = 5, kCastleLedgeRows = 3,
  kCastleSoftWindowCells = 2*(kCastleSoftFanColumns-1)*(kCastleFanRows-1)+
      (kCastleOpeningColumns-1)*(kCastleOpeningRows-1)+
      (kCastleLedgeColumns-1)*(kCastleLedgeRows-1),
  kCastleBossWindowCells = 2*(kCastleBossFanColumns-1)*(kCastleFanRows-1)+
      (kCastleOpeningColumns-1)*(kCastleOpeningRows-1)+
      (kCastleLedgeColumns-1)*(kCastleLedgeRows-1),
  kCastleGalleryStoneCells = 8+2*4,
  kCastleTorchVertices = 24*12+2*kActionEffectGlowVertices+7*4,
  kCastleTorchIndices = 24*24+2*kActionEffectGlowIndices+7*6,
};

/* A convex grid cell is two triangles. Rectangle clipping adds at most eight
 * boundary vertices and two duplicated diagonal intersections: <=12 vertices
 * and 24 indices per cell. These bounds include dust, native torches and their
 * bounce, without relying on viewport culling or increasing shared scratch. */
_Static_assert(8*(kCastleSoftWindowCells*12+14*12)+3*kCastleTorchVertices+7 <=
                   kActionSceneEffectRenderMaxVertices &&
               8*(kCastleSoftWindowCells*24+14*24)+3*kCastleTorchIndices <=
                   kActionSceneEffectRenderMaxIndices, "castle shaft geometry capacity");
_Static_assert(11*((kCastleSoftWindowCells+kCastleGalleryStoneCells)*12+8*12)+7 <=
                   kActionSceneEffectRenderMaxVertices &&
               11*((kCastleSoftWindowCells+kCastleGalleryStoneCells)*24+8*24) <=
                   kActionSceneEffectRenderMaxIndices, "gallery geometry capacity");
_Static_assert(kCastleBossWindowCells*12+32*12+7 <= kActionSceneEffectRenderMaxVertices &&
               kCastleBossWindowCells*24+32*24 <= kActionSceneEffectRenderMaxIndices,
               "boss window geometry capacity");
_Static_assert(5*(kCastleSoftWindowCells*12+14*12)+3*kCastleTorchVertices+
                   kActionCastleWaterStripCount*(11+2*8)*12+7 <= kActionSceneEffectRenderMaxVertices &&
               5*(kCastleSoftWindowCells*24+14*24)+3*kCastleTorchIndices+
                   kActionCastleWaterStripCount*(11+2*8)*24 <= kActionSceneEffectRenderMaxIndices,
               "castle water and window geometry capacity");

static bool CastleClip(const ActionEffectInstance *effect, ActionEffectClipBoundsFn clip_bounds,
    void *userdata, ActionEffectLocalRect *clip) {
  *clip = effect->geometry.data.rect;
  if (clip_bounds && !clip_bounds(userdata,effect,clip)) return false;
  const ActionEffectLocalRect *field = &effect->geometry.data.rect;
  clip->x0 = fmaxf(clip->x0,field->x0);
  clip->y0 = fmaxf(clip->y0,field->y0);
  clip->x1 = fminf(clip->x1,field->x1);
  clip->y1 = fminf(clip->y1,field->y1);
  if (effect->flags&kActionEffectFlag_ClipToRect) {
    if (!RectIsSane(&effect->clip_rect)) return false;
    clip->x0 = fmaxf(clip->x0,effect->clip_rect.x0);
    clip->y0 = fmaxf(clip->y0,effect->clip_rect.y0);
    clip->x1 = fminf(clip->x1,effect->clip_rect.x1);
    clip->y1 = fminf(clip->y1,effect->clip_rect.y1);
  }
  return RectIsSane(clip) && clip->x0 < clip->x1 && clip->y0 < clip->y1;
}

static bool CastleDiamond(ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ActionEffectLocalRect *clip, float x, float y, float size, ArRenderColorF color,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const ArRenderVertex2D vertices[] = {
    {{x-size,y},color,{0,0}},{{x,y-size},color,{0,0}},
    {{x+size,y},color,{0,0}},{{x,y+size},color,{0,0}},
  };
  int mapped[] = {-1,-1,-1,-1};
  const int triangles[] = {0,1,3,1,2,3};
  for (unsigned t = 0; t < 6; t += 3)
    if (!AppendSceneClippedTriangle(writer,mesh,vertices,mapped,&triangles[t],clip,
            project_point,userdata)) return false;
  return true;
}

static bool CastleWaterGlint(const ActionCastleField *f, ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ActionEffectLocalRect *clip, float x, float y, float rx, float ry, float alpha,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const ArRenderColorF clear = {f->WaterColor[0],f->WaterColor[1],f->WaterColor[2],0}, lit = {f->WaterColor[0],f->WaterColor[1],f->WaterColor[2],alpha};
  const ArRenderVertex2D vertices[] = {
    {{x-rx,y},clear,{0,0}},{{x,y-ry},lit,{0,0}},
    {{x+rx,y},clear,{0,0}},{{x,y+ry},lit,{0,0}},
  };
  int mapped[] = {-1,-1,-1,-1};
  const int triangles[] = {0,1,3,1,2,3};
  for (unsigned t = 0; t < 6; t += 3)
    if (!AppendSceneClippedTriangle(writer,mesh,vertices,mapped,&triangles[t],clip,
            project_point,userdata)) return false;
  return true;
}

static bool CastleWater(const ActionCastleField *f, ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ActionEffectLocalRect *clip, bool lighting, bool particles,
    ActionEffectProjectPointFn project_point, void *userdata) {
  for (unsigned strip = 0; strip < (unsigned)f->WaterSurface[3]; strip++) {
    if (!(mesh->source_mask&(1u<<strip))) continue;
    const float left = f->WaterSurface[0]+(int)strip*f->WaterSurface[2]-mesh->world_x;
    ActionEffectLocalRect region = *clip;
    region.x0 = fmaxf(region.x0,left);
    region.x1 = fminf(region.x1,left+f->WaterSurface[2]);
    if (region.x0 >= region.x1) continue;
    const uint32_t seed = DeterministicHash_Mix32(strip*131u+(unsigned)f->WaterSeed[0]);
    for (unsigned i = 0; i < 11; i++) {
      const bool broad = i >= 9;
      if (broad ? !lighting : !particles) continue;
      const uint32_t cell = DeterministicHash_Mix32(seed+i*71u);
      const float t = ((mesh->phase_ticks+cell)&((unsigned)f->WaterMotion[0]-1))/f->WaterMotion[0];
      const float fade = sinf(t*3.14159265f);
      const float x = left+(broad ? f->WaterBroad[0]+(i-9)*f->WaterBroad[1] : f->WaterFine[0]+i*f->WaterFine[1])+f->WaterMotion[1]*sinf(t*6.2831853f);
      const float y = f->WaterRows[0]+f->WaterRows[1]*HashUnit(cell^0x39u);
      if (!CastleWaterGlint(f, writer,mesh,&region,x,y,broad ? f->WaterBroad[2] : f->WaterFine[2]+f->WaterFine[3]*HashUnit(cell),
              broad ? f->WaterBroad[3] : f->WaterFine[4],(broad ? f->WaterBroad[4] : f->WaterFine[5])*fade*fade,
              project_point,userdata)) return false;
    }
    if (!particles) continue;
    for (unsigned i = 0; i < 2; i++) {
      const uint32_t cell = DeterministicHash_Mix32(seed+i*197u);
      const float age = ((mesh->phase_ticks+cell)&((unsigned)f->RippleTiming[0]-1))/f->RippleTiming[1];
      if (age >= 1) continue;
      const float x = left+f->RipplePosition[0]+f->RipplePosition[1]*HashUnit(cell), y = f->RipplePosition[2]+f->RipplePosition[3]*HashUnit(cell^17u);
      const float rx = f->RippleShape[0]+age*f->RippleShape[1], ry = f->RippleShape[2]+age*f->RippleShape[3];
      const ArRenderColorF color = {f->RippleColor[0],f->RippleColor[1],f->RippleColor[2],f->RippleColor[3]*(1-age)*fminf(1,age*f->RippleFade[0])};
      for (unsigned segment = 0; segment < 32; segment += 4) {
        const unsigned end = (segment+4)&31;
        const ArRenderVertex2D vertices[] = {
          {{x+kCircle32[segment][0]*rx,y+kCircle32[segment][1]*ry},color,{0,0}},
          {{x+kCircle32[end][0]*rx,y+kCircle32[end][1]*ry},color,{0,0}},
          {{x+kCircle32[end][0]*(rx+f->RippleShape[4]),y+kCircle32[end][1]*(ry+f->RippleShape[5])},color,{0,0}},
          {{x+kCircle32[segment][0]*(rx+f->RippleShape[4]),y+kCircle32[segment][1]*(ry+f->RippleShape[5])},color,{0,0}},
        };
        int mapped[] = {-1,-1,-1,-1};
        const int triangles[] = {0,1,3,1,2,3};
        for (unsigned t = 0; t < 6; t += 3)
          if (!AppendSceneClippedTriangle(writer,mesh,vertices,mapped,&triangles[t],&region,
                  project_point,userdata)) return false;
      }
    }
  }
  return true;
}

typedef ActionCastleArchProfile CastleArchProfile;

typedef struct CastleScatter {
  float x, y, length, width, spread, lean;
  const CastleArchProfile *arch;
} CastleScatter;

enum { kCastleArchRayMaxReach = 42, kCastleArchTopPadding = 16 };

static CastleScatter CastleWindowScatter(const ActionCastleField *f, const ActionCastleSource *s, bool upper) {
  const CastleArchProfile *arch=upper?ActionCastleField_Arch(f,s->arch):NULL;
  return (CastleScatter){s->x,upper?s->y:s->sill,
    upper?-fminf(f->UpperShape[0],(s->sill-s->y)*f->UpperShape[1]):s->length,
    upper?arch->width:s->width,upper?fminf(f->UpperShape[2],s->spread*f->UpperShape[3]):s->spread,
    upper?s->lean*f->UpperShape[4]:s->lean,arch};
}

static float CastleArchRow(const CastleArchProfile *arch, float offset) {
  const float at = fminf(arch->count-1,fmaxf(0,fabsf(offset)-.5f));
  const unsigned lo = (unsigned)at;
  const unsigned hi = lo+1 < arch->count ? lo+1 : lo;
  return arch->rows[lo]+(arch->rows[hi]-arch->rows[lo])*(at-lo);
}

static float CastleWindowStrength(const ActionCastleField *f, const ActionCastleSource *s) {
  (void)s;return f->Strength[0];
}

static const ActionCastleSource *CastleStackNeighbor(const ActionCastleField *f, const ActionEffectInstance *effect,
                                                     const ActionCastleSource *s,
                                                     const ActionCastleSource *sources,
                                                     bool below) {
  const ActionCastleSource *nearest = NULL;
  bool enabled = false;
  unsigned index = 0;
  for (unsigned i = 0; i < (unsigned)f->SourceCount[0]; i++) {
    const ActionCastleSource *other = &sources[i];
    if (index >= 16) return NULL;
    const bool active = (effect->source_mask&(1u<<index++)) != 0;
    if (other->kind != kActionCastleSource_Window || other->x != s->x || other == s ||
        (below ? other->y <= s->y : other->y >= s->y)) continue;
    if (!nearest || (below ? other->y < nearest->y : other->y > nearest->y)) {
      nearest = other;
      enabled = active;
    }
  }
  /* A missing intermediate window breaks the stack; never skip over it or
   * choose neighbors from viewport visibility. Capture validates both frames. */
  if (!nearest || !enabled) return NULL;
  const ActionCastleSource *top = below ? s : nearest;
  const ActionCastleSource *bottom = below ? nearest : s;
  const CastleScatter upper = CastleWindowScatter(f, bottom,true);
  return top->sill < bottom->y &&
      fminf(top->sill+top->length,top->bottom) >= bottom->y+upper.arch->min_row+upper.length ? nearest : NULL;
}

typedef struct CastleWindowJoin {
  CastleScatter down, up;
} CastleWindowJoin;

static CastleWindowJoin CastleMakeJoin(const ActionCastleField *f, const ActionCastleSource *s,
    const ActionCastleSource *below) {
  CastleWindowJoin join = {CastleWindowScatter(f, s,false),CastleWindowScatter(f, below,true)};
  const float gap = below->y+join.up.arch->min_row-s->sill;
  /* Fade each outgoing fan to zero before it can cross the adjacent opening.
   * Keep these endpoints in world space, independent of viewport clipping. */
  join.down.length = fminf(join.down.length,gap);
  join.up.length = fmaxf(join.up.length,-gap);
  return join;
}

static ArRenderPointF CastleRayPoint(const CastleScatter *s, float t, float across) {
  /* Light follows the highlighted arch or flat sill. Its outgoing paths
   * stay straight in world space; dust uses the same mapping. */
  const float root_y = s->y+(s->arch ? CastleArchRow(s->arch,across*s->width) : 0);
  return (ArRenderPointF){s->x+s->lean*t+across*(s->width+(s->spread-s->width)*t),
      root_y+s->length*t};
}

static bool CastleGrid(ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ActionEffectLocalRect *clip, ArRenderVertex2D *vertices, int *mapped, int cols, int rows,
    ActionEffectProjectPointFn project_point, void *userdata) {
  for (int row = 0; row < rows-1; row++) for (int col = 0; col < cols-1; col++) {
    const int a = row*cols+col, b = a+cols;
    /* Mirrored diagonals keep smooth, centred fields free of shading bias. */
    const int left[] = {a,a+1,b+1,a,b+1,b}, right[] = {a,a+1,b,a+1,b+1,b};
    const int *triangles = col < (cols-1)/2 ? left : right;
    for (unsigned t = 0; t < 6; t += 3) {
      const int *tri = &triangles[t];
      if (vertices[tri[0]].color.a == 0 && vertices[tri[1]].color.a == 0 &&
          vertices[tri[2]].color.a == 0) continue;
      if (!AppendSceneClippedTriangle(writer,mesh,vertices,mapped,tri,clip,
              project_point,userdata)) return false;
    }
  }
  return true;
}

static bool CastleOpening(const ActionCastleField *f, ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ActionCastleSource *s, const ActionEffectLocalRect *clip,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const CastleScatter up = CastleWindowScatter(f, s,true);
  enum { cols = kCastleOpeningColumns, rows = kCastleOpeningRows };
  ArRenderVertex2D vertices[cols*rows];
  int mapped[cols*rows];
  const float gain = f->Opening[0];
  for (int row = 0; row < rows; row++) for (int col = 0; col < cols; col++) {
    const float across = col*.5f-1, depth = row/(float)(rows-1);
    const float width = fmaxf(1,s->width-f->Opening[1]);
    const float top = s->y+CastleArchRow(up.arch,across*width)+f->Opening[2];
    const float y = top+(s->sill-f->Opening[3]-top)*depth;
    const float vertical = row == 0 || row == rows-1 ? 0 : 1;
    const int at = row*cols+col;
    vertices[at] = (ArRenderVertex2D){{s->x+width*across-mesh->world_x,y-mesh->world_y},
      {f->OpeningColor[0],f->OpeningColor[1],f->OpeningColor[2],gain*SceneSoftFalloff(across)*vertical},{0,0}};
    mapped[at] = -1;
  }
  if (!CastleGrid(writer,mesh,clip,vertices,mapped,cols,rows,project_point,userdata)) return false;
  /* A shallow wash on the stone sill connects the airborne light to its source. */
  enum { ledge_cols = kCastleLedgeColumns, ledge_rows = kCastleLedgeRows };
  ArRenderVertex2D ledge[ledge_cols*ledge_rows];
  int indices[ledge_cols*ledge_rows];
  for (int row = 0; row < ledge_rows; row++) for (int col = 0; col < ledge_cols; col++) {
    const float across = col*.5f-1, down = (float)row-1;
    const int at = row*ledge_cols+col;
    ledge[at] = (ArRenderVertex2D){{s->x+across*(s->width+f->Ledge[0])-mesh->world_x,
      s->sill+f->Ledge[1]+down*f->Ledge[2]-mesh->world_y},
      {f->LedgeColor[0],f->LedgeColor[1],f->LedgeColor[2],row == 1 ? f->Ledge[3]*SceneSoftFalloff(across) : 0},{0,0}};
    indices[at] = -1;
  }
  return CastleGrid(writer,mesh,clip,ledge,indices,ledge_cols,ledge_rows,project_point,userdata);
}

static CastleScatter CastleLitScatter(const ActionCastleField *f, const CastleScatter *source, bool upper) {
  CastleScatter result = *source;
  result.length *= upper ? f->Scatter[0] : f->Scatter[1];
  if (!upper) result.spread = result.width+(result.spread-result.width)*f->Scatter[2];
  return result;
}

static bool CastleWindowFan(const ActionCastleField *f, ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ActionEffectLocalRect *clip, const CastleScatter *source,
    float strength, unsigned identity, bool upper,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const bool boss = f->Style[0] == 2;
  const CastleScatter scatter = CastleLitScatter(f, source,upper);
  const float breath = f->Breath[2]+f->Breath[3]*sinf((mesh->phase_ticks&((unsigned)f->Breath[0]-1))*f->Breath[1]+identity);
  strength *= breath*(upper ? f->Scatter[3] : f->Scatter[4]);
  const int cols = boss ? kCastleBossFanColumns : kCastleSoftFanColumns;
  enum { rows = kCastleFanRows };
  const float *depths=f->FanDepths;
  ArRenderVertex2D vertices[kCastleBossFanColumns*rows];
  int mapped[kCastleBossFanColumns*rows];
  const uint32_t seed = DeterministicHash_Mix32(identity*113u+(unsigned)f->Seeds[0]);
  const float offset = (HashUnit(seed)-.5f)*f->FanOffset[0];
  for (int row = 0; row < rows; row++) for (int col = 0; col < cols; col++) {
    const float across = col*(2.0f/(cols-1))-1;
    ArRenderPointF point = CastleRayPoint(&scatter,depths[row],across);
    if (scatter.arch) point.y -= scatter.arch->haze_inset;
    point.x -= mesh->world_x;
    point.y -= mesh->world_y;
    /* Only the boss has distinct broad shafts. Ordinary windows use the
     * accepted soft wash; their lower-frequency envelope needs fewer columns. */
    float pattern = 1;
    if (boss && !upper) pattern = f->FanPattern[0]+f->FanPattern[1]*fminf(1,
        f->FanPattern[2]*SceneSoftFalloff((across+f->FanPattern[3]-offset)/f->FanPattern[4])+
        SceneSoftFalloff((across-offset)/f->FanPattern[5])+
        f->FanPattern[6]*SceneSoftFalloff((across-f->FanPattern[7]-offset)/f->FanPattern[8]));
    const float envelope = f->FanFade[row];
    const int at = row*cols+col;
    vertices[at] = (ArRenderVertex2D){point,{upper ? f->UpperColor[0] : f->LowerColor[0],upper ? f->UpperColor[1] : f->LowerColor[1],upper ? f->UpperColor[2] : f->LowerColor[2],
        strength*envelope*SceneSoftFalloff(across)*pattern},{0,0}};
    mapped[at] = -1;
  }
  return CastleGrid(writer,mesh,clip,vertices,mapped,cols,rows,project_point,userdata);
}

static bool CastleGalleryStone(const ActionCastleField *f, ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ActionCastleSource *s, const ActionEffectLocalRect *clip, unsigned identity,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const uint32_t seed = DeterministicHash_Mix32(identity*157u+(unsigned)f->Seeds[1]);
  const float intensity = f->GalleryGain[0]+f->GalleryGain[1]*HashUnit(seed);
  /* These 40px-spaced arches meet the walkway at y=208. Each soft pool
   * leaves a dark interval below its neighboring pillars, never a solid bar. */
  ArRenderVertex2D floor[15];
  int mapped[15];
  const float *radius=f->GalleryRadius;
  for (int row = 0; row < 3; row++) for (int col = 0; col < 5; col++) {
    const float across = col*.5f-1;
    const int at = row*5+col;
    floor[at] = (ArRenderVertex2D){{s->x+across*radius[row]-mesh->world_x,s->sill+f->GalleryFloor[row]-mesh->world_y},
        {f->GalleryColor[0],f->GalleryColor[1],f->GalleryColor[2],row == 1 ? intensity*SceneSoftFalloff(across) : 0},{0,0}};
    mapped[at] = -1;
  }
  if (!CastleGrid(writer,mesh,clip,floor,mapped,5,3,project_point,userdata)) return false;
  /* Highlight only the inner edges; pillar centres and gaps remain dark.
   * The field is fixed to BG1, not to the visible position of the BG2 moon. */
  for (int side = -1; side <= 1; side += 2) {
    ArRenderVertex2D edge[9];
    int indices[9];
    const float x = s->x+side*f->GalleryEdges[0];
    for (int row = 0; row < 3; row++) for (int col = 0; col < 3; col++) {
      const int at = row*3+col;
      edge[at] = (ArRenderVertex2D){{x+(col-1)*f->GalleryEdges[1]-mesh->world_x,
          s->y+f->GalleryEdges[2]+row*f->GalleryEdges[3]-mesh->world_y},
          {f->GalleryColor[0],f->GalleryColor[1],f->GalleryColor[2],row == 1 && col == 1 ? intensity*f->GalleryGain[2] : 0},{0,0}};
      indices[at] = -1;
    }
    if (!CastleGrid(writer,mesh,clip,edge,indices,3,3,project_point,userdata)) return false;
  }
  return true;
}

static bool CastleDust(const ActionCastleField *f, ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ActionCastleSource *s, const ActionEffectLocalRect *clip, unsigned identity,
    const CastleWindowJoin *join, const ActionCastleSource *above,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const CastleScatter up = above ? CastleMakeJoin(f, above,s).up : CastleWindowScatter(f, s,true);
  const CastleScatter down = join ? join->down : CastleWindowScatter(f, s,false);
  const unsigned count = (unsigned)f->DustCounts[s->kind != kActionCastleSource_Window];
  for (unsigned i = 0; i < count; i++) {
    const uint32_t seed = DeterministicHash_Mix32(identity*131u+i*71u+(unsigned)f->Seeds[2]);
    const unsigned period = (unsigned)f->DustPeriods[(seed&1u)?0:1];
    const float t = ((mesh->phase_ticks+seed)&(period-1))/(float)period;
    const float angle = t*6.2831853f;
    const float depth = f->DustDepth[0]+(s->kind != kActionCastleSource_Window ? f->DustDepth[2] : f->DustDepth[1])*
        HashUnit(seed^0xB1u)+f->DustDepth[3]*sinf(angle);
    const float across = HashUnit(seed^0x72u)*f->DustAcross[0]-f->DustAcross[1]+f->DustAcross[2]*sinf(angle*f->DustAcross[3]+i);
    ArRenderPointF point;
    if (s->kind != kActionCastleSource_Window) {
      const float radius = s->width+(s->spread-s->width)*depth;
      point = (ArRenderPointF){s->x+s->lean*depth+radius*across,s->y+s->length*depth};
    } else {
      const bool upper = (i&3u) == 0;
      const CastleScatter scatter = CastleLitScatter(f, upper ? &up : &down,upper);
      /* Place motes in the same shortened volumes as the chosen light style. */
      point = CastleRayPoint(&scatter,depth,across);
    }
    /* Advect inside the light volume rather than orbiting a fixed point by
     * a fraction of a pixel. Each seed has an independent phase and lifetime;
     * the unchanged captured clock freezes both motion and fade on pause. */
    const float x = point.x-mesh->world_x;
    const float y = point.y-mesh->world_y;
    const float fade = sinf(t*3.14159265f);
    if (x < clip->x0-1 || x > clip->x1+1 || y < clip->y0-1 || y > clip->y1+1) continue;
    const float alpha = (f->DustGain[0]+f->DustGain[1]*HashUnit(seed^0x33u))*fade*fade;
    if (!CastleDiamond(writer,mesh,clip,x,y,f->DustSize[0]+f->DustSize[1]*HashUnit(seed),
            (ArRenderColorF){f->DustColor[0],f->DustColor[1],f->DustColor[2],alpha},project_point,userdata)) return false;
  }
  return true;
}

bool AppendCastleEnvironment(const ActionCastleField *f, ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
                             const ActionNativeMembers *members, bool lighting, bool particles,
                             ActionEffectProjectPointFn project_point,
                             ActionEffectClipBoundsFn clip_bounds, void *userdata) {
  if(!f)return true;
  ActionEffectLocalRect clip;
  if (!CastleClip(effect,clip_bounds,userdata,&clip)) return true;
  ActionEffectInstance mesh = *effect;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  if (effect->kind == kActionEffect_CastleWater)
    return CastleWater(f, writer,&mesh,&clip,lighting,particles,project_point,userdata);
  if (effect->kind == kActionEffect_CastleSky) {
    if (!lighting) return true;
    /* Sky receives light behind the native castle silhouette, so windows and
     * transparent stone cutouts remain open. No foreground-wide brightness. */
    if (!AppendSceneSoftPatch(writer,&mesh,&clip,0,0,f->SkyShape[0],f->SkyShape[1],
            (ArRenderColorF){f->SkyColor[0],f->SkyColor[1],f->SkyColor[2],f->SkyColor[3]},0,project_point,userdata)) return false;
    /* In the boss chamber the same fan stays on BG2, behind the opaque BG1
     * back wall. Its actual window pixels reveal only the surviving portions;
     * the arch/sill scatter is still a separate BG1 contribution. */
    return !f->SkyRays[0] ||
        AppendBloodpoolSkyRays(&f->moon,f->SkyRays[1],writer,&mesh,project_point,clip_bounds,userdata);
  }
  if (effect->kind == kActionEffect_CastleMist) {
    if (!particles) return true;
    /* Keep each patch's bottom on the captured collision floor. Stable world
     * cells carry the variation; clipping does not reposition a patch. */
    const int left = ((int)floorf((clip.x0+effect->world_x)/f->MistCells[0])-1)*(int)f->MistCells[0];
    const int right = (int)ceilf(clip.x1+effect->world_x);
    for (int x = left; x < right; x += (int)f->MistCells[0]) {
      const unsigned source_begin = writer->source ? writer->source->count : 0;
      const uint32_t seed = DeterministicHash_Mix32((unsigned)x+(unsigned)f->MistSeed[0]);
      const float t = ((effect->phase_ticks+seed)&((unsigned)f->MistCells[1]-1))*f->MistCells[2];
      const float ry = f->MistShape[0]+f->MistShape[1]*HashUnit(seed);
      const float opacity = f->MistColor[3];
      if (!AppendSceneSoftPatch(writer,&mesh,&clip,x-effect->world_x+f->MistShape[2]*sinf(t),-ry,
              f->MistShape[3]+f->MistShape[4]*HashUnit(seed^7u),ry,(ArRenderColorF){f->MistColor[0],f->MistColor[1],f->MistColor[2],opacity},
              f->MistShape[5]*cosf(t),project_point,userdata)) return false;
      /* The reference selects whole cells using the projected plane's clip
       * window, then clips each patch. Defer that same selection when the
       * window depends on GPU-resident motion; triangle clipping alone would
       * admit an extra patch overlapping the far edge. */
      if (writer->source) for (unsigned i=source_begin;i<writer->source->count;++i) {
        ActionEffectSourcePrimitive *p=&writer->source->primitives[i];
        p->origin[2]=(float)((unsigned)p->origin[2]|8u);
        p->points[5][0]=effect->world_x;
        p->points[5][1]=x;
        p->points[5][2]=f->MistCells[0];
      }
    }
    return true;
  }
  if (effect->kind != kActionEffect_CastleLight) return true;
  ActionCastleSource sources[kActionCastleFieldMaxSources];
  memcpy(sources,f->sources,(unsigned)f->SourceCount[0]*sizeof(sources[0]));
  ActionEffectInstance source_effect = *effect;
  unsigned ordinal = 0;
  for (unsigned i = 0; i < (unsigned)f->SourceCount[0]; ++i) {
    ActionCastleSource *s = &sources[i];
    const ActionNativeMember *m = ActionEffectMembers_Find(members, effect->kind, ordinal++);
    if (!m) continue;
    if (!m->enabled) source_effect.source_mask &= (uint16_t)~(1u << (ordinal - 1));
    const float x = s->x, y = s->y;
    s->left = (int16_t)lroundf(x + m->offset_x + (s->left - x) * m->width_scale);
    s->right = (int16_t)lroundf(x + m->offset_x + (s->right - x) * m->width_scale);
    s->bottom = (int16_t)lroundf(s->sill + m->offset_y + (s->bottom - s->sill) * m->length_scale);
    s->x = (int16_t)lroundf(x + m->offset_x);
    s->y = (int16_t)lroundf(y + m->offset_y);
    s->sill += (int16_t)lroundf(m->offset_y);
    s->width = (int16_t)lroundf(s->width * m->width_scale);
    s->spread = (int16_t)lroundf(s->spread * m->width_scale);
    s->length = (int16_t)lroundf(s->length * m->length_scale);
    s->lean += (int16_t)lroundf(tanf(m->angle * .01745329252f) * s->length);
  }
  unsigned index = 0;
  for (unsigned i = 0; i < (unsigned)f->SourceCount[0]; i++) {
    const ActionCastleSource *s = &sources[i];
    const unsigned identity=s->identity;
    if (index >= 16) return false;
    const ActionNativeMember *member = ActionEffectMembers_Find(members, effect->kind, index);
    const int first_vertex = writer->vertex_count;
    int joined_start = -1, joined_end = -1;
    const bool enabled = (source_effect.source_mask & (1u << index++)) != 0;
    if (!enabled) continue;
    const ActionCastleSource *above = s->kind != kActionCastleSource_Window
                                          ? NULL
                                          : CastleStackNeighbor(f, &source_effect, s, sources, false);
    const ActionCastleSource *below = s->kind != kActionCastleSource_Window
                                          ? NULL
                                          : CastleStackNeighbor(f, &source_effect, s, sources, true);
    CastleWindowJoin join = {0};
    if (below) join = CastleMakeJoin(f, s,below);
    ActionEffectLocalRect region = clip;
    region.x0 = fmaxf(region.x0,s->left-effect->world_x);
    region.x1 = fminf(region.x1,s->right-effect->world_x);
    region.y0 = fmaxf(region.y0,s->y-(s->kind == kActionCastleSource_Torch ? 92 : s->kind != kActionCastleSource_Window ? 8 :
        f->UpperShape[0]+kCastleArchTopPadding)-effect->world_y);
    region.y1 = fminf(region.y1,(below ? below->y+join.up.arch->max_row : s->bottom)-effect->world_y);
    if (region.x0 >= region.x1 || region.y0 >= region.y1) continue;
    const float x = s->x-effect->world_x, y = s->y-effect->world_y;
    if (lighting) {
      if (s->kind == kActionCastleSource_Torch) {
        const float t = (effect->phase_ticks&((unsigned)f->TorchPulse[0]-1))*f->TorchPulse[1]+identity;
        const float pulse = f->TorchPulse[2]+f->TorchPulse[3]*sinf(t)+f->TorchPulse[4]*sinf(t*f->TorchPulse[5]+f->TorchPulse[6]);
        if (!AppendSceneSoftPatch(writer, &mesh, &region, x, y + f->TorchShape[0], s->spread,
                                  f->TorchShape[1] * (float)s->length / f->sources[i].length,
                                  (ArRenderColorF){f->TorchColor[0],f->TorchColor[1],f->TorchColor[2],f->TorchColor[3] * pulse},
                                  f->TorchShape[2] * sinf((effect->phase_ticks & ((unsigned)f->TorchShape[3]-1)) * f->TorchShape[4] + identity),
                                  project_point, userdata))
          return false;
      } else if (s->kind != kActionCastleSource_Window) {
        if (!AppendSceneSoftPatch(writer,&mesh,&region,x,y+s->length*f->AmbientShape[0],
                s->spread,s->length*f->AmbientShape[1],(ArRenderColorF){f->AmbientColor[0],f->AmbientColor[1],f->AmbientColor[2],f->AmbientColor[3]},
                s->lean,project_point,userdata)) return false;
      } else {
        if (!CastleOpening(f, writer,&mesh,s,&region,project_point,userdata) ||
            (s->style == 1 && !CastleGalleryStone(f, writer,&mesh,s,&region,identity,project_point,userdata)))
          return false;
        const float strength = CastleWindowStrength(f, s);
        const CastleScatter upper = CastleWindowScatter(f, s,true);
        const CastleScatter lower = CastleWindowScatter(f, s,false);
        if (!above && !CastleWindowFan(f, writer,&mesh,&region,&upper,strength*f->Strength[1],identity,true,
                project_point,userdata)) return false;
        if (below) {
          if (!CastleWindowFan(f, writer, &mesh, &region, &join.down, strength * f->Strength[2], identity, false,
                               project_point, userdata))
            return false;
          joined_start = writer->vertex_count;
          if (!CastleWindowFan(f, writer, &mesh, &region, &join.up,
                               CastleWindowStrength(f, below) * f->Strength[1], below->identity,
                               true, project_point, userdata))
            return false;
          joined_end = writer->vertex_count;
        } else if (!CastleWindowFan(f, writer,&mesh,&region,&lower,strength*f->Strength[2],identity,false,
                project_point,userdata)) return false;
      }
    }
    if (particles && s->kind != kActionCastleSource_Torch &&
        !CastleDust(f, writer,&mesh,s,&region,identity,below ? &join : NULL,above,
            project_point,userdata)) return false;
    /* Opening, arch, sill and column spill are light received by this masonry.
     * Sampling that same BG1 surface as an intervening shadow caster erases
     * the highlights (and the upper fan). Rear moon shafts remain occluded by
     * the ordinary BG1 painter order, including the gallery pillars. */
    if (joined_start >= 0) {
      unsigned below_ordinal = 0;
      for (const ActionCastleSource *p = sources; p < below; ++p)
        ++below_ordinal;
      const ActionNativeMember *below_member =
          ActionEffectMembers_Find(members, effect->kind, below_ordinal);
      TintEffectMember(writer, member, first_vertex, joined_start, false);
      TintEffectMember(writer, below_member, joined_start, joined_end, false);
      TintEffectMember(writer, member, joined_end, writer->vertex_count, false);
    } else
      TintEffectMember(writer, member, first_vertex, writer->vertex_count, false);
  }
  return true;
}
