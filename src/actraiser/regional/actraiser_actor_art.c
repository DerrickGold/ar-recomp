#include "actraiser/regional/actraiser_actor_art.h"

#include "actraiser/actraiser_lzss.h"
#include "actraiser/regional/actraiser_regional_runtime.h"

static ArRegionalActorArtView s_donor;
static ArRegionalActorArtResidency s_residency;
static ArRegionalMediaBytes s_pictures[2];
static bool s_active;
static bool s_selected;
static struct {uint16_t scene;bool boundary,captured,enabled;} s_load;
bool ActRaiserActorArt_Active(void) {return s_active;}

static void Observe(void *context,uint32_t source,uint16_t destination,
    const uint8_t *bytes,size_t size) {
  (void)context;
  // Native callers already consumed the compressed image's two-byte size.
  const uint32_t linear=(source&0xffff)>=0x8002 ?
      ((source>>16)&0x7f)*0x8000+((source-2)&0x7fff) : UINT32_MAX;
  if(ArRegionalActorArt_ObserveDecode(&s_residency,linear,destination,
      (ArRegionalMediaBytes){bytes,size})) {
    const unsigned slot=(destination-0x4000)/0x1000;
    const ArRegionalActorArtBinding *binding=s_residency.banks[slot].binding;
    s_pictures[slot]=ArRegionalActorArt_Find(&s_donor,binding->scene,
        kArRegionalActorArt_Pictures,slot);
  }
}
void ActRaiserActorArt_Initialize(const ArRegionalActorArtView *donor) {
  ActRaiserActorArt_Shutdown();
  if(donor && donor->count) {
    s_donor=*donor;
    ActRaiserLzss_SetObserver(Observe,NULL);
  }
}
void ActRaiserActorArt_Shutdown(void) {
  ActRaiserLzss_SetObserver(NULL,NULL);
  s_donor=(ArRegionalActorArtView){0};
  s_residency=(ArRegionalActorArtResidency){0};
  s_pictures[0]=s_pictures[1]=(ArRegionalMediaBytes){0};
  s_active=s_selected=false;s_load.scene=0;
  s_load.boundary=s_load.captured=s_load.enabled=false;
}
void ActRaiserActorArt_BeginRoom(uint16_t scene) {
  s_load.scene=scene;s_load.captured=s_load.enabled=false;
  // The 13 act-entry scripts replace the ordinary animation bank. Other
  // rooms may change only the boss atlas, retaining ordinary graphics and
  // their shared palette. Do not activate a new region in that partial load.
  s_load.boundary=ArRegionalActorArt_Binding(scene,kArRegionalActorArt_Pictures,0)!=NULL;
}
static const ArRegionalActorArtBinding *UploadBinding(uint16_t scene,
    ArRegionalActorArtKind kind,unsigned slot,uint32_t source) {
  if(scene!=s_load.scene || (kind!=kArRegionalActorArt_Characters && kind!=kArRegionalActorArt_Palette))return NULL;
  const ArRegionalActorArtBinding *binding=ArRegionalActorArt_Binding(scene,kind,slot);
  return binding && binding->source==source?binding:NULL;
}
bool ActRaiserActorArt_NeedsUpload(uint16_t scene,ArRegionalActorArtKind kind,
    unsigned slot,uint32_t source) {
  if(!s_donor.count || !UploadBinding(scene,kind,slot,source))return false;
  bool enabled=s_load.captured?s_load.enabled:s_selected;
  if(!s_load.captured && s_load.boundary &&
      !ActRaiserRegional_ActorArtwork((scene&255)-1,false,&enabled))return false;
  // An active JP image also needs interception when returning to US so the
  // drawing owner retires the old projection along with the first US upload.
  return s_active || enabled;
}
bool ActRaiserActorArt_Upload(uint16_t scene,ArRegionalActorArtKind kind,
    unsigned slot,uint32_t source,ArRegionalMediaBytes *out) {
  if(!out)return false;
  const ArRegionalActorArtBinding *binding=UploadBinding(scene,kind,slot,source);
  if(!binding) {*out=(ArRegionalMediaBytes){0};return true;}
  if(!s_load.captured) {
    s_load.enabled=s_selected;
    if(s_load.boundary &&
        !ActRaiserRegional_ActorArtwork((scene&255)-1,true,&s_load.enabled))return false;
    s_selected=s_load.enabled;
    s_load.captured=true;s_active=s_load.enabled && s_donor.count;
  }
  if(!s_active) {*out=(ArRegionalMediaBytes){0};return true;}
  const ArRegionalMediaBytes bytes=ArRegionalActorArt_Find(&s_donor,scene,kind,slot);
  if(bytes.size!=binding->size)return false;
  *out=bytes;return true;
}
bool ActRaiserActorArt_Draw(uint16_t base,uint16_t composition,unsigned visual,
    ActRaiserActorArtDraw *out) {
  if(!out || !s_active)return false;
  const ArRegionalActorArtBinding *binding=ArRegionalActorArt_Resident(
      &s_residency,base,composition,visual);
  if(!binding)return false;
  unsigned donor_visual=visual;
  if(binding->scene==0x0405 && binding->slot==0 && visual==0x36)donor_visual=0x2c;
  ActRaiserActorArtDraw result={0};
  if(!ArRegionalActorArt_Picture(s_pictures[binding->slot],donor_visual,&result.picture))return false;
  // This body's height/cardinality and the closed head's anchor are already
  // governed by B19. Donor art may change attributes, never undo that choice.
  result.attributes_only=binding->scene==0x0305 && binding->slot==1;
  *out=result;return true;
}

void ActRaiserActorArt_ResolvePart(const ActRaiserActorArtDraw *draw,unsigned index,
    bool flip_x,bool flip_y,int16_t left,int16_t top,ActRaiserActorArtPart *part) {
  ArRegionalActorArtPart donor;
  if(!draw || !part || !ArRegionalActorArt_Part(&draw->picture,index,&donor))return;
  part->attributes=donor.attributes;
  if(!draw->attributes_only) {
    part->x=donor.x[flip_x]+left;
    part->y=donor.y[flip_y]+top;
    part->large=donor.large;
  }
}
