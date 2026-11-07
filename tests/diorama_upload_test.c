#include "diorama/diorama_upload.h"
#include "diorama/diorama_performance.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <string.h>

/* Real upload and mirror code with a bounded in-memory texture backend. */
enum { W=64, H=48, Pitch=72 };
typedef struct Backend {
  uint32_t pixels[W*H];
  unsigned calls;
  bool fail;
} Backend;

static bool Update(void *context, ArRenderTexture texture, const ArRenderRectI *r,
                   const void *pixels, int pitch) {
  Backend *b=context;
  assert(texture.value==1 && r && r->x>=0 && r->y>=0 && r->w>0 && r->h>0);
  assert(r->x+r->w<=W && r->y+r->h<=H && pitch>=r->w*4);
  ++b->calls;
  if(b->fail)return false;
  for(int y=0;y<r->h;++y)memcpy(b->pixels+(r->y+y)*W+r->x,
      (const uint8_t *)pixels+(size_t)y*pitch,(size_t)r->w*4);
  return true;
}
static const ArRenderBackendOps ops={.struct_size=sizeof(ArRenderBackendOps),.update_texture=Update};

DioramaPerformanceScope DioramaPerformance_Begin(DioramaPerformanceStage stage) {
  return (DioramaPerformanceScope){.stage=stage};
}
void DioramaPerformance_End(DioramaPerformanceScope scope) { (void)scope; }
void DioramaPerformance_AddPlaneSync(bool synchronized, bool changed, uint64_t bytes) {
  (void)synchronized;(void)changed;(void)bytes;
}

static void Artwork(uint32_t pixels[Pitch*H],unsigned x,unsigned y) {
  memset(pixels,0,Pitch*H*4);
  pixels[y*Pitch+x]=0xff735a29u;
  /* Nontransparent pitch padding must never enter coverage or the upload. */
  for(unsigned row=0;row<H;++row)for(unsigned column=W;column<Pitch;++column)
    pixels[row*Pitch+column]=0xffffffffu;
}
static void CheckTexture(const Backend *b,const uint32_t *pixels) {
  for(unsigned y=0;y<H;++y)assert(!memcmp(b->pixels+y*W,pixels+y*Pitch,W*4));
}

static void ToggleAndFailure(unsigned plane) {
  DioramaUpload_Reset();
  Backend backend={0};ArRenderDevice device={.ops=&ops,.context=&backend};
  ArRenderTexture textures[kDioramaPlane_Count]={0};textures[plane]=(ArRenderTexture){1};
  uint32_t art[Pitch*H];const uint8_t *pixels[kDioramaPlane_Count]={0};
  size_t pitches[kDioramaPlane_Count]={0};pixels[plane]=(const uint8_t *)art;pitches[plane]=Pitch*4;
  const uint32_t bit=1u<<plane;
  const DioramaCoverageMask upper=DioramaCoverage_Dilate(UINT64_C(1));
  const DioramaCoverageMask lower=DioramaCoverage_Dilate(UINT64_C(1)<<47);
  Artwork(art,0,0);
  DioramaUploadResult r=Diorama_Upload(&device,textures,pixels,pitches,W,H,0,0,bit);
  assert(r.synchronized_plane_mask==bit && r.changed_plane_mask==bit && r.coverage_masks[plane]==upper);
  CheckTexture(&backend,art);

  Artwork(art,W-1,H-1);
  r=Diorama_UploadResolved(&device,textures,pixels,pitches,W,H,0,0,bit,0,0,false);
  assert(r.synchronized_plane_mask==bit && r.changed_plane_mask==bit);
  assert(r.coverage_masks[plane]==DioramaCoverage_FullMask());
  CheckTexture(&backend,art);
  unsigned calls=backend.calls;
  /* No texture bytes change when interpolation is disabled again. Its old
   * upper-left mask must still be replaced by the current lower-right mask. */
  r=Diorama_UploadResolved(&device,textures,pixels,pitches,W,H,0,0,bit,0,0,true);
  assert(r.synchronized_plane_mask==bit && !r.changed_plane_mask && backend.calls==calls);
  assert(r.coverage_masks[plane]==lower && lower!=upper);

  Artwork(art,0,0);
  backend.fail=true;
  r=Diorama_UploadResolved(&device,textures,pixels,pitches,W,H,0,0,bit,0,0,false);
  assert(!r.synchronized_plane_mask && !r.changed_plane_mask && !r.coverage_masks[plane]);
  backend.fail=false;
  r=Diorama_Upload(&device,textures,pixels,pitches,W,H,0,0,bit);
  assert(r.synchronized_plane_mask==bit && r.changed_plane_mask==bit && r.coverage_masks[plane]==upper);
  CheckTexture(&backend,art);

  r=Diorama_UploadResolved(NULL,textures,pixels,pitches,W,H,0,0,bit,0,0,false);
  assert(!r.synchronized_plane_mask && !r.changed_plane_mask);
  calls=backend.calls;
  r=Diorama_Upload(&device,textures,pixels,pitches,W,H,0,0,bit);
  assert(r.synchronized_plane_mask==bit && !r.changed_plane_mask && backend.calls==calls);
  assert(r.coverage_masks[plane]==upper);

  /* A GPU-owned frame has no CPU upload. Returning to a CPU source must retry
   * the current bytes and coverage, even if that GPU frame skipped masks. */
  Artwork(art,W-1,H-1);calls=backend.calls;
  pixels[plane]=NULL;
  r=Diorama_UploadResolved(&device,textures,pixels,pitches,W,H,0,0,bit,bit,bit,false);
  assert(r.synchronized_plane_mask==bit && r.changed_plane_mask==bit && backend.calls==calls);
  pixels[plane]=(const uint8_t *)art;
  r=Diorama_Upload(&device,textures,pixels,pitches,W,H,0,0,bit);
  assert(r.synchronized_plane_mask==bit && r.changed_plane_mask==bit && r.coverage_masks[plane]==lower);
  CheckTexture(&backend,art);
  DioramaUpload_Reset();
}

