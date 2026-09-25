#include "regional/session/regional_campaign.h"
#include "host/campaign_identity.h"
#include "snesrecomp/support/utf8_fs.h"

#include "byte_order.h"
#include "deterministic_hash.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define MAKE_DIR(p) _mkdir(p)
#define REMOVE_DIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#define MAKE_DIR(p) mkdir(p, 0700)
#define REMOVE_DIR(p) rmdir(p)
#endif

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static bool Identity(void *context, uint8_t id[16]) {
  memset(id, 0, 16);
  id[0] = ++*(uint8_t *)context;
  return true;
}
static void Remove(const char *path) {
  char sidecar[256];
  remove(path);
  snprintf(sidecar, sizeof(sidecar), "%s.archeckpoint", path);
  remove(sidecar);
  snprintf(sidecar, sizeof(sidecar), "%s.tmp", path);
  REMOVE_DIR(sidecar); remove(sidecar);
}
static void Run(SaveBackend backend) {
  const char *native = "regional-campaign-test.srm", *ini = "regional-campaign-test.ini";
  const char *path = backend == kSaveBackend_Ini ? ini : native;
  const char *donor = "regional-campaign-import.srm";
  SaveFileFormat format = backend == kSaveBackend_Ini ? kSaveFileFormat_Ini : kSaveFileFormat_NativeSrm;
  Remove(native); Remove(ini); Remove(donor);
  uint8_t image[kActRaiserSramSize] = {0}, durable[kActRaiserSramSize], disk[kActRaiserSramSize];
  Save_RecomputeChecksum(image);
  SaveError error = {{0}};
  CHECK(Save_WriteFile(format, path, image, &error));
  CHECK(SaveSystem_Attach(image, sizeof(image), backend, native, ini, &error));
  CHECK(SaveSystem_LoadActive(&error));
  CHECK(SaveSystem_CopyDurableImage(durable));
  uint8_t sequence = 0;
  ArRegionalCampaign campaign;
  ArRegionalCampaign_Init(&campaign, 0, Identity, &sequence);
  SaveCommitHost host = ArRegionalCampaign_SaveHost(&campaign);
  CHECK(SaveSystem_SetCommitHost(&host));
  CHECK(ArRegionalCampaign_Continue(&campaign, path, durable, &error));
  CHECK(campaign.active.requested.costs.source[0] == kArRegionalSource_US);
  ArRegionalSession loaded;
  CHECK(ArRegionalSession_Load(&loaded, 0, path, durable, &error) == kSaveCheckpoint_Missing);
  ArRegionalCostPolicy jp, eu;
  ArRegionalCosts_Init(&jp, kArRegionalSource_Japan);
  ArRegionalCosts_Init(&eu, kArRegionalSource_Europe);
  CHECK(ArRegionalCampaign_NewGame(&campaign, &jp, &error));
  campaign.active.randomizer=RandomizerConfig_Default();
  campaign.active.randomizer.enabled=true;campaign.active.randomizer.seed=123456;
  campaign.active.randomizer.hp_percent=200;campaign.active.randomizer.regional_action=true;
  CHECK(ArRegionalSession_RequestTimers(&campaign.active, campaign.active.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestRetryScore(&campaign.active, campaign.active.revision, kArRegionalSource_Japan));
  ArRegionalSession saving = campaign.active;
  CHECK(ArRegionalSession_Load(&loaded, 0, path, durable, &error) == kSaveCheckpoint_Missing);
  CHECK(SaveSystem_BeginNativeWrite(&error));
  image[100] = 1;
  CHECK(SaveSystem_AutoPersistIfChanged(&error));
  Save_RecomputeChecksum(image);
  CHECK(SaveSystem_EndNativeWrite(true, &error));
  CHECK(campaign.pending_valid);
  CHECK(ArRegionalCampaign_NewGame(&campaign, &eu, &error));
  campaign.active.randomizer=RandomizerConfig_Default();
  campaign.active.randomizer.enabled=true;campaign.active.randomizer.seed=654321;
  CHECK(campaign.active.requested.timers.source[0] == kArRegionalSource_US);
  CHECK(Save_LoadFile(format, path, disk, &error));
  CHECK(!memcmp(disk, durable, sizeof(disk)));
  char blocked[256];
  snprintf(blocked, sizeof(blocked), "%s.tmp", path);
  CHECK(MAKE_DIR(blocked) == 0);
  CHECK(!SaveSystem_AutoPersistIfChanged(&error));
  CHECK(campaign.pending_valid);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, durable, &error) == kSaveCheckpoint_Missing);
  CHECK(REMOVE_DIR(blocked) == 0);
  CHECK(SaveSystem_AutoPersistIfChanged(&error));
  CHECK(!campaign.pending_valid);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!memcmp(&loaded, &saving, sizeof(saving)));
  CHECK(campaign.active.requested.costs.source[0] == kArRegionalSource_Europe);
  /* A marker outside the checksum belongs to the durable JP game, not the
   * unsaved EU one. No direct Save_WriteFile bypass of companion rotation. */
  image[0x1ff0] ^= 1;
  CHECK(SaveSystem_AutoPersistIfChanged(&error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!memcmp(loaded.campaign, saving.campaign, 16));
  CHECK(loaded.randomizer.seed==123456 && loaded.randomizer.hp_percent==200);
  SaveEditRequest edits;
  SaveEditRequest_Clear(&edits); edits.master_level = 5;
  CHECK(SaveSystem_ApplyEdits(&edits, true, true, false, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.costs.source[0] == kArRegionalSource_Japan);
  CHECK(loaded.requested.timers.source[0] == kArRegionalSource_Japan);
  CHECK(loaded.requested.retry_score == kArRegionalSource_Japan);
  CHECK(loaded.randomizer.seed==123456);

  /* Foreign import gets its own identity/provenance, never the running game's. */
  memset(disk, 0, sizeof(disk)); Save_RecomputeChecksum(disk);
  CHECK(ArRegionalSession_Save(&campaign.active, kSaveFileFormat_NativeSrm, donor, NULL, disk, &error));
  CHECK(SaveSystem_Import(donor, false, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!memcmp(&loaded, &campaign.active, sizeof(loaded)));
  CHECK(SaveSystem_LoadActive(&error));
  CHECK(!campaign.active_valid && !campaign.pending_valid);
  CHECK(SaveSystem_CopyDurableImage(durable));
  CHECK(ArRegionalCampaign_Continue(&campaign, path, durable, &error));
  CHECK(campaign.active.requested.costs.source[0] == kArRegionalSource_Europe);


  /* Same-image story saves still commit pending metadata; SRAM memcmp alone
   * must not swallow a region-only change. */
  CHECK(ArRegionalSession_RequestCosts(&campaign.active, campaign.active.revision,
                                      kArRegionalCostGroup_Miracles, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestTimers(&campaign.active, campaign.active.revision, kArRegionalSource_Japan));
  CHECK(SaveSystem_BeginNativeWrite(&error));
  CHECK(SaveSystem_EndNativeWrite(true, &error));
  CHECK(SaveSystem_AutoPersistIfChanged(&error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!memcmp(&loaded, &campaign.active, sizeof(loaded)));
  disk[0] ^= 1; Save_RecomputeChecksum(disk);
  CHECK(!ArRegionalCampaign_Continue(&campaign, path, disk, &error));
  CHECK(!campaign.active_valid);
  Remove(native); Remove(ini); Remove(donor);
}

static void Acknowledge(SaveFileFormat format) {
  const char *path = "regional-adoption-test.srm";
  Remove(path);
  uint8_t image[kActRaiserSramSize] = {0}, disk[kActRaiserSramSize], sequence = 0;
  uint16_t stocks[kArRegionalLairCount];
  for (unsigned i = 0; i < kArRegionalLairCount; ++i) stocks[i] = i * 2900;
  Save_RecomputeChecksum(image);
  SaveError error = {{0}};
  ArRegionalCampaign campaign;
  ArRegionalCampaign_Init(&campaign, 0, Identity, &sequence);
  CHECK(Save_WriteFile(format, path, image, &error));
  CHECK(ArRegionalCampaign_Continue(&campaign, path, image, &error));
  CHECK(campaign.active.lairs.initialized_towns == 0);
  ArRegionalSession loaded;
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Missing);
  /* A failed metadata replacement is neither consent persisted nor permission
   * to continue. It never rewrites the native save, and retry can succeed. */
  CHECK(MAKE_DIR("regional-adoption-test.srm.archeckpoint.tmp") == 0);
  CHECK(!ArRegionalCampaign_AcknowledgeLairHistory(&campaign, format, path, image, stocks, &error));
  CHECK(!campaign.active_valid);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Missing);
  CHECK(REMOVE_DIR("regional-adoption-test.srm.archeckpoint.tmp") == 0);
  CHECK(ArRegionalCampaign_AcknowledgeLairHistory(&campaign, format, path, image, stocks, &error));
  CHECK(campaign.active_valid && campaign.active.lairs.approximate_towns == 0x3f);
  CHECK(campaign.active.lairs.initialized_towns == 0x3f);
  CHECK(!memcmp(campaign.active.lairs.stock[0], stocks, sizeof(stocks)));
  CHECK(Save_LoadFile(format, path, disk, &error) && !memcmp(disk, image, sizeof(image)));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!memcmp(&loaded.lairs, &campaign.active.lairs, sizeof(loaded.lairs)));
  /* Cold Continue already has the estimate; a second acknowledgement is
   * idempotent, and exact/approximate distinctions are retained. */
  CHECK(ArRegionalCampaign_Continue(&campaign, path, image, &error));
  CHECK(ArRegionalCampaign_AcknowledgeLairHistory(&campaign, format, path, image, stocks, &error));
  CHECK(!memcmp(&loaded.lairs, &campaign.active.lairs, sizeof(loaded.lairs)));
  stocks[0] ^= 1;
  CHECK(!ArRegionalCampaign_AcknowledgeLairHistory(&campaign, format, path, image, stocks, &error));
  CHECK(!campaign.active_valid);
  stocks[0] ^= 1;
  CHECK(ArRegionalLairHistory_MarkDiverged(&loaded.lairs, 0));
  CHECK(ArRegionalSession_Save(&loaded, format, path, image, image, &error));
  CHECK(!ArRegionalCampaign_AcknowledgeLairHistory(&campaign, format, path, image, stocks, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.lairs.diverged_towns == 1);
  Remove(path);
  /* Partial histories retain their exact towns; only absent towns are adopted. */
  CHECK(Save_WriteFile(format, path, image, &error));
  CHECK(ArRegionalCampaign_Continue(&campaign, path, image, &error));
  CHECK(ArRegionalLairHistory_InitTown(&campaign.active.lairs, 0));
  for (unsigned i = 0; i < 4; ++i) stocks[i] = campaign.active.lairs.stock[0][i];
  CHECK(ArRegionalSession_Save(&campaign.active, format, path, image, image, &error));
  CHECK(ArRegionalCampaign_AcknowledgeLairHistory(&campaign, format, path, image, stocks, &error));
  CHECK(campaign.active.lairs.initialized_towns == 0x3f && campaign.active.lairs.approximate_towns == 0x3e);
  Remove(path);
  /* External save replacement while the prompt is open cannot be overwritten. */
  CHECK(Save_WriteFile(format, path, image, &error));
  image[10] = 1; Save_RecomputeChecksum(image);
  CHECK(!ArRegionalCampaign_AcknowledgeLairHistory(&campaign, format, path, image, stocks, &error));
  CHECK(!campaign.active_valid);
  CHECK(Save_LoadFile(format, path, disk, &error) && disk[10] == 0);
  Remove(path);
}

static void RecoveryCopy(SaveBackend backend) {
  const char *native = "regional-recovery-test.srm", *ini = "regional-recovery-test.ini";
  const char *path = backend == kSaveBackend_Ini ? ini : native;
  const SaveFileFormat format = backend == kSaveBackend_Ini ? kSaveFileFormat_Ini : kSaveFileFormat_NativeSrm;
  const char *folder = "regional-recovery-copy", *copy = "regional-recovery-copy/save.srm";
  const char *copy_name = "regional-recovery-copy/save.srm.arname";
  char name_path[256], journal_path[256];
  snprintf(name_path, sizeof(name_path), "%s.arname", path);
  snprintf(journal_path, sizeof(journal_path), "%s.archeckpoint", path);
  Remove(native); Remove(ini); Remove(copy); remove(copy_name); REMOVE_DIR(folder);
  remove(name_path);
  uint8_t image[kActRaiserSramSize] = {0}, disk[kActRaiserSramSize], sequence = 0;
  memcpy(image + 0x1439, "ELISE", 5);
  Save_RecomputeChecksum(image);
  SaveError error = {{0}};
  CHECK(SaveSystem_Attach(image, sizeof(image), backend, native, ini, &error));
  CHECK(!SaveSystem_CreateRecoveryCopy(folder, &error));
  CHECK(!sr_path_exists(folder));
  CHECK(Save_WriteFile(format, path, image, &error));
  CHECK(SaveSystem_LoadActive(&error));
  ArRegionalCampaign campaign;
  ArRegionalCampaign_Init(&campaign, 0, Identity, &sequence);
  SaveCommitHost host = ArRegionalCampaign_SaveHost(&campaign);
  CHECK(SaveSystem_SetCommitHost(&host));
  CHECK(ArRegionalCampaign_Continue(&campaign, path, image, &error));
  ArRegionalCampaign before = campaign;
  CHECK(SaveSystem_CreateRecoveryCopy(folder, &error));
  CHECK(Save_LoadFile(kSaveFileFormat_NativeSrm, copy, disk, &error));
  CHECK(!memcmp(disk, image, sizeof(disk)));
  ArRegionalSession loaded;
  CHECK(ArRegionalSession_Load(&loaded, 0, copy, disk, &error) == kSaveCheckpoint_Missing);
  CHECK(!memcmp(&campaign, &before, sizeof(campaign)));
  Remove(copy); CHECK(REMOVE_DIR(folder) == 0);

  ArRegionalCostPolicy jp, eu;
  ArRegionalCosts_Init(&jp, kArRegionalSource_Japan);
  ArRegionalCosts_Init(&eu, kArRegionalSource_Europe);
  CHECK(ArRegionalCampaign_NewGame(&campaign, &jp, &error));
  CHECK(SaveSystem_SetLocalizedPlayerName("Élise", "ELISE"));
  CHECK(!SaveSystem_CreateRecoveryCopy(folder, &error)); /* Unpersisted name. */
  CHECK(SaveSystem_BeginNativeWrite(&error));
  CHECK(!SaveSystem_CreateRecoveryCopy(folder, &error));
  CHECK(SaveSystem_EndNativeWrite(true, &error));
  CHECK(!SaveSystem_CreateRecoveryCopy(folder, &error)); /* Same SRAM, pending metadata. */
  CHECK(!sr_path_exists(folder));
  CHECK(SaveSystem_WriteActive(&error));
  ArRegionalSession saved = campaign.active;
  CHECK(ArRegionalCampaign_NewGame(&campaign, &eu, &error));
  before = campaign;
  CHECK(SaveSystem_CreateRecoveryCopy(folder, &error));
  CHECK(!memcmp(&campaign, &before, sizeof(campaign)));
  CHECK(Save_LoadFile(kSaveFileFormat_NativeSrm, copy, disk, &error));
  CHECK(!memcmp(disk, image, sizeof(disk)));
  CHECK(ArRegionalSession_Load(&loaded, 0, copy, disk, &error) == kSaveCheckpoint_Ready);
  CHECK(!memcmp(&loaded, &saved, sizeof(loaded))); /* Not the unsaved EU campaign. */
  CHECK(!SaveSystem_CreateRecoveryCopy(folder, &error)); /* Never overwrite. */
  CHECK(SaveSystem_CopyDurableImage(disk) && !memcmp(disk, image, sizeof(disk)));
  CHECK(Save_LoadFile(format, path, disk, &error) && !memcmp(disk, image, sizeof(disk)));

  /* Cold-load the recovery using the normal codec: native-compatible bytes,
   * regional identity/policies, and Unicode name all round-trip together. */
  CHECK(SaveSystem_Attach(disk, sizeof(disk), kSaveBackend_NativeSrm,
                         copy, "regional-recovery-copy/unused.ini", &error));
  CHECK(SaveSystem_LoadActive(&error));
  char name[64];
  CHECK(SaveSystem_CopyLocalizedPlayerName("ELISE", name, sizeof(name)));
  CHECK(!strcmp(name, "Élise"));
  CHECK(ArRegionalCampaign_Continue(&campaign, copy, disk, &error));
  CHECK(!memcmp(&campaign.active, &saved, sizeof(saved)));
  Remove(copy); remove(copy_name); CHECK(REMOVE_DIR(folder) == 0);

  CHECK(SaveSystem_Attach(image, sizeof(image), backend, native, ini, &error));
  CHECK(SaveSystem_LoadActive(&error));
  CHECK(SaveSystem_SetCommitHost(&host));
  /* Session-only or external image edits cannot silently become the recovery. */
  image[123] ^= 1; Save_RecomputeChecksum(image); SaveSystem_ResyncShadow();
  CHECK(!SaveSystem_CreateRecoveryCopy(folder, &error));
  CHECK(!sr_path_exists(folder));
  CHECK(SaveSystem_LoadActive(&error));
  memcpy(disk, image, sizeof(disk)); disk[0x1ff0] ^= 1;
  CHECK(Save_WriteFile(format, path, disk, &error));
  CHECK(!SaveSystem_CreateRecoveryCopy(folder, &error));
  CHECK(!sr_path_exists(folder));
  CHECK(Save_WriteFile(format, path, image, &error));
  SaveCommitHost unsupported = host; unsupported.copy_recovery = NULL;
  CHECK(SaveSystem_SetCommitHost(&unsupported));
  CHECK(!SaveSystem_CreateRecoveryCopy(folder, &error));
  CHECK(!sr_path_exists(folder));
  CHECK(SaveSystem_SetCommitHost(&host));

  /* A damaged source companion must not produce an apparently complete SRAM
   * recovery or change the source/live session. Partial directory is retained. */
  FILE *bad = fopen(journal_path, "wb");
  CHECK(bad != NULL);
  if (bad) { CHECK(fputs("broken", bad) >= 0); CHECK(fclose(bad) == 0); }
  before = campaign;
  CHECK(!SaveSystem_CreateRecoveryCopy(folder, &error));
  CHECK(sr_path_is_directory(folder) && !sr_path_exists(copy));
  CHECK(sr_path_exists(copy_name)); /* Name is durable before native image. */
  CHECK(!memcmp(&campaign, &before, sizeof(campaign)));
  CHECK(Save_LoadFile(format, path, disk, &error) && !memcmp(disk, image, sizeof(disk)));
  CHECK(!SaveSystem_CreateRecoveryCopy(folder, &error));
  remove(copy_name); CHECK(REMOVE_DIR(folder) == 0);
  Remove(native); Remove(ini); remove(name_path);
}

static void StorySnapshot(SaveBackend backend) {
  const char *native="regional-snapshot-test.srm", *ini="regional-snapshot-test.ini";
  const char *path=backend==kSaveBackend_Ini?ini:native;
  const SaveFileFormat format=backend==kSaveBackend_Ini?kSaveFileFormat_Ini:kSaveFileFormat_NativeSrm;
  Remove(native); Remove(ini);
  char blocked[256], name_path[256];
  snprintf(name_path,sizeof(name_path),"%s.arname",path); remove(name_path);
  uint8_t live[kActRaiserSramSize]={0}, candidate[kActRaiserSramSize], disk[kActRaiserSramSize], old[kActRaiserSramSize];
  uint8_t sequence=0;
  memcpy(live+0x1439,"ELISE",5); Save_RecomputeChecksum(live);
  memcpy(old,live,sizeof(old)); memcpy(candidate,live,sizeof(candidate));
  candidate[99]=73; Save_RecomputeChecksum(candidate);
  SaveError error={{0}};
  CHECK(SaveSystem_Attach(live,sizeof(live),backend,native,ini,&error));
  CHECK(Save_WriteFile(format,path,live,&error) && SaveSystem_LoadActive(&error));
  ArRegionalCampaign campaign; ArRegionalCampaign_Init(&campaign,0,Identity,&sequence);
  SaveCommitHost host=ArRegionalCampaign_SaveHost(&campaign);
  CHECK(SaveSystem_SetCommitHost(&host));
  CHECK(SaveSystem_CommitStorySnapshot(candidate,&error)==kSaveStorySnapshot_NotCommitted);
  CHECK(!memcmp(live,old,sizeof(live)));
  ArRegionalCostPolicy jp; ArRegionalCosts_Init(&jp,kArRegionalSource_Japan);
  CHECK(ArRegionalCampaign_NewGame(&campaign,&jp,&error));
  CHECK(SaveSystem_SetLocalizedPlayerName("Élise","ELISE"));
  CHECK(SaveSystem_BeginNativeWrite(&error));
  CHECK(SaveSystem_CommitStorySnapshot(candidate,&error)==kSaveStorySnapshot_NotCommitted);
  CHECK(SaveSystem_EndNativeWrite(true,&error));
  CHECK(SaveSystem_CommitStorySnapshot(candidate,&error)==kSaveStorySnapshot_NotCommitted);
  CHECK(SaveSystem_WriteActive(&error));
  CHECK(!campaign.pending_valid);
  const ArRegionalCampaign before=campaign;
  candidate[0]^=1;
  CHECK(SaveSystem_CommitStorySnapshot(candidate,&error)==kSaveStorySnapshot_NotCommitted);
  candidate[0]^=1;
  CHECK(SaveSystem_CommitStorySnapshot(NULL,&error)==kSaveStorySnapshot_NotCommitted);
  CHECK(SaveSystem_CommitStorySnapshot(live,&error)==kSaveStorySnapshot_NotCommitted);
  /* Fail after journal staging, before native replacement. A cold read still
   * chooses the original image. No pending snapshot or live/shadow mutation. */
  snprintf(blocked,sizeof(blocked),"%s.tmp",path); CHECK(MAKE_DIR(blocked)==0);
  CHECK(SaveSystem_CommitStorySnapshot(candidate,&error)==kSaveStorySnapshot_NotCommitted);
  CHECK(!memcmp(live,old,sizeof(live)) && !memcmp(&campaign,&before,sizeof(campaign)));
  CHECK(SaveSystem_CopyDurableImage(disk) && !memcmp(disk,old,sizeof(disk)));
  CHECK(Save_LoadFile(format,path,disk,&error) && !memcmp(disk,old,sizeof(disk)));
  ArRegionalSession loaded;
  CHECK(ArRegionalSession_Load(&loaded,0,path,disk,&error)==kSaveCheckpoint_Ready);
  CHECK(!memcmp(&loaded,&campaign.active,sizeof(loaded)));
  CHECK(REMOVE_DIR(blocked)==0);
  /* Name failure is explicitly post-commit. Retrying auto-persistence must
   * repair only the companion, not run conversion or revert campaign data. */
  snprintf(blocked,sizeof(blocked),"%s.arname.tmp",path); CHECK(MAKE_DIR(blocked)==0);
  CHECK(SaveSystem_CommitStorySnapshot(candidate,&error)==kSaveStorySnapshot_NamePending);
  CHECK(!memcmp(live,candidate,sizeof(live)) && !memcmp(&campaign,&before,sizeof(campaign)));
  CHECK(SaveSystem_CopyDurableImage(disk) && !memcmp(disk,candidate,sizeof(disk)));
  CHECK(Save_LoadFile(format,path,disk,&error) && !memcmp(disk,candidate,sizeof(disk)));
  CHECK(ArRegionalSession_Load(&loaded,0,path,disk,&error)==kSaveCheckpoint_Ready);
  CHECK(!memcmp(&loaded,&campaign.active,sizeof(loaded)));
  CHECK(REMOVE_DIR(blocked)==0 && SaveSystem_AutoPersistIfChanged(&error));
  CHECK(SaveSystem_LoadActive(&error));
  char name[64]; CHECK(SaveSystem_CopyLocalizedPlayerName("ELISE",name,sizeof(name)) && !strcmp(name,"Élise"));
  /* Semantic snapshots cannot overwrite a newer external image. */
  CHECK(ArRegionalCampaign_Continue(&campaign,path,live,&error));
  disk[5]^=1; Save_RecomputeChecksum(disk); CHECK(Save_WriteFile(format,path,disk,&error));
  CHECK(SaveSystem_CommitStorySnapshot(old,&error)==kSaveStorySnapshot_NotCommitted);
  CHECK(!memcmp(live,candidate,sizeof(live)));
  CHECK(Save_LoadFile(format,path,old,&error) && !memcmp(old,disk,sizeof(old)));
  Remove(native); Remove(ini); remove(name_path);
}

static bool FindArchiveBackup(char *out,size_t capacity) {
#ifdef _WIN32
  WIN32_FIND_DATAA data;
  HANDLE search=FindFirstFileA("regional-archive-test/backups/03/backup-*.arsave",&data);
  if(search==INVALID_HANDLE_VALUE)return false;
  snprintf(out,capacity,"regional-archive-test/backups/03/%s",data.cFileName);FindClose(search);return true;
#else
  DIR *dir=opendir("regional-archive-test/backups/03");if(!dir)return false;
  struct dirent *entry;bool found=false;
  while((entry=readdir(dir)))if(!strncmp(entry->d_name,"backup-",7) && strstr(entry->d_name,".arsave")) {
    snprintf(out,capacity,"regional-archive-test/backups/03/%s",entry->d_name);found=true;break;
  }
  closedir(dir);return found;
#endif
}
static void CampaignArchive(SaveBackend backend) {
  const char *root="regional-archive-test",*donor="regional-archive-test/donor.srm";
  const char *native="regional-archive-test/target.srm",*ini="regional-archive-test/target.ini";
  const char *path=backend==kSaveBackend_Ini?ini:native;
  const char *archive="regional-archive-test/campaign.arsave",*bad="regional-archive-test/bad.arsave";
  CHECK(MAKE_DIR(root)==0);
  SaveError error={{0}};uint8_t image[kActRaiserSramSize]={0},saved[kActRaiserSramSize],disk[kActRaiserSramSize];
  memcpy(image+0x1439,"ELISE",5);Save_RecomputeChecksum(image);
  uint8_t sequence=0;ArRegionalCampaign campaign;
  ArRegionalCampaign_Init(&campaign,7,Identity,&sequence);
  ArRegionalCostPolicy jp,eu;ArRegionalCosts_Init(&jp,kArRegionalSource_Japan);ArRegionalCosts_Init(&eu,kArRegionalSource_Europe);
  CHECK(ArRegionalCampaign_NewGame(&campaign,&jp,&error));
  campaign.active.randomizer=RandomizerConfig_Default();campaign.active.randomizer.enabled=true;
  campaign.active.randomizer.seed=424242;campaign.active.randomizer.regional_action=true;
  CHECK(ArRegionalSession_RequestTimers(&campaign.active,campaign.active.revision,kArRegionalSource_Europe));
  CHECK(ArRegionalLairHistory_InitTown(&campaign.active.lairs,0));
  CHECK(ArRegionalSession_Save(&campaign.active,kSaveFileFormat_NativeSrm,donor,NULL,image,&error));
  ArRegionalSession exported=campaign.active;
  CHECK(SaveSystem_Attach(image,sizeof(image),kSaveBackend_NativeSrm,donor,"unused-archive.ini",&error));
  CHECK(SaveSystem_LoadActive(&error));SaveCommitHost host=ArRegionalCampaign_SaveHost(&campaign);
  CHECK(SaveSystem_SetCommitHost(&host));
  CHECK(SaveSystem_SetLocalizedPlayerName("Élise","ELISE") && SaveSystem_AutoPersistIfChanged(&error));
  CHECK(ArRegionalCampaign_NewGame(&campaign,&eu,&error)); /* Export is the durable JP campaign. */
  CHECK(SaveSystem_ExportCampaign(archive,&error));
  char blocked[256];snprintf(blocked,sizeof(blocked),"%s.tmp",archive);CHECK(MAKE_DIR(blocked)==0);
  CHECK(!SaveSystem_ExportCampaign(archive,&error));CHECK(REMOVE_DIR(blocked)==0); /* Prior archive survives. */
  SaveCommitHost unsupported=host;unsupported.read_campaign=NULL;
  CHECK(SaveSystem_SetCommitHost(&unsupported));CHECK(!SaveSystem_ExportCampaign(bad,&error));
  CHECK(!sr_path_exists(bad));

  ArRegionalCampaign_Init(&campaign,2,Identity,&sequence);CHECK(ArRegionalCampaign_NewGame(&campaign,&eu,&error));
  campaign.active.randomizer=RandomizerConfig_Default();campaign.active.randomizer.seed=555;
  ArRegionalSession previous=campaign.active;
  image[100]=42;Save_RecomputeChecksum(image);memcpy(saved,image,sizeof(saved));
  CHECK(ArRegionalSession_Save(&campaign.active,(SaveFileFormat)backend,path,NULL,image,&error));
  CHECK(SaveSystem_Attach(image,sizeof(image),backend,native,ini,&error) && SaveSystem_LoadActive(&error));
  host=ArRegionalCampaign_SaveHost(&campaign);CHECK(SaveSystem_SetCommitHost(&host));
  CHECK(SaveSystem_SetStorageRoot(root,2,&error));
  CHECK(SaveSystem_SetLocalizedPlayerName("エリーゼ","ELISE") && SaveSystem_AutoPersistIfChanged(&error));
  CHECK(SaveSystem_Import(archive,true,&error));
  ArRegionalSession loaded;exported.slot=2;
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  CHECK(!memcmp(&loaded,&exported,sizeof(loaded)));
  char name[64];CHECK(SaveSystem_CopyLocalizedPlayerName("ELISE",name,sizeof(name)) && !strcmp(name,"Élise"));
  CHECK(SaveSystem_LoadActive(&error));
  CHECK(SaveSystem_CopyLocalizedPlayerName("ELISE",name,sizeof(name)) && !strcmp(name,"Élise"));
  char backup[512]={0};CHECK(FindArchiveBackup(backup,sizeof(backup)));
  CHECK(SaveSystem_Import(backup,false,&error));
  CHECK(!memcmp(image,saved,sizeof(image)));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready && !memcmp(&loaded,&previous,sizeof(loaded)));
  CHECK(SaveSystem_CopyLocalizedPlayerName("ELISE",name,sizeof(name)) && !strcmp(name,"エリーゼ"));

  /* Reject truncated, corrupt, future-version and invalid feature payloads
   * before replacing native bytes, campaign metadata or the enhanced name. */
  uint8_t bytes[kSaveCampaignPayloadCapacity+kActRaiserSramSize+512];
  FILE *file=fopen(archive,"rb");CHECK(file!=NULL);size_t count=file?fread(bytes,1,sizeof(bytes),file):0;
  if(file)fclose(file);CHECK(count>16+kActRaiserSramSize);
  for(unsigned fault=0;fault<4;++fault) {
    size_t length=count;
    unsigned offset=fault==1?100:fault==2?7:16+kActRaiserSramSize;
    if(!fault)--length;else bytes[offset]^=0x40;
    if(fault>=2) {
      uint64_t hash=DeterministicHash_Fnv1a64(DETERMINISTIC_HASH_FNV1A64_OFFSET,bytes,length-8);
      ByteOrder_WriteLe32(bytes+length-8,(uint32_t)hash);ByteOrder_WriteLe32(bytes+length-4,(uint32_t)(hash>>32));
    }
    CHECK(Save_WriteCompanionFile(bad,bytes,length,&error));CHECK(!SaveSystem_Import(bad,false,&error));
    CHECK(Save_LoadFile((SaveFileFormat)backend,path,disk,&error) && !memcmp(disk,saved,sizeof(disk)));
    CHECK(ArRegionalSession_Load(&loaded,2,path,disk,&error)==kSaveCheckpoint_Ready && !memcmp(&loaded,&previous,sizeof(loaded)));
    CHECK(SaveSystem_CopyLocalizedPlayerName("ELISE",name,sizeof(name)) && !strcmp(name,"エリーゼ"));
    if(fault)bytes[offset]^=0x40;
  }
  snprintf(blocked,sizeof(blocked),"%s.tmp",path);CHECK(MAKE_DIR(blocked)==0);
  CHECK(!SaveSystem_Import(archive,false,&error));CHECK(REMOVE_DIR(blocked)==0);
  CHECK(Save_LoadFile((SaveFileFormat)backend,path,disk,&error) && !memcmp(disk,saved,sizeof(disk)));
  CHECK(ArRegionalSession_Load(&loaded,2,path,disk,&error)==kSaveCheckpoint_Ready && !memcmp(&loaded,&previous,sizeof(loaded)));
  /* A post-commit name failure is reported as committed and retried without
   * rerunning the import or losing the imported campaign/seed. */
  snprintf(blocked,sizeof(blocked),"%s.arname.tmp",path);CHECK(MAKE_DIR(blocked)==0);
  CHECK(SaveSystem_Import(archive,false,&error));CHECK(!SaveSystem_AutoPersistIfChanged(&error));
  CHECK(REMOVE_DIR(blocked)==0);CHECK(SaveSystem_AutoPersistIfChanged(&error));
  CHECK(SaveSystem_LoadActive(&error));
  CHECK(SaveSystem_CopyLocalizedPlayerName("ELISE",name,sizeof(name)) && !strcmp(name,"Élise"));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready && !memcmp(&loaded,&exported,sizeof(loaded)));
  Remove(donor);Remove(native);Remove(ini);remove(archive);remove(bad);remove(backup);
  snprintf(blocked,sizeof(blocked),"%s.arname",donor);remove(blocked);
  snprintf(blocked,sizeof(blocked),"%s.arname",path);remove(blocked);
  CHECK(REMOVE_DIR("regional-archive-test/backups/03")==0);
  CHECK(REMOVE_DIR("regional-archive-test/backups")==0);CHECK(REMOVE_DIR(root)==0);
}

