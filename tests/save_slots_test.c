#include "save_slot_manager.h"
#include "byte_order.h"
#include "deterministic_hash.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p,0700)
#define RMDIR(p) rmdir(p)
#endif

/* Every root is created exclusively by this test. Remove only known fixture
 * files, including retained migration originals and otherwise empty slots. */
static void RemoveCollection(const char *root) {
  char path[768];
  const char *files[]={"save.srm","save.ini","save.srm.archeckpoint","save.ini.archeckpoint",
      "save.srm.arname","save.ini.arname","actraiser.srm","actraiser.srm.archeckpoint","actraiser.srm.arname",
      "slots.armanager","slots.lock","slot-switch.arrequest","new-game.ardraft"};
  for(unsigned i=0;i<sizeof(files)/sizeof(files[0]);++i) {
    snprintf(path,sizeof(path),"%s/%s",root,files[i]);remove(path);
    snprintf(path,sizeof(path),"%s/legacy-layout/%s",root,files[i]);remove(path);
    for(unsigned slot=1;slot<=kSaveSlotCount;++slot) {
      snprintf(path,sizeof(path),"%s/slots/%02u/%s",root,slot,files[i]);remove(path);
    }
  }
  for(unsigned slot=1;slot<=kSaveSlotCount;++slot) {
    snprintf(path,sizeof(path),"%s/new-game-%02u.ardraft",root,slot);remove(path);
    snprintf(path,sizeof(path),"%s/legacy-layout/new-game-%02u.ardraft",root,slot);remove(path);
    snprintf(path,sizeof(path),"%s/slots/%02u",root,slot);RMDIR(path);
    snprintf(path,sizeof(path),"%s/backups/%02u",root,slot);RMDIR(path);
  }
  const char *directories[]={"slots","legacy-layout","imports","exports","backups"};
  for(unsigned i=0;i<5;++i){snprintf(path,sizeof(path),"%s/%s",root,directories[i]);RMDIR(path);}
  assert(!RMDIR(root));
}
static void IndexHash(uint8_t bytes[264]) {
  uint64_t hash=DeterministicHash_Fnv1a64(DETERMINISTIC_HASH_FNV1A64_OFFSET,bytes,256);
  ByteOrder_WriteLe32(bytes+256,(uint32_t)hash);ByteOrder_WriteLe32(bytes+260,(uint32_t)(hash>>32));
}
static void Run(const char *root,SaveBackend backend) {
  assert(!MKDIR(root));
  char native[512],ini[512],other[512],companion[550];
  SaveSlots paths={0};snprintf(paths.root,sizeof(paths.root),"%s",root);
  assert(SaveSlots_Paths(&paths,0,native,ini,sizeof(native)));
  char original[512];snprintf(original,sizeof(original),"%s",backend==kSaveBackend_Ini?ini:native);
  uint8_t image[kActRaiserSramSize]={0},disk[kActRaiserSramSize];
  memcpy(image+0x1439,"ASTRA",5);ByteOrder_WriteLe16(image+0x1442,5);
  assert(Save_SetRegionState(image,0,4));Save_RecomputeChecksum(image);
  SaveError error={{0}};const uint8_t id[16]={7};const ArRegionalCostPolicy costs={{0}};
  ArRegionalSession campaign;
  assert(ArRegionalSession_NewGame(&campaign,0,id,&costs));
  campaign.randomizer=RandomizerConfig_Default();campaign.randomizer.enabled=true;campaign.randomizer.seed=123456;
  assert(ArRegionalSession_Save(&campaign,(SaveFileFormat)backend,original,NULL,image,&error));
  SaveSlots s;
  assert(SaveSlots_Open(&s,root,backend,&error) && s.adopted && s.active==0);
  assert(s.layout==3 && SaveSlots_Paths(&s,0,native,ini,sizeof(native)));
  snprintf(original,sizeof(original),"%s",backend==kSaveBackend_Ini?ini:native);
  SaveSlots second;assert(!SaveSlots_Open(&second,root,backend,&error));
  SaveSlotDetails a,b;
  assert(SaveSlotManager_Inspect(&s,0,&a) && a.state==kSaveSlot_Ready && a.randomizer.seed==123456);
  /* Losing a managed companion must never look like a legacy campaign. */
  assert(s.records[0].checkpoint_required);
  snprintf(companion,sizeof(companion),"%s.archeckpoint",original);
  snprintf(other,sizeof(other),"%s/checkpoint-held",root);
  assert(!rename(companion,other));
  assert(!SaveSlotManager_Inspect(&s,0,&a));
  assert(!SaveSlots_BeforeCommit(&s,&error));
  SaveSlots_Close(&s);assert(SaveSlots_Open(&s,root,backend,&error));
  assert(!SaveSlotManager_Inspect(&s,0,&a));
  assert(!rename(other,companion));
  assert(SaveSlotManager_Inspect(&s,0,&a));
  assert(a.summary.level==5 && a.summary.acts_cleared==2 && !strcmp(a.summary.name,"ASTRA"));
  assert(SaveSlotManager_Inspect(&s,1,&b) && b.state==kSaveSlot_Empty);
  ArRegionalRules jp;assert(ArRegionalProfiles_Expand(&campaign.requested,kArRegionalProfile_Gameplay,kArRegionalSource_Japan,&jp));
  ArRegionalSession draft,restored;
  RandomizerConfig recipe=RandomizerConfig_Default();recipe.enabled=true;recipe.seed=0;recipe.regional_action=true;recipe.hp_percent=200;
  assert(SaveSlotManager_Draft(&draft,1,id,&jp,&recipe));
  const OverlayRegionRow profile={.kind=kOverlayRegionRow_Preset,.group=kArRegionalProfile_Artwork};
  ArRegionalSession before=draft;
  assert(SaveSlotManager_Edit(&draft,&profile,kArRegionalSource_Japan));
  assert(!memcmp(&before.randomizer,&draft.randomizer,sizeof(recipe)));
  uint8_t encoded[kSaveSlotDraftCapacity];size_t size=0;
  assert(ArRegionalSession_Encode(&draft,encoded,sizeof(encoded),&size));
  /* Choosing the new-slot default pins B without reinterpreting A. */
  SaveBackend destination_backend=backend==kSaveBackend_Ini?kSaveBackend_NativeSrm:kSaveBackend_Ini;
  assert(!SaveSlots_Request(&s,1,b.fingerprint+1,encoded,size,destination_backend,&error));
  assert(SaveSlots_Request(&s,1,b.fingerprint,encoded,size,destination_backend,&error));
  assert(s.active==0 && s.destination==1 && s.pending);
  assert(!SaveSlots_BeforeCommit(&s,&error));
  assert(Save_LoadFile((SaveFileFormat)backend,original,disk,&error) && !memcmp(disk,image,sizeof(disk)));
  SaveSlots_Close(&s);
  assert(SaveSlots_Open(&s,root,kSaveBackend_NativeSrm,&error) && s.pending && s.destination==1);
  assert(s.records[0].backend==backend); /* global default cannot reinterpret A */
  assert(SaveSlots_ValidateDestination(&s,&error));
  assert(SaveSlots_DestinationBackend(&s)==destination_backend);
  assert(SaveSlotManager_ReadDraft(&s,1,&restored,&error) && restored.randomizer.seed==0 && restored.randomizer.enabled);
  assert(!memcmp(&draft.requested,&restored.requested,sizeof(draft.requested)));
  assert(SaveSlots_Acknowledge(&s,&error) && s.active==1 && !s.pending);
  assert(s.records[1].backend==destination_backend);
  SaveSlots_Close(&s);assert(SaveSlots_Open(&s,root,backend,&error));
  assert(SaveSlotManager_ReadDraft(&s,1,&restored,&error) && restored.randomizer.seed==0);
  assert(s.records[1].backend==destination_backend);
  assert(SaveSlotManager_Inspect(&s,1,&b) && b.state==kSaveSlot_Empty && !b.saved_at);
  assert(b.prepared && b.randomizer.enabled && b.randomizer.seed==0 && b.randomizer.hp_percent==200);
  assert(!memcmp(&b.regions.requested,&draft.requested,sizeof(draft.requested)));
  assert(SaveSlots_BeforeCommit(&s,&error));
  assert(!SaveSlotManager_Inspect(&s,1,&b)); /* first-write crash cannot look empty */
  assert(SaveSlots_BeforeCommit(&s,&error)); /* A live failed first write can retry. */
  assert(SaveSlots_Paths(&s,1,native,ini,sizeof(native)));
  char destination[512];snprintf(destination,sizeof(destination),"%s",destination_backend==kSaveBackend_Ini?ini:native);
  uint8_t new_image[kActRaiserSramSize];memcpy(new_image,image,sizeof(new_image));new_image[100]=9;Save_RecomputeChecksum(new_image);
  assert(ArRegionalSession_Save(&restored,(SaveFileFormat)destination_backend,destination,NULL,new_image,&error));
  SaveSlots_DidCommit(&s,new_image);
  assert(SaveSlotManager_Inspect(&s,1,&b) && b.randomizer.seed==0 && b.saved_at && !b.approximate_time);
  assert(!SaveSlots_Request(&s,1,b.fingerprint,NULL,0,backend,&error));
  assert(SaveSystem_Attach(new_image,sizeof(new_image),destination_backend,native,ini,&error));
  assert(SaveSystem_LoadActive(&error));
  assert(SaveSystem_SetLocalizedPlayerName("アストラ","ASTRA"));
  assert(SaveSystem_AutoPersistIfChanged(&error));
  assert(SaveSlotManager_Inspect(&s,1,&b) && !strcmp(b.summary.name,"アストラ"));
  assert(SaveSystem_BeginNativeWrite(&error));
  assert(!SaveSystem_FlushForSwitch(&error));
  assert(!SaveSystem_EndNativeWrite(false,&error));
  assert(!SaveSystem_FlushForSwitch(&error));
  assert(SaveSystem_LoadActive(&error) && SaveSystem_FlushForSwitch(&error));
  assert(SaveSlotManager_Inspect(&s,0,&a));
  assert(SaveSlots_Request(&s,0,a.fingerprint,NULL,0,backend,&error));
  SaveSlots_Close(&s);assert(SaveSlots_Open(&s,root,backend,&error));
  assert(SaveSlots_Acknowledge(&s,&error) && s.active==0);
  assert(SaveSlotManager_Inspect(&s,0,&a) && a.randomizer.seed==123456 && !strcmp(a.summary.name,"ASTRA"));
  /* External companion replacement invalidates a reviewed destination. */
  assert(SaveSlotManager_Inspect(&s,1,&b));
  snprintf(companion,sizeof(companion),"%s.arname",destination);FILE *f=fopen(companion,"wb");assert(f);fputs("changed",f);fclose(f);
  assert(!SaveSlots_Request(&s,1,b.fingerprint,NULL,0,backend,&error) && !s.pending);remove(companion);
  /* Pending request corruption is caught on restart without touching A/B. */
  assert(SaveSlotManager_Inspect(&s,1,&b));assert(SaveSlots_Request(&s,1,b.fingerprint,NULL,0,backend,&error));
  f=fopen(companion,"wb");assert(f);fputs("changed",f);fclose(f);
  SaveSlots_Close(&s);assert(SaveSlots_Open(&s,root,backend,&error));
  assert(!SaveSlots_ValidateDestination(&s,&error));
  assert(SaveSlots_ReturnToPrevious(&s,&error) && s.active==0);remove(companion);
  /* Missing occupied files and future slot IDs remain unavailable. */
  assert(!remove(destination));assert(!SaveSlotManager_Inspect(&s,1,&b));
  assert(Save_WriteFile((SaveFileFormat)destination_backend,destination,new_image,&error));
  restored.slot=7;assert(ArRegionalSession_Save(&restored,(SaveFileFormat)destination_backend,destination,new_image,new_image,&error)==false);
  /* A request write failure keeps A active and leaves B unchanged. */
  snprintf(other,sizeof(other),"%s/slot-switch.arrequest.tmp",root);assert(!MKDIR(other));
  assert(SaveSlotManager_Inspect(&s,1,&b));
  assert(!SaveSlots_Request(&s,1,b.fingerprint,NULL,0,backend,&error) && !s.pending && s.active==0);
  assert(!RMDIR(other));
  /* Malformed prepared drafts are unavailable, not fresh blank campaigns. */
  assert(SaveSlotManager_Inspect(&s,2,&b) && b.state==kSaveSlot_Empty);
  draft.slot=2;assert(ArRegionalSession_Encode(&draft,encoded,sizeof(encoded),&size));
  /* A prepared draft may reach disk before the restart request fails. It is
   * still recoverable, and a freshly reviewed fingerprint permits a retry. */
  snprintf(other,sizeof(other),"%s/slot-switch.arrequest.tmp",root);assert(!MKDIR(other));
  uint64_t empty_fingerprint=b.fingerprint;
  assert(!SaveSlots_Request(&s,2,empty_fingerprint,encoded,size,destination_backend,&error));
  assert(!s.pending && s.active==0);
  assert(SaveSlotManager_Inspect(&s,2,&b) && b.prepared && b.randomizer.hp_percent==200);
  assert(b.fingerprint!=empty_fingerprint);
  assert(!RMDIR(other));
  assert(!SaveSlots_Request(&s,2,empty_fingerprint,encoded,size,destination_backend,&error));
  assert(SaveSlots_Request(&s,2,b.fingerprint,encoded,size,destination_backend,&error));
  snprintf(other,sizeof(other),"%s/slots/03/new-game.ardraft",root);
  f=fopen(other,"r+b");assert(f);fputc('!',f);fclose(f);
  assert(!SaveSlotManager_Inspect(&s,2,&b));
  assert(!SaveSlots_ValidateDestination(&s,&error));
  assert(SaveSlots_ReturnToPrevious(&s,&error));
  SaveSlots_Close(&s);
  snprintf(other,sizeof(other),"%s/slots.armanager",root);f=fopen(other,"r+b");assert(f);fputc('!',f);fclose(f);
  assert(!SaveSlots_Open(&s,root,backend,&error));
  assert(Save_LoadFile((SaveFileFormat)backend,original,disk,&error));
  RemoveCollection(root);
}
/* A failed preparation of the currently active empty slot must not redirect
 * the writer that is already attached to its original backend. */
