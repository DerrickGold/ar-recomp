#include "save_paths.h"
#include "save_system.h"
#include "snesrecomp/support/utf8_fs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif
static void RemoveDirectory(const char *path) {
#ifdef _WIN32
  wchar_t *wide=sr_win_path(path);assert(wide);assert(!_wrmdir(wide));free(wide);
#else
  assert(!rmdir(path));
#endif
}
static void Write(const char *path,const char *text) {
  FILE *file=sr_fopen(path,"wb");assert(file);assert(fputs(text,file)>=0);assert(!fclose(file));
}
int main(void) {
  const char *root="save-paths-é-セーブ-test";assert(!sr_mkdir(root));
  SaveError error={{0}};SavePaths paths;assert(SavePaths_Init(&paths,root,2,&error));
  char first[1024],second[1024],third[1024],directory[1024];
  assert(!SavePaths_Import(&paths,first,sizeof(first),&error));
  snprintf(first,sizeof(first),"%s/import.srm",root);Write(first,"legacy source");
  assert(SavePaths_Import(&paths,second,sizeof(second),&error) && !strcmp(first,second));
  snprintf(directory,sizeof(directory),"%s/imports",root);assert(SavePaths_EnsureDirectory(directory,&error));
  snprintf(third,sizeof(third),"%s/imports/import.arsave",root);Write(third,"new source");
  assert(SavePaths_Import(&paths,second,sizeof(second),&error) && !strcmp(third,second));
  assert(!sr_remove(first) && !sr_remove(third));
  assert(SavePaths_Export(&paths,"arsave",first,sizeof(first),&error));
  assert(strstr(first,"/exports/slot-03-") && strstr(first,".arsave"));
  /* A reserved or already written export cannot be reused. */
  assert(SavePaths_Export(&paths,"arsave",second,sizeof(second),&error) && strcmp(first,second));
  Write(first,"keep this export");SavePaths_Release(first);SavePaths_Release(second);
  assert(SavePaths_Export(&paths,"arsave",third,sizeof(third),&error) && strcmp(first,third));SavePaths_Release(third);
  FILE *file=sr_fopen(first,"rb");assert(file);char contents[32]={0};assert(fread(contents,1,16,file)==16);fclose(file);
  assert(!strcmp(contents,"keep this export"));assert(!sr_remove(first));
  assert(SavePaths_Backup(&paths,first,sizeof(first),&error));
  assert(strstr(first,"/backups/03/backup-") && strstr(first,".arsave"));SavePaths_Release(first);
  const uint8_t id[16]={7};assert(SavePaths_Recovery(&paths,id,first,sizeof(first),&error));
  assert(strstr(first,"/backups/03/redevelopment-07000000000000000000000000000000"));
  char tiny[4]="old";assert(!SavePaths_Export(&paths,"srm",tiny,sizeof(tiny),&error) && !tiny[0]);
  assert(!SavePaths_Export(&paths,"../bad",first,sizeof(first),&error));
  assert(!SavePaths_Init(&paths,root,-2,&error));
  assert(SavePaths_Init(&paths,root,-1,&error));
  assert(!SavePaths_Backup(&paths,first,sizeof(first),&error));
  assert(SavePaths_Export(&paths,"ini",first,sizeof(first),&error) && strstr(first,"/exports/external-"));SavePaths_Release(first);
  const char *dirs[]={"backups/03","backups","imports","exports"};
  for(unsigned i=0;i<4;++i){snprintf(directory,sizeof(directory),"%s/%s",root,dirs[i]);RemoveDirectory(directory);}
  RemoveDirectory(root);puts("save paths: Unicode roots, import fallback, unique exports and per-slot backups passed");return 0;
}