static void SettingsOnly(SaveFileFormat format) {
  const char *path="regional-settings-only.srm";
  Remove(path);
  uint8_t image[kActRaiserSramSize]={0},disk[kActRaiserSramSize];
  Save_RecomputeChecksum(image);SaveError error={{0}};
  const uint8_t id[16]={19};const ArRegionalCostPolicy costs={{0}};
  ArRegionalSession saved,live,after,loaded;
  CHECK(ArRegionalSession_NewGame(&saved,2,id,&costs));
  for(unsigned town=0;town<6;++town)CHECK(ArRegionalLairHistory_InitTown(&saved.lairs,town));
  CHECK(ArRegionalLairReloads_Init(&saved.reloads));
  saved.randomizer=RandomizerConfig_Default();saved.randomizer.enabled=true;saved.randomizer.seed=321;
  CHECK(ArRegionalSession_Save(&saved,format,path,NULL,image,&error));
  live=saved;
  live.arrival_locked=true;live.effective.terrain=kArRegionalSource_Japan;
  live.randomizer.seed=999; /* Live data must not be copied into the old save. */
  after=live;
  CHECK(ArRegionalSession_RequestArtwork(&after,after.revision,kArRegionalArtwork_TitleBackground,kArRegionalSource_Japan));
  CHECK(ArRegionalCampaign_SaveSettings(&live,&after,format,path,image,&error));
  CHECK(Save_LoadFile(format,path,disk,&error) && !memcmp(image,disk,sizeof(image)));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  CHECK(loaded.requested.artwork.source[kArRegionalArtwork_TitleBackground]==kArRegionalSource_Japan);
  CHECK(!memcmp(&loaded.effective,&saved.effective,sizeof(saved.effective)));
  CHECK(!memcmp(&loaded.lairs,&saved.lairs,sizeof(saved.lairs)));
  CHECK(!memcmp(&loaded.reloads,&saved.reloads,sizeof(saved.reloads)));
  CHECK(!memcmp(&loaded.sim_actors,&saved.sim_actors,sizeof(saved.sim_actors)));
  CHECK(!loaded.arrival_locked && loaded.randomizer.seed==321);
  CHECK(!ArRegionalCampaign_SaveSettings(&live,&after,format,path,image,&error)); /* stale request */
  live=loaded;after=live;after.campaign[0]^=1;
  CHECK(!ArRegionalCampaign_SaveSettings(&live,&after,format,path,image,&error));
  after=live;
  CHECK(ArRegionalSession_SetPopulationProfile(&after,after.revision,kArRegionalSource_Japan));
  CHECK(!ArRegionalCampaign_SaveSettings(&live,&after,format,path,image,&error)); /* no silent town reset */
  after=live;CHECK(ArRegionalSession_RequestTerrain(&after,after.revision,kArRegionalSource_Japan));
  char blocked[256];snprintf(blocked,sizeof(blocked),"%s.archeckpoint.tmp",path);CHECK(MAKE_DIR(blocked)==0);
  CHECK(!ArRegionalCampaign_SaveSettings(&live,&after,format,path,image,&error));
  CHECK(REMOVE_DIR(blocked)==0);
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready && !memcmp(&live,&loaded,sizeof(live)));
  disk[100]=1;Save_RecomputeChecksum(disk);CHECK(Save_WriteFile(format,path,disk,&error));
  CHECK(!ArRegionalCampaign_SaveSettings(&live,&after,format,path,image,&error)); /* external file edit */
  Remove(path);
  /* A cosmetic edit may create metadata for a legacy save, but never invents
   * lair history or acknowledges the Continue prompt. */
  CHECK(Save_WriteFile(format,path,image,&error));
  CHECK(ArRegionalSession_NewGame(&live,2,id,&costs));after=live;
  CHECK(ArRegionalSession_RequestArtwork(&after,after.revision,kArRegionalArtwork_TitleBackground,kArRegionalSource_Japan));
  CHECK(ArRegionalCampaign_SaveSettings(&live,&after,format,path,image,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  CHECK(!loaded.lairs.initialized_towns && !loaded.reloads.initialized_towns && !loaded.randomizer.generator);
  Remove(path);
}

int main(void) {
  SettingsOnly(kSaveFileFormat_NativeSrm);SettingsOnly(kSaveFileFormat_Ini);
  CampaignArchive(kSaveBackend_NativeSrm);
  CampaignArchive(kSaveBackend_Ini);
  uint8_t a[16], b[16];
  CHECK(HostCampaignIdentity_Create(NULL, a));
  CHECK(HostCampaignIdentity_Create(NULL, b));
  CHECK(memcmp(a, b, sizeof(a)) != 0);
  CHECK((a[6] & 0xf0) == 0x40 && (a[8] & 0xc0) == 0x80);
  CHECK(!HostCampaignIdentity_Create(NULL, NULL));
  Run(kSaveBackend_NativeSrm); Run(kSaveBackend_Ini);
  Acknowledge(kSaveFileFormat_NativeSrm); Acknowledge(kSaveFileFormat_Ini);
  RecoveryCopy(kSaveBackend_NativeSrm); RecoveryCopy(kSaveBackend_Ini);
  StorySnapshot(kSaveBackend_NativeSrm); StorySnapshot(kSaveBackend_Ini);
  return failures ? 1 : 0;
}