static void ActiveEmptyFailure(const char *root,SaveBackend backend) {
  assert(!MKDIR(root));
  SaveSlots slots;SaveError error={{0}};SaveSlotDetails details;
  assert(SaveSlots_Open(&slots,root,backend,&error));
  assert(SaveSlotManager_Inspect(&slots,0,&details));
  ArRegionalSession draft;const uint8_t id[16]={9};ArRegionalRules rules={0};
  RandomizerConfig recipe=RandomizerConfig_Default();
  assert(SaveSlotManager_Draft(&draft,0,id,&rules,&recipe));
  uint8_t bytes[kSaveSlotDraftCapacity];size_t size;
  assert(ArRegionalSession_Encode(&draft,bytes,sizeof(bytes),&size));
  char blocked[512],native[512],ini[512];
  snprintf(blocked,sizeof(blocked),"%s/slot-switch.arrequest.tmp",root);assert(!MKDIR(blocked));
  SaveBackend next=backend==kSaveBackend_Ini?kSaveBackend_NativeSrm:kSaveBackend_Ini;
  assert(!SaveSlots_Request(&slots,0,details.fingerprint,bytes,size,next,&error));
  assert(!slots.pending && slots.records[0].backend==backend && slots.records[0].prepared_backend==next);
  assert(!RMDIR(blocked));
  assert(SaveSlotManager_Inspect(&slots,0,&details));
  assert(SaveSlots_Request(&slots,0,details.fingerprint,bytes,size,next,&error));
  assert(SaveSlots_DestinationBackend(&slots)==next && slots.records[0].backend==backend);
  assert(SaveSlots_ReturnToPrevious(&slots,&error));
  assert(!slots.pending && slots.active==0 && slots.records[0].backend==backend);
  assert(SaveSlots_BeforeCommit(&slots,&error));
  assert(SaveSlots_Paths(&slots,0,native,ini,sizeof(native)));
  const char *active=backend==kSaveBackend_Ini?ini:native;
  uint8_t image[kActRaiserSramSize]={0};Save_RecomputeChecksum(image);
  assert(ArRegionalSession_Save(&draft,(SaveFileFormat)backend,active,NULL,image,&error));
  SaveSlots_DidCommit(&slots,image);SaveSlots_Close(&slots);
  assert(SaveSlots_Open(&slots,root,next,&error));
  assert(slots.records[0].backend==backend && SaveSlotManager_Inspect(&slots,0,&details));
  SaveSlots_Close(&slots);
  RemoveCollection(root);
}
static void LegacyAdoption(void) {
  for(unsigned managed=0;managed<2;++managed) {
    const char *root=managed?"save-slots-v1-managed":"save-slots-v1-legacy";assert(!MKDIR(root));
    char path[512];SaveError error={{0}};uint8_t image[kActRaiserSramSize]={0};Save_RecomputeChecksum(image);
    snprintf(path,sizeof(path),"%s/save.srm",root);assert(Save_WriteFile(kSaveFileFormat_NativeSrm,path,image,&error));
    uint8_t index[264]={0};memcpy(index,"ARSLOTS1",8);index[17]=1;
    if(managed)ByteOrder_WriteLe32(index+24,1);
    IndexHash(index);snprintf(path,sizeof(path),"%s/slots.armanager",root);
    assert(Save_WriteCompanionFile(path,index,sizeof(index),&error));
    SaveSlots slots;SaveSlotDetails details;
    if(managed) {
      assert(!SaveSlots_Open(&slots,root,kSaveBackend_NativeSrm,&error));
      snprintf(path,sizeof(path),"%s/save.srm",root);assert(Save_LoadFile(kSaveFileFormat_NativeSrm,path,image,&error));
    } else {
      assert(SaveSlots_Open(&slots,root,kSaveBackend_NativeSrm,&error));
      assert(slots.layout==3 && SaveSlotManager_Inspect(&slots,0,&details) && details.legacy);
      assert(!slots.records[0].checkpoint_required);
      assert(SaveSlots_BeforeCommit(&slots,&error));
      assert(SaveSlots_BeforeCommit(&slots,&error)); /* A live failed upgrade can retry. */
      SaveSlots_Close(&slots);
    }
    RemoveCollection(root);
  }
}
static void MigrationResume(const char *root,SaveBackend backend) {
  assert(!MKDIR(root));SaveError error={{0}};
  SaveSlots old={.layout=2};snprintf(old.root,sizeof(old.root),"%s",root);
  old.records[0]=(SaveSlotRecord){.backend=backend,.ever_saved=true,.checkpoint_required=true};
  SaveBackend next=backend==kSaveBackend_Ini?kSaveBackend_NativeSrm:kSaveBackend_Ini;
  old.records[1]=(SaveSlotRecord){.prepared=true,.prepared_backend=next};
  char native[512],ini[512],source[512],path[768],blocked[768];
  assert(SaveSlots_Paths(&old,0,native,ini,sizeof(native)));
  snprintf(source,sizeof(source),"%s",backend==kSaveBackend_Ini?ini:native);
  uint8_t image[kActRaiserSramSize]={0},disk[kActRaiserSramSize];
  memcpy(image+0x1439,"ELISE",5);Save_RecomputeChecksum(image);
  ArRegionalSession session,draft;const uint8_t id[16]={19};const ArRegionalCostPolicy costs={{0}};
  assert(ArRegionalSession_NewGame(&session,0,id,&costs));
  session.randomizer=RandomizerConfig_Default();session.randomizer.enabled=true;session.randomizer.seed=98765;
  assert(ArRegionalSession_Save(&session,(SaveFileFormat)backend,source,NULL,image,&error));
  assert(SaveSystem_Attach(image,sizeof(image),backend,native,ini,&error) && SaveSystem_LoadActive(&error));
  assert(SaveSystem_SetLocalizedPlayerName("Élise","ELISE") && SaveSystem_AutoPersistIfChanged(&error));
  assert(SaveSlotManager_Draft(&draft,1,id,&session.requested,&session.randomizer));
  uint8_t encoded[kSaveSlotDraftCapacity];size_t size=0;
  assert(ArRegionalSession_Encode(&draft,encoded,sizeof(encoded),&size));
  snprintf(path,sizeof(path),"%s/new-game-02.ardraft",root);assert(Save_WriteCompanionFile(path,encoded,size,&error));
  uint8_t index[264]={0};memcpy(index,"ARSLOTS2",8);
  index[16]=backend;index[17]=1;index[19]=1;index[42]=1;index[44]=next;IndexHash(index);
  snprintf(path,sizeof(path),"%s/slots.armanager",root);assert(Save_WriteCompanionFile(path,index,sizeof(index),&error));
  SaveSlotInspection target;assert(SaveSlots_Inspect(&old,1,&target));
  uint8_t request[40]={0};memcpy(request,"ARSWITCH",8);request[8]=1;request[10]=1;request[11]=1;
  ByteOrder_WriteLe32(request+16,(uint32_t)target.fingerprint);ByteOrder_WriteLe32(request+20,(uint32_t)(target.fingerprint>>32));
  uint64_t hash=DeterministicHash_Fnv1a64(DETERMINISTIC_HASH_FNV1A64_OFFSET,request,32);
  ByteOrder_WriteLe32(request+32,(uint32_t)hash);ByteOrder_WriteLe32(request+36,(uint32_t)(hash>>32));
  snprintf(path,sizeof(path),"%s/slot-switch.arrequest",root);assert(Save_WriteCompanionFile(path,request,sizeof(request),&error));
  snprintf(path,sizeof(path),"%s/slots",root);assert(!MKDIR(path));
  snprintf(path,sizeof(path),"%s/slots/01",root);assert(!MKDIR(path));
  snprintf(path,sizeof(path),"%s/slots/01/save.%s",root,backend==kSaveBackend_Ini?"ini":"srm");
  snprintf(blocked,sizeof(blocked),"%s.tmp",path);assert(!MKDIR(blocked));
  SaveSlots slots;assert(!SaveSlots_Open(&slots,root,backend,&error));
  assert(Save_LoadFile((SaveFileFormat)backend,source,disk,&error) && !memcmp(disk,image,sizeof(disk)));
  assert(!RMDIR(blocked));
  FILE *conflict=fopen(path,"wb");assert(conflict);fputs("foreign",conflict);fclose(conflict);
  assert(!SaveSlots_Open(&slots,root,backend,&error));
  conflict=fopen(path,"rb");assert(conflict);char check[8]={0};assert(fread(check,1,7,conflict)==7);fclose(conflict);
  assert(!strcmp(check,"foreign"));assert(!remove(path));
  assert(SaveSlots_Open(&slots,root,backend,&error));
  assert(slots.layout==3 && slots.pending && slots.active==0 && slots.destination==1);
  assert(SaveSlots_ValidateDestination(&slots,&error) && SaveSlots_DestinationBackend(&slots)==next);
  SaveSlotDetails details;assert(SaveSlotManager_Inspect(&slots,0,&details));
  assert(details.randomizer.seed==98765 && !strcmp(details.summary.name,"Élise"));
  assert(!fopen(source,"rb"));
  snprintf(path,sizeof(path),"%s/legacy-layout/save.%s",root,backend==kSaveBackend_Ini?"ini":"srm");
  assert(Save_LoadFile((SaveFileFormat)backend,path,disk,&error) && !memcmp(disk,image,sizeof(disk)));
  assert(SaveSlotManager_ReadDraft(&slots,1,&session,&error) && !memcmp(&session,&draft,sizeof(draft)));
  assert(SaveSlots_Acknowledge(&slots,&error) && slots.records[1].backend==next);
  SaveSlots_Close(&slots);assert(SaveSlots_Open(&slots,root,backend,&error) && slots.active==1 && !slots.pending);
  SaveSlots_Close(&slots);RemoveCollection(root);
}