typedef struct BatchBackend {
  uint32_t pixels[2][W*H], pending[2][W*H];
  unsigned begins, ends, updates, fail_update;
  bool open, fail_begin, fail_end;
} BatchBackend;

static bool BeginBatch(void *context) {
  BatchBackend *b=context;
  assert(!b->open);
  ++b->begins;
  if(b->fail_begin)return false;
  memcpy(b->pending,b->pixels,sizeof(b->pixels));
  b->updates=0;b->open=true;return true;
}
static bool EndBatch(void *context,bool commit) {
  BatchBackend *b=context;
  assert(b->open);++b->ends;b->open=false;
  if(commit&&!b->fail_end)memcpy(b->pixels,b->pending,sizeof(b->pixels));
  return !commit||!b->fail_end;
}
static bool BatchUpdate(void *context,ArRenderTexture texture,const ArRenderRectI *r,
    const void *pixels,int pitch) {
  BatchBackend *b=context;
  assert(b->open && texture.value>=1 && texture.value<=2 && r);
  assert(r->x>=0 && r->y>=0 && r->x+r->w<=W && r->y+r->h<=H);
  if(++b->updates==b->fail_update)return false;
  for(int y=0;y<r->h;++y)memcpy(b->pending[texture.value-1]+(r->y+y)*W+r->x,
      (const uint8_t *)pixels+(size_t)y*pitch,(size_t)r->w*4);
  return true;
}
static const ArRenderBackendOps batch_ops={.struct_size=sizeof(ArRenderBackendOps),
  .update_texture=BatchUpdate,.begin_texture_uploads=BeginBatch,.end_texture_uploads=EndBatch};

static void CheckBatchPixels(const BatchBackend *b,const uint32_t art[2][Pitch*H]) {
  for(unsigned i=0;i<2;++i)for(unsigned y=0;y<H;++y)
    assert(!memcmp(b->pixels[i]+y*W,art[i]+y*Pitch,W*4));
}

