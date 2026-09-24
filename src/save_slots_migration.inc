/* Layout migration runs under the unchanged collection lock. Layout 2 remains
 * authoritative until all copies are verified and the layout-3 index commits.
 * Exact originals survive in legacy-layout; a retry never replaces a conflict. */
enum { kMigrationFileLimit=1024*1024 };
static bool MigrationDirectory(const char *path,SaveError *error) {
  if(!sr_mkdir(path) || (errno==EEXIST && sr_path_is_directory(path)))return true;
  return Fail(error,"Cannot create the save-layout directory.");
}
static bool SameFileBytes(const char *a,const char *b) {
  FILE *left=sr_fopen(a,"rb"),*right=sr_fopen(b,"rb");
  if(!left || !right){if(left)fclose(left);if(right)fclose(right);return false;}
  uint8_t x[4096],y[4096];bool equal=true;size_t total=0;
  for(;;) {
    size_t nx=fread(x,1,sizeof(x),left),ny=fread(y,1,sizeof(y),right);
    total+=nx;
    if(nx!=ny || memcmp(x,y,nx) || ferror(left) || ferror(right) || total>kMigrationFileLimit){equal=false;break;}
    if(nx<sizeof(x))break;
  }
  fclose(left);fclose(right);return equal;
}
static void PreserveModifiedTime(const char *source,const char *destination) {
  uint64_t modified=0;if(Probe(source,&modified)!=1 || !modified)return;
#ifdef _WIN32
  struct __utimbuf64 times={(__time64_t)modified,(__time64_t)modified};
  wchar_t *wide=sr_win_path(destination);if(wide){(void)_wutime64(wide,&times);free(wide);}
#else
  struct utimbuf times={(time_t)modified,(time_t)modified};(void)utime(destination,&times);
#endif
}
static bool CopyLayoutFile(const char *source,const char *destination,SaveError *error) {
  int exists=Probe(source,NULL);
  if(!exists)return true;
  if(exists<0)return Fail(error,"A save-layout source is inaccessible; original files preserved.");
  int target=Probe(destination,NULL);
  if(target) return target==1 && SameFileBytes(source,destination) ? true :
      Fail(error,"Save-layout migration found conflicting files; neither copy was replaced.");
  uint8_t *bytes=malloc(kMigrationFileLimit);size_t size=0;
  if(!bytes)return Fail(error,"Out of memory migrating save storage.");
  bool ok=Read(source,bytes,kMigrationFileLimit,&size,error) &&
      Save_WriteCompanionFile(destination,bytes,size,error);
  free(bytes);
  if(!ok)return false;
  if(!SameFileBytes(source,destination))return Fail(error,"Save-layout copy verification failed; originals preserved.");
  PreserveModifiedTime(source,destination);return true;
}
static bool PreserveOldIndex(const SaveSlots *slots,SaveError *error) {
  char directory[kSaveSlotPathCapacity],source[kSaveSlotPathCapacity],backup[kSaveSlotPathCapacity];
  if(!Path(slots,"legacy-layout",directory,sizeof(directory)) || !MigrationDirectory(directory,error) ||
      !Path(slots,"slots.armanager",source,sizeof(source)) ||
      !Path(slots,"legacy-layout/slots.armanager",backup,sizeof(backup)))return false;
  return CopyLayoutFile(source,backup,error);
}
static bool PreserveLayoutSource(const SaveSlots *slots,const char *leaf,char *source,SaveError *error) {
  char backup_leaf[128],backup[kSaveSlotPathCapacity];
  int n=snprintf(backup_leaf,sizeof(backup_leaf),"legacy-layout/%s",leaf);
  return n>0 && (size_t)n<sizeof(backup_leaf) && Path(slots,leaf,source,kSaveSlotPathCapacity) &&
      Path(slots,backup_leaf,backup,sizeof(backup)) && CopyLayoutFile(source,backup,error);
}
static bool MoveLayoutFile(const SaveSlots *slots,const char *leaf,const char *destination,SaveError *error) {
  char source[kSaveSlotPathCapacity];
  return PreserveLayoutSource(slots,leaf,source,error) && CopyLayoutFile(source,destination,error);
}
static bool MigrateLayout(SaveSlots *slots,SaveError *error) {
  char path[kSaveSlotPathCapacity];
  const char *directories[]={"slots","legacy-layout","imports","exports","backups"};
  for(unsigned i=0;i<sizeof(directories)/sizeof(directories[0]);++i)
    if(!Path(slots,directories[i],path,sizeof(path)) || !MigrationDirectory(path,error))return false;
  SaveSlots next=*slots;next.layout=3;next.legacy_native=false;
  for(unsigned i=0;i<kSaveSlotCount;++i) {
    char leaf[64];snprintf(leaf,sizeof(leaf),"slots/%02u",i+1);
    if(!Path(slots,leaf,path,sizeof(path)) || !MigrationDirectory(path,error))return false;
    if(!(i==0 && slots->legacy_native)) {
      SaveSlotInspection old;
      if(!SaveSlots_Inspect(slots,i,&old)){if(error)*error=old.error;return false;}
    }
    char draft[kSaveSlotPathCapacity];DraftPath(&next,i,draft);
    snprintf(leaf,sizeof(leaf),"new-game-%02u.ardraft",i+1);
    if(!MoveLayoutFile(slots,leaf,draft,error))return false;
  }
  /* Slot 1 and prepared drafts used to live at the collection root. Keep both
   * possible formats intact, even though only the indexed format is selected. */
  char native[kSaveSlotPathCapacity],ini[kSaveSlotPathCapacity];
  SaveSlots_Paths(&next,0,native,ini,sizeof(native));
  const char *names[]={"save.srm","save.ini","actraiser.srm"};
  const char *suffixes[]={"",".archeckpoint",".arname"};
  for(unsigned file=0;file<3;++file)for(unsigned suffix=0;suffix<3;++suffix) {
    char leaf[64],source[kSaveSlotPathCapacity],destination[kSaveSlotPathCapacity];
    snprintf(leaf,sizeof(leaf),"%s%s",names[file],suffixes[suffix]);
    if(!PreserveLayoutSource(slots,leaf,source,error))return false;
    if(file==2)continue;
    snprintf(destination,sizeof(destination),"%s%s",file?ini:native,suffixes[suffix]);
    if(!CopyLayoutFile(source,destination,error))return false;
  }
  if(slots->legacy_native) {
    char source[kSaveSlotPathCapacity];Path(slots,"actraiser.srm",source,sizeof(source));
    uint8_t image[kActRaiserSramSize],existing[kActRaiserSramSize];
    if(!Save_LoadFile(kSaveFileFormat_NativeSrm,source,image,error))return false;
    const SaveFileFormat format=(SaveFileFormat)slots->records[0].backend;
    const char *destination=format==kSaveFileFormat_Ini?ini:native;
    if(Probe(destination,NULL)) {
      if(!Save_LoadFile(format,destination,existing,error) || memcmp(existing,image,sizeof(image)))
        return Fail(error,"Legacy adoption found a conflicting destination; originals preserved.");
    } else if(!Save_WriteFile(format,destination,image,error))return false;
    if(!Save_LoadFile(format,destination,existing,error) || memcmp(existing,image,sizeof(image)))
      return Fail(error,"Legacy adoption verification failed; original preserved.");
    PreserveModifiedTime(source,destination);
    for(unsigned i=1;i<3;++i) {
      char leaf[64],target[kSaveSlotPathCapacity];snprintf(leaf,sizeof(leaf),"actraiser.srm%s",suffixes[i]);
      snprintf(target,sizeof(target),"%s%s",destination,suffixes[i]);
      if(!MoveLayoutFile(slots,leaf,target,error))return false;
    }
  }
  for(unsigned i=0;i<kSaveSlotCount;++i) {
    SaveSlotInspection check;
    if(!SaveSlots_Inspect(&next,i,&check)){if(error)*error=check.error;return false;}
  }
#ifndef _WIN32
  /* The file writer syncs each leaf directory. Persist the newly created slot
   * directory entries too before publishing an index that depends on them. */
  Path(slots,"slots",path,sizeof(path));
  int directory=open(path,O_RDONLY);
  if(directory<0)return Fail(error,"Cannot sync the migrated slot directories.");
  bool synced=fsync(directory)==0;close(directory);
  if(!synced)return Fail(error,"Cannot sync the migrated slot directories.");
#endif
  next.dirty=true;
  if(!SaveSlots_Flush(&next,error))return false;
  fprintf(stderr,"[saves] save collection migrated to layout 3%s\n",slots->legacy_native?" (legacy native adoption)":"");
  *slots=next;return true;
}
static void CleanupLayoutFile(const SaveSlots *slots,const char *leaf) {
  char source[kSaveSlotPathCapacity],backup[kSaveSlotPathCapacity],saved[128];
  snprintf(saved,sizeof(saved),"legacy-layout/%s",leaf);
  if(Path(slots,leaf,source,sizeof(source)) && Path(slots,saved,backup,sizeof(backup)) &&
      Probe(source,NULL)==1 && Probe(backup,NULL)==1 && SameFileBytes(source,backup))
    (void)sr_remove(source); /* Failure is harmless and retried on the next open. */
}
static void CleanupOldLayout(const SaveSlots *slots) {
  const char *names[]={"save.srm","save.ini","actraiser.srm"};
  const char *suffixes[]={"",".archeckpoint",".arname"};
  char leaf[64];
  for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j) {
    snprintf(leaf,sizeof(leaf),"%s%s",names[i],suffixes[j]);CleanupLayoutFile(slots,leaf);
  }
  for(unsigned i=0;i<kSaveSlotCount;++i) {
    snprintf(leaf,sizeof(leaf),"new-game-%02u.ardraft",i+1);CleanupLayoutFile(slots,leaf);
  }
}
