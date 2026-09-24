#include "save_paths.h"
#include "save_system.h"
#include "save_slots.h"
#include "snesrecomp/support/utf8_fs.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif
static bool Fail(SaveError *error,const char *message) {
  if(error)snprintf(error->message,sizeof(error->message),"%s",message);
  return false;
}
static bool Join(const char *root,const char *leaf,char *out,size_t capacity,SaveError *error) {
  if(!out || !capacity || !root || !*root)return Fail(error,"No save storage directory is configured.");
  int n=snprintf(out,capacity,"%s/%s",root,leaf);
  if(n>0 && (size_t)n<capacity)return true;
  out[0]=0;return Fail(error,"Save storage path is too long.");
}
bool SavePaths_Init(SavePaths *paths,const char *root,int slot,SaveError *error) {
  if(!paths || !root || !*root || strlen(root)>sizeof(paths->root)-128 || slot < -1 || slot>=kSaveSlotCount)
    return Fail(error,"Invalid save storage location.");
  *paths=(SavePaths){.slot=slot};snprintf(paths->root,sizeof(paths->root),"%s",root);return true;
}
bool SavePaths_EnsureDirectory(const char *path,SaveError *error) {
  if(!path || !*path)return Fail(error,"Save directory is empty.");
  if(!sr_mkdir(path) || (errno==EEXIST && sr_path_is_directory(path)))return true;
  if(errno!=ENOENT)return Fail(error,"Cannot create the save storage directory.");
  char parent[kHostPathCapacity];
  if(strlen(path)>=sizeof(parent))return Fail(error,"Save directory path is too long.");
  snprintf(parent,sizeof(parent),"%s",path);
  char *slash=strrchr(parent,'/');
#ifdef _WIN32
  char *backslash=strrchr(parent,'\\');if(backslash && (!slash || backslash>slash))slash=backslash;
#endif
  if(!slash || slash==parent)return Fail(error,"Cannot create the save storage parent.");
  *slash=0;
  if(!SavePaths_EnsureDirectory(parent,error))return false;
  if(!sr_mkdir(path) || (errno==EEXIST && sr_path_is_directory(path)))return true;
  return Fail(error,"Cannot create the save storage directory.");
}
bool SavePaths_Import(const SavePaths *paths,char *out,size_t capacity,SaveError *error) {
  const char *leaves[]={"imports/import.arsave","imports/import.srm","imports/import.ini",
      "import.arsave","import.srm","import.ini"};
  if(!paths)return Fail(error,"No save storage location is configured.");
  for(unsigned i=0;i<sizeof(leaves)/sizeof(leaves[0]);++i) {
    if(!Join(paths->root,leaves[i],out,capacity,error))return false;
    FILE *file=sr_fopen(out,"rb");if(file){fclose(file);return true;}
    if(errno!=ENOENT)return Fail(error,"Cannot read the selected import file.");
  }
  out[0]=0;return Fail(error,"Place a campaign, SRAM or INI file in saves/imports as import.arsave, import.srm or import.ini.");
}
static bool BackupDirectory(const SavePaths *paths,char *directory,size_t capacity,SaveError *error) {
  if(!paths || paths->slot<0)return Fail(error,"No managed slot for this backup.");
  char leaf[64];snprintf(leaf,sizeof(leaf),"backups/%02u",paths->slot+1);
  return Join(paths->root,leaf,directory,capacity,error) && SavePaths_EnsureDirectory(directory,error);
}
static bool UniquePath(const char *directory,const char *prefix,const char *extension,
    char *out,size_t capacity,SaveError *error) {
  if(!SavePaths_EnsureDirectory(directory,error))return false;
  time_t now=time(NULL);struct tm local={0};
#ifdef _WIN32
  if(localtime_s(&local,&now))return Fail(error,"Cannot timestamp the save archive.");
#else
  if(!localtime_r(&now,&local))return Fail(error,"Cannot timestamp the save archive.");
#endif
  char timestamp[32];if(!strftime(timestamp,sizeof(timestamp),"%Y%m%d-%H%M%S",&local))return false;
  for(unsigned serial=0;serial<1000;++serial) {
    char leaf[128],reservation[kHostPathCapacity];
    snprintf(leaf,sizeof(leaf),"%s-%s-%03u.%s",prefix,timestamp,serial,extension);
    if(!Join(directory,leaf,out,capacity,error))return false;
    int n=snprintf(reservation,sizeof(reservation),"%s.pending",out);
    if(n<0 || (size_t)n>=sizeof(reservation))return Fail(error,"Archive reservation path is too long.");
    FILE *file=sr_fopen(out,"rb");if(file){fclose(file);continue;}
    if(errno!=ENOENT)return Fail(error,"Cannot inspect the archive destination.");
    if(!sr_mkdir(reservation))return true;
    if(errno!=EEXIST)return Fail(error,"Cannot reserve an archive destination.");
  }
  return Fail(error,"No unused archive filename is available.");
}
void SavePaths_Release(const char *path) {
  if(!path || !*path)return;
  char reservation[kHostPathCapacity];int n=snprintf(reservation,sizeof(reservation),"%s.pending",path);
  if(n<0 || (size_t)n>=sizeof(reservation))return;
#ifdef _WIN32
  wchar_t *wide=sr_win_path(reservation);if(wide){(void)_wrmdir(wide);free(wide);}
#else
  (void)rmdir(reservation);
#endif
}
bool SavePaths_Export(const SavePaths *paths,const char *extension,char *out,size_t capacity,SaveError *error) {
  if(!paths || !extension || (strcmp(extension,"arsave") && strcmp(extension,"srm") && strcmp(extension,"ini")))
    return Fail(error,"Invalid export format.");
  char directory[kHostPathCapacity],prefix[32];
  if(!Join(paths->root,"exports",directory,sizeof(directory),error))return false;
  if(paths->slot>=0)snprintf(prefix,sizeof(prefix),"slot-%02u",paths->slot+1);
  else snprintf(prefix,sizeof(prefix),"external");
  return UniquePath(directory,prefix,extension,out,capacity,error);
}
bool SavePaths_Backup(const SavePaths *paths,char *out,size_t capacity,SaveError *error) {
  char directory[kHostPathCapacity];
  return BackupDirectory(paths,directory,sizeof(directory),error) &&
      UniquePath(directory,"backup","arsave",out,capacity,error);
}
bool SavePaths_Recovery(const SavePaths *paths,const unsigned char id[16],char *out,size_t capacity,SaveError *error) {
  if(!id)return Fail(error,"Recovery identity is missing.");
  char directory[kHostPathCapacity],leaf[64]="redevelopment-";
  for(unsigned i=0;i<16;++i)snprintf(leaf+14+i*2,3,"%02x",id[i]);
  return BackupDirectory(paths,directory,sizeof(directory),error) && Join(directory,leaf,out,capacity,error);
}