/* Optional fixture for a real game boot. The caller owns a fresh
 * temporary data directory; this mode never uses the player's save location. */
static int BootFixture(const char *mode,const char *root) {
  SaveSlots slots;SaveError error={{0}};SaveSlotDetails target;
  if(!strcmp(mode,"--prepare-legacy-boot")) {
    assert(!MKDIR(root));
    char path[512];snprintf(path,sizeof(path),"%s/actraiser.srm",root);
    uint8_t image[kActRaiserSramSize]={0};memcpy(image+0x1439,"ASTRA",5);Save_RecomputeChecksum(image);
    assert(Save_WriteFile(kSaveFileFormat_NativeSrm,path,image,&error));return 0;
  }
  assert(SaveSlots_Open(&slots,root,kSaveBackend_NativeSrm,&error));
  if(!strcmp(mode,"--prepare-boot")) {
    assert(slots.adopted);
    const uint8_t id[16]={12};ArRegionalSession draft;
    ArRegionalRules rules={0};RandomizerConfig recipe=RandomizerConfig_Default();
    recipe.enabled=true;recipe.seed=424242;recipe.regional_action=true;
    assert(SaveSlotManager_Draft(&draft,1,id,&rules,&recipe));
    assert(SaveSlotManager_Inspect(&slots,1,&target));
    uint8_t bytes[kSaveSlotDraftCapacity];size_t size;
    assert(ArRegionalSession_Encode(&draft,bytes,sizeof(bytes),&size));
    assert(SaveSlots_Request(&slots,1,target.fingerprint,bytes,size,kSaveBackend_NativeSrm,&error));
  } else if(!strcmp(mode,"--verify-boot")) {
    assert(slots.active==1 && !slots.pending);
    assert(SaveSlotManager_Inspect(&slots,0,&target) && target.state==kSaveSlot_Empty);
    assert(SaveSlotManager_Inspect(&slots,1,&target) && target.state==kSaveSlot_Empty);
    ArRegionalSession draft;assert(SaveSlotManager_ReadDraft(&slots,1,&draft,&error));
    assert(draft.randomizer.enabled && draft.randomizer.seed==424242);
  } else if(!strcmp(mode,"--verify-legacy-boot")) {
    assert(slots.active==0 && !slots.pending && slots.records[0].ever_saved);
    assert(!slots.records[0].checkpoint_required);
    assert(SaveSlotManager_Inspect(&slots,0,&target) && target.state==kSaveSlot_Ready && target.legacy);
    assert(!strcmp(target.summary.name,"ASTRA"));
    char path[512];uint8_t before[kActRaiserSramSize],after[kActRaiserSramSize];
    snprintf(path,sizeof(path),"%s/legacy-layout/actraiser.srm",root);assert(Save_LoadFile(kSaveFileFormat_NativeSrm,path,before,&error));
    snprintf(path,sizeof(path),"%s/slots/01/save.srm",root);assert(Save_LoadFile(kSaveFileFormat_NativeSrm,path,after,&error));
    assert(!memcmp(before,after,sizeof(before)));
  } else assert(false);
  SaveSlots_Close(&slots);return 0;
}
static void UpdateEmptyDraft(const char *root,SaveBackend backend) {
  assert(!MKDIR(root));
  SaveSlots slots;SaveError error={{0}};
  assert(SaveSlots_Open(&slots,root,backend,&error));
  ArRegionalSession draft,loaded;const uint8_t id[16]={11};ArRegionalRules rules={0};
  rules.artwork.source[kArRegionalArtwork_TitleBackground]=kArRegionalSource_Japan;
  RandomizerConfig recipe=RandomizerConfig_Default();recipe.seed=987;
  assert(SaveSlotManager_Draft(&draft,0,id,&rules,&recipe));
  uint8_t bytes[kSaveSlotDraftCapacity];size_t size;
  assert(ArRegionalSession_Encode(&draft,bytes,sizeof(bytes),&size));
  char blocked[512];snprintf(blocked,sizeof(blocked),"%s/slots.armanager.tmp",root);assert(!MKDIR(blocked));
  assert(!SaveSlots_UpdateDraft(&slots,bytes,size,&error));
  assert(!slots.records[0].prepared && !slots.records[0].ever_saved);
  SaveSlotDetails details;
  assert(SaveSlotManager_Inspect(&slots,0,&details) && details.state==kSaveSlot_Empty);
  assert(!RMDIR(blocked));
  assert(SaveSlots_UpdateDraft(&slots,bytes,size,&error));
  assert(!slots.pending && !slots.records[0].ever_saved && slots.records[0].prepared);
  SaveSlots_Close(&slots);assert(SaveSlots_Open(&slots,root,backend,&error));
  assert(SaveSlotManager_ReadDraft(&slots,0,&loaded,&error));
  assert(!memcmp(&loaded,&draft,sizeof(draft)) && loaded.randomizer.seed==987);
  assert(SaveSlots_DestinationBackend(&slots)==backend);
  assert(SaveSlotManager_Inspect(&slots,0,&details) && details.state==kSaveSlot_Empty);
  snprintf(blocked,sizeof(blocked),"%s/slots/01/new-game.ardraft.tmp",root);assert(!MKDIR(blocked));
  draft.requested.artwork.source[kArRegionalArtwork_TitleBackground]=kArRegionalSource_US;
  draft.effective=draft.requested;
  assert(ArRegionalSession_Encode(&draft,bytes,sizeof(bytes),&size));
  assert(!SaveSlots_UpdateDraft(&slots,bytes,size,&error));assert(!RMDIR(blocked));
  assert(SaveSlotManager_ReadDraft(&slots,0,&loaded,&error));
  assert(loaded.requested.artwork.source[kArRegionalArtwork_TitleBackground]==kArRegionalSource_Japan);
  assert(SaveSlots_UpdateDraft(&slots,bytes,size,&error));
  char native[512],ini[512];assert(SaveSlots_Paths(&slots,0,native,ini,sizeof(native)));
  uint8_t image[kActRaiserSramSize]={0};Save_RecomputeChecksum(image);
  assert(SaveSlots_BeforeCommit(&slots,&error));
  assert(ArRegionalSession_Save(&draft,(SaveFileFormat)backend,backend==kSaveBackend_Ini?ini:native,NULL,image,&error));
  SaveSlots_DidCommit(&slots,image);
  assert(!SaveSlots_UpdateDraft(&slots,bytes,size,&error)); /* occupied slots use their checkpoint */
  SaveSlots_Close(&slots);RemoveCollection(root);
}

int main(int argc,char **argv) {
  if(argc==3)return BootFixture(argv[1],argv[2]);
  UpdateEmptyDraft("save-slots-draft-native",kSaveBackend_NativeSrm);
  UpdateEmptyDraft("save-slots-draft-ini",kSaveBackend_Ini);
  LegacyAdoption();
  MigrationResume("save-layout-native-resume",kSaveBackend_NativeSrm);
  MigrationResume("save-layout-ini-resume",kSaveBackend_Ini);
  ActiveEmptyFailure("save-slots-empty-native",kSaveBackend_NativeSrm);
  ActiveEmptyFailure("save-slots-empty-ini",kSaveBackend_Ini);
  Run("save-slots-native-test",kSaveBackend_NativeSrm);
  Run("save-slots-ini-test",kSaveBackend_Ini);
  puts("save slots: ownership, restart, draft, timestamp, lock and corruption checks passed");
  return 0;
}
