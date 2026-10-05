#include "diorama/diorama_snapshot.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Transport-only resolver seam; replay tests exercise real layer resolution. */
int Diorama_ResolveSceneLayers(const DioramaScene *scene, DioramaResolvedLayer *out) {
  (void)scene;
  out[0]=(DioramaResolvedLayer){.plane=0,.z=0.5f,.alpha=255};
  return 1;
}
static uint8_t packet[kDioramaSnapshotCapacity];
static uint32_t pixels[640*352];
static void Put(unsigned offset, uint32_t n) {
  for (unsigned i=0;i<4;i++) packet[offset+i]=(uint8_t)(n>>(i*8));
}
static void Checksum(size_t size) {
  uint32_t h=2166136261u;
  for (size_t i=0;i<size;i++) h=(h^(i>=12&&i<16?0:packet[i]))*16777619u;
  Put(12,h);
}
static void Reject(size_t size) {
  DioramaSnapshot out;
  memset(&out,0xa5,sizeof(out));
  assert(!DioramaSnapshot_Decode(packet,size,&out));
  for (size_t i=0;i<sizeof(out);i++) assert(((uint8_t *)&out)[i]==0xa5);
}
static PresentationOutcome HostBackdrop(void *userdata, ArRenderRectI viewport) {
  (void)userdata;
  (void)viewport;
  return kPresentationOutcome_Complete;
}
int main(void) {
  const DioramaRenderOptions options={.visible_planes=1,.skybox=kDioramaSky_Off};
  const DioramaScene scene={.render=&options,.map_group=1,.map_number=1};
  const DioramaCapture capture={.width=360,.height=352,.authentic_y0=64,.obj_apron=32};
  const DioramaView view={.visible_width=360,.distance_scale=1,.camera_framing_weight=1,
      .viewport={0,0,960,600}};
  DioramaSnapshot snapshot, decoded;
  assert(DioramaSnapshot_Describe(&snapshot,&capture,&view,&scene,1,0,0));
  DioramaScene host_scene=scene;
  host_scene.backdrop=HostBackdrop;
  memset(&decoded,0xa5,sizeof(decoded));
  assert(!DioramaSnapshot_Describe(&decoded,&capture,&view,&host_scene,1,0,0));
  for (size_t i=0;i<sizeof(decoded);i++) assert(((uint8_t *)&decoded)[i]==0xa5);
  for (int i=0;i<640*352;i++) pixels[i]=0x7f000000u | (uint32_t)i;
  DioramaSnapshotImage images[kDioramaSnapshotImageCount]={0};
  images[0]=(DioramaSnapshotImage){(const uint8_t *)pixels,640*4};
  size_t size=9;
  assert(!DioramaSnapshot_Encode(&snapshot,images,packet,10,&size) && size==0);
  assert(DioramaSnapshot_Encode(&snapshot,images,packet,sizeof(packet),&size));
  assert(size==4096+640*352*4);
  assert(DioramaSnapshot_Decode(packet,size,&decoded));
  assert(decoded.scene.render==&decoded.options);
  assert(decoded.capture.textures==decoded.textures && decoded.capture.pixels==decoded.rgba);
  assert(decoded.view.viewport.w==960 && decoded.layers[0].z==0.5f);
  for (int i=0;i<640*352;i++) {
    const uint8_t *p=decoded.rgba[0]+i*4;
    assert(p[0]==(uint8_t)(pixels[i]>>16) && p[1]==(uint8_t)(pixels[i]>>8));
    assert(p[2]==(uint8_t)pixels[i] && p[3]==127);
  }
  for (size_t i=0;i<size;i+=137) Reject(i);
  Reject(size+1);
  packet[4096]^=1;
  Reject(size);
  packet[4096]^=1;
  /* Valid checksums must not disguise invalid extents, flags or nonfinite floats. */
  const unsigned offsets[]={4,16,20,24,28,32,36,120,144,3000};
  for (unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
    uint8_t previous[4];
    memcpy(previous,packet+offsets[i],4);
    Put(offsets[i],0xffffffffu);
    Checksum(size);
    Reject(size);
    memcpy(packet+offsets[i],previous,4);
    Checksum(size);
  }
  snapshot.layers[0].source=1;
  memset(packet,0x5a,32);
  assert(!DioramaSnapshot_Encode(&snapshot,images,packet,sizeof(packet),&size) && !size);
  for (int i=0;i<32;i++) assert(packet[i]==0x5a);
  puts("diorama snapshot: roundtrip, canonical RGBA, validation and atomic rejection passed");
  return 0;
}
