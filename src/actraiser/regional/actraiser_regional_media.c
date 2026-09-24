#include "actraiser/regional/actraiser_regional_media.h"
#include "regional/presentation/regional_artwork.h"

static ArRegionalMediaView s_donors[kArRegionalMediaRelease_Count];
static ArRegionalActorArtView s_actor_art;
const ArRegionalActorArtView *ActRaiserRegionalMedia_ActorArt(void) {
  return s_actor_art.count?&s_actor_art:NULL;
}
ArRegionalMediaBytes ActRaiserRegionalMedia_Sequence(unsigned rule,bool enabled) {
  if(enabled && rule<2) {
    const ArRegionalMediaBytes bytes=ArRegionalMedia_Find(&s_donors[kArRegionalMediaRelease_Japan],kArRegionalMedia_Sequence09+rule);
    if(bytes.data && bytes.size==(rule?1325u:2197u))return bytes;
  }
  return (ArRegionalMediaBytes){0};
}
uint8_t ActRaiserRegionalMedia_AvailableSequences(void) {
  return (uint8_t)((ActRaiserRegionalMedia_Sequence(0,true).data?1:0) |
      (ActRaiserRegionalMedia_Sequence(1,true).data?2:0));
}
bool ActRaiserRegionalMedia_AddDonor(const ArRegionalMediaView *view) {
  if(!view || view->release<=0 || view->release>=kArRegionalMediaRelease_Count ||
      !view->count || view->count>kArRegionalMediaMaximumEntries || s_donors[view->release].count)return false;
  if(view->release==kArRegionalMediaRelease_Japan) {
    const ArRegionalMediaBytes bytes=ArRegionalMedia_Find(view,kArRegionalMedia_ActorArt);
    ArRegionalActorArtView candidate={0};
    if(bytes.data && !ArRegionalActorArt_Parse(bytes,&candidate))return false;
    s_actor_art=candidate;
  }
  s_donors[view->release]=*view;return true;
}
void ActRaiserRegionalMedia_ClearDonors(void) {
  s_actor_art=(ArRegionalActorArtView){0};
  for(unsigned i=0;i<kArRegionalMediaRelease_Count;++i)s_donors[i]=(ArRegionalMediaView){0};
}
uint8_t ActRaiserRegionalMedia_AvailableArtwork(void) {
  const ArRegionalMediaBytes bytes=ArRegionalMedia_Find(&s_donors[kArRegionalMediaRelease_Japan],kArRegionalMedia_DeathHeimBG2);
  uint8_t mask=bytes.data && bytes.size==8192?1u<<kArRegionalArtwork_DeathHeim:0;
  if(ActRaiserRegionalMedia_ActionHealth(true).data && ActRaiserRegionalMedia_ActionHud(true,0).data)
    mask|=1u<<kArRegionalArtwork_ActionItems;
  if(ActRaiserRegionalMedia_Title(true).characters.data)mask|=kArRegionalArtwork_TitleMask;
  const uint32_t ids[]={kArRegionalMedia_TownFollowerSymbols,kArRegionalMedia_TownLairSymbols,kArRegionalMedia_TownPyramidDetail};
  const size_t sizes[]={256,128,32};
  for(unsigned i=0;i<3;++i) {
    const ArRegionalMediaBytes town=ArRegionalMedia_Find(&s_donors[kArRegionalMediaRelease_Japan],ids[i]);
    if(town.data && town.size==sizes[i])mask|=1u<<(kArRegionalArtwork_FollowerSymbols+i);
  }
  return mask;
}
ArRegionalMediaBytes ActRaiserRegionalMedia_DeathHeimCharacters(bool enabled,uint16_t scene) {
  if(enabled && (scene==0x0107 || scene==0x0807)) {
    const ArRegionalMediaBytes bytes=ArRegionalMedia_Find(&s_donors[kArRegionalMediaRelease_Japan],kArRegionalMedia_DeathHeimBG2);
    if(bytes.data && bytes.size==8192)return bytes;
  }
  return (ArRegionalMediaBytes){0};
}
static ArRegionalMediaBytes EuropeanResource(uint32_t id,size_t size) {
  for(unsigned release=kArRegionalMediaRelease_Europe;release<kArRegionalMediaRelease_Count;++release) {
    const ArRegionalMediaBytes bytes=ArRegionalMedia_Find(&s_donors[release],id);
    if(bytes.data && bytes.size==size)return bytes;
  }
  return (ArRegionalMediaBytes){0};
}
ArRegionalMediaBytes ActRaiserRegionalMedia_ActionHealth(bool enabled) {
  return enabled?EuropeanResource(kArRegionalMedia_ActionHealthPickup,128):(ArRegionalMediaBytes){0};
}
ArRegionalMediaBytes ActRaiserRegionalMedia_ActionHud(bool enabled,unsigned spell) {
  if(enabled && spell<=4) {
    const ArRegionalMediaBytes bytes=EuropeanResource(kArRegionalMedia_ActionSpellHud,768);
    if(bytes.data)return (ArRegionalMediaBytes){bytes.data+(spell?spell-1:4)*128,256};
  }
  return (ArRegionalMediaBytes){0};
}
static ArRegionalMediaBytes TownResource(uint8_t mask,unsigned rule,uint32_t id,size_t size) {
  if(mask&(1u<<rule)) {
    const ArRegionalMediaBytes bytes=ArRegionalMedia_Find(&s_donors[kArRegionalMediaRelease_Japan],id);
    if(bytes.data && bytes.size==size)return bytes;
  }
  return (ArRegionalMediaBytes){0};
}
ActRaiserTitleArt ActRaiserRegionalMedia_Title(bool enabled) {
  const ArRegionalMediaBytes bytes=ArRegionalMedia_Find(&s_donors[kArRegionalMediaRelease_Japan],kArRegionalMedia_TitleBackground);
  if(enabled && bytes.data && bytes.size==33024)
    return (ActRaiserTitleArt){{bytes.data,16384},{bytes.data+16384,16384},{bytes.data+32768,256}};
  return (ActRaiserTitleArt){0};
}
unsigned ActRaiserRegionalMedia_TownSpans(uint8_t mask,ActRaiserTownArtBank bank,
    ActRaiserTownArtSpan out[kActRaiserTownArtMaximumSpans]) {
  if(!out || (unsigned)bank>kActRaiserTownArt_Objects)return 0;
  unsigned count=0;
  if(bank==kActRaiserTownArt_Objects) {
    const ArRegionalMediaBytes bytes=TownResource(mask,kArRegionalArtwork_FollowerSymbols,kArRegionalMedia_TownFollowerSymbols,256);
    if(bytes.data) {
      const uint16_t tiles[]={0x1a2,0x1a8,0x1b2,0x1b8};
      const unsigned offsets[]={0,128,64,192};
      for(unsigned i=0;i<4;++i)out[count++]=(ActRaiserTownArtSpan){tiles[i]*32,{bytes.data+offsets[i],64}};
    }
  } else {
    const ArRegionalMediaBytes pyramid=TownResource(mask,kArRegionalArtwork_PyramidDetail,kArRegionalMedia_TownPyramidDetail,32);
    if(pyramid.data)out[count++]=(ActRaiserTownArtSpan){0x113*32,pyramid};
    if(bank==kActRaiserTownArt_Early) {
      const ArRegionalMediaBytes lairs=TownResource(mask,kArRegionalArtwork_LairSymbols,kArRegionalMedia_TownLairSymbols,128);
      if(lairs.data) {
        out[count++]=(ActRaiserTownArtSpan){0x1ce*32,{lairs.data,64}};
        out[count++]=(ActRaiserTownArtSpan){0x1de*32,{lairs.data+64,64}};
      }
    }
  }
  return count;
}
