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

int main(void) {
  assert(DioramaUpload_UsesCoverage(false));
  assert(!DioramaUpload_UsesCoverage(true));
  ToggleAndFailure(kDioramaPlane_Bg1Far);
  ToggleAndFailure(DioramaPlaneForObjectPriority(3));
  puts("diorama_upload_test: coverage toggles, unchanged bytes, failed uploads, device failure and GPU ownership passed");
  return 0;
}
