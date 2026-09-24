#include "regional/media/regional_actor_art.h"

#include <string.h>

static uint16_t Read16(const uint8_t *p) {
  return (uint16_t)(p[0]|(uint16_t)p[1]<<8);
}
static uint32_t Read32(const uint8_t *p) {
  return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static uint32_t Key(const uint8_t *p) {
  return (uint32_t)Read16(p)<<16|(uint32_t)p[2]<<8|p[3];
}
static int16_t Signed16(const uint8_t *p) {
  const uint16_t value=Read16(p);
  return (int16_t)(value<0x8000 ? (int32_t)value : (int32_t)value-0x10000);
}
static bool PicturesValid(ArRegionalMediaBytes bytes) {
  if(bytes.size<12 || Read16(bytes.data+2))return false;
  const unsigned count=Read16(bytes.data);
  if(!count || count>kArRegionalActorArt_MaximumPictures ||
      bytes.size<4u+4u*(count+1))return false;
  size_t cursor=4u+4u*(count+1);
  for(unsigned i=0;i<count;++i) {
    if(Read32(bytes.data+4+4*i)!=cursor || bytes.size-cursor<4)return false;
    const uint8_t *record=bytes.data+cursor;
    const unsigned parts=Read16(record);
    if(!parts || parts>kArRegionalActorArt_MaximumParts || Read16(record+2) ||
        parts>(bytes.size-cursor-4)/12)return false;
    for(unsigned p=0;p<parts;++p) {
      const uint8_t *part=record+4+12*p;
      if(part[0]>1 || part[1])return false;
      for(unsigned axis=0;axis<4;++axis) {
        const int coordinate=Signed16(part+2+axis*2);
        if(coordinate < -127 || coordinate > 383)return false;
      }
    }
    cursor+=4+12*parts;
  }
  return Read32(bytes.data+4+count*4)==cursor && cursor==bytes.size;
}
bool ArRegionalActorArt_Parse(ArRegionalMediaBytes bytes,ArRegionalActorArtView *out) {
  if(!bytes.data || !out || bytes.size<12 || bytes.size>kArRegionalMediaMaximumBytes ||
      memcmp(bytes.data,"ARACTOR1",8))return false;
  const unsigned count=Read32(bytes.data+8);
  if(!count || count>kArRegionalActorArt_MaximumEntries || bytes.size<12u+16u*count)return false;
  size_t cursor=12u+16u*count;
  uint32_t previous=0;
  for(unsigned i=0;i<count;++i) {
    const uint8_t *record=bytes.data+12+16*i;
    const unsigned area=record[0],room=record[1],kind=record[2],slot=record[3];
    const size_t size=Read32(record+8);
    if(area<1 || area>7 || room<1 || room>8 || Key(record)<=previous ||
        Read32(record+4)!=cursor || Read32(record+12) || size>bytes.size-cursor)return false;
    switch(kind) {
      case kArRegionalActorArt_Characters:if(slot>1 || size!=8192)return false;break;
      case kArRegionalActorArt_Palette:if(slot || size!=128)return false;break;
      case kArRegionalActorArt_Pictures:
        if(slot>1 || !PicturesValid((ArRegionalMediaBytes){bytes.data+cursor,size}))return false;
        break;
      default:return false;
    }
    previous=Key(record);cursor+=size;
  }
  if(cursor!=bytes.size)return false;
  *out=(ArRegionalActorArtView){bytes,count};return true;
}
ArRegionalMediaBytes ArRegionalActorArt_Find(const ArRegionalActorArtView *view,
    uint16_t scene,ArRegionalActorArtKind kind,unsigned slot) {
  if(!view || !view->bytes.data || !view->count ||
      view->count>kArRegionalActorArt_MaximumEntries ||
      view->bytes.size<12u+16u*view->count || kind<1 || kind>3 || slot>1)
    return (ArRegionalMediaBytes){0};
  const uint32_t key=(uint32_t)scene<<16|(uint32_t)kind<<8|slot;
  unsigned low=0,high=view->count;
  while(low<high) {
    const unsigned mid=low+(high-low)/2;
    const uint8_t *record=view->bytes.data+12+16*mid;
    if(Key(record)<key)low=mid+1;
    else if(Key(record)>key)high=mid;
    else {
      const size_t offset=Read32(record+4),size=Read32(record+8);
      if(offset>view->bytes.size || size>view->bytes.size-offset)break;
      return (ArRegionalMediaBytes){view->bytes.data+offset,size};
    }
  }
  return (ArRegionalMediaBytes){0};
}
bool ArRegionalActorArt_Picture(ArRegionalMediaBytes table,unsigned ordinal,
    ArRegionalActorArtPicture *out) {
  if(!table.data || !out || table.size<12)return false;
  const unsigned count=Read16(table.data);
  if(count>kArRegionalActorArt_MaximumPictures || ordinal>=count || table.size<4u+4u*(count+1))return false;
  const size_t start=Read32(table.data+4+ordinal*4),end=Read32(table.data+8+ordinal*4);
  if(start<4u+4u*(count+1) || start>table.size || end>table.size || end<start || end-start<4)return false;
  const unsigned parts=Read16(table.data+start);
  if(!parts || parts>kArRegionalActorArt_MaximumParts || end-start!=4u+12u*parts)return false;
  *out=(ArRegionalActorArtPicture){{table.data+start+4,12u*parts},parts};return true;
}
bool ArRegionalActorArt_Part(const ArRegionalActorArtPicture *picture,unsigned index,
    ArRegionalActorArtPart *out) {
  if(!picture || !out || !picture->parts.data || index>=picture->count ||
      picture->count>kArRegionalActorArt_MaximumParts || picture->parts.size!=12u*picture->count)return false;
  const uint8_t *p=picture->parts.data+12*index;
  *out=(ArRegionalActorArtPart){{Signed16(p+2),Signed16(p+4)},
      {Signed16(p+6),Signed16(p+8)},Read16(p+10),p[0]!=0};
  return true;
}