static void TransactionsAndSourceRevisions(void) {
  DioramaUpload_Reset();
  BatchBackend backend={0};ArRenderDevice device={.ops=&batch_ops,.context=&backend};
  const unsigned planes[]={SR_PPU_OVERLAY_BG1,kDioramaPlane_Bg1Far};
  const uint32_t mask=(1u<<planes[0])|(1u<<planes[1]);
  ArRenderTexture textures[kDioramaPlane_Count]={0};
  const uint8_t *pixels[kDioramaPlane_Count]={0};size_t pitches[kDioramaPlane_Count]={0};
  uint32_t art[2][Pitch*H];uint8_t rows[H]={0};
  for(unsigned i=0;i<2;++i) {
    Artwork(art[i],i,0);textures[planes[i]]=(ArRenderTexture){i+1};
    pixels[planes[i]]=(const uint8_t *)art[i];pitches[planes[i]]=Pitch*4;
  }
  DioramaUploadChanges changes={.plane_mask=mask,.source={.revision=1,.row_count=H}};
#define UPLOAD(m) Diorama_UploadResolvedWithChanges(&device,textures,pixels,pitches, \
    W,H,0,0,(m),0,0,false,&changes)
  DioramaUploadResult r=UPLOAD(mask);
  assert(r.synchronized_plane_mask==mask && r.changed_plane_mask==mask);
  assert(backend.updates==2 && backend.begins==1 && backend.ends==1 && !backend.open);
  CheckBatchPixels(&backend,art);
  changes.source.previous_revision=1;
  r=UPLOAD(mask);
  assert(r.synchronized_plane_mask==mask && !r.changed_plane_mask && !backend.updates);

  /* A failed later copy must discard the earlier one and invalidate both
   * speculative mirrors. Same-revision hints cannot suppress the retry. */
  art[0][Pitch+3]=0xff112233u;art[1][5*Pitch+7]=0xff456789u;
  rows[1]=rows[5]=1;changes.source.changed_rows=rows;changes.source.revision=2;
  backend.fail_update=2;
  r=UPLOAD(mask);
  assert(!r.synchronized_plane_mask && !r.changed_plane_mask && !backend.open);
  assert(backend.pixels[0][W+3]!=art[0][Pitch+3]);
  backend.fail_update=0;changes.source.previous_revision=2;
  r=UPLOAD(mask);
  assert(r.synchronized_plane_mask==mask && r.changed_plane_mask==mask && backend.updates==2);
  CheckBatchPixels(&backend,art);

  art[0][Pitch+3]^=0xff;art[1][5*Pitch+7]^=0xff;
  changes.source.revision=3;backend.fail_end=true;
  r=UPLOAD(mask);
  assert(!r.synchronized_plane_mask && !r.changed_plane_mask && !backend.open);
  backend.fail_end=false;changes.source.previous_revision=3;
  r=UPLOAD(mask);
  assert(r.synchronized_plane_mask==mask && backend.updates==2);
  CheckBatchPixels(&backend,art);

  backend.fail_begin=true;const unsigned ends=backend.ends;
  r=UPLOAD(mask);
  assert(!r.synchronized_plane_mask && backend.ends==ends && !backend.open);
  backend.fail_begin=false;r=UPLOAD(mask);
  assert(r.synchronized_plane_mask==mask && backend.updates==2);
  CheckBatchPixels(&backend,art);

  /* The rear plane misses revision four. Its next changed-row hint only
   * describes four-to-five, so its older mirror must compare every row. */
  art[0][5*Pitch+2]=0xffabcdefu;art[1][5*Pitch+2]=0xff234567u;
  memset(rows,0,sizeof(rows));rows[5]=1;changes.source.revision=4;
  r=UPLOAD(1u<<planes[0]);assert(r.synchronized_plane_mask==(1u<<planes[0]));
  art[1][9*Pitch+4]=0xff987654u;
  memset(rows,0,sizeof(rows));rows[9]=1;
  changes.source.previous_revision=4;changes.source.revision=5;
  r=UPLOAD(mask);assert(r.synchronized_plane_mask==mask);
  CheckBatchPixels(&backend,art);
  art[1][11*Pitch+3]=0xff102938u;
  memset(rows,0,sizeof(rows));rows[11]=1;
  changes.source.previous_revision=5;changes.source.revision=6;
  r=UPLOAD(1u<<planes[0]);assert(r.synchronized_plane_mask==(1u<<planes[0]));
  changes.source.previous_revision=6; /* Entire source is now unchanged. */
  r=UPLOAD(mask);assert(r.synchronized_plane_mask==mask);
  CheckBatchPixels(&backend,art);
#undef UPLOAD
  DioramaUpload_Reset();
}

int main(void) {
  assert(DioramaUpload_UsesCoverage(false));
  assert(!DioramaUpload_UsesCoverage(true));
  ToggleAndFailure(kDioramaPlane_Bg1Far);
  ToggleAndFailure(DioramaPlaneForObjectPriority(3));
  TransactionsAndSourceRevisions();
  puts("diorama_upload_test: coverage, GPU ownership, transaction rollback and skipped-revision recovery passed");
  return 0;
}
