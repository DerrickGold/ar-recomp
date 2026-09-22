#include "regional/regional_campaign.h"
#include "host/campaign_identity.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MAKE_DIR(p) _mkdir(p)
#define REMOVE_DIR(p) _rmdir(p)
#else
#include <sys/stat.h>
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
  CHECK(campaign.active.requested.source[0] == kArRegionalCost_US);
  ArRegionalSession loaded;
  CHECK(ArRegionalSession_Load(&loaded, 0, path, durable, &error) == kSaveCheckpoint_Missing);
  ArRegionalCostPolicy jp, eu;
  ArRegionalCosts_Init(&jp, kArRegionalCost_Japan);
  ArRegionalCosts_Init(&eu, kArRegionalCost_Europe);
  CHECK(ArRegionalCampaign_NewGame(&campaign, &jp, &error));
  ArRegionalSession saving = campaign.active;
  CHECK(ArRegionalSession_Load(&loaded, 0, path, durable, &error) == kSaveCheckpoint_Missing);
  CHECK(SaveSystem_BeginNativeWrite(&error));
  image[100] = 1;
  CHECK(SaveSystem_AutoPersistIfChanged(&error));
  Save_RecomputeChecksum(image);
  CHECK(SaveSystem_EndNativeWrite(true, &error));
  CHECK(campaign.pending_valid);
  CHECK(ArRegionalCampaign_NewGame(&campaign, &eu, &error));
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
  CHECK(campaign.active.requested.source[0] == kArRegionalCost_Europe);
  /* A marker outside the checksum belongs to the durable JP game, not the
   * unsaved EU one. No direct Save_WriteFile bypass of companion rotation. */
  image[0x1ff0] ^= 1;
  CHECK(SaveSystem_AutoPersistIfChanged(&error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!memcmp(loaded.campaign, saving.campaign, 16));
  SaveEditRequest edits;
  SaveEditRequest_Clear(&edits); edits.master_level = 5;
  CHECK(SaveSystem_ApplyEdits(&edits, true, true, false, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.source[0] == kArRegionalCost_Japan);

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
  CHECK(campaign.active.requested.source[0] == kArRegionalCost_Europe);

  /* Same-image story saves still commit pending metadata; SRAM memcmp alone
   * must not swallow a region-only change. */
  CHECK(ArRegionalSession_RequestCosts(&campaign.active, campaign.active.revision,
                                      kArRegionalCostGroup_Miracles, kArRegionalCost_Japan));
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

int main(void) {
  uint8_t a[16], b[16];
  CHECK(HostCampaignIdentity_Create(NULL, a));
  CHECK(HostCampaignIdentity_Create(NULL, b));
  CHECK(memcmp(a, b, sizeof(a)) != 0);
  CHECK((a[6] & 0xf0) == 0x40 && (a[8] & 0xc0) == 0x80);
  CHECK(!HostCampaignIdentity_Create(NULL, NULL));
  Run(kSaveBackend_NativeSrm); Run(kSaveBackend_Ini);
  return failures ? 1 : 0;
}
