#include "snesrecomp/support/utf8_fs.h"
#include "save_slots.h"
#include "byte_order.h"
#include "deterministic_hash.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <sys/utime.h>
#else
#include <utime.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <direct.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

enum { kIndexSize = 16 + kSaveSlotCount * 24 + 8, kRequestSize = 40 };
static bool Fail(SaveError *e, const char *message) {
  if(e)snprintf(e->message,sizeof(e->message),"%s",message);
  return false;
}
static uint64_t Hash(const void *bytes,size_t size) {
  return DeterministicHash_Fnv1a64(DETERMINISTIC_HASH_FNV1A64_OFFSET,bytes,size);
}
static uint64_t Read64(const uint8_t *p) {
  return ByteOrder_ReadLe32(p) | ((uint64_t)ByteOrder_ReadLe32(p+4)<<32);
}
static void Write64(uint8_t *p,uint64_t n) {
  ByteOrder_WriteLe32(p,(uint32_t)n);ByteOrder_WriteLe32(p+4,(uint32_t)(n>>32));
}
static bool Path(const SaveSlots *s,const char *leaf,char *out,size_t cap) {
  int n=snprintf(out,cap,"%s/%s",s->root,leaf);return n>0 && (size_t)n<cap;
}
bool SaveSlots_Paths(const SaveSlots *s,unsigned slot,char *native,char *ini,size_t cap) {
  if(!s || !native || !ini || !cap || slot>=kSaveSlotCount)return false;
  char leaf[64];
  snprintf(leaf,sizeof(leaf),s->layout==3 || slot?"slots/%02u/save.srm":"save.srm",slot+1);
  if(!Path(s,leaf,native,cap))return false;
  snprintf(leaf,sizeof(leaf),s->layout==3 || slot?"slots/%02u/save.ini":"save.ini",slot+1);
  return Path(s,leaf,ini,cap);
}
static bool DraftPath(const SaveSlots *s,unsigned slot,char *out) {
  char leaf[64];snprintf(leaf,sizeof(leaf),s->layout==3?"slots/%02u/new-game.ardraft":"new-game-%02u.ardraft",slot+1);
  return slot<kSaveSlotCount && Path(s,leaf,out,kSaveSlotPathCapacity);
}
/* 0 missing, 1 regular file, -1 inaccessible/non-file. */
static int ProbePath(const char *path,uint64_t *mtime,bool regular) {
#ifdef _WIN32
  struct _stat64 st;
  wchar_t *wide=sr_win_path(path);
  if(!wide)return -1;
  int result=_wstat64(wide,&st);free(wide);
  if(result)return errno==ENOENT?0:-1;
  if(regular && (st.st_mode&_S_IFMT)!=_S_IFREG)return -1;
#else
  struct stat st;
  if(stat(path,&st))return errno==ENOENT?0:-1;
  if(regular && !S_ISREG(st.st_mode))return -1;
#endif
  if(mtime)*mtime=st.st_mtime>0?(uint64_t)st.st_mtime:0;
  return 1;
}
static int Probe(const char *path,uint64_t *mtime) {return ProbePath(path,mtime,true);}
static bool Read(const char *path,void *out,size_t cap,size_t *size,SaveError *e) {
  FILE *f=sr_fopen(path,"rb");if(!f)return Fail(e,"Cannot read save-slot metadata.");
  size_t n=fread(out,1,cap,f);
  int extra=(!ferror(f) && !feof(f))?fgetc(f):EOF;
  bool ok=extra==EOF && !ferror(f);
  if(fclose(f))ok=false;if(!ok)return Fail(e,"Save-slot metadata is unreadable or too large.");
  *size=n;return true;
}
bool SaveSlots_Flush(SaveSlots *s,SaveError *e) {
  if(!s || !s->lock)return Fail(e,"The save collection is not writable.");
  if(!s->dirty)return true;
  uint8_t bytes[kIndexSize]={0};memcpy(bytes,"ARSLOTS3",8);
  bytes[8]=s->active;bytes[9]=s->previous;bytes[10]=s->layout;bytes[11]=s->legacy_native;
  for(unsigned i=0;i<kSaveSlotCount;++i) {
    uint8_t *p=bytes+16+i*24;const SaveSlotRecord *r=&s->records[i];
    p[0]=r->backend;p[1]=r->ever_saved;p[2]=r->prepared;
    p[3]=r->checkpoint_required;p[4]=r->prepared_backend;
    Write64(p+8,r->saved_at);Write64(p+16,r->saved_image);
  }
  Write64(bytes+kIndexSize-8,Hash(bytes,kIndexSize-8));
  char path[kSaveSlotPathCapacity];
  if(!Path(s,"slots.armanager",path,sizeof(path)) || !Save_WriteCompanionFile(path,bytes,sizeof(bytes),e))return false;
  s->dirty=false;return true;
}
bool SaveSlots_ObserveCheckpoints(SaveSlots *s,SaveError *e) {
  if(!s || !s->lock)return Fail(e,"The save collection is not writable.");
  for(unsigned i=0;i<kSaveSlotCount;++i) {
    char native[kSaveSlotPathCapacity],ini[kSaveSlotPathCapacity],checkpoint[kSaveSlotPathCapacity+16];
    SaveSlots_Paths(s,i,native,ini,sizeof(native));
    if(i==0 && s->legacy_native)Path(s,"actraiser.srm",native,sizeof(native));
    snprintf(checkpoint,sizeof(checkpoint),"%s.archeckpoint",i==0 && s->legacy_native?native:s->records[i].backend==kSaveBackend_Ini?ini:native);
    if(!s->records[i].checkpoint_required && Probe(checkpoint,NULL)!=0) {
      s->records[i].checkpoint_required=true;s->dirty=true;
    }
  }
  return SaveSlots_Flush(s,e);
}
void SaveSlots_Close(SaveSlots *s) {
  if(s && s->lock){fclose((FILE *)s->lock);s->lock=NULL;}
}
#include "save_slots_migration.inc"

