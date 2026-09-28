/* Bloodpool castle: world-attached window light, dust and supported floor haze.
 * Pure bounded geometry; shares the existing BG1/BG2 batches and direct masks.
 * No native slots, texture resolves, backend calls or persistent particles. */
#include "action/action_effect_render_internal.h"
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

static bool CastleWaterGlint(ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ActionEffectLocalRect *clip, float x, float y, float rx, float ry, float alpha,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const ArRenderColorF clear = {.44f,.72f,1,0}, lit = {.44f,.72f,1,alpha};
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

static bool CastleWater(ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ActionEffectLocalRect *clip, bool lighting, bool particles,
    ActionEffectProjectPointFn project_point, void *userdata) {
  for (unsigned strip = 0; strip < kActionCastleWaterStripCount; strip++) {
    if (!(mesh->source_mask&(1u<<strip))) continue;
    const float left = kActionCastleWaterLeft+(int)strip*kActionCastleWaterStripWidth-mesh->world_x;
    ActionEffectLocalRect region = *clip;
    region.x0 = fmaxf(region.x0,left);
    region.x1 = fminf(region.x1,left+kActionCastleWaterStripWidth);
    if (region.x0 >= region.x1) continue;
    const uint32_t seed = DeterministicHash_Mix32(strip*131u+0xCA57Eu);
    for (unsigned i = 0; i < 11; i++) {
      const bool broad = i >= 9;
      if (broad ? !lighting : !particles) continue;
      const uint32_t cell = DeterministicHash_Mix32(seed+i*71u);
      const float t = ((mesh->phase_ticks+cell)&511u)/512.0f;
      const float fade = sinf(t*3.14159265f);
      const float x = left+(broad ? 36+(i-9)*72 : 8+i*16)+3*sinf(t*6.2831853f);
      const float y = 3+7*HashUnit(cell^0x39u);
      if (!CastleWaterGlint(writer,mesh,&region,x,y,broad ? 28 : 3+4*HashUnit(cell),
              broad ? 1.4f : .7f,(broad ? .32f : .85f)*fade*fade,
              project_point,userdata)) return false;
    }
    if (!particles) continue;
    for (unsigned i = 0; i < 2; i++) {
      const uint32_t cell = DeterministicHash_Mix32(seed+i*197u);
      const float age = ((mesh->phase_ticks+cell)&511u)/104.0f;
      if (age >= 1) continue;
      const float x = left+20+104*HashUnit(cell), y = 5+3*HashUnit(cell^17u);
      const float rx = 1+age*12, ry = .3f+age*1.6f;
      const ArRenderColorF color = {.40f,.72f,1,.65f*(1-age)*fminf(1,age*8)};
      for (unsigned segment = 0; segment < 32; segment += 4) {
        const unsigned end = (segment+4)&31;
        const ArRenderVertex2D vertices[] = {
          {{x+kCircle32[segment][0]*rx,y+kCircle32[segment][1]*ry},color,{0,0}},
          {{x+kCircle32[end][0]*rx,y+kCircle32[end][1]*ry},color,{0,0}},
          {{x+kCircle32[end][0]*(rx+.75f),y+kCircle32[end][1]*(ry+.4f)},color,{0,0}},
          {{x+kCircle32[segment][0]*(rx+.75f),y+kCircle32[segment][1]*(ry+.4f)},color,{0,0}},
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

typedef struct CastleArchProfile {
  const int8_t *rows;
  unsigned count;
  float width, min_row, max_row, haze_inset;
} CastleArchProfile;

/* Upper highlighted stone edges measured from the native BG1 artwork. Each
 * entry is the row at a pixel center on the right half of the opening;
 * the left half mirrors it. Keep the stepped boss crown, not an ideal ellipse.
 * Narrow arches repeat in rooms 3/5; the same wide cap repeats in rooms 3/7. */
static const int8_t kCastleNarrowArchRows[] = {0,1,3,5};
static const int8_t kCastleWideArchRows[] = {-1,-2,-2,-2,-1,-1,0,1,3,2,1,5,6,8,8};
static const int8_t kCastleBossArchRows[] = {
  -5,-6,-6,-6,-5,-5,-4,-3,-1,-2,-3,1,2,4,4,8,
  11,10,10,10,11,11,12,13,15,14,13,17,18,20,20,
};
static const CastleArchProfile kCastleNarrowArch = {kCastleNarrowArchRows,4,4,0,5,1};
static const CastleArchProfile kCastleWideArch = {kCastleWideArchRows,15,16,-2,8,3};
static const CastleArchProfile kCastleBossArch = {kCastleBossArchRows,31,32,-6,20,7};

typedef struct CastleScatter {
  float x, y, length, width, spread, lean;
  const CastleArchProfile *arch;
} CastleScatter;

enum { kCastleArchRayMaxReach = 42, kCastleArchTopPadding = 16 };

static CastleScatter CastleWindowScatter(const ActionCastleSource *s, bool upper) {
  const CastleArchProfile *arch = !upper ? NULL : s->room == 8 ? &kCastleBossArch :
      s->sill_tile == 0x54 ? &kCastleNarrowArch : &kCastleWideArch;
  return (CastleScatter){s->x,upper ? s->y : s->sill,
      upper ? -fminf(kCastleArchRayMaxReach,(s->sill-s->y)*.53f) : s->length,
      upper ? arch->width : s->width,
      upper ? fminf(42,s->spread*.70f) : s->spread,
      upper ? s->lean*.25f : s->lean,arch};
}

static float CastleArchRow(const CastleArchProfile *arch, float offset) {
  const float at = fminf(arch->count-1,fmaxf(0,fabsf(offset)-.5f));
  const unsigned lo = (unsigned)at;
  const unsigned hi = lo+1 < arch->count ? lo+1 : lo;
  return arch->rows[lo]+(arch->rows[hi]-arch->rows[lo])*(at-lo);
}

static float CastleWindowStrength(const ActionCastleSource *s) {
  return s->room == 8 ? .86f : s->room == 7 ? .70f : .60f;
}

static const ActionCastleSource *CastleStackNeighbor(const ActionEffectInstance *effect,
    const ActionCastleSource *s, bool below) {
  const ActionCastleSource *nearest = NULL;
  bool enabled = false;
  unsigned index = 0;
  for (unsigned i = 0; i < kActionCastleSourceCount; i++) {
    const ActionCastleSource *other = &kActionCastleSources[i];
    if (other->room != s->room) continue;
    if (index >= 16) return NULL;
    const bool active = (effect->source_mask&(1u<<index++)) != 0;
    if (other->diffuse || other->x != s->x || other == s ||
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
  const CastleScatter upper = CastleWindowScatter(bottom,true);
  return top->sill < bottom->y &&
      fminf(top->sill+top->length,top->bottom) >= bottom->y+upper.arch->min_row+upper.length ? nearest : NULL;
}

typedef struct CastleWindowJoin {
  CastleScatter down, up;
} CastleWindowJoin;

static CastleWindowJoin CastleMakeJoin(const ActionCastleSource *s,
    const ActionCastleSource *below) {
  CastleWindowJoin join = {CastleWindowScatter(s,false),CastleWindowScatter(below,true)};
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

static float CastleSoftFalloff(float x) {
  const float edge = fmaxf(0,1-x*x);
  return edge*edge;
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

static bool CastleOpening(ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ActionCastleSource *s, const ActionEffectLocalRect *clip,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const CastleScatter up = CastleWindowScatter(s,true);
  enum { cols = kCastleOpeningColumns, rows = kCastleOpeningRows };
  ArRenderVertex2D vertices[cols*rows];
  int mapped[cols*rows];
  const float gain = s->room == 7 ? .11f : s->room == 8 ? .085f : .065f;
  for (int row = 0; row < rows; row++) for (int col = 0; col < cols; col++) {
    const float across = col*.5f-1, depth = row/(float)(rows-1);
    const float width = fmaxf(1,s->width-1.5f);
    const float top = s->y+CastleArchRow(up.arch,across*width)+3;
    const float y = top+(s->sill-2-top)*depth;
    const float vertical = row == 0 || row == rows-1 ? 0 : 1;
    const int at = row*cols+col;
    vertices[at] = (ArRenderVertex2D){{s->x+width*across-mesh->world_x,y-mesh->world_y},
      {.61f,.74f,.96f,gain*CastleSoftFalloff(across)*vertical},{0,0}};
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
    ledge[at] = (ArRenderVertex2D){{s->x+across*(s->width+5)-mesh->world_x,
      s->sill-1+down*3-mesh->world_y},
      {.67f,.78f,.98f,row == 1 ? .16f*CastleSoftFalloff(across) : 0},{0,0}};
    indices[at] = -1;
  }
  return CastleGrid(writer,mesh,clip,ledge,indices,ledge_cols,ledge_rows,project_point,userdata);
}

static CastleScatter CastleLitScatter(const CastleScatter *source, bool boss, bool upper) {
  CastleScatter result = *source;
  result.length *= upper ? (boss ? .22f : .35f) : (boss ? 1 : .85f);
  if (!upper) result.spread = result.width+(result.spread-result.width)*(boss ? .9f : .8f);
  return result;
}

static bool CastleWindowFan(ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ActionEffectLocalRect *clip, const CastleScatter *source,
    float strength, unsigned identity, bool upper,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const bool boss = mesh->visual == 8;
  const CastleScatter scatter = CastleLitScatter(source,boss,upper);
  const float breath = .98f+.02f*sinf((mesh->phase_ticks&4095u)*.0015339808f+identity);
  strength *= breath*(upper ? (boss ? .10f : .16f) : (boss ? .66f : .42f));
  const int cols = boss ? kCastleBossFanColumns : kCastleSoftFanColumns;
  enum { rows = kCastleFanRows };
  static const float depths[rows] = {0,.13f,.48f,1};
  static const float soft_fade[rows] = {.24f,1,.48f,0};
  static const float boss_fade[rows] = {1,.78f,.30f,0};
  ArRenderVertex2D vertices[kCastleBossFanColumns*rows];
  int mapped[kCastleBossFanColumns*rows];
  const uint32_t seed = DeterministicHash_Mix32(identity*113u+0x571Du);
  const float offset = (HashUnit(seed)-.5f)*.10f;
  for (int row = 0; row < rows; row++) for (int col = 0; col < cols; col++) {
    const float across = col*(2.0f/(cols-1))-1;
    ArRenderPointF point = CastleRayPoint(&scatter,depths[row],across);
    if (scatter.arch) point.y -= scatter.arch->haze_inset;
    point.x -= mesh->world_x;
    point.y -= mesh->world_y;
    /* Only the boss has distinct broad shafts. Ordinary windows use the
     * accepted soft wash; their lower-frequency envelope needs fewer columns. */
    float pattern = 1;
    if (boss && !upper) pattern = .12f+.88f*fminf(1,
        .75f*CastleSoftFalloff((across+.59f-offset)/.29f)+
        CastleSoftFalloff((across-offset)/.26f)+
        .66f*CastleSoftFalloff((across-.60f-offset)/.31f));
    const float envelope = boss ? boss_fade[row] : soft_fade[row];
    const int at = row*cols+col;
    vertices[at] = (ArRenderVertex2D){point,{upper ? .70f : .65f,.78f,.98f,
        strength*envelope*CastleSoftFalloff(across)*pattern},{0,0}};
    mapped[at] = -1;
  }
  return CastleGrid(writer,mesh,clip,vertices,mapped,cols,rows,project_point,userdata);
}

static bool CastleGalleryStone(ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ActionCastleSource *s, const ActionEffectLocalRect *clip, unsigned identity,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const uint32_t seed = DeterministicHash_Mix32(identity*157u+0xA7C4u);
  const float intensity = .24f+.08f*HashUnit(seed);
  /* These 40px-spaced arches meet the walkway at y=208. Each soft pool
   * leaves a dark interval below its neighboring pillars, never a solid bar. */
  ArRenderVertex2D floor[15];
  int mapped[15];
  static const float y[] = {204,208,226};
  static const float radius[] = {16,17,19};
  for (int row = 0; row < 3; row++) for (int col = 0; col < 5; col++) {
    const float across = col*.5f-1;
    const int at = row*5+col;
    floor[at] = (ArRenderVertex2D){{s->x+across*radius[row]-mesh->world_x,y[row]-mesh->world_y},
        {.54f,.72f,.98f,row == 1 ? intensity*CastleSoftFalloff(across) : 0},{0,0}};
    mapped[at] = -1;
  }
  if (!CastleGrid(writer,mesh,clip,floor,mapped,5,3,project_point,userdata)) return false;
  /* Highlight only the inner edges; pillar centres and gaps remain dark.
   * The field is fixed to BG1, not to the visible position of the BG2 moon. */
  for (int side = -1; side <= 1; side += 2) {
    ArRenderVertex2D edge[9];
    int indices[9];
    const float x = s->x+side*17.5f;
    for (int row = 0; row < 3; row++) for (int col = 0; col < 3; col++) {
      const int at = row*3+col;
      edge[at] = (ArRenderVertex2D){{x+(col-1)*1.25f-mesh->world_x,
          132+row*32-mesh->world_y},
          {.54f,.72f,.98f,row == 1 && col == 1 ? intensity*.8f : 0},{0,0}};
      indices[at] = -1;
    }
    if (!CastleGrid(writer,mesh,clip,edge,indices,3,3,project_point,userdata)) return false;
  }
  return true;
}

static bool CastleDust(ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ActionCastleSource *s, const ActionEffectLocalRect *clip, unsigned identity,
    const CastleWindowJoin *join, const ActionCastleSource *above,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const CastleScatter up = above ? CastleMakeJoin(above,s).up : CastleWindowScatter(s,true);
  const CastleScatter down = join ? join->down : CastleWindowScatter(s,false);
  const unsigned count = s->room == 8 ? 32 : s->room == 7 ? 8 : s->diffuse ? 24 : 14;
  for (unsigned i = 0; i < count; i++) {
    const uint32_t seed = DeterministicHash_Mix32(identity*131u+i*71u+0xCA571Eu);
    const float t = ((mesh->phase_ticks+seed)&2047u)/2048.0f;
    const float depth = .12f+(s->diffuse ? .82f : .48f)*HashUnit(seed^0xB1u);
    const float across = HashUnit(seed^0x72u)*1.5f-.75f;
    ArRenderPointF point;
    float drift = 2;
    if (s->diffuse) {
      const float radius = s->width+(s->spread-s->width)*depth;
      point = (ArRenderPointF){s->x+s->lean*depth+radius*across,s->y+s->length*depth};
    } else {
      const bool upper = (i&3u) == 0;
      const CastleScatter scatter = CastleLitScatter(upper ? &up : &down,s->room == 8,upper);
      /* Place motes in the same shortened volumes as the chosen light style. */
      point = CastleRayPoint(&scatter,depth,across);
      drift = .12f;
    }
    const float x = point.x+drift*sinf(t*6.2831853f+i)-mesh->world_x;
    const float y = point.y+drift*cosf(t*6.2831853f+i*.7f)-mesh->world_y;
    const float fade = sinf(t*3.14159265f);
    if (x < clip->x0-1 || x > clip->x1+1 || y < clip->y0-1 || y > clip->y1+1) continue;
    const float alpha = (.36f+.24f*HashUnit(seed^0x33u))*fade*fade;
    if (!CastleDiamond(writer,mesh,clip,x,y,.45f+.32f*HashUnit(seed),
            (ArRenderColorF){.76f,.72f,.63f,alpha},project_point,userdata)) return false;
  }
  return true;
}

bool AppendCastleEnvironment(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    bool lighting, bool particles, ActionEffectProjectPointFn project_point,
    ActionEffectClipBoundsFn clip_bounds, void *userdata) {
  ActionEffectLocalRect clip;
  if (!CastleClip(effect,clip_bounds,userdata,&clip)) return true;
  ActionEffectInstance mesh = *effect;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  if (effect->kind == kActionEffect_CastleWater)
    return CastleWater(writer,&mesh,&clip,lighting,particles,project_point,userdata);
  if (effect->kind == kActionEffect_CastleSky) {
    if (!lighting) return true;
    /* Sky receives light behind the native castle silhouette, so windows and
     * transparent stone cutouts remain open. No foreground-wide brightness. */
    if (!AppendBloodpoolSoftPatch(writer,&mesh,&clip,0,0,34,28,
            (ArRenderColorF){.34f,.48f,.82f,.18f},0,project_point,userdata)) return false;
    /* In the boss chamber the same fan stays on BG2, behind the opaque BG1
     * back wall. Its actual window pixels reveal only the surviving portions;
     * the arch/sill scatter is still a separate BG1 contribution. */
    return (effect->visual != 2 && effect->visual != 6 && effect->visual != 7 && effect->visual != 8) ||
        AppendBloodpoolSkyRays(writer,&mesh,project_point,clip_bounds,userdata);
  }
  if (effect->kind == kActionEffect_CastleMist) {
    if (!particles) return true;
    /* Keep each patch's bottom on the captured collision floor. Stable world
     * cells carry the variation; clipping does not reposition a patch. */
    const int left = ((int)floorf((clip.x0+effect->world_x)/48)-1)*48;
    const int right = (int)ceilf(clip.x1+effect->world_x);
    for (int x = left; x < right; x += 48) {
      const uint32_t seed = DeterministicHash_Mix32((unsigned)x+effect->visual*127u);
      const float t = ((effect->phase_ticks+seed)&4095u)*.0015339808f;
      const float ry = 5+3*HashUnit(seed);
      const float opacity = effect->visual == 5 ? .23f : .14f;
      if (!AppendBloodpoolSoftPatch(writer,&mesh,&clip,x-effect->world_x+5*sinf(t),-ry,
              31+9*HashUnit(seed^7u),ry,(ArRenderColorF){.31f,.35f,.48f,opacity},
              4*cosf(t),project_point,userdata)) return false;
    }
    return true;
  }
  if (effect->kind != kActionEffect_CastleLight) return true;
  unsigned index = 0;
  for (unsigned i = 0; i < kActionCastleSourceCount; i++) {
    const ActionCastleSource *s = &kActionCastleSources[i];
    if (s->room != effect->visual) continue;
    if (index >= 16) return false;
    const bool enabled = (effect->source_mask&(1u<<index++)) != 0;
    if (!enabled) continue;
    const ActionCastleSource *above = s->diffuse ? NULL : CastleStackNeighbor(effect,s,false);
    const ActionCastleSource *below = s->diffuse ? NULL : CastleStackNeighbor(effect,s,true);
    CastleWindowJoin join = {0};
    if (below) join = CastleMakeJoin(s,below);
    ActionEffectLocalRect region = clip;
    region.x0 = fmaxf(region.x0,s->left-effect->world_x);
    region.x1 = fminf(region.x1,s->right-effect->world_x);
    region.y0 = fmaxf(region.y0,s->y-(s->diffuse == 2 ? 92 : s->diffuse ? 8 :
        kCastleArchRayMaxReach+kCastleArchTopPadding)-effect->world_y);
    region.y1 = fminf(region.y1,(below ? below->y+join.up.arch->max_row : s->bottom)-effect->world_y);
    if (region.x0 >= region.x1 || region.y0 >= region.y1) continue;
    const float x = s->x-effect->world_x, y = s->y-effect->world_y;
    if (lighting) {
      if (s->diffuse == 2) {
        const float t = (effect->phase_ticks&255u)*.024543693f+i;
        const float pulse = .86f+.09f*sinf(t)+.05f*sinf(t*3+.7f);
        if (!AppendBloodpoolSoftPatch(writer,&mesh,&region,x,y+6,s->spread,86,
                (ArRenderColorF){1,.49f,.15f,.28f*pulse},
                6*sinf((effect->phase_ticks&511u)*.012271846f+i),
                project_point,userdata)) return false;
      } else if (s->diffuse) {
        if (!AppendBloodpoolSoftPatch(writer,&mesh,&region,x,y+s->length*.5f,
                s->spread,s->length*.54f,(ArRenderColorF){.30f,.40f,.65f,.13f},
                s->lean,project_point,userdata)) return false;
      } else {
        if (!CastleOpening(writer,&mesh,s,&region,project_point,userdata) ||
            (s->room == 7 && !CastleGalleryStone(writer,&mesh,s,&region,i,project_point,userdata)))
          return false;
        const float strength = CastleWindowStrength(s);
        const CastleScatter upper = CastleWindowScatter(s,true);
        const CastleScatter lower = CastleWindowScatter(s,false);
        if (!above && !CastleWindowFan(writer,&mesh,&region,&upper,strength*1.12f,i,true,
                project_point,userdata)) return false;
        if (below) {
          if (!CastleWindowFan(writer,&mesh,&region,&join.down,strength*1.35f,i,false,
                  project_point,userdata) ||
              !CastleWindowFan(writer,&mesh,&region,&join.up,CastleWindowStrength(below)*1.12f,
                  (unsigned)(below-kActionCastleSources),true,project_point,userdata)) return false;
        } else if (!CastleWindowFan(writer,&mesh,&region,&lower,strength*1.35f,i,false,
                project_point,userdata)) return false;
      }
    }
    if (particles && s->diffuse != 2 &&
        !CastleDust(writer,&mesh,s,&region,i,below ? &join : NULL,above,
            project_point,userdata)) return false;
  }
  return true;
}
