/* A complete portable campaign is one atomic file. The game feature owner
 * validates the opaque payload; the save system binds it and the UTF-8 name
 * to a canonical SRAM image. Raw emulator exports remain a separate API. */
#include "deterministic_hash.h"
enum { kArchiveHeaderBytes=16, kArchiveHashBytes=8,
  kArchiveCapacity=kArchiveHeaderBytes+kActRaiserSramSize+kSaveCampaignPayloadCapacity+
      kLocalizedNameCapacity-1+kArchiveHashBytes };
static const char kArchiveMagic[8]={'A','R','S','A','V','E','0','1'};
typedef struct SaveCampaignArchive {
  uint8_t image[kActRaiserSramSize],payload[kSaveCampaignPayloadCapacity];
  size_t payload_size;
  char name[kLocalizedNameCapacity];
} SaveCampaignArchive;
static SaveFileFormat FormatFromPath(const char *path);
static bool ArchivePathExists(const char *path) {
  FILE *file=sr_fopen(path,"rb");
  if(file){fclose(file);return true;}
  return errno!=ENOENT;
}
static uint64_t ArchiveHash(const uint8_t *bytes,size_t size) {
  return DeterministicHash_Fnv1a64(DETERMINISTIC_HASH_FNV1A64_OFFSET,bytes,size);
}
static void ArchiveNativeName(const uint8_t *image,char *name) {
  unsigned n=0;
  for(;n<kActRaiserPlayerNameCharacterLimit;++n) {
    unsigned c=image[kSavePlayerName+n];
    if(!c || c==255)break;
    if(c<32 || c>126){n=0;break;}
    name[n]=(char)c;
  }
  name[n]=0;
}
static bool ArchiveReadName(const char *path,const uint8_t *image,char *name,bool strict,SaveError *error) {
  char companion[kLocalizedNamePathBytes],native[kActRaiserPlayerNameStorageBytes];
  int n=snprintf(companion,sizeof(companion),"%s.arname",path);
  if(n<0 || (size_t)n>=sizeof(companion))return Fail(error,"campaign name path is too long");
  FILE *file=sr_fopen(companion,"rb");
  if(!file)return errno==ENOENT || Fail(error,"cannot read campaign name companion");
  ArchiveNativeName(image,native);
  bool valid=ReadLocalizedName(file,image,native,name,kLocalizedNameCapacity);
  fclose(file);
  if(!valid && !strict){name[0]=0;return true;}
  return valid || Fail(error,"campaign name companion is stale or damaged; restore it before exporting");
}
static bool WriteCampaignArchive(const char *path,const SaveCampaignArchive *archive,SaveError *error) {
  size_t name_size=strlen(archive->name);
  size_t size=kArchiveHeaderBytes+kActRaiserSramSize+archive->payload_size+name_size;
  uint8_t *bytes=calloc(1,size+kArchiveHashBytes);
  if(!bytes)return Fail(error,"out of memory encoding campaign archive");
  memcpy(bytes,kArchiveMagic,sizeof(kArchiveMagic));
  ByteOrder_WriteLe32(bytes+8,(uint32_t)archive->payload_size);
  ByteOrder_WriteLe32(bytes+12,(uint32_t)name_size);
  memcpy(bytes+kArchiveHeaderBytes,archive->image,kActRaiserSramSize);
  memcpy(bytes+kArchiveHeaderBytes+kActRaiserSramSize,archive->payload,archive->payload_size);
  memcpy(bytes+kArchiveHeaderBytes+kActRaiserSramSize+archive->payload_size,archive->name,name_size);
  uint64_t hash=ArchiveHash(bytes,size);
  ByteOrder_WriteLe32(bytes+size,(uint32_t)hash);
  ByteOrder_WriteLe32(bytes+size+4,(uint32_t)(hash>>32));
  bool ok=Save_WriteCompanionFile(path,bytes,size+kArchiveHashBytes,error);
  free(bytes);return ok;
}
static bool ReadCampaignArchive(FILE *file,SaveCampaignArchive *out,SaveError *error) {
  uint8_t *bytes=malloc(kArchiveCapacity);
  if(!bytes)return Fail(error,"out of memory reading campaign archive");
  size_t n=fread(bytes,1,kArchiveCapacity,file);
  int extra=(!feof(file) && !ferror(file))?fgetc(file):EOF;
  bool valid=!ferror(file) && extra==EOF && n>=kArchiveHeaderBytes+kActRaiserSramSize+kArchiveHashBytes;
  if(valid) {
    size_t payload_size=ByteOrder_ReadLe32(bytes+8),name_size=ByteOrder_ReadLe32(bytes+12);
    uint64_t stored=ByteOrder_ReadLe32(bytes+n-8)|((uint64_t)ByteOrder_ReadLe32(bytes+n-4)<<32);
    valid=!memcmp(bytes,kArchiveMagic,sizeof(kArchiveMagic)) && payload_size<=sizeof(out->payload) &&
        name_size<sizeof(out->name) && n==kArchiveHeaderBytes+kActRaiserSramSize+payload_size+name_size+kArchiveHashBytes &&
        ArchiveHash(bytes,n-kArchiveHashBytes)==stored;
    if(valid) {
      memcpy(out->image,bytes+kArchiveHeaderBytes,kActRaiserSramSize);
      memcpy(out->payload,bytes+kArchiveHeaderBytes+kActRaiserSramSize,payload_size);
      memcpy(out->name,bytes+kArchiveHeaderBytes+kActRaiserSramSize+payload_size,name_size);
      out->name[name_size]=0;out->payload_size=payload_size;
      char native[kActRaiserPlayerNameStorageBytes];ArchiveNativeName(out->image,native);
      valid=Save_ChecksumValid(out->image) && strlen(out->name)==name_size &&
          (!name_size || (native[0] && ValidLocalizedName(out->name,NULL)));
    }
  }
  free(bytes);
  return valid || Fail(error,"campaign archive is damaged or unsupported; active save unchanged");
}
static bool ReadCampaignImport(const char *path,SaveCampaignArchive *out,bool *is_archive,SaveError *error) {
  FILE *file=sr_fopen(path,"rb");
  if(!file)return Fail(error,"cannot open campaign import");
  char magic[8]={0};size_t n=fread(magic,1,sizeof(magic),file);
  size_t length=strlen(path);
  *is_archive=(n>=6 && !memcmp(magic,"ARSAVE",6)) ||
      (length>=7 && !strcmp(path+length-7,".arsave"));
  if(*is_archive) {
    if(fseek(file,0,SEEK_SET)){fclose(file);return Fail(error,"cannot read campaign archive");}
    bool ok=ReadCampaignArchive(file,out,error);fclose(file);return ok;
  }
  fclose(file);
  return Save_LoadFile(FormatFromPath(path),path,out->image,error) &&
      ArchiveReadName(path,out->image,out->name,false,error);
}
bool SaveSystem_ExportCampaign(const char *path,SaveError *error) {
  ClearError(error);
  if(!s_runtime.live || !s_runtime.durable_valid || !path || !path[0] || !strcmp(path,ActivePath()))
    return Fail(error,"no durable campaign or invalid export destination");
  if(s_runtime.native_write_active || s_runtime.native_write_aborted || s_runtime.story_pending)
    return Fail(error,"complete the pending native save before exporting a campaign");
  if(!SaveSystem_ValidateActive(error))return false;
  SaveCampaignArchive *archive=calloc(1,sizeof(*archive));
  if(!archive)return Fail(error,"out of memory preparing campaign archive");
  bool ok=Save_LoadFile(ActiveFormat(),ActivePath(),archive->image,error);
  if(ok && memcmp(archive->image,s_runtime.durable,sizeof(archive->image)))
    ok=Fail(error,"save changed since load; reload before exporting");
  const SaveCommitHost *host=&s_runtime.commit_host;
  if(ok && host->read_campaign)
    ok=host->read_campaign(host->context,ActivePath(),archive->image,archive->payload,sizeof(archive->payload),&archive->payload_size,error);
  else if(ok) {
    char checkpoint[kLocalizedNamePathBytes];
    snprintf(checkpoint,sizeof(checkpoint),"%s.archeckpoint",ActivePath());
    if(host->commit || ArchivePathExists(checkpoint))ok=Fail(error,"feature owner cannot export complete campaign metadata");
  }
  if(ok && archive->payload_size>sizeof(archive->payload))ok=Fail(error,"campaign metadata exceeds archive capacity");
  if(ok)ok=ArchiveReadName(ActivePath(),archive->image,archive->name,true,error);
  if(ok)ok=WriteCampaignArchive(path,archive,error);
  free(archive);return ok;
}