bool SaveSlots_Open(SaveSlots *s,const char *root,SaveBackend legacy,SaveError *e) {
  if(!s || !root || strlen(root)>kSaveSlotPathCapacity-64 || (unsigned)legacy>=kSaveBackend_Count)
    return Fail(e,"Invalid save collection path or format.");
  *s=(SaveSlots){.layout=2};snprintf(s->root,sizeof(s->root),"%s",root);
  if(sr_mkdir(root) && errno!=EEXIST)return Fail(e,"Cannot create the save directory.");
  char path[kSaveSlotPathCapacity];Path(s,"slots.lock",path,sizeof(path));
  FILE *lock=sr_fopen(path,"a+b");if(!lock)return Fail(e,"Cannot open the save collection lock.");
#ifdef _WIN32
  OVERLAPPED overlapped={0};
  bool locked=LockFileEx((HANDLE)_get_osfhandle(_fileno(lock)),LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,0,1,0,&overlapped)!=0;
  if(locked)SetHandleInformation((HANDLE)_get_osfhandle(_fileno(lock)),HANDLE_FLAG_INHERIT,0);
#else
  bool locked=flock(fileno(lock),LOCK_EX|LOCK_NB)==0;
  if(locked)fcntl(fileno(lock),F_SETFD,FD_CLOEXEC);
#endif
  if(!locked){fclose(lock);return Fail(e,"Another game instance is using these saves. Close it and retry.");}
  s->lock=lock;
  Path(s,"slots.armanager",path,sizeof(path));int exists=Probe(path,NULL);
  if(exists<0)goto invalid;
  if(exists) {
    uint8_t bytes[kIndexSize];size_t n=0;
    if(!Read(path,bytes,sizeof(bytes),&n,e))goto failed;
    bool version1=n==sizeof(bytes) && !memcmp(bytes,"ARSLOTS1",8);
    bool version2=n==sizeof(bytes) && !memcmp(bytes,"ARSLOTS2",8);
    bool version3=n==sizeof(bytes) && !memcmp(bytes,"ARSLOTS3",8);
    if(n!=sizeof(bytes) || (!version1 && !version2 && !version3) || Read64(bytes+n-8)!=Hash(bytes,n-8) ||
        bytes[8]>=kSaveSlotCount || bytes[9]>=kSaveSlotCount)goto invalid;
    if(version3) {
      if((bytes[10]!=2 && bytes[10]!=3) || bytes[11]>1 || (bytes[10]==3 && bytes[11]))goto invalid;
      s->layout=bytes[10];s->legacy_native=bytes[11];
    }
    for(unsigned i=version3?12:10;i<16;++i)if(bytes[i])goto invalid;
    if(!version3 && !PreserveOldIndex(s,e))goto failed;
    s->active=bytes[8];s->previous=bytes[9];
    for(unsigned i=0;i<kSaveSlotCount;++i) {
      const uint8_t *p=bytes+16+i*24;
      if(p[0]>=kSaveBackend_Count || p[1]>1 || p[2]>1)goto invalid;
      if(!version1 && (p[3]>1 || p[4]>=kSaveBackend_Count))goto invalid;
      for(unsigned j=version1?3:5;j<8;++j)if(p[j])goto invalid;
      s->records[i]=(SaveSlotRecord){.backend=p[0],.ever_saved=p[1],.prepared=p[2],
        .checkpoint_required=!version1 && p[3],.prepared_backend=version1?p[0]:p[4],
        .saved_at=Read64(p+8),.saved_image=Read64(p+16)};
      if(version1) {
        /* Preserve the sole legacy-adoption case, but require metadata for
         * every campaign written or prepared by the original slot manager. */
        s->records[i].checkpoint_required=p[2] || Read64(p+8) || (i && p[1]);
        s->dirty=true;
      }
    }
  } else {
    /* A missing index is only a first-run adoption when no managed collection
     * exists. Never infer empty slots over directories from a damaged index. */
    Path(s,"slots",path,sizeof(path));
    if(ProbePath(path,NULL,false)!=0)goto invalid;
    Path(s,"slot-switch.arrequest",path,sizeof(path));if(Probe(path,NULL)!=0)goto invalid;
    for(unsigned i=0;i<kSaveSlotCount;++i){DraftPath(s,i,path);if(Probe(path,NULL)!=0)goto invalid;}
    s->records[0].backend=legacy;s->adopted=true;
    char native[kSaveSlotPathCapacity],ini[kSaveSlotPathCapacity];
    SaveSlots_Paths(s,0,native,ini,sizeof(native));
    s->records[0].ever_saved=Probe(legacy==kSaveBackend_Ini?ini:native,NULL)!=0;
    if(!s->records[0].ever_saved) {
      Path(s,"actraiser.srm",path,sizeof(path));
      s->legacy_native=Probe(path,NULL)!=0;
      s->records[0].ever_saved=s->legacy_native;
    }
    s->dirty=true;
  }
  if(s->layout!=3)s->dirty=true;
  if(!SaveSlots_ObserveCheckpoints(s,e))goto failed;
  s->destination=s->active;
  Path(s,"slot-switch.arrequest",path,sizeof(path));exists=Probe(path,NULL);
  if(exists<0)goto invalid;
  if(exists) {
    uint8_t bytes[kRequestSize];size_t n=0;
    if(!Read(path,bytes,sizeof(bytes),&n,e))goto failed;
    if(n!=sizeof(bytes) || memcmp(bytes,"ARSWITCH",8) || bytes[8]!=1 ||
        bytes[9]>=kSaveSlotCount || bytes[10]>=kSaveSlotCount || bytes[11]>1 ||
        Read64(bytes+32)!=Hash(bytes,32))goto invalid;
    for(unsigned i=12;i<16;++i)if(bytes[i])goto invalid;
    for(unsigned i=24;i<32;++i)if(bytes[i])goto invalid;
    if(s->active!=bytes[9] && s->active!=bytes[10])goto invalid;
    s->pending=true;s->previous=bytes[9];s->destination=bytes[10];s->new_game=bytes[11];s->expected=Read64(bytes+16);
  }
  if(s->layout!=3 && !MigrateLayout(s,e))goto failed;
  CleanupOldLayout(s);
  return true;
invalid: Fail(e,"Save-slot metadata is damaged or unsupported. All save files have been preserved.");
failed: SaveSlots_Close(s);return false;
}
static bool FingerprintFile(const char *path,uint64_t *hash) {
  int exists=Probe(path,NULL);if(exists<0)return false;
  *hash=DeterministicHash_Fnv1a64Byte(*hash,(uint8_t)exists);
  if(!exists)return true;
  FILE *f=sr_fopen(path,"rb");if(!f)return false;
  uint8_t bytes[4096];size_t n,total=0;
  while(!ferror(f) && !feof(f) && (n=fread(bytes,1,sizeof(bytes),f))!=0) {
    *hash=DeterministicHash_Fnv1a64(*hash,bytes,n);total+=n;
    if(total>1024*1024){fclose(f);return false;}
  }
  bool ok=!ferror(f);fclose(f);
  uint8_t length[8];Write64(length,total);*hash=DeterministicHash_Fnv1a64(*hash,length,8);return ok;
}
bool SaveSlots_Inspect(const SaveSlots *s,unsigned slot,SaveSlotInspection *out) {
  if(!s || !out || slot>=kSaveSlotCount)return false;
  *out=(SaveSlotInspection){.state=kSaveSlot_Unavailable};
  char native[kSaveSlotPathCapacity],ini[kSaveSlotPathCapacity],path[kSaveSlotPathCapacity];
  if(!SaveSlots_Paths(s,slot,native,ini,sizeof(native)))return false;
  const SaveSlotRecord *r=&s->records[slot];
  snprintf(out->path,sizeof(out->path),"%s",r->backend==kSaveBackend_Ini?ini:native);
  int exists=Probe(out->path,&out->modified_at);
  if(exists<0)return Fail(&out->error,"Save file is inaccessible.");
  if(!exists && Probe(r->backend==kSaveBackend_Ini?native:ini,NULL)!=0)
    return Fail(&out->error,"The slot contains a different save format. Restore its selected format before loading.");
  uint64_t fingerprint=DETERMINISTIC_HASH_FNV1A64_OFFSET;
  if(!FingerprintFile(out->path,&fingerprint))return Fail(&out->error,"Cannot inspect the save file.");
  const char *suffixes[]={".arname",".archeckpoint"};
  for(unsigned i=0;i<2;++i) {
    snprintf(path,sizeof(path),"%s%s",out->path,suffixes[i]);
    if(exists && i==1 && r->checkpoint_required && Probe(path,NULL)!=1)
      return Fail(&out->error,"This managed save requires its regional checkpoint. Restore the companion before loading.");
    if(!exists && Probe(path,NULL)!=0)return Fail(&out->error,"Save image is missing but its companion remains. Recovery is required.");
    if(!FingerprintFile(path,&fingerprint))return Fail(&out->error,"Cannot inspect a save companion.");
  }
  DraftPath(s,slot,path);
  if(!FingerprintFile(path,&fingerprint))return Fail(&out->error,"Cannot inspect the prepared new game.");
  if(!exists && (r->ever_saved || (!r->prepared && Probe(path,NULL)!=0)))
    return Fail(&out->error,"Previously saved data is missing or creation was interrupted. Recovery is required.");
  if(!exists && r->prepared && Probe(path,NULL)!=1)return Fail(&out->error,"The prepared new game is missing.");
  if(exists && !Save_LoadFile((SaveFileFormat)r->backend,out->path,out->image,&out->error))return false;
  out->state=exists?kSaveSlot_Ready:kSaveSlot_Empty;out->fingerprint=fingerprint;
  out->approximate_time=true;
  if(exists && r->saved_at && r->saved_image==Hash(out->image,sizeof(out->image))) {
    out->modified_at=r->saved_at;out->approximate_time=false;
  }
  return true;
}
bool SaveSlots_ReadDraft(const SaveSlots *s,unsigned slot,void *out,size_t cap,size_t *size,SaveError *e) {
  char path[kSaveSlotPathCapacity];
  if(!s || !out || !size || slot>=kSaveSlotCount || !s->records[slot].prepared || !DraftPath(s,slot,path))
    return Fail(e,"No prepared new game for this slot.");
  return Read(path,out,cap,size,e);
}
SaveBackend SaveSlots_DestinationBackend(const SaveSlots *s) {
  const SaveSlotRecord *record=&s->records[s->destination];
  return s->pending && s->new_game?record->prepared_backend:record->backend;
}
bool SaveSlots_Request(SaveSlots *s,unsigned slot,uint64_t inspected,const void *draft,size_t size,SaveBackend new_backend,SaveError *e) {
  if(!s || !s->lock || s->pending || slot>=kSaveSlotCount ||
      (size && (!draft || (unsigned)new_backend>=kSaveBackend_Count)) || size>kSaveSlotDraftCapacity)return Fail(e,"Cannot switch to this slot.");
  SaveSlotInspection before;
  if(!SaveSlots_Inspect(s,slot,&before)){if(e)*e=before.error;return false;}
  if(before.fingerprint!=inspected)return Fail(e,"The selected slot changed. Review it again before switching.");
  if(slot==s->active && before.state==kSaveSlot_Ready)return Fail(e,"This slot is already active.");
  if((before.state==kSaveSlot_Empty)!=!!size)return Fail(e,"An empty slot needs a new-game setup; an occupied slot must be loaded.");
  char path[kSaveSlotPathCapacity];
  if(size) {
    Path(s,"slots",path,sizeof(path));if(sr_mkdir(path) && errno!=EEXIST)return Fail(e,"Cannot create slots directory.");
    char leaf[32];snprintf(leaf,sizeof(leaf),"slots/%02u",slot+1);Path(s,leaf,path,sizeof(path));
    if(sr_mkdir(path) && errno!=EEXIST)return Fail(e,"Cannot create the slot directory.");
    DraftPath(s,slot,path);if(!Save_WriteCompanionFile(path,draft,size,e))return false;
    if(!s->records[slot].prepared)s->records[slot].prepared_backend=new_backend;
    s->records[slot].prepared=true;s->dirty=true;if(!SaveSlots_Flush(s,e))return false;
  }
  SaveSlotInspection after;
  if(!SaveSlots_Inspect(s,slot,&after)){if(e)*e=after.error;return false;}
  uint8_t bytes[kRequestSize]={0};memcpy(bytes,"ARSWITCH",8);bytes[8]=1;
  bytes[9]=s->active;bytes[10]=slot;bytes[11]=size!=0;Write64(bytes+16,after.fingerprint);Write64(bytes+32,Hash(bytes,32));
  Path(s,"slot-switch.arrequest",path,sizeof(path));
  if(!Save_WriteCompanionFile(path,bytes,sizeof(bytes),e))return false;
  s->pending=true;s->previous=s->active;s->destination=slot;s->new_game=size!=0;s->expected=after.fingerprint;
  return true;
}
bool SaveSlots_ValidateDestination(const SaveSlots *s,SaveError *e) {
  if(!s || !s->lock || s->destination>=kSaveSlotCount)return Fail(e,"The save collection is not ready.");
  SaveSlotInspection target;
  if(!SaveSlots_Inspect(s,s->destination,&target)){if(e)*e=target.error;return false;}
  if(s->pending && (target.fingerprint!=s->expected ||
      (target.state==kSaveSlot_Empty)!=s->new_game))return Fail(e,"The destination changed after the restart request. Saves have been preserved.");
  return true;
}
bool SaveSlots_Acknowledge(SaveSlots *s,SaveError *e) {
  if(!SaveSlots_ValidateDestination(s,e))return false;
  SaveSlotInspection inspection;
  if(!SaveSlots_Inspect(s,s->destination,&inspection)){if(e)*e=inspection.error;return false;}
  if(inspection.state==kSaveSlot_Ready)s->records[s->destination].ever_saved=true;
  unsigned source=s->active;
  SaveBackend previous_backend=s->records[s->destination].backend;
  s->records[s->destination].backend=SaveSlots_DestinationBackend(s);
  s->active=s->destination;s->dirty=true;
  if(!SaveSlots_Flush(s,e)) {
    s->active=source;s->records[s->destination].backend=previous_backend;return false;
  }
  if(s->pending) {
    char path[kSaveSlotPathCapacity];Path(s,"slot-switch.arrequest",path,sizeof(path));
    if(sr_remove(path) && errno!=ENOENT)return Fail(e,"Could not finish the slot restart request.");
#ifndef _WIN32
    /* Persist removal before permitting new destination writes. A crash before
     * removal is safe to retry because the destination fingerprint is intact. */
    int directory=open(s->root,O_RDONLY);
    if(directory>=0){(void)fsync(directory);close(directory);}
#endif
  }
  s->pending=false;return true;
}
bool SaveSlots_ReturnToPrevious(SaveSlots *s,SaveError *e) {
  if(!s || !s->lock || s->previous>=kSaveSlotCount)return Fail(e,"No previous save slot is available.");
  unsigned target=s->destination;s->destination=s->previous;
  SaveSlotInspection old;
  if(!SaveSlots_Inspect(s,s->destination,&old)){s->destination=target;if(e)*e=old.error;return false;}
  s->expected=old.fingerprint;s->new_game=old.state==kSaveSlot_Empty;
  s->records[s->destination].prepared_backend=s->records[s->destination].backend;
  return SaveSlots_Acknowledge(s,e);
}
bool SaveSlots_BeforeCommit(SaveSlots *s,SaveError *e) {
  if(!s || !s->lock || s->pending)return Fail(e,"Save routing is not ready for a write.");
  if(s->records[s->active].ever_saved && s->records[s->active].checkpoint_required && !s->first_write_in_progress) {
    SaveSlotInspection inspection;
    if(!SaveSlots_Inspect(s,s->active,&inspection)){if(e)*e=inspection.error;return false;}
  }
  if(!s->records[s->active].checkpoint_required){s->first_write_in_progress=true;s->records[s->active].checkpoint_required=true;s->dirty=true;}
  if(!s->records[s->active].ever_saved){s->first_write_in_progress=true;s->records[s->active].ever_saved=true;s->dirty=true;}
  return SaveSlots_Flush(s,e);
}
void SaveSlots_DidCommit(SaveSlots *s,const uint8_t *image) {
  if(!s || !s->lock || !image || s->active>=kSaveSlotCount)return;
  s->first_write_in_progress=false;
  SaveSlotRecord *r=&s->records[s->active];
  time_t now=time(NULL);
  r->ever_saved=true;r->saved_at=now>0?(uint64_t)now:0;r->saved_image=Hash(image,kActRaiserSramSize);
  /* Keep the prepared file for crash diagnostics; occupied slots ignore it. */
  s->dirty=true;SaveError ignored={{0}};(void)SaveSlots_Flush(s,&ignored);
}
