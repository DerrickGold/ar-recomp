#include "regional_media_files.h"
#include "snesrecomp/support/utf8_fs.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct MediaFiles {
  void *bytes[kArRegionalMediaRelease_Count];
  ArRegionalMediaView views[kArRegionalMediaRelease_Count];
} MediaFiles;

static bool Fail(char *error,size_t capacity,const char *message) {
  if(error && capacity)snprintf(error,capacity,"%s",message);
  return false;
}
bool ArHostRegionalMediaFiles_Load(ArHostRegionalMediaFiles *host,const char *path,ArRegionalMediaRelease expected,
                                  char *error,size_t capacity) {
  if(error && capacity)error[0]=0;
  if(!host || !path || !*path || expected<=0 || expected>=kArRegionalMediaRelease_Count)
    return Fail(error,capacity,"Invalid regional media path or expected release");
  FILE *file=sr_fopen(path,"rb");
  if(!file)return Fail(error,capacity,"Cannot open regional media package");
  long length=-1;
  if(!fseek(file,0,SEEK_END))length=ftell(file);
  if(length<48 || length>kArRegionalMediaMaximumBytes || fseek(file,0,SEEK_SET)) {
    fclose(file);return Fail(error,capacity,"Invalid regional media file size");
  }
  void *bytes=malloc((size_t)length);
  bool read=bytes && fread(bytes,1,(size_t)length,file)==(size_t)length && fgetc(file)==EOF && !ferror(file);
  if(fclose(file))read=false;
  ArRegionalMediaView view;
  if(!read || !ArRegionalMedia_Parse(bytes,(size_t)length,&view)) {
    free(bytes);return Fail(error,capacity,"Regional media is incomplete, modified or unsupported");
  }
  if(view.release!=expected) {
    free(bytes);return Fail(error,capacity,"Regional donor does not match the expected release");
  }
  MediaFiles *store=host->implementation;
  if(store && store->bytes[view.release]) {
    free(bytes);return Fail(error,capacity,"Regional donor is already loaded; restart to replace assets");
  }
  if(!store) {
    store=calloc(1,sizeof(*store));
    if(!store) { free(bytes);return Fail(error,capacity,"Cannot allocate regional media store"); }
    host->implementation=store;
  }
  store->bytes[view.release]=bytes;store->views[view.release]=view;return true;
}
const ArRegionalMediaView *ArHostRegionalMediaFiles_View(
    const ArHostRegionalMediaFiles *host,ArRegionalMediaRelease release) {
  const MediaFiles *store=host?host->implementation:NULL;
  return store && release>0 && release<kArRegionalMediaRelease_Count && store->bytes[release]?
      &store->views[release]:NULL;
}
void ArHostRegionalMediaFiles_Destroy(ArHostRegionalMediaFiles *host) {
  if(!host || !host->implementation)return;
  MediaFiles *store=host->implementation;
  for(unsigned i=0;i<kArRegionalMediaRelease_Count;++i)free(store->bytes[i]);
  free(store);host->implementation=NULL;
}
